#include <stdint.h>
#include <stdio.h>

#include "err.h"
#include "libfdt.h"
#include "vm.h"

/* Helper function to simplify device tree creation */
#define _FDT(exp)                                                         \
    do {                                                                  \
        int __ret = (exp);                                                \
        if (__ret < 0)                                                    \
            return throw_err("Failed to create device tree:\n %s\n %s\n", \
                             #exp, fdt_strerror(__ret));                  \
    } while (0)

#define FDT_PHANDLE_GIC 1
#define FDT_PCI_IO_SPACE 0x01000000L
#define FDT_PCI_MMIO_SPACE 0x02000000L

int vm_arch_generate_fdt(vm_t *v)
{
    void *fdt = vm_guest_to_host(v, ARM_FDT_BASE);

    /* Create an empty FDT */
    _FDT(fdt_create(fdt, FDT_MAX_SIZE));
    _FDT(fdt_finish_reservemap(fdt));

    /* Create / node */
    _FDT(fdt_begin_node(fdt, ""));
    _FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
    _FDT(fdt_property_cell(fdt, "#size-cells", 0x2));
    _FDT(fdt_property_cell(fdt, "interrupt-parent", FDT_PHANDLE_GIC));
    _FDT(fdt_property_string(fdt, "compatible", "linux,dummy-virt"));

    /* Create /chosen node */
    _FDT(fdt_begin_node(fdt, "chosen"));
    _FDT(fdt_property_string(fdt, "bootargs", KERNEL_OPTS));
    _FDT(fdt_property_string(fdt, "stdout-path", "/uart"));
    if (v->arch.initrdsz > 0) {
        _FDT(fdt_property_u64(fdt, "linux,initrd-start", ARM_INITRD_BASE));
        _FDT(fdt_property_u64(fdt, "linux,initrd-end",
                              ARM_INITRD_BASE + v->arch.initrdsz));
    }
    _FDT(fdt_end_node(fdt));

    /* Create /memory node */
    _FDT(fdt_begin_node(fdt, "memory"));
    _FDT(fdt_property_string(fdt, "device_type", "memory"));
    uint64_t mem_reg[2] = {cpu_to_fdt64(RAM_BASE), cpu_to_fdt64(RAM_SIZE)};
    _FDT(fdt_property(fdt, "reg", mem_reg, sizeof(mem_reg)));
    _FDT(fdt_end_node(fdt));

    /* Create /cpus node */
    _FDT(fdt_begin_node(fdt, "cpus"));
    /* /cpus node headers */
    _FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
    _FDT(fdt_property_cell(fdt, "#size-cells", 0x0));
    /* The only one CPU */
    _FDT(fdt_begin_node(fdt, "cpu")); /* /cpus/cpu */
    uint64_t mpidr;
    if (vm_arch_get_mpidr(v, &mpidr) < 0)
        return -1;
    _FDT(fdt_property_cell(fdt, "reg", mpidr));
    _FDT(fdt_property_string(fdt, "device_type", "cpu"));
    _FDT(fdt_property_string(fdt, "compatible", "arm,arm-v8"));
    _FDT(fdt_property_string(fdt, "enable-method", "psci"));
    _FDT(fdt_end_node(fdt)); /* /cpus/cpu */
    _FDT(fdt_end_node(fdt)); /* /cpu */

    /* Create /timer node */
    _FDT(fdt_begin_node(fdt, "timer"));
    _FDT(fdt_property_string(fdt, "compatible", "arm,armv8-timer"));
    uint32_t timer_irq[] = {
        cpu_to_fdt32(0x01), cpu_to_fdt32(0x0d), cpu_to_fdt32(0x08),
        cpu_to_fdt32(0x01), cpu_to_fdt32(0x0e), cpu_to_fdt32(0x08),
        cpu_to_fdt32(0x01), cpu_to_fdt32(0x0b), cpu_to_fdt32(0x08),
        cpu_to_fdt32(0x01), cpu_to_fdt32(0x0a), cpu_to_fdt32(0x08)};
    _FDT(fdt_property(fdt, "interrupts", &timer_irq, sizeof(timer_irq)));
    _FDT(fdt_property(fdt, "always-on", NULL, 0));
    _FDT(fdt_end_node(fdt));

    /* Create /intr node */
    _FDT(fdt_begin_node(fdt, "intr"));
    uint64_t gic_reg[] = {
        cpu_to_fdt64(ARM_GIC_DIST_BASE), cpu_to_fdt64(ARM_GIC_DIST_SIZE),
        cpu_to_fdt64(ARM_GIC_REDIST_BASE), cpu_to_fdt64(ARM_GIC_REDIST_SIZE)};
    _FDT(fdt_property_string(fdt, "compatible", "arm,gic-v3"));
    _FDT(fdt_property_cell(fdt, "#interrupt-cells", 3));
    _FDT(fdt_property_cell(fdt, "#address-cells", 2));
    _FDT(fdt_property_cell(fdt, "#size-cells", 2));
    _FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
    _FDT(fdt_property(fdt, "reg", &gic_reg, sizeof(gic_reg)));
    _FDT(fdt_property_cell(fdt, "phandle", FDT_PHANDLE_GIC));
    _FDT(fdt_end_node(fdt));

    /* Serial device */
    /* The node name of the serial device is different from kvmtool. */
    _FDT(fdt_begin_node(fdt, "uart"));
    _FDT(fdt_property_string(fdt, "compatible", "ns16550a"));
    _FDT(fdt_property_cell(fdt, "clock-frequency", 1843200));
    uint64_t serial_reg[] = {cpu_to_fdt64(COM1_PORT_BASE),
                             cpu_to_fdt64(COM1_PORT_SIZE)};
    _FDT(fdt_property(fdt, "reg", &serial_reg, sizeof(serial_reg)));
    uint32_t serial_irq[] = {cpu_to_fdt32(ARM_FDT_IRQ_TYPE_SPI),
                             cpu_to_fdt32(v->serial.irq_num),
                             cpu_to_fdt32(ARM_FDT_IRQ_LEVEL_TRIGGER)};
    _FDT(fdt_property(fdt, "interrupts", &serial_irq, sizeof(serial_irq)));
    _FDT(fdt_end_node(fdt));

    /* /psci node */
    _FDT(fdt_begin_node(fdt, "psci"));
    const char psci_compatible[] = "arm,psci-0.2\0arm,psci";
    _FDT(fdt_property(fdt, "compatible", psci_compatible,
                      sizeof(psci_compatible)));
    _FDT(fdt_property_string(fdt, "method", "hvc"));
    _FDT(fdt_property_cell(fdt, "cpu_suspend", PSCI_0_2_FN64_CPU_SUSPEND));
    _FDT(fdt_property_cell(fdt, "cpu_off", PSCI_0_2_FN_CPU_OFF));
    _FDT(fdt_property_cell(fdt, "cpu_on", PSCI_0_2_FN64_CPU_ON));
    _FDT(fdt_property_cell(fdt, "migrate", PSCI_0_2_FN64_MIGRATE));
    _FDT(fdt_end_node(fdt));

    /* /pci node */
    _FDT(fdt_begin_node(fdt, "pci"));
    _FDT(fdt_property_string(fdt, "device_type", "pci"));
    _FDT(fdt_property_cell(fdt, "#address-cells", 3));
    _FDT(fdt_property_cell(fdt, "#size-cells", 2));
    _FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
    _FDT(fdt_property_string(fdt, "compatible", "pci-host-cam-generic"));
    _FDT(fdt_property(fdt, "dma-coherent", NULL, 0));
    uint32_t pci_bus_range[] = {cpu_to_fdt32(0), cpu_to_fdt32(0)};
    _FDT(fdt_property(fdt, "bus-range", &pci_bus_range, sizeof(pci_bus_range)));
    uint64_t pci_reg[] = {cpu_to_fdt64(ARM_PCI_CFG_BASE),
                          cpu_to_fdt64(ARM_PCI_CFG_SIZE)};
    _FDT(fdt_property(fdt, "reg", &pci_reg, sizeof(pci_reg)));
    struct {
        uint32_t pci_hi;
        uint64_t pci_addr;
        uint64_t cpu_addr;
        uint64_t size;
    } __attribute__((packed)) pci_ranges[] = {
        {cpu_to_fdt32(FDT_PCI_MMIO_SPACE), cpu_to_fdt64(ARM_PCI_MMIO_BASE),
         cpu_to_fdt64(ARM_PCI_MMIO_BASE),
         cpu_to_fdt64(ARM_PCI_MMIO_SIZE)}, /* For MMIO */
    };
    _FDT(fdt_property(fdt, "ranges", &pci_ranges, sizeof(pci_ranges)));
    /* Currently only virtio-blk device */
    struct virtio_blk_dev *virtio_blk = &v->virtio_blk_dev;
    struct pci_dev *virtio_blk_pci = (struct pci_dev *) virtio_blk;
    struct {
        uint32_t pci_hi;
        uint64_t pci_addr;
        uint32_t pci_irq;
        uint32_t intc;
        uint64_t intc_addr;
        uint32_t gic_type;
        uint32_t gic_irqn;
        uint32_t gic_irq_type;
    } __attribute__((packed)) pci_irq_map[] = {
        {cpu_to_fdt32(virtio_blk_pci->config_dev.base & ~(1UL << 31)), 0,
         cpu_to_fdt32(1), cpu_to_fdt32(FDT_PHANDLE_GIC), 0,
         cpu_to_fdt32(ARM_FDT_IRQ_TYPE_SPI),
         cpu_to_fdt32(v->virtio_blk_dev.irq_num),
         cpu_to_fdt32(ARM_FDT_IRQ_EDGE_TRIGGER)}};
    _FDT(fdt_property(fdt, "interrupt-map", &pci_irq_map, sizeof(pci_irq_map)));
    _FDT(fdt_end_node(fdt)); /* /pci */

    /* finalize */
    _FDT(fdt_end_node(fdt));
    _FDT(fdt_finish(fdt));

    return 0;
}
