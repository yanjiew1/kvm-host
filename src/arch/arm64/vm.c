#if !defined(__aarch64__) || !defined(__linux__)
#error "This virtual machine requires Linux/aarch64."
#endif

#include <asm/kvm.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "err.h"
#include "vm-arch.h"
#include "vm.h"

static int _create_gic(vm_t *v)
{
    uint64_t dist_addr = ARM_GIC_DIST_BASE;
    uint64_t redist_addr = ARM_GIC_REDIST_BASE;

    struct kvm_create_device gic_device = {
        .type = KVM_DEV_TYPE_ARM_VGIC_V3,
    };

    struct kvm_device_attr dist_attr = {.group = KVM_DEV_ARM_VGIC_GRP_ADDR,
                                        .attr = KVM_VGIC_V3_ADDR_TYPE_DIST,
                                        .addr = (uint64_t) &dist_addr};

    struct kvm_device_attr redist_attr = {.group = KVM_DEV_ARM_VGIC_GRP_ADDR,
                                          .attr = KVM_VGIC_V3_ADDR_TYPE_REDIST,
                                          .addr = (uint64_t) &redist_addr};

    if (ioctl(v->vm_fd, KVM_CREATE_DEVICE, &gic_device) < 0)
        return throw_err(
            "Failed to create IRQ chip.\n"
            "Please make sure that the host support gicv3.\n");

    int gic_fd = gic_device.fd;

    /* Setup memory mapping */
    if (ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &dist_attr) < 0) {
        close(gic_fd);
        return throw_err("Failed to set distributer address\n");
    }

    if (ioctl(gic_fd, KVM_SET_DEVICE_ATTR, &redist_attr) < 0) {
        close(gic_fd);
        return throw_err("Failed to set redistributer address\n");
    }

    v->arch.gic_fd = gic_fd;

    return 0;
}

static int _finalize_gic(vm_t *v)
{
    /* Although the kernel document says that the maximum number can be set is
     * 1024, the fact is that the maximum number of IRQs is 992 */
    int nirqs = 992;

    struct kvm_device_attr nr_irqs_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_NR_IRQS,
        .addr = (uint64_t) &nirqs,
    };

    struct kvm_device_attr vgic_init_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_CTRL,
        .attr = KVM_DEV_ARM_VGIC_CTRL_INIT,
    };

    /* setup IRQ lines */
    if (ioctl(v->arch.gic_fd, KVM_SET_DEVICE_ATTR, &nr_irqs_attr) < 0)
        return throw_err("Failed to set the number of IRQs\n");

    /* initialize GIC */
    if (ioctl(v->arch.gic_fd, KVM_SET_DEVICE_ATTR, &vgic_init_attr) < 0)
        return throw_err("Failed to initialize the vgic\n");

    return 0;
}

int vm_arch_init(vm_t *v)
{
    /* Create IRQ chip */
    if (_create_gic(v) < 0)
        return -1;

    return 0;
}

int vm_arch_cpu_init(vm_t *v)
{
    struct kvm_vcpu_init vcpu_init;
    if (ioctl(v->vm_fd, KVM_ARM_PREFERRED_TARGET, &vcpu_init) < 0)
        return throw_err("Failed to find perferred target cpu type\n");

    vcpu_init.features[0] |= (1UL << KVM_ARM_VCPU_PSCI_0_2);

    if (ioctl(v->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init))
        return throw_err("Failed to initialize vCPU\n");

    return 0;
}

static void _pio_handler(void *owner,
                         void *data,
                         uint8_t is_write,
                         uint64_t offset,
                         uint8_t size)
{
    vm_t *v = (vm_t *) owner;
    bus_handle_io(&v->io_bus, data, is_write, offset, size);
}

int vm_arch_init_platform_device(vm_t *v)
{
    bus_init(&v->io_bus);
    bus_init(&v->mmio_bus);
    dev_init(&v->arch.iodev, ARM_IOPORT_BASE, ARM_IOPORT_SIZE, v, _pio_handler);
    bus_register_dev(&v->mmio_bus, &v->arch.iodev);
    pci_init(&v->pci);
    v->pci.pci_mmio_dev.base = ARM_PCI_CFG_BASE;
    bus_register_dev(&v->mmio_bus, &v->pci.pci_mmio_dev);
    if (serial_init(&v->serial, &v->io_bus))
        return throw_err("Failed to init UART device");

    v->serial.irq_num = vm_irq_alloc(v);

    if (_finalize_gic(v) < 0)
        return -1;

    return 0;
}

/* Reference https://docs.kernel.org/arm64/booting.html */
typedef struct {
    uint32_t code0;       /* Executable code */
    uint32_t code1;       /* Executable code */
    uint64_t text_offset; /* Image load offset, little endian */
    uint64_t image_size;  /* Effective Image size, little endian */
    uint64_t flags;       /* kernel flags, little endian */
    uint64_t res2;        /* reserved */
    uint64_t res3;        /* reserved */
    uint64_t res4;        /* reserved */
    uint32_t magic;       /* Magic number, little endian, "ARM\x64" */
    uint32_t res5;        /* reserved (used for PE COFF offset) */
} arm64_kernel_header_t;

int vm_arch_load_image(vm_t *v, void *data, size_t datasz)
{
    arm64_kernel_header_t *header = data;
    if (header->magic != 0x644d5241U) {
        munmap(data, datasz);
        return throw_err("Invalid kernel image\n");
    }

    uint64_t offset;
    if (header->image_size == 0)
        offset = 0x80000;
    else
        offset = header->text_offset;

    if (offset + datasz >= ARM_KERNEL_SIZE ||
        offset + header->image_size >= ARM_KERNEL_SIZE) {
        munmap(data, datasz);
        return throw_err("Image size too large\n");
    }

    void *dest = vm_guest_to_host(v, ARM_KERNEL_BASE + offset);
    assert(dest);

    memmove(dest, data, datasz);
    v->arch.entry = ARM_KERNEL_BASE + offset;
    return 0;
}

int vm_arch_load_initrd(vm_t *v, void *data, size_t datasz)
{
    void *dest = vm_guest_to_host(v, ARM_INITRD_BASE);
    memmove(dest, data, datasz);
    v->arch.initrdsz = datasz;
    return 0;
}

static int _init_reg(vm_t *v)
{
    struct kvm_one_reg reg;
    uint64_t data;

    reg.addr = (uint64_t) &data;
#define _REG(r)                                                   \
    (KVM_REG_ARM_CORE_REG(r) | KVM_REG_ARM_CORE | KVM_REG_ARM64 | \
     KVM_REG_SIZE_U64)
    /* Set PSTATE: Mask all interrupts */
    data = PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h;
    reg.id = _REG(regs.pstate);
    if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
        return throw_err("Failed to set PSTATE register\n");

    /* Clear x1 ~ x3 */
    for (int i = 0; i < 3; i++) {
        data = 0;
        reg.id = _REG(regs.regs[i]);
        if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
            return throw_err("Failed to set x%d\n", i);
    }

    /* Set x0 to device tree */
    data = ARM_FDT_BASE;
    reg.id = _REG(regs.regs[0]);
    if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
        return throw_err("Failed to set x0\n");

    /* Set program counter */
    data = v->arch.entry;
    reg.id = _REG(regs.pc);
    if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
        return throw_err("Failed to set program counter\n");

#undef _REG
    return 0;
}

int vm_late_init(vm_t *v)
{
    if (vm_arch_generate_fdt(v) < 0)
        return -1;

    if (_init_reg(v) < 0)
        return -1;

    return 0;
}

int vm_irq_line(vm_t *v, int irq, int level)
{
    struct kvm_irq_level irq_level = {
        .level = level,
    };

    irq_level.irq = (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT) |
                    ((irq + ARM_GIC_SPI_BASE) & KVM_ARM_IRQ_NUM_MASK);

    if (ioctl(v->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
        return throw_err("Failed to set the status of an IRQ line, %llx\n",
                         irq_level.irq);

    return 0;
}

/* Reference:
 * https://developer.arm.com/documentation/ddi0601/2022-03/AArch64-Registers/MPIDR-EL1--Multiprocessor-Affinity-Register?lang=en
 */
#define ARM_MPIDR_BITMASK 0xFF00FFFFFFUL
#define ARM_MPIDR_REG_ID ARM64_SYS_REG(3, 0, 0, 0, 5)

int vm_arch_get_mpidr(vm_t *v, uint64_t *mpidr)
{
    struct kvm_one_reg reg;
    reg.addr = (uint64_t) mpidr;
    reg.id = ARM_MPIDR_REG_ID;

    if (ioctl(v->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
        return throw_err("Failed to get MPIDR register\n");

    *mpidr &= ARM_MPIDR_BITMASK;
    return 0;
}
