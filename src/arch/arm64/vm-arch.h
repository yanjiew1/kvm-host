#pragma once

#include <asm/kvm.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"

typedef struct vm vm_t;

struct vm_arch {
    int gic_fd;
    int gic_type;
    uint64_t entry;
    size_t initrdsz;
    struct dev iodev;
};

#define RAM_BASE (1UL << 31)
#define SZ_64K (1UL << 16)

/*
 * The maximum size of the device tree is 2MB.
 * Reference: https://docs.kernel.org/arm64/booting.html
 */
#define FDT_MAX_SIZE (1UL << 21)

/* GIC Type */
#define ARM_GIC_V2 0
#define ARM_GIC_V3 1
#define ARM_GIC_SPI_BASE 32

/* FDT Related Definitions */
#define ARM_FDT_IRQ_TYPE_SPI 0
#define ARM_FDT_IRQ_EDGE_TRIGGER 1
#define ARM_FDT_IRQ_LEVEL_TRIGGER 4

/*
 *  Memory map for guest memory
 *
 *    0 -  64K  I/O Ports
 *   1M -  16M  GIC
 *  1GB -  2GB  PCI MMIO
 *  2GB -       DRAM
 */

#define ARM_IOPORT_BASE 0
#define ARM_IOPORT_SIZE (1UL << 16)

#define ARM_GIC_BASE 0x100000UL

#define ARM_GIC_CPUI_BASE ARM_GIC_BASE
#define ARM_GIC_CPUI_SIZE 0x20000

#define ARM_GIC_DIST_BASE (ARM_GIC_BASE + ARM_GIC_CPUI_SIZE)
#define ARM_GIC_DIST_SIZE KVM_VGIC_V3_DIST_SIZE

#define ARM_GIC_REDIST_BASE (ARM_GIC_DIST_BASE + ARM_GIC_DIST_SIZE)
#define ARM_GIC_REDIST_SIZE KVM_VGIC_V3_REDIST_SIZE

#define ARM_GIC_ITS_BASE (ARM_GIC_REDIST_BASE + ARM_GIC_REDIST_SIZE)
#define ARM_GIC_ITS_SIZE KVM_VGIC_V3_ITS_SIZE

#define ARM_PCI_CFG_BASE 0x40000000UL
#define ARM_PCI_CFG_SIZE PCI_CONFIG_MMIO_SIZE

#define ARM_PCI_MMIO_BASE (ARM_PCI_CFG_BASE + ARM_PCI_CFG_SIZE)
#define ARM_PCI_MMIO_SIZE (RAM_BASE - ARM_PCI_MMIO_BASE)

/* 128 MB for iernel */
#define ARM_KERNEL_BASE RAM_BASE
#define ARM_KERNEL_SIZE 0x8000000UL

/* 128 MB for initrd */
#define ARM_INITRD_BASE (ARM_KERNEL_BASE + ARM_KERNEL_SIZE)
#define ARM_INITRD_SIZE 0x8000000UL

/* For FTB */
#define ARM_FDT_BASE (ARM_INITRD_BASE + ARM_INITRD_SIZE)
#define ARM_FDT_SIZE FDT_MAX_SIZE

int vm_arch_generate_fdt(vm_t *v);
int vm_arch_get_mpidr(vm_t *v, uint64_t *mpidr);
