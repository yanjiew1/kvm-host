#if !defined(__x86_64__) || !defined(__linux__)
#error "This virtual machine requires Linux/x86_64."
#endif

#include <asm/bootparam.h>
#include <asm/e820.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "err.h"
#include "vm.h"

#define SERIAL_IRQ 4

static int vm_init_regs(vm_t *v)
{
    struct kvm_sregs sregs;
    if (ioctl(v->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
        return throw_err("Failed to get registers");

#define X(R) sregs.R.base = 0, sregs.R.limit = ~0, sregs.R.g = 1
    X(cs), X(ds), X(fs), X(gs), X(es), X(ss);
#undef X

    sregs.cs.db = 1;
    sregs.ss.db = 1;
    sregs.cr0 |= 1; /* enable protected mode */

    if (ioctl(v->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
        return throw_err("Failed to set special registers");

    struct kvm_regs regs;
    if (ioctl(v->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return throw_err("Failed to get registers");

    regs.rflags = 2;
    regs.rip = 0x100000, regs.rsi = 0x10000;
    if (ioctl(v->vcpu_fd, KVM_SET_REGS, &regs) < 0)
        return throw_err("Failed to set registers");

    return 0;
}

#define N_ENTRIES 100
static void vm_init_cpu_id(vm_t *v)
{
    struct {
        uint32_t nent;
        uint32_t padding;
        struct kvm_cpuid_entry2 entries[N_ENTRIES];
    } kvm_cpuid = {.nent = N_ENTRIES};
    ioctl(v->kvm_fd, KVM_GET_SUPPORTED_CPUID, &kvm_cpuid);

    for (unsigned int i = 0; i < N_ENTRIES; i++) {
        struct kvm_cpuid_entry2 *entry = &kvm_cpuid.entries[i];
        if (entry->function == KVM_CPUID_SIGNATURE) {
            entry->eax = KVM_CPUID_FEATURES;
            entry->ebx = 0x4b4d564b; /* KVMK */
            entry->ecx = 0x564b4d56; /* VMKV */
            entry->edx = 0x4d;       /* M */
        }
    }
    ioctl(v->vcpu_fd, KVM_SET_CPUID2, &kvm_cpuid);
}

int vm_arch_init(vm_t *v)
{
    if (ioctl(v->vm_fd, KVM_SET_TSS_ADDR, 0xffffd000) < 0)
        return throw_err("Failed to set TSS addr");

    __u64 map_addr = 0xffffc000;
    if (ioctl(v->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr) < 0)
        return throw_err("Failed to set identity map address");

    if (ioctl(v->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0)
        return throw_err("Failed to create IRQ chip");

    struct kvm_pit_config pit = {.flags = 0};
    if (ioctl(v->vm_fd, KVM_CREATE_PIT2, &pit) < 0)
        return throw_err("Failed to create i8254 interval timer");

    return 0;
}

int vm_arch_init_cpu(vm_t *v)
{
    if (vm_init_regs(v) != 0)
        return -1;

    vm_init_cpu_id(v);

    return 0;
}

int vm_arch_init_platform_devices(vm_t *v)
{
    bus_init(&v->io_bus);
    bus_init(&v->mmio_bus);
    pci_init(&v->pci, &v->io_bus, NULL);
    if (serial_init(&v->serial, &v->io_bus, SERIAL_IRQ))
        return throw_err("Failed to init UART device");

    return 0;
}

int vm_arch_post_init(vm_t *v)
{
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
    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    struct boot_params *boot =
        (struct boot_params *) ((uint8_t *) v->mem + 0x10000);
    void *cmdline = ((uint8_t *) v->mem) + 0x20000;
    void *kernel = ((uint8_t *) v->mem) + 0x100000;

    memset(boot, 0, sizeof(struct boot_params));
    memmove(boot, data, sizeof(struct boot_params));

    size_t setup_sectors = boot->hdr.setup_sects;
    size_t setupsz = (setup_sectors + 1) * 512;
    boot->hdr.vid_mode = 0xFFFF;  // VGA
    boot->hdr.type_of_loader = 0xFF;
    boot->hdr.loadflags |= CAN_USE_HEAP | 0x01 | KEEP_SEGMENTS;
    boot->hdr.heap_end_ptr = 0xFE00;
    boot->hdr.ext_loader_ver = 0x0;
    boot->hdr.cmd_line_ptr = 0x20000;
    memset(cmdline, 0, boot->hdr.cmdline_size);
    memcpy(cmdline, KERNEL_OPTS, sizeof(KERNEL_OPTS));
    memmove(kernel, (char *) data + setupsz, datasz - setupsz);

    /* setup E820 memory map to report usable address ranges for initrd */
    unsigned int idx = 0;
    boot->e820_table[idx++] = (struct boot_e820_entry){
        .addr = 0x0,
        .size = ISA_START_ADDRESS - 1,
        .type = E820_RAM,
    };
    boot->e820_table[idx++] = (struct boot_e820_entry){
        .addr = ISA_END_ADDRESS,
        .size = RAM_SIZE - ISA_END_ADDRESS,
        .type = E820_RAM,
    };
    boot->e820_entries = idx;
    munmap(data, datasz);
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
    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    struct boot_params *boot =
        (struct boot_params *) ((uint8_t *) v->mem + 0x10000);
    unsigned long addr = boot->hdr.initrd_addr_max & ~0xfffff;

    for (;;) {
        if (addr < 0x100000)
            return throw_err("Not enough memory for initrd");
        if (addr < (RAM_SIZE - datasz))
            break;
        addr -= 0x100000;
    }

    void *initrd = ((uint8_t *) v->mem) + addr;

    memset(initrd, 0, datasz);
    memmove(initrd, data, datasz);

    boot->hdr.ramdisk_image = addr;
    boot->hdr.ramdisk_size = datasz;
    munmap(data, datasz);
    return 0;
}

int vm_irq_line(vm_t *v, int irq, int level)
{
    struct kvm_irq_level irq_level = {
        {.irq = irq},
        .level = level,
    };

    if (ioctl(v->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
        return throw_err("Failed to set the status of an IRQ line");

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
    bus_handle_io(bus, run->mmio.data, run->mmio.is_write, run->mmio.phys_addr,
                  run->mmio.len);
}
