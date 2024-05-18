// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2020, Loongson Corporation
 */

#include <linux/clk-provider.h>
#include <linux/pci.h>
#include <linux/dmi.h>
#include <linux/device.h>
#include <linux/of_irq.h>
#include "stmmac.h"

struct stmmac_pci_info {
	int (*setup)(struct pci_dev *pdev, struct plat_stmmacenet_data *plat);
};

static void common_default_data(struct pci_dev *pdev,
				struct plat_stmmacenet_data *plat)
{
	plat->bus_id = (pci_domain_nr(pdev->bus) << 16) | PCI_DEVID(pdev->bus->number, pdev->devfn);
	plat->mac_interface = PHY_INTERFACE_MODE_GMII;

	plat->clk_csr = 2;	/* clk_csr_i = 20-35MHz & MDC = clk_csr_i/16 */
	plat->has_gmac = 1;
	plat->force_sf_dma_mode = 1;

	/* Set default value for multicast hash bins */
	plat->multicast_filter_bins = 256;

	/* Set default value for unicast filter entries */
	plat->unicast_filter_entries = 1;

	/* Set the maxmtu to a default of JUMBO_LEN */
	plat->maxmtu = JUMBO_LEN;

	/* Set default number of RX and TX queues to use */
	plat->tx_queues_to_use = 1;
	plat->rx_queues_to_use = 1;

	/* Disable Priority config by default */
	plat->tx_queues_cfg[0].use_prio = false;
	plat->rx_queues_cfg[0].use_prio = false;

	/* Disable RX queues routing by default */
	plat->rx_queues_cfg[0].pkt_route = 0x0;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;

	plat->clk_ref_rate = 125000000;
	plat->clk_ptp_rate = 125000000;
}

static int loongson_gmac_data(struct pci_dev *pdev,
			      struct plat_stmmacenet_data *plat)
{
	common_default_data(pdev, plat);

	plat->mdio_bus_data->phy_mask = 0;

	plat->phy_addr = -1;
	plat->phy_interface = PHY_INTERFACE_MODE_RGMII_ID;

	stmmac_flow_ctrl = FLOW_OFF;

	return 0;
}

static struct stmmac_pci_info loongson_gmac_pci_info = {
	.setup = loongson_gmac_data,
};

static void loongson_gnet_fix_speed(void *priv, unsigned int speed, unsigned int mode)
{
	struct net_device *ndev = (struct net_device *)(*(unsigned long *)priv);
	struct stmmac_priv *ptr = netdev_priv(ndev);

	if (speed == SPEED_1000) {
		if (readl(ptr->ioaddr + MAC_CTRL_REG) & (1 << 15) /* PS */) {
			/* reset phy */
			phy_set_bits(ndev->phydev, 0 /*MII_BMCR*/,
				     0x200 /*BMCR_ANRESTART*/);
		}
	}
}

static int loongson_gnet_data(struct pci_dev *pdev,
			      struct plat_stmmacenet_data *plat)
{
	common_default_data(pdev, plat);

	plat->mdio_bus_data->phy_mask = 0xfffffffb;

	plat->phy_addr = 2;
	plat->phy_interface = PHY_INTERFACE_MODE_GMII;

	/* GNET 1000M speed need workaround */
	plat->fix_mac_speed = loongson_gnet_fix_speed;

	/* Get netdev pointer address */
	plat->bsp_priv = &(pdev->dev.driver_data);

	return 0;
}

static struct stmmac_pci_info loongson_gnet_pci_info = {
	.setup = loongson_gnet_data,
};

static int loongson_dwmac_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct plat_stmmacenet_data *plat;
	struct stmmac_pci_info *info;
	struct stmmac_resources res;
	struct device_node *np;
	int ret, i, bus_id, phy_mode;

	np = dev_of_node(&pdev->dev);
	plat = devm_kzalloc(&pdev->dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return -ENOMEM;

	plat->mdio_bus_data = devm_kzalloc(&pdev->dev,
					   sizeof(*plat->mdio_bus_data),
					   GFP_KERNEL);
	if (!plat->mdio_bus_data)
		return -ENOMEM;

	plat->mdio_node = of_get_child_by_name(np, "mdio");
	if (plat->mdio_node) {
		dev_info(&pdev->dev, "Found MDIO subnode\n");
		plat->mdio_bus_data->needs_reset = true;
	}

	plat->dma_cfg = devm_kzalloc(&pdev->dev, sizeof(*plat->dma_cfg), GFP_KERNEL);
	if (!plat->dma_cfg) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	/* Enable pci device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: ERROR: failed to enable device\n", __func__);
		goto err_put_node;
	}

	/* Get the base address of device */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (pci_resource_len(pdev, i) == 0)
			continue;
		ret = pcim_iomap_regions(pdev, BIT(0), pci_name(pdev));
		if (ret)
			goto err_disable_device;
		break;
	}

	pci_set_master(pdev);

	info = (struct stmmac_pci_info *)id->driver_data;
	ret = info->setup(pdev, plat);
	if (ret)
		return ret;

	if (np) {
		bus_id = of_alias_get_id(np, "ethernet");
		if (bus_id >= 0)
			plat->bus_id = bus_id;

		phy_mode = device_get_phy_mode(&pdev->dev);
		if (phy_mode < 0) {
			dev_err(&pdev->dev, "phy_mode not found\n");
			ret = phy_mode;
			goto err_disable_device;
		}
		plat->phy_interface = phy_mode;
	}

	pci_enable_msi(pdev);

	memset(&res, 0, sizeof(res));
	res.addr = pcim_iomap_table(pdev)[0];
	if (np) {
		res.irq = of_irq_get_byname(np, "macirq");
		if (res.irq < 0) {
			dev_err(&pdev->dev, "IRQ macirq not found\n");
			ret = -ENODEV;
			goto err_disable_msi;
		}

		res.wol_irq = of_irq_get_byname(np, "eth_wake_irq");
		if (res.wol_irq < 0) {
			dev_info(&pdev->dev,
				 "IRQ eth_wake_irq not found, using macirq\n");
			res.wol_irq = res.irq;
		}

		res.lpi_irq = of_irq_get_byname(np, "eth_lpi");
		if (res.lpi_irq < 0) {
			dev_err(&pdev->dev, "IRQ eth_lpi not found\n");
			ret = -ENODEV;
			goto err_disable_msi;
		}
	} else {
		res.irq = pdev->irq;
		res.wol_irq = pdev->irq;
	}

	ret = stmmac_dvr_probe(&pdev->dev, plat, &res);
	if (ret)
		goto err_disable_msi;

	return ret;

err_disable_msi:
	pci_disable_msi(pdev);
err_disable_device:
	pci_disable_device(pdev);
err_put_node:
	of_node_put(plat->mdio_node);
	return ret;
}

static void loongson_dwmac_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = dev_get_drvdata(&pdev->dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	int i;

	of_node_put(priv->plat->mdio_node);
	stmmac_dvr_remove(&pdev->dev);

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (pci_resource_len(pdev, i) == 0)
			continue;
		pcim_iounmap_regions(pdev, BIT(i));
		break;
	}

	pci_disable_msi(pdev);
	pci_disable_device(pdev);
}

static int __maybe_unused loongson_dwmac_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	ret = stmmac_suspend(dev);
	if (ret)
		return ret;

	ret = pci_save_state(pdev);
	if (ret)
		return ret;

	pci_disable_device(pdev);
	pci_wake_from_d3(pdev, true);
	return 0;
}

static int __maybe_unused loongson_dwmac_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	pci_restore_state(pdev);
	pci_set_power_state(pdev, PCI_D0);

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	return stmmac_resume(dev);
}

static SIMPLE_DEV_PM_OPS(loongson_dwmac_pm_ops, loongson_dwmac_suspend,
			 loongson_dwmac_resume);

#define PCI_DEVICE_ID_LOONGSON_GMAC	0x7a03
#define PCI_DEVICE_ID_LOONGSON_GNET	0x7a13

static const struct pci_device_id loongson_dwmac_id_table[] = {
	{ PCI_DEVICE_DATA(LOONGSON, GMAC, &loongson_gmac_pci_info) },
	{ PCI_DEVICE_DATA(LOONGSON, GNET, &loongson_gnet_pci_info) },
	{}
};
MODULE_DEVICE_TABLE(pci, loongson_dwmac_id_table);

static struct pci_driver loongson_dwmac_driver = {
	.name = "dwmac-loongson-pci",
	.id_table = loongson_dwmac_id_table,
	.probe = loongson_dwmac_probe,
	.remove = loongson_dwmac_remove,
	.driver = {
		.pm = &loongson_dwmac_pm_ops,
	},
};

module_pci_driver(loongson_dwmac_driver);

MODULE_DESCRIPTION("Loongson DWMAC PCI driver");
MODULE_AUTHOR("Qing Zhang <zhangqing@loongson.cn>");
MODULE_LICENSE("GPL v2");
