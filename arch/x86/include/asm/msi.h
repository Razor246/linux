/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSI_H
#define _ASM_X86_MSI_H
#include <asm/hw_irq.h>
#include <asm/irqdomain.h>

typedef struct irq_alloc_info msi_alloc_info_t;

int pci_msi_prepare(struct irq_domain *domain, struct device *dev, int nvec,
		    msi_alloc_info_t *arg);

void pci_msi_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc);
void irq_msi_compose_msg(struct irq_data *data, struct msi_msg *msg);

#endif /* _ASM_X86_MSI_H */
