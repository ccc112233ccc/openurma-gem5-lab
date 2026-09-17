// SPDX-License-Identifier: GPL-2.0
/*
 * Simulation-only bridge between gem5's GICv2m frame and the OLK UBUS USI
 * domain.  The stock GICv2m driver publishes PCI and platform MSI domains;
 * UBUS deliberately looks up a distinct DOMAIN_BUS_UB_MSI domain.  Real UB
 * platforms provide that domain through their interrupt controller (normally
 * a GICv3 ITS).  This module adds the missing domain type without modifying
 * the official UBUS, UBASE or UDMA drivers.
 */

#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of.h>

#include <ub/ubfi/ubfi.h>
#include <ub/ubus/ubus.h>

static struct irq_domain *ub_v2m_domain;

static void ub_v2m_update_device_mask(struct irq_data *data, bool mask)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	u32 bit = (u32)BIT(data->irq - desc->irq);

	if (desc->ub_intr.intr_attrib.is_type1) {
		struct ub_entity *uent = to_ub_entity(desc->dev);
		unsigned long flags;

		raw_spin_lock_irqsave(&uent->usi_lock, flags);
		if (mask)
			desc->ub_intr.intr_attrib.mask |= bit;
		else
			desc->ub_intr.intr_attrib.mask &= ~bit;
		ub_cfg_write_dword(uent, UB_INT_TYPE1_INT_MASK,
				   desc->ub_intr.intr_attrib.mask);
		raw_spin_unlock_irqrestore(&uent->usi_lock, flags);
	} else {
		void __iomem *addr;
		u32 value;

		addr = desc->ub_intr.vector_base +
		       desc->ub_intr.intr_attrib.entry_nr *
		       UB_INTR_VECTOR_ENTRY_SIZE;
		value = readl(addr + UB_INTR_VECTOR_ADDR_INDEX);
		if (mask)
			value |= UB_INTR_VECTOR_MASK_MASK;
		else
			value &= ~UB_INTR_VECTOR_MASK_MASK;
		desc->ub_intr.intr_attrib.mask = mask;
		writel(value, addr + UB_INTR_VECTOR_ADDR_INDEX);
	}
}

/*
 * UBUS owns the device-side Type-1/Type-2 mask state, while GICv2m owns the
 * parent SPI.  Both levels must be updated.  This mirrors the official ITS
 * UBUS irqchip; using the default UBUS callbacks alone leaves the parent SPI
 * masked and the guest never observes the otherwise valid MSI write.
 */
static void ub_v2m_mask_msi_irq(struct irq_data *data)
{
	ub_v2m_update_device_mask(data, true);
	irq_chip_mask_parent(data);
}

static void ub_v2m_unmask_msi_irq(struct irq_data *data)
{
	ub_v2m_update_device_mask(data, false);
	irq_chip_unmask_parent(data);
}

static struct irq_chip ub_v2m_irq_chip = {
	.name = "openurma-ub-v2m",
	.irq_mask = ub_v2m_mask_msi_irq,
	.irq_unmask = ub_v2m_unmask_msi_irq,
	.irq_eoi = irq_chip_eoi_parent,
};

static struct msi_domain_ops ub_v2m_domain_ops;

static struct msi_domain_info ub_v2m_domain_info = {
	.flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		 MSI_FLAG_UB_INTR,
	.ops = &ub_v2m_domain_ops,
	.chip = &ub_v2m_irq_chip,
};

static int __init openurma_ub_v2m_init(void)
{
	struct irq_domain *parent;
	struct device_node *node;
	struct fwnode_handle *fwnode;

	node = of_find_compatible_node(NULL, NULL, "arm,gic-v2m-frame");
	if (!node)
		return -ENODEV;

	fwnode = of_node_to_fwnode(node);
	parent = irq_find_matching_fwnode(fwnode, DOMAIN_BUS_NEXUS);
	if (!parent) {
		of_node_put(node);
		pr_err("openurma_ub_v2m: GICv2m parent domain not found\n");
		return -EPROBE_DEFER;
	}

	ub_v2m_domain = ub_msi_create_irq_domain(fwnode,
						  &ub_v2m_domain_info, parent);
	if (!ub_v2m_domain) {
		of_node_put(node);
		return -ENOMEM;
	}

	/*
	 * UBRT creates the UBC before this module can load (the domain factory is
	 * exported by ubus.ko).  Its one-shot DT lookup has therefore already run.
	 * Attach the newly published domain to each existing controller; entities
	 * inherit it when hisi_ubus subsequently enumerates them.
	 */
	{
		struct ub_bus_controller *ubc;

		list_for_each_entry(ubc, &ubc_list, node)
			dev_set_msi_domain(&ubc->dev, ub_v2m_domain);
	}

	/* The IRQ domain retains the firmware-node reference until removal. */
	pr_info("openurma_ub_v2m: registered UBUS USI domain over GICv2m\n");
	return 0;
}

static void __exit openurma_ub_v2m_exit(void)
{
	struct device_node *node;

	if (!ub_v2m_domain)
		return;
	node = to_of_node(ub_v2m_domain->fwnode);
	irq_domain_remove(ub_v2m_domain);
	ub_v2m_domain = NULL;
	of_node_put(node);
}

module_init(openurma_ub_v2m_init);
module_exit(openurma_ub_v2m_exit);

MODULE_DESCRIPTION("OpenURMA simulation UBUS domain bridge for GICv2m");
MODULE_LICENSE("GPL");
