#include "pci.h"
#include "vm-arch.h"

void pci_arch_init(struct pci *pci,
                   dev_io_fn addr_io,
                   dev_io_fn data_io,
                   dev_io_fn mmio_io)
{
    dev_init(&pci->pci_mmio_dev, ARM_PCI_CFG_BASE, ARM_PCI_CFG_SIZE, pci,
             mmio_io);
}
