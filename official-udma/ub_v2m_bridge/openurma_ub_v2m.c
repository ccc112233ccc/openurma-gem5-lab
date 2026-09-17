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

static struct irq_chip ub_v2m_irq_chip = {
	.name = "openurma-ub-v2m",
};

static struct msi_domain_ops ub_v2m_domain_ops;

static struct msi_domain_info ub_v2m_domain_info = {
	.flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS,
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
