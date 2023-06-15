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
#include "vm-arch.h"
#include "vm.h"

static int_fast32_t vm_init_ram(vm_t *v)
{
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

    return 0;
}

static int vm_init_vcpu(vm_t *v)
{
    if ((v->vcpu_fd = ioctl(v->vm_fd, KVM_CREATE_VCPU, 0)) < 0)
        return throw_err("Failed to create vcpu");

    if (vm_arch_init_cpu(v) != 0)
        return -1;

    return 0;
}

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

    if (vm_init_ram(v) != 0)
        return -1;

    if (vm_init_vcpu(v) != 0)
        return -1;

    if (vm_arch_init_platform_devices(v) != 0)
        return -1;

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

int vm_late_init(vm_t *v)
{
    if (vm_arch_late_init(v) != 0)
        return -1;

    return 0;
}

int vm_run(vm_t *v)
{
    int run_size = ioctl(v->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run =
        mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, v->vcpu_fd, 0);

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
