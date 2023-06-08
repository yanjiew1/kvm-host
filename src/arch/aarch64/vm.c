#include <assert.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "err.h"
#include "vm-arch.h"
#include "vm.h"

static int vm_create_gic(vm_t *v)
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
        return throw_err("Failed to create IRQ chip\n");

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

static int vm_init_gic(vm_t *v)
{
    int nirqs = 1024;

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

int vm_arch_post_init(vm_t *v)
{
    if (vm_init_gic(v) < 0)
        return -1;

    return 0;
}

int vm_arch_init(vm_t *v)
{
    /* Create IRQ chip */
    if (vm_create_gic(v) < 0)
        return -1;

    return 0;
}

int vm_arch_init_cpu(vm_t *v)
{
    struct kvm_vcpu_init vcpu_init;
    if (ioctl(v->vm_fd, KVM_ARM_PREFERRED_TARGET, &vcpu_init) < 0)
        return throw_err("Failed to find perferred target cpu type\n");

    if (ioctl(v->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init))
        return throw_err("Failed to initialize vCPU\n");

    if (ioctl(v->kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_SVE) > 0) {
        if (ioctl(v->vcpu_fd, KVM_ARM_VCPU_FINALIZE, KVM_ARM_VCPU_SVE) < 0)
            return throw_err("Failed to initialize SVE feature\n");
    }

    return 0;
}

int vm_load_image(vm_t *v, const char *image_path)
{
    int fd = open(image_path, O_RDONLY);
    if (fd < 0)
        return 1;

    struct stat st;
    fstat(fd, &st);
    size_t datasz = st.st_size;

    if (datasz < sizeof(arm64_kernel_header_t)) {
        close(fd);
        return throw_err("Kernel image too small\n");
    }

    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED)
        return throw_err("Failed to mmap the kernel image\n");

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
    munmap(data, datasz);

    v->arch.entry = ARM_KERNEL_BASE + offset;

    return 0;
}

int vm_load_initrd(vm_t *v, const char *initrd_path)
{
    int fd = open(initrd_path, O_RDONLY);
    if (fd < 0)
        return 1;

    struct stat st;
    fstat(fd, &st);
    size_t datasz = st.st_size;
    if (datasz > ARM_INITRD_SIZE) {
        close(fd);
        return throw_err("Initrd image too large\n");
    }

    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED)
        return throw_err("Failed to mmap the initrd image\n");

    void *dest = vm_guest_to_host(v, ARM_INITRD_BASE);
    memmove(dest, data, datasz);
    munmap(data, datasz);

    v->arch.has_initrd = true;

    return 0;
}
