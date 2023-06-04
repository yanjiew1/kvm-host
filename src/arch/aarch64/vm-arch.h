#pragma once

#include <asm/kvm.h>
#include <stdint.h>

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
#define ARM_GIC_REDIST_SIZE KVM_VGIC_V3_REDIST_SIZE

#define ARM_GIC_ITS_BASE (ARM_GIC_REDIST_BASE + ARM_GIC_REDIST_SIZE)
#define ARM_GIC_ITS_SIZE KVM_VGIC_V3_ITS_SIZE

#define DRAM_BASE 0x80000000UL

/* 128 MB for iernel */
#define KERNEL_BASE DRAM_BASE
#define KERNEL_SIZE 0x8000000UL

/* 128 MB for initrd */
#define INITRD_BASE (KERNEL_BASE + KERNEL_SIZE)
#define INITRD_SIZE 0x8000000UL

/* For DTB */
#define DTB_BASE (INITRD_BASE + INITRD_SIZE)

typedef struct {
    int gic_fd;
    uint64_t entry;
} vm_arch_t;

/* Interrupt */
#define ARM_GIC_SGI_BASE 0
#define ARM_GIC_PPI_BASE 16
#define ARM_GIC_SPI_BASE 32
#define ARM_GIC_IRQ_MAX 992

#define VM_IRQ_BASE ARM_GIC_SPI_BASE
#define VM_IRQ_MAX ARM_GIC_IRQ_MAX

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
