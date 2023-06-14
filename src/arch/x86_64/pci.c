#include "pci.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

void pci_arch_init(struct pci *pci, dev_io_fn addr_io, dev_io_fn data_io, dev_io_fn mmio_io)
{
    dev_init(&pci->pci_addr_dev, PCI_CONFIG_ADDR, sizeof(uint32_t), pci,
             addr_io);
    dev_init(&pci->pci_bus_dev, PCI_CONFIG_DATA, sizeof(uint32_t), pci,
             data_io);
}
