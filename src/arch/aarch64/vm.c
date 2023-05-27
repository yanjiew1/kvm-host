#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <sys/ioctl.h>
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

    struct kvm_device_attr dist_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = KVM_VGIC_V3_ADDR_TYPE_DIST,
        .addr = (uint64_t) &dist_addr
    };

    struct kvm_device_attr redist_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = KVM_VGIC_V3_ADDR_TYPE_REDIST,
        .addr = (uint64_t) &redist_addr
    };

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

    return 0;
}

int vm_arch_init(vm_t *v)
{
    /* Create IRQ chip */
    if (vm_create_gic(v) < 0)
        return -1;

    return 0;
}

int vm_arch_init_mem(vm_t *v)
{
    return throw_err("TODO: vm_arch_init_mem");
}

int vm_arch_init_cpu(vm_t *v)
{
    return throw_err("TODO: vm_arch_init_cpu");
}

int vm_load_image(vm_t *v, const char *image_path)
{
    return throw_err("TODO: vm_load_image");
}

int vm_load_initrd(vm_t *v, const char *initrd_path)
{
    return throw_err("TODO: vm_load_initrd");
}