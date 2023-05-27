#pragma once

#include <asm/kvm.h>

#define SZ_64K (64 * 1024)

/*
 *  Memory map for guest memory
 *
 *  0   - 64K  PCI I/O port
 *  64K - 16M  GIC
 *  16M - 32M  Platform MMIO: UART, RTC, PVTIME
 *  32M - 48M  Flash
 *  48M - 1GB  Virtio MMIO
 *  1GB - 2GB  PCI
 *  2GB -      DRAM
 */

#define ARM_IOPORT_BASE 0
#define ARM_IOPORT_SIZE (64 * 1024)

#define ARM_GIC_BASE 0x10000UL

#define ARM_GIC_CPUI_BASE ARM_GIC_BASE
#define ARM_GIC_CPUI_SIZE 0x20000

#define ARM_GIC_DIST_BASE (ARM_GIC_BASE + ARM_GIC_CPUI_SIZE)
#define ARM_GIC_DIST_SIZE KVM_VGIC_V3_DIST_SIZE

#define ARM_GIC_REDIST_BASE (ARM_GIC_DIST_BASE + ARM_GIC_DIST_SIZE)
#define ARM_GIC_REDIST_SIZE	KVM_VGIC_V3_REDIST_SIZE

#define ARM_GIC_ITS_BASE (ARM_GIC_REDIST_BASE + ARM_GIC_REDIST_SIZE)
#define ARM_GIC_ITS_SIZE KVM_VGIC_V3_ITS_SIZE

#define DRAM_BASE 0x80000000UL

typedef struct {
    int gic_fd;
} vm_arch_t;
