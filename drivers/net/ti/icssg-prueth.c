// SPDX-License-Identifier: GPL-2.0+
/*
 * Texas Instruments K3 AM65 PRU Ethernet Driver
 *
 * Copyright (C) 2019, Texas Instruments, Incorporated
 *
 */

#include <common.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <clk.h>
#include <dm.h>
#include <dm/lists.h>
#include <dm/device.h>
#include <dma-uclass.h>
#include <dm/of_access.h>
#include <fs_loader.h>
#include <miiphy.h>
#include <misc.h>
#include <net.h>
#include <phy.h>
#include <power-domain.h>
#include <linux/soc/ti/ti-udma.h>
#include <regmap.h>
#include <remoteproc.h>
#include <syscon.h>
#include <ti-pruss.h>

#include "cpsw_mdio.h"
#include "icssg.h"

#define ICSS_SLICE0     0
#define ICSS_SLICE1     1

#define MSMC_RAM_SIZE	0x10000

#ifdef PKTSIZE_ALIGN
#define UDMA_RX_BUF_SIZE PKTSIZE_ALIGN
#else
#define UDMA_RX_BUF_SIZE ALIGN(1522, ARCH_DMA_MINALIGN)
#endif

#ifdef PKTBUFSRX
#define UDMA_RX_DESC_NUM PKTBUFSRX
#else
#define UDMA_RX_DESC_NUM 4
#endif

enum prueth_mac {
	PRUETH_MAC0 = 0,
	PRUETH_MAC1,
	PRUETH_NUM_MACS,
};

enum prueth_port {
	PRUETH_PORT_HOST = 0,	/* host side port */
	PRUETH_PORT_MII0,	/* physical port MII 0 */
	PRUETH_PORT_MII1,	/* physical port MII 1 */
};

/* Below used to support 2 icssgs per pru port */
#define ICSSG0          0
#define ICSSG1          1
#define NUM_ICSSG       2

/* Config region lies in shared RAM */
#define ICSS_CONFIG_OFFSET_SLICE0	0
#define ICSS_CONFIG_OFFSET_SLICE1	0x8000

/* Firmware flags */
#define ICSS_SET_RUN_FLAG_VLAN_ENABLE		BIT(0)	/* switch only */
#define ICSS_SET_RUN_FLAG_FLOOD_UNICAST		BIT(1)	/* switch only */
#define ICSS_SET_RUN_FLAG_PROMISC		BIT(2)	/* MAC only */
#define ICSS_SET_RUN_FLAG_MULTICAST_PROMISC	BIT(3)	/* MAC only */

/* CTRLMMR_ICSSG_RGMII_CTRL register bits */
#define ICSSG_CTRL_RGMII_ID_MODE		BIT(24)

/**
 * enum pruss_pru_id - PRU core identifiers
 */
enum pruss_pru_id {
	PRUSS_PRU0 = 0,
	PRUSS_PRU1,
	PRUSS_NUM_PRUS,
};

struct prueth {
	struct udevice		*dev;
	struct regmap		*miig_rt[NUM_ICSSG];
	fdt_addr_t		mdio_base;
	phys_addr_t		pruss_shrdram2[NUM_ICSSG];
	phys_addr_t		tmaddr[NUM_ICSSG];
	struct mii_dev		*bus;
	u32			port_id;
	u32			sram_pa[NUM_ICSSG];
	struct phy_device	*phydev;
	bool			has_phy;
	ofnode			phy_node;
	u32			phy_addr;
	ofnode			eth_node[PRUETH_NUM_MACS];
	struct icssg_config	config[NUM_ICSSG][PRUSS_NUM_PRUS];
	u32			mdio_freq;
	int			phy_interface;
	struct			clk mdiofck;
	struct dma		dma_tx;
	struct dma		dma_rx;
	u32			rx_next;
	u32			rx_pend;
	int			slice[NUM_ICSSG];
	int			ingress_icssg;
	int			ingress_slice;
	int			egress_icssg;
	int			egress_slice;
	bool			dual_icssg;
};

static int icssg_phy_init(struct udevice *dev)
{
	struct prueth *priv = dev_get_priv(dev);
	struct phy_device *phydev;
	u32 supported = PHY_GBIT_FEATURES;
	int ret;

	phydev = phy_connect(priv->bus,
			     priv->phy_addr,
			     priv->dev,
			     priv->phy_interface);

	if (!phydev) {
		dev_err(dev, "phy_connect() failed\n");
		return -ENODEV;
	}

	phydev->supported &= supported;
	phydev->advertising = phydev->supported;

#ifdef CONFIG_DM_ETH
	if (ofnode_valid(priv->phy_node))
		phydev->node = priv->phy_node;
#endif

	priv->phydev = phydev;
	ret = phy_config(phydev);
	if (ret < 0)
		pr_err("phy_config() failed: %d", ret);

	return ret;
}

static int icssg_mdio_init(struct udevice *dev, int icssg)
{
	struct prueth *prueth = dev_get_priv(dev);

	prueth->bus = cpsw_mdio_init(dev->name, prueth->mdio_base,
				     prueth->mdio_freq,
				     clk_get_rate(&prueth->mdiofck));
	if (!prueth->bus)
		return -EFAULT;

	return 0;
}

static void icssg_config_set(struct prueth *prueth, int icssg, int slice)
{
	struct icssg_config *config;
	void __iomem *va;
	int i;

	config = &prueth->config[icssg][0];
	memset(config, 0, sizeof(*config));
	config->addr_lo = cpu_to_le32(lower_32_bits(prueth->sram_pa[icssg]));
	config->addr_hi = cpu_to_le32(upper_32_bits(prueth->sram_pa[icssg]));
	config->num_tx_threads = 0;
	config->rx_flow_id = 0; /* flow id for host port */

	for (i = 8; i < 16; i++)
		config->tx_buf_sz[i] = cpu_to_le32(0x1800);


	va = (void __iomem *)prueth->pruss_shrdram2[icssg] +
		slice * ICSSG_CONFIG_OFFSET_SLICE1;

	memcpy_toio(va, &prueth->config[icssg][0],
		    sizeof(prueth->config[icssg][0]));
}

static int prueth_start(struct udevice *dev)
{
	struct prueth *priv = dev_get_priv(dev);
	struct eth_pdata *pdata = dev->platdata;
	int ret, i;
	char tx_chn_name[16];
	char rx_chn_name[16];

	icssg_class_set_mac_addr(priv->miig_rt[priv->ingress_icssg],
				 priv->ingress_slice,
				 (u8 *)pdata->enetaddr);
	icssg_class_default(priv->miig_rt[priv->ingress_icssg],
			    priv->ingress_slice);

	/* To differentiate channels for SLICE0 vs SLICE1 for single icssg
	 * and ICSSG0 vs ICSSG1 for dual icssg
	 */
	if (!priv->dual_icssg) {
		snprintf(tx_chn_name, sizeof(tx_chn_name), "tx%d-0",
			 priv->egress_slice);
		snprintf(rx_chn_name, sizeof(rx_chn_name), "rx%d",
			 priv->ingress_slice);
	} else {
		snprintf(tx_chn_name, sizeof(tx_chn_name), "tx%d-0",
			 priv->egress_icssg);
		snprintf(rx_chn_name, sizeof(rx_chn_name), "rx%d",
			 priv->ingress_icssg);
	}
	ret = dma_get_by_name(dev, tx_chn_name, &priv->dma_tx);
	if (ret)
		dev_err(dev, "TX dma get failed %d\n", ret);

	ret = dma_get_by_name(dev, rx_chn_name, &priv->dma_rx);
	if (ret)
		dev_err(dev, "RX dma get failed %d\n", ret);

	for (i = 0; i < UDMA_RX_DESC_NUM; i++) {
		ret = dma_prepare_rcv_buf(&priv->dma_rx,
					  net_rx_packets[i],
					  UDMA_RX_BUF_SIZE);
		if (ret)
			dev_err(dev, "RX dma add buf failed %d\n", ret);
	}

	ret = dma_enable(&priv->dma_tx);
	if (ret) {
		dev_err(dev, "TX dma_enable failed %d\n", ret);
		return ret;
	}

	ret = dma_enable(&priv->dma_rx);
	if (ret) {
		dev_err(dev, "RX dma_enable failed %d\n", ret);
		goto rx_fail;
	}

	ret = phy_startup(priv->phydev);
	if (ret) {
		dev_err(dev, "phy_startup failed\n");
		goto phy_fail;
	}

	return 0;
phy_fail:
	dma_disable(&priv->dma_rx);
rx_fail:
	dma_disable(&priv->dma_tx);

	return ret;
}

void prueth_print_buf(ulong addr, const void *data, uint width,
		      uint count, uint linelen)
{
	print_buffer(addr, data, width, count, linelen);
}

static int prueth_send(struct udevice *dev, void *packet, int length)
{
	struct prueth *priv = dev_get_priv(dev);
	int ret;

	ret = dma_send(&priv->dma_tx, packet, length, NULL);

	return ret;
}

static int prueth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct prueth *priv = dev_get_priv(dev);
	int ret;

	/* try to receive a new packet */
	ret = dma_receive(&priv->dma_rx, (void **)packetp, NULL);

	return ret;
}

static int prueth_free_pkt(struct udevice *dev, uchar *packet, int length)
{
	struct prueth *priv = dev_get_priv(dev);
	int ret = 0;

	if (length > 0) {
		u32 pkt = priv->rx_next % UDMA_RX_DESC_NUM;

		dev_dbg(dev, "%s length:%d pkt:%u\n", __func__, length, pkt);

		ret = dma_prepare_rcv_buf(&priv->dma_rx,
					  net_rx_packets[pkt],
					  UDMA_RX_BUF_SIZE);
		priv->rx_next++;
	}

	return ret;
}

static void prueth_stop(struct udevice *dev)
{
	struct prueth *priv = dev_get_priv(dev);
	int icssg = priv->ingress_icssg, slice = priv->ingress_slice;

	icssg_class_disable(priv->miig_rt[icssg], slice);

	phy_shutdown(priv->phydev);

	dma_disable(&priv->dma_tx);
	dma_free(&priv->dma_tx);

	dma_disable(&priv->dma_rx);
	dma_free(&priv->dma_rx);

	/* Workaround for shutdown command */
	writel(0x0, priv->tmaddr[icssg] + slice * 0x200);
	if (!priv->dual_icssg)
		return;

	icssg = priv->egress_icssg;
	slice = priv->egress_slice;
	writel(0x0, priv->tmaddr[icssg] + slice * 0x200);
}

static const struct eth_ops prueth_ops = {
	.start		= prueth_start,
	.send		= prueth_send,
	.recv		= prueth_recv,
	.free_pkt	= prueth_free_pkt,
	.stop		= prueth_stop,
};

static int icssg_ofdata_parse_phy(struct udevice *dev, ofnode port_np)
{
	struct prueth *priv = dev_get_priv(dev);
	struct ofnode_phandle_args out_args;
	const char *phy_mode;
	int ret = 0;

	phy_mode = ofnode_read_string(port_np, "phy-mode");
	if (phy_mode) {
		priv->phy_interface =
				phy_get_interface_by_name(phy_mode);
		if (priv->phy_interface == -1) {
			dev_err(dev, "Invalid PHY mode '%s'\n",
				phy_mode);
			ret = -EINVAL;
			goto out;
		}
	}

	ret = ofnode_parse_phandle_with_args(port_np, "phy-handle",
					     NULL, 0, 0, &out_args);
	if (ret) {
		dev_err(dev, "can't parse phy-handle port (%d)\n", ret);
		ret = 0;
	}

	priv->phy_node = out_args.node;
	ret = ofnode_read_u32(priv->phy_node, "reg", &priv->phy_addr);
	if (ret)
		dev_err(dev, "failed to get phy_addr port (%d)\n", ret);

out:
	return ret;
}

static int prueth_config_rgmiidelay(struct prueth *prueth,
				    ofnode eth_np)
{
	struct regmap *ctrl_mmr;
	u32 val;
	int ret = 0;
	ofnode node;
	u32 tmp[2];

	ret = ofnode_read_u32_array(eth_np, "syscon-rgmii-delay", tmp, 2);
	if (ret) {
		dev_err(dev, "no syscon-rgmii-delay\n");
		return ret;
	}

	node = ofnode_get_by_phandle(tmp[0]);
	if (!ofnode_valid(node)) {
		dev_err(dev, "can't get syscon-rgmii-delay node\n");
		return -EINVAL;
	}

	ctrl_mmr = syscon_node_to_regmap(node);
	if (!ctrl_mmr) {
		dev_err(dev, "can't get ctrl_mmr regmap\n");
		return -EINVAL;
	}

	if (ofnode_read_bool(eth_np, "enable-rgmii-delay"))
		val = 0;
	else
		val = ICSSG_CTRL_RGMII_ID_MODE;

	regmap_update_bits(ctrl_mmr, tmp[1], ICSSG_CTRL_RGMII_ID_MODE, val);

	return 0;
}

static int get_pruss_info(struct prueth *prueth,
			  ofnode node, ofnode *pruss_node, int icssg)
{
	struct udevice **prussdev = NULL;
	int err;

	*pruss_node = ofnode_get_parent(node);
	err = misc_init_by_ofnode(*pruss_node);
	if (err)
		return err;

	err = device_find_global_by_ofnode(*pruss_node, prussdev);
	if (err)
		dev_err(dev, "error getting the pruss dev\n");

	err = pruss_request_shrmem_region(*prussdev,
					  &prueth->pruss_shrdram2[icssg]);
	if (err)
		return err;

	if (icssg)
		prueth->miig_rt[icssg] =
			syscon_regmap_lookup_by_phandle(prueth->dev,
							"mii-g-rt-paired");
	else
		prueth->miig_rt[icssg] =
			syscon_regmap_lookup_by_phandle(prueth->dev,
							"mii-g-rt");
	if (!prueth->miig_rt[icssg]) {
		dev_err(dev, "No mii-g-rt syscon regmap for icssg %d\n", icssg);
		return -ENODEV;
	}

	return pruss_request_tm_region(*prussdev, &prueth->tmaddr[icssg]);
}

static int prueth_probe(struct udevice *dev)
{
	ofnode eth0_node, eth1_node, node, pruss_node, mdio_node, sram_node,
	dev_node;
	struct prueth *prueth;
	u32 err, sp, tmp[8];
	int ret = 0;

	prueth = dev_get_priv(dev);
	prueth->dev = dev;
	dev_node = dev_ofnode(dev);

	if (ofnode_device_is_compatible(dev_node, "ti,am654-dualicssg-prueth"))
		prueth->dual_icssg = true;

	if (prueth->dual_icssg)
		err = ofnode_read_u32_array(dev_node, "prus", tmp, 8);
	else
		err = ofnode_read_u32_array(dev_node, "prus", tmp, 4);
	if (err)
		return err;

	node = ofnode_get_by_phandle(tmp[0]);
	if (!ofnode_valid(node))
		return -EINVAL;

	ret = get_pruss_info(prueth, node, &pruss_node, ICSSG0);
	if (ret)
		return ret;

	if (prueth->dual_icssg) {
		ofnode pruss_node_pair;

		node = ofnode_get_by_phandle(tmp[4]);
		ret = get_pruss_info(prueth, node, &pruss_node_pair, ICSSG1);
		if (ret)
			return ret;
	}

	node = dev_node;
	eth0_node = ofnode_find_subnode(node, "ethernet-mii0");
	eth1_node = ofnode_find_subnode(node, "ethernet-mii1");
	/* one node must be present and available else we fail */
	if (!ofnode_valid(eth0_node) && !ofnode_valid(eth1_node)) {
		dev_err(dev, "neither ethernet-mii0 nor ethernet-mii1 node available\n");
		return -ENODEV;
	}

	/*
	 * Exactly one node must be present as uboot ethernet framework does
	 * not support two interfaces in a single probe. So Device Tree should
	 * have exactly one of mii0 or mii1 interface.
	 */
	if (ofnode_valid(eth0_node) && ofnode_valid(eth1_node)) {
		dev_err(dev, "Both slices cannot be supported\n");
		return -EINVAL;
	}

	if (ofnode_valid(eth0_node)) {
		if (!prueth->dual_icssg) {
			prueth->slice[ICSSG0] = 0;
			prueth->egress_icssg = ICSSG0;
			prueth->egress_slice = 0;
			prueth->ingress_icssg = ICSSG0;
			prueth->ingress_slice = 0;
		} else {
			prueth->slice[ICSSG0] = 0;
			prueth->slice[ICSSG1] = 1;
			prueth->egress_icssg = ICSSG1;
			prueth->egress_slice = 1;
			prueth->ingress_icssg = ICSSG0;
			prueth->ingress_slice = 0;
		}
		icssg_ofdata_parse_phy(dev, eth0_node);
		prueth->eth_node[PRUETH_MAC0] = eth0_node;
	}

	if (ofnode_valid(eth1_node)) {
		if (!prueth->dual_icssg) {
			prueth->slice[ICSSG0] = 1;
			prueth->egress_icssg = ICSSG0;
			prueth->egress_slice = 0;
			prueth->ingress_icssg = ICSSG0;
			prueth->ingress_slice = 0;
		} else {
			prueth->slice[ICSSG0] = 1;
			prueth->slice[ICSSG1] = 0;
			prueth->egress_icssg = ICSSG0;
			prueth->egress_slice = 1;
			prueth->ingress_icssg = ICSSG1;
			prueth->ingress_slice = 0;
		}
		icssg_ofdata_parse_phy(dev, eth1_node);
		prueth->eth_node[PRUETH_MAC0] = eth1_node;
	}

	ret = clk_get_by_name(dev, "mdio_fck", &prueth->mdiofck);
	if (ret) {
		dev_err(dev, "failed to get clock %d\n", ret);
		return ret;
	}
	ret = clk_enable(&prueth->mdiofck);
	if (ret) {
		dev_err(dev, "clk_enable failed %d\n", ret);
		return ret;
	}

	ret = ofnode_read_u32(node, "sram", &sp);
	if (ret) {
		dev_err(dev, "sram node fetch failed %d\n", ret);
		return ret;
	}

	sram_node = ofnode_get_by_phandle(sp);
	if (!ofnode_valid(node))
		return -EINVAL;

	prueth->sram_pa[ICSSG0] = ofnode_get_addr(sram_node);
	if (prueth->dual_icssg)
		prueth->sram_pa[ICSSG1] =
				prueth->sram_pa[ICSSG0] + MSMC_RAM_SIZE;

	if (ofnode_valid(eth0_node)) {
		ret = prueth_config_rgmiidelay(prueth, eth0_node);
		if (ret) {
			dev_err(dev, "prueth_config_rgmiidelay failed\n");
			return ret;
		}
	}

	if (ofnode_valid(eth1_node)) {
		ret = prueth_config_rgmiidelay(prueth, eth1_node);
		if (ret) {
			dev_err(dev, "prueth_config_rgmiidelay failed\n");
			return ret;
		}
	}

	mdio_node = ofnode_find_subnode(pruss_node, "mdio");
	prueth->mdio_base = ofnode_get_addr(mdio_node);
	ofnode_read_u32(mdio_node, "bus_freq", &prueth->mdio_freq);

	ret = icssg_mdio_init(dev, ICSSG0);
	if (ret)
		return ret;

	ret = icssg_phy_init(dev);
	if (ret) {
		dev_err(dev, "phy_init failed\n");
		goto out;
	}

	/* Set Load time configuration */
	icssg_config_set(prueth, ICSSG0, prueth->slice[ICSSG0]);
	if (prueth->dual_icssg)
		icssg_config_set(prueth, ICSSG1, prueth->slice[ICSSG1]);

	return 0;
out:
	cpsw_mdio_free(prueth->bus);
	clk_disable(&prueth->mdiofck);

	return ret;
}

static const struct udevice_id prueth_ids[] = {
	{ .compatible = "ti,am654-icssg-prueth" },
	{ .compatible = "ti,am654-dualicssg-prueth" },
	{ }
};

U_BOOT_DRIVER(prueth) = {
	.name	= "prueth",
	.id	= UCLASS_ETH,
	.of_match = prueth_ids,
	.probe	= prueth_probe,
	.ops	= &prueth_ops,
	.priv_auto_alloc_size = sizeof(struct prueth),
	.platdata_auto_alloc_size = sizeof(struct eth_pdata),
	.flags = DM_FLAG_ALLOC_PRIV_DMA,
};
