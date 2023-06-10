#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "err.h"
#include "pci.h"
#include "serial.h"
#include "virtio-pci.h"
#include "vm.h"
#include "vm-arch.h"

int vm_init(vm_t *v)
{
    /* Clear vm_t structure */
    memset(v, 0, sizeof(*v));

    if ((v->kvm_fd = open("/dev/kvm", O_RDWR)) < 0)
        return throw_err("Failed to open /dev/kvm");

    if ((v->vm_fd = ioctl(v->kvm_fd, KVM_CREATE_VM, 0)) < 0)
        return throw_err("Failed to create vm");

    if (vm_arch_init(v) != 0)
        return -1;

    v->mem = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!v->mem)
        return throw_err("Failed to mmap vm memory");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = DRAM_BASE,
        .memory_size = RAM_SIZE,
        .userspace_addr = (__u64) v->mem,
    };

    if (ioctl(v->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        return throw_err("Failed to set user memory region");

    if ((v->vcpu_fd = ioctl(v->vm_fd, KVM_CREATE_VCPU, 0)) < 0)
        return throw_err("Failed to create vcpu");

    if (vm_arch_init_cpu(v) != 0)
        return -1;

    bus_init(&v->io_bus);
    bus_init(&v->mmio_bus);
    pci_init(&v->pci, &v->io_bus);
    if (serial_init(&v->serial, &v->io_bus))
        return throw_err("Failed to init UART device");
    virtio_blk_init(&v->virtio_blk_dev);

    return 0;
}

int vm_load_diskimg(vm_t *v, const char *diskimg_file)
{
    if (diskimg_init(&v->diskimg, diskimg_file) < 0)
        return -1;
    virtio_blk_init_pci(&v->virtio_blk_dev, &v->diskimg, &v->pci, &v->io_bus,
                        &v->mmio_bus);
    return 0;
}

void vm_handle_io(vm_t *v, struct kvm_run *run)
{
    uint64_t addr = run->io.port;
    void *data = (void *) run + run->io.data_offset;
    bool is_write = run->io.direction == KVM_EXIT_IO_OUT;

    for (int i = 0; i < run->io.count; i++) {
        bus_handle_io(&v->io_bus, data, is_write, addr, run->io.size);
        addr += run->io.size;
    }
}

void vm_handle_mmio(vm_t *v, struct kvm_run *run)
{
    struct bus *bus = &v->mmio_bus;
    #ifdef CONFIG_AARCH64
    if (run->mmio.phys_addr < 0x10000)
        bus = &v->io_bus;
    #endif
    bus_handle_io(bus, run->mmio.data, run->mmio.is_write,
                  run->mmio.phys_addr, run->mmio.len);
}

int vm_run(vm_t *v)
{
    int run_size = ioctl(v->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run =
        mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, v->vcpu_fd, 0);

    if (vm_arch_late_init(v) != 0)
        return -1;

    while (1) {
        int err = ioctl(v->vcpu_fd, KVM_RUN, 0);
        if (err < 0 && (errno != EINTR && errno != EAGAIN)) {
            munmap(run, run_size);
            return throw_err("Failed to execute kvm_run");
        }
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            vm_handle_io(v, run);
            break;
        case KVM_EXIT_MMIO:
            vm_handle_mmio(v, run);
            break;
        case KVM_EXIT_INTR:
            serial_console(&v->serial);
            break;
        case KVM_EXIT_SHUTDOWN:
            printf("shutdown\n");
            munmap(run, run_size);
            return 0;
        default:
            printf("reason: %d\n", run->exit_reason);
            munmap(run, run_size);
            return -1;
        }
    }
}

void *vm_guest_to_host(vm_t *v, uint64_t guest)
{
    if (guest < DRAM_BASE || guest >= DRAM_BASE + RAM_SIZE)
        return NULL;

    return (void *) ((uintptr_t) v->mem + (guest - DRAM_BASE));
}

void vm_irqfd_register(vm_t *v, int fd, int gsi, int flags)
{
    struct kvm_irqfd irqfd = {
        .fd = fd,
        .gsi = gsi,
        .flags = flags,
    };

    if (ioctl(v->vm_fd, KVM_IRQFD, &irqfd) < 0)
        throw_err("Failed to set the status of IRQFD");
}

void vm_ioeventfd_register(vm_t *v,
                           int fd,
                           unsigned long long addr,
                           int len,
                           int flags)
{
    struct kvm_ioeventfd ioeventfd = {
        .fd = fd,
        .addr = addr,
        .len = len,
        .flags = flags,
    };

    if (ioctl(v->vm_fd, KVM_IOEVENTFD, &ioeventfd) < 0)
        throw_err("Failed to set the status of IOEVENTFD");
}

void vm_exit(vm_t *v)
{
    serial_exit(&v->serial);
    virtio_blk_exit(&v->virtio_blk_dev);
    close(v->kvm_fd);
    close(v->vm_fd);
    close(v->vcpu_fd);
    munmap(v->mem, RAM_SIZE);
}

int vm_alloc_irq(vm_t *v)
{
    if (v->nirq < VM_IRQ_BASE)
        v->nirq = VM_IRQ_BASE;

    return v->nirq++;
}
