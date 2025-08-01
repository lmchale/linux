// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments K3 AM65 Ethernet Switch SubSystem Driver
 *
 * Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com/
 *
 */

#include <linux/bpf_trace.h>
#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/net_tstamp.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/phylink.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>
#include <linux/mfd/syscon.h>
#include <linux/sys_soc.h>
#include <linux/dma/ti-cppi5.h>
#include <linux/dma/k3-udma-glue.h>
#include <net/page_pool/helpers.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "cpsw_ale.h"
#include "cpsw_sl.h"
#include "am65-cpsw-nuss.h"
#include "am65-cpsw-switchdev.h"
#include "k3-cppi-desc-pool.h"
#include "am65-cpts.h"

#define AM65_CPSW_SS_BASE	0x0
#define AM65_CPSW_SGMII_BASE	0x100
#define AM65_CPSW_XGMII_BASE	0x2100
#define AM65_CPSW_CPSW_NU_BASE	0x20000
#define AM65_CPSW_NU_PORTS_BASE	0x1000
#define AM65_CPSW_NU_FRAM_BASE	0x12000
#define AM65_CPSW_NU_STATS_BASE	0x1a000
#define AM65_CPSW_NU_ALE_BASE	0x1e000
#define AM65_CPSW_NU_CPTS_BASE	0x1d000

#define AM65_CPSW_NU_PORTS_OFFSET	0x1000
#define AM65_CPSW_NU_STATS_PORT_OFFSET	0x200
#define AM65_CPSW_NU_FRAM_PORT_OFFSET	0x200

#define AM65_CPSW_MAX_PORTS	8

#define AM65_CPSW_MIN_PACKET_SIZE	VLAN_ETH_ZLEN
#define AM65_CPSW_MAX_PACKET_SIZE	2024

#define AM65_CPSW_REG_CTL		0x004
#define AM65_CPSW_REG_STAT_PORT_EN	0x014
#define AM65_CPSW_REG_PTYPE		0x018

#define AM65_CPSW_P0_REG_CTL			0x004
#define AM65_CPSW_PORT0_REG_FLOW_ID_OFFSET	0x008

#define AM65_CPSW_PORT_REG_PRI_CTL		0x01c
#define AM65_CPSW_PORT_REG_RX_PRI_MAP		0x020
#define AM65_CPSW_PORT_REG_RX_MAXLEN		0x024

#define AM65_CPSW_PORTN_REG_CTL			0x004
#define AM65_CPSW_PORTN_REG_DSCP_MAP		0x120
#define AM65_CPSW_PORTN_REG_SA_L		0x308
#define AM65_CPSW_PORTN_REG_SA_H		0x30c
#define AM65_CPSW_PORTN_REG_TS_CTL              0x310
#define AM65_CPSW_PORTN_REG_TS_SEQ_LTYPE_REG	0x314
#define AM65_CPSW_PORTN_REG_TS_VLAN_LTYPE_REG	0x318
#define AM65_CPSW_PORTN_REG_TS_CTL_LTYPE2       0x31C

#define AM65_CPSW_SGMII_CONTROL_REG		0x010
#define AM65_CPSW_SGMII_MR_ADV_ABILITY_REG	0x018
#define AM65_CPSW_SGMII_CONTROL_MR_AN_ENABLE	BIT(0)

#define AM65_CPSW_CTL_VLAN_AWARE		BIT(1)
#define AM65_CPSW_CTL_P0_ENABLE			BIT(2)
#define AM65_CPSW_CTL_P0_TX_CRC_REMOVE		BIT(13)
#define AM65_CPSW_CTL_P0_RX_PAD			BIT(14)

/* AM65_CPSW_P0_REG_CTL */
#define AM65_CPSW_P0_REG_CTL_RX_CHECKSUM_EN	BIT(0)
#define AM65_CPSW_P0_REG_CTL_RX_REMAP_VLAN	BIT(16)

/* AM65_CPSW_PORT_REG_PRI_CTL */
#define AM65_CPSW_PORT_REG_PRI_CTL_RX_PTYPE_RROBIN	BIT(8)

/* AM65_CPSW_PN_REG_CTL */
#define AM65_CPSW_PN_REG_CTL_DSCP_IPV4_EN	BIT(1)
#define AM65_CPSW_PN_REG_CTL_DSCP_IPV6_EN	BIT(2)

/* AM65_CPSW_PN_TS_CTL register fields */
#define AM65_CPSW_PN_TS_CTL_TX_ANX_F_EN		BIT(4)
#define AM65_CPSW_PN_TS_CTL_TX_VLAN_LT1_EN	BIT(5)
#define AM65_CPSW_PN_TS_CTL_TX_VLAN_LT2_EN	BIT(6)
#define AM65_CPSW_PN_TS_CTL_TX_ANX_D_EN		BIT(7)
#define AM65_CPSW_PN_TS_CTL_TX_ANX_E_EN		BIT(10)
#define AM65_CPSW_PN_TS_CTL_TX_HOST_TS_EN	BIT(11)
#define AM65_CPSW_PN_TS_CTL_MSG_TYPE_EN_SHIFT	16

#define AM65_CPSW_PN_TS_CTL_RX_ANX_F_EN		BIT(0)
#define AM65_CPSW_PN_TS_CTL_RX_VLAN_LT1_EN	BIT(1)
#define AM65_CPSW_PN_TS_CTL_RX_VLAN_LT2_EN	BIT(2)
#define AM65_CPSW_PN_TS_CTL_RX_ANX_D_EN		BIT(3)
#define AM65_CPSW_PN_TS_CTL_RX_ANX_E_EN		BIT(9)

/* AM65_CPSW_PORTN_REG_TS_SEQ_LTYPE_REG register fields */
#define AM65_CPSW_PN_TS_SEQ_ID_OFFSET_SHIFT	16

/* AM65_CPSW_PORTN_REG_TS_CTL_LTYPE2 */
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_107	BIT(16)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_129	BIT(17)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_130	BIT(18)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_131	BIT(19)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_132	BIT(20)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_319	BIT(21)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_320	BIT(22)
#define AM65_CPSW_PN_TS_CTL_LTYPE2_TS_TTL_NONZERO BIT(23)

/* The PTP event messages - Sync, Delay_Req, Pdelay_Req, and Pdelay_Resp. */
#define AM65_CPSW_TS_EVENT_MSG_TYPE_BITS (BIT(0) | BIT(1) | BIT(2) | BIT(3))

#define AM65_CPSW_TS_SEQ_ID_OFFSET (0x1e)

#define AM65_CPSW_TS_TX_ANX_ALL_EN		\
	(AM65_CPSW_PN_TS_CTL_TX_ANX_D_EN |	\
	 AM65_CPSW_PN_TS_CTL_TX_ANX_E_EN |	\
	 AM65_CPSW_PN_TS_CTL_TX_ANX_F_EN)

#define AM65_CPSW_TS_RX_ANX_ALL_EN		\
	(AM65_CPSW_PN_TS_CTL_RX_ANX_D_EN |	\
	 AM65_CPSW_PN_TS_CTL_RX_ANX_E_EN |	\
	 AM65_CPSW_PN_TS_CTL_RX_ANX_F_EN)

#define AM65_CPSW_ALE_AGEOUT_DEFAULT	30
/* Number of TX/RX descriptors per channel/flow */
#define AM65_CPSW_MAX_TX_DESC	500
#define AM65_CPSW_MAX_RX_DESC	500

#define AM65_CPSW_NAV_PS_DATA_SIZE 16
#define AM65_CPSW_NAV_SW_DATA_SIZE 16

#define AM65_CPSW_DEBUG	(NETIF_MSG_HW | NETIF_MSG_DRV | NETIF_MSG_LINK | \
			 NETIF_MSG_IFUP	| NETIF_MSG_PROBE | NETIF_MSG_IFDOWN | \
			 NETIF_MSG_RX_ERR | NETIF_MSG_TX_ERR)

#define AM65_CPSW_DEFAULT_TX_CHNS	8
#define AM65_CPSW_DEFAULT_RX_CHN_FLOWS	1

/* CPPI streaming packet interface */
#define AM65_CPSW_CPPI_TX_FLOW_ID  0x3FFF
#define AM65_CPSW_CPPI_TX_PKT_TYPE 0x7

/* XDP */
#define AM65_CPSW_XDP_TX       BIT(2)
#define AM65_CPSW_XDP_CONSUMED BIT(1)
#define AM65_CPSW_XDP_REDIRECT BIT(0)
#define AM65_CPSW_XDP_PASS     0

/* Include headroom compatible with both skb and xdpf */
#define AM65_CPSW_HEADROOM_NA (max(NET_SKB_PAD, XDP_PACKET_HEADROOM) + NET_IP_ALIGN)
#define AM65_CPSW_HEADROOM ALIGN(AM65_CPSW_HEADROOM_NA, sizeof(long))

static void am65_cpsw_port_set_sl_mac(struct am65_cpsw_port *slave,
				      const u8 *dev_addr)
{
	u32 mac_hi = (dev_addr[0] << 0) | (dev_addr[1] << 8) |
		     (dev_addr[2] << 16) | (dev_addr[3] << 24);
	u32 mac_lo = (dev_addr[4] << 0) | (dev_addr[5] << 8);

	writel(mac_hi, slave->port_base + AM65_CPSW_PORTN_REG_SA_H);
	writel(mac_lo, slave->port_base + AM65_CPSW_PORTN_REG_SA_L);
}

#define AM65_CPSW_DSCP_MAX	GENMASK(5, 0)
#define AM65_CPSW_PRI_MAX	GENMASK(2, 0)
#define AM65_CPSW_DSCP_PRI_PER_REG	8
#define AM65_CPSW_DSCP_PRI_SIZE		4	/* in bits */
static int am65_cpsw_port_set_dscp_map(struct am65_cpsw_port *slave, u8 dscp, u8 pri)
{
	int reg_ofs;
	int bit_ofs;
	u32 val;

	if (dscp > AM65_CPSW_DSCP_MAX)
		return -EINVAL;

	if (pri > AM65_CPSW_PRI_MAX)
		return -EINVAL;

	/* 32-bit register offset to this dscp */
	reg_ofs = (dscp / AM65_CPSW_DSCP_PRI_PER_REG) * 4;
	/* bit field offset to this dscp */
	bit_ofs = AM65_CPSW_DSCP_PRI_SIZE * (dscp % AM65_CPSW_DSCP_PRI_PER_REG);

	val = readl(slave->port_base + AM65_CPSW_PORTN_REG_DSCP_MAP + reg_ofs);
	val &= ~(AM65_CPSW_PRI_MAX << bit_ofs);	/* clear */
	val |= pri << bit_ofs;			/* set */
	writel(val, slave->port_base + AM65_CPSW_PORTN_REG_DSCP_MAP + reg_ofs);

	return 0;
}

static void am65_cpsw_port_enable_dscp_map(struct am65_cpsw_port *slave)
{
	int dscp, pri;
	u32 val;

	/* Default DSCP to User Priority mapping as per:
	 * https://datatracker.ietf.org/doc/html/rfc8325#section-4.3
	 * and
	 * https://datatracker.ietf.org/doc/html/rfc8622#section-11
	 */
	for (dscp = 0; dscp <= AM65_CPSW_DSCP_MAX; dscp++) {
		switch (dscp) {
		case 56:	/* CS7 */
		case 48:	/* CS6 */
			pri = 7;
			break;
		case 46:	/* EF */
		case 44:	/* VA */
			pri = 6;
			break;
		case 40:	/* CS5 */
			pri = 5;
			break;
		case 34:	/* AF41 */
		case 36:	/* AF42 */
		case 38:	/* AF43 */
		case 32:	/* CS4 */
		case 26:	/* AF31 */
		case 28:	/* AF32 */
		case 30:	/* AF33 */
		case 24:	/* CS3 */
			pri = 4;
			break;
		case 18:	/* AF21 */
		case 20:	/* AF22 */
		case 22:	/* AF23 */
			pri = 3;
			break;
		case 16:	/* CS2 */
		case 10:	/* AF11 */
		case 12:	/* AF12 */
		case 14:	/* AF13 */
		case 0:		/* DF */
			pri = 0;
			break;
		case 8:		/* CS1 */
		case 1:		/* LE */
			pri = 1;
			break;
		default:
			pri = 0;
			break;
		}

		am65_cpsw_port_set_dscp_map(slave, dscp, pri);
	}

	/* enable port IPV4 and IPV6 DSCP for this port */
	val = readl(slave->port_base + AM65_CPSW_PORTN_REG_CTL);
	val |= AM65_CPSW_PN_REG_CTL_DSCP_IPV4_EN |
		AM65_CPSW_PN_REG_CTL_DSCP_IPV6_EN;
	writel(val, slave->port_base + AM65_CPSW_PORTN_REG_CTL);
}

static void am65_cpsw_sl_ctl_reset(struct am65_cpsw_port *port)
{
	cpsw_sl_reset(port->slave.mac_sl, 100);
	/* Max length register has to be restored after MAC SL reset */
	writel(AM65_CPSW_MAX_PACKET_SIZE,
	       port->port_base + AM65_CPSW_PORT_REG_RX_MAXLEN);
}

static void am65_cpsw_nuss_get_ver(struct am65_cpsw_common *common)
{
	common->nuss_ver = readl(common->ss_base);
	common->cpsw_ver = readl(common->cpsw_base);
	dev_info(common->dev,
		 "initializing am65 cpsw nuss version 0x%08X, cpsw version 0x%08X Ports: %u quirks:%08x\n",
		common->nuss_ver,
		common->cpsw_ver,
		common->port_num + 1,
		common->pdata.quirks);
}

static int am65_cpsw_nuss_ndo_slave_add_vid(struct net_device *ndev,
					    __be16 proto, u16 vid)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	u32 port_mask, unreg_mcast = 0;
	int ret;

	if (!common->is_emac_mode)
		return 0;

	if (!netif_running(ndev) || !vid)
		return 0;

	ret = pm_runtime_resume_and_get(common->dev);
	if (ret < 0)
		return ret;

	port_mask = BIT(port->port_id) | ALE_PORT_HOST;
	if (!vid)
		unreg_mcast = port_mask;
	dev_info(common->dev, "Adding vlan %d to vlan filter\n", vid);
	ret = cpsw_ale_vlan_add_modify(common->ale, vid, port_mask,
				       unreg_mcast, port_mask, 0);

	pm_runtime_put(common->dev);
	return ret;
}

static int am65_cpsw_nuss_ndo_slave_kill_vid(struct net_device *ndev,
					     __be16 proto, u16 vid)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	int ret;

	if (!common->is_emac_mode)
		return 0;

	if (!netif_running(ndev) || !vid)
		return 0;

	ret = pm_runtime_resume_and_get(common->dev);
	if (ret < 0)
		return ret;

	dev_info(common->dev, "Removing vlan %d from vlan filter\n", vid);
	ret = cpsw_ale_del_vlan(common->ale, vid,
				BIT(port->port_id) | ALE_PORT_HOST);

	pm_runtime_put(common->dev);
	return ret;
}

static void am65_cpsw_slave_set_promisc(struct am65_cpsw_port *port,
					bool promisc)
{
	struct am65_cpsw_common *common = port->common;

	if (promisc && !common->is_emac_mode) {
		dev_dbg(common->dev, "promisc mode requested in switch mode");
		return;
	}

	if (promisc) {
		/* Enable promiscuous mode */
		cpsw_ale_control_set(common->ale, port->port_id,
				     ALE_PORT_MACONLY_CAF, 1);
		dev_dbg(common->dev, "promisc enabled\n");
	} else {
		/* Disable promiscuous mode */
		cpsw_ale_control_set(common->ale, port->port_id,
				     ALE_PORT_MACONLY_CAF, 0);
		dev_dbg(common->dev, "promisc disabled\n");
	}
}

static void am65_cpsw_nuss_ndo_slave_set_rx_mode(struct net_device *ndev)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	u32 port_mask;
	bool promisc;

	promisc = !!(ndev->flags & IFF_PROMISC);
	am65_cpsw_slave_set_promisc(port, promisc);

	if (promisc)
		return;

	/* Restore allmulti on vlans if necessary */
	cpsw_ale_set_allmulti(common->ale,
			      ndev->flags & IFF_ALLMULTI, port->port_id);

	port_mask = ALE_PORT_HOST;
	/* Clear all mcast from ALE */
	cpsw_ale_flush_multicast(common->ale, port_mask, -1);

	if (!netdev_mc_empty(ndev)) {
		struct netdev_hw_addr *ha;

		/* program multicast address list into ALE register */
		netdev_for_each_mc_addr(ha, ndev) {
			cpsw_ale_add_mcast(common->ale, ha->addr,
					   port_mask, 0, 0, 0);
		}
	}
}

static void am65_cpsw_nuss_ndo_host_tx_timeout(struct net_device *ndev,
					       unsigned int txqueue)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_tx_chn *tx_chn;
	struct netdev_queue *netif_txq;
	unsigned long trans_start;

	netif_txq = netdev_get_tx_queue(ndev, txqueue);
	tx_chn = &common->tx_chns[txqueue];
	trans_start = READ_ONCE(netif_txq->trans_start);

	netdev_err(ndev, "txq:%d DRV_XOFF:%d tmo:%u dql_avail:%d free_desc:%zu\n",
		   txqueue,
		   netif_tx_queue_stopped(netif_txq),
		   jiffies_to_msecs(jiffies - trans_start),
		   netdev_queue_dql_avail(netif_txq),
		   k3_cppi_desc_pool_avail(tx_chn->desc_pool));

	if (netif_tx_queue_stopped(netif_txq)) {
		/* try recover if stopped by us */
		txq_trans_update(netif_txq);
		netif_tx_wake_queue(netif_txq);
	}
}

static int am65_cpsw_nuss_rx_push(struct am65_cpsw_common *common,
				  struct page *page, u32 flow_idx)
{
	struct am65_cpsw_rx_chn *rx_chn = &common->rx_chns;
	struct cppi5_host_desc_t *desc_rx;
	struct device *dev = common->dev;
	struct am65_cpsw_swdata *swdata;
	dma_addr_t desc_dma;
	dma_addr_t buf_dma;

	desc_rx = k3_cppi_desc_pool_alloc(rx_chn->desc_pool);
	if (!desc_rx) {
		dev_err(dev, "Failed to allocate RXFDQ descriptor\n");
		return -ENOMEM;
	}
	desc_dma = k3_cppi_desc_pool_virt2dma(rx_chn->desc_pool, desc_rx);

	buf_dma = dma_map_single(rx_chn->dma_dev,
				 page_address(page) + AM65_CPSW_HEADROOM,
				 AM65_CPSW_MAX_PACKET_SIZE, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(rx_chn->dma_dev, buf_dma))) {
		k3_cppi_desc_pool_free(rx_chn->desc_pool, desc_rx);
		dev_err(dev, "Failed to map rx buffer\n");
		return -EINVAL;
	}

	cppi5_hdesc_init(desc_rx, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 AM65_CPSW_NAV_PS_DATA_SIZE);
	k3_udma_glue_rx_dma_to_cppi5_addr(rx_chn->rx_chn, &buf_dma);
	cppi5_hdesc_attach_buf(desc_rx, buf_dma, AM65_CPSW_MAX_PACKET_SIZE,
			       buf_dma, AM65_CPSW_MAX_PACKET_SIZE);
	swdata = cppi5_hdesc_get_swdata(desc_rx);
	swdata->page = page;
	swdata->flow_id = flow_idx;

	return k3_udma_glue_push_rx_chn(rx_chn->rx_chn, flow_idx,
					desc_rx, desc_dma);
}

void am65_cpsw_nuss_set_p0_ptype(struct am65_cpsw_common *common)
{
	struct am65_cpsw_host *host_p = am65_common_get_host(common);
	u32 val, pri_map;

	/* P0 set Receive Priority Type */
	val = readl(host_p->port_base + AM65_CPSW_PORT_REG_PRI_CTL);

	if (common->pf_p0_rx_ptype_rrobin) {
		val |= AM65_CPSW_PORT_REG_PRI_CTL_RX_PTYPE_RROBIN;
		/* Enet Ports fifos works in fixed priority mode only, so
		 * reset P0_Rx_Pri_Map so all packet will go in Enet fifo 0
		 */
		pri_map = 0x0;
	} else {
		val &= ~AM65_CPSW_PORT_REG_PRI_CTL_RX_PTYPE_RROBIN;
		/* restore P0_Rx_Pri_Map */
		pri_map = 0x76543210;
	}

	writel(pri_map, host_p->port_base + AM65_CPSW_PORT_REG_RX_PRI_MAP);
	writel(val, host_p->port_base + AM65_CPSW_PORT_REG_PRI_CTL);
}

static void am65_cpsw_init_host_port_switch(struct am65_cpsw_common *common);
static void am65_cpsw_init_host_port_emac(struct am65_cpsw_common *common);
static void am65_cpsw_init_port_switch_ale(struct am65_cpsw_port *port);
static void am65_cpsw_init_port_emac_ale(struct am65_cpsw_port *port);
static inline void am65_cpsw_put_page(struct am65_cpsw_rx_flow *flow,
				      struct page *page,
				      bool allow_direct);
static void am65_cpsw_nuss_rx_cleanup(void *data, dma_addr_t desc_dma);
static void am65_cpsw_nuss_tx_cleanup(void *data, dma_addr_t desc_dma);

static void am65_cpsw_destroy_rxq(struct am65_cpsw_common *common, int id)
{
	struct am65_cpsw_rx_chn *rx_chn = &common->rx_chns;
	struct am65_cpsw_rx_flow *flow;
	struct xdp_rxq_info *rxq;
	int port;

	flow = &rx_chn->flows[id];
	napi_disable(&flow->napi_rx);
	hrtimer_cancel(&flow->rx_hrtimer);
	k3_udma_glue_reset_rx_chn(rx_chn->rx_chn, id, rx_chn,
				  am65_cpsw_nuss_rx_cleanup);

	for (port = 0; port < common->port_num; port++) {
		if (!common->ports[port].ndev)
			continue;

		rxq = &common->ports[port].xdp_rxq[id];

		if (xdp_rxq_info_is_reg(rxq))
			xdp_rxq_info_unreg(rxq);
	}

	if (flow->page_pool) {
		page_pool_destroy(flow->page_pool);
		flow->page_pool = NULL;
	}
}

static void am65_cpsw_destroy_rxqs(struct am65_cpsw_common *common)
{
	struct am65_cpsw_rx_chn *rx_chn = &common->rx_chns;
	int id;

	reinit_completion(&common->tdown_complete);
	k3_udma_glue_tdown_rx_chn(rx_chn->rx_chn, true);

	if (common->pdata.quirks & AM64_CPSW_QUIRK_DMA_RX_TDOWN_IRQ) {
		id = wait_for_completion_timeout(&common->tdown_complete, msecs_to_jiffies(1000));
		if (!id)
			dev_err(common->dev, "rx teardown timeout\n");
	}

	for (id = common->rx_ch_num_flows - 1; id >= 0; id--)
		am65_cpsw_destroy_rxq(common, id);

	k3_udma_glue_disable_rx_chn(common->rx_chns.rx_chn);
}

static int am65_cpsw_create_rxq(struct am65_cpsw_common *common, int id)
{
	struct am65_cpsw_rx_chn *rx_chn = &common->rx_chns;
	struct page_pool_params pp_params = {
		.flags = PP_FLAG_DMA_MAP,
		.order = 0,
		.pool_size = AM65_CPSW_MAX_RX_DESC,
		.nid = dev_to_node(common->dev),
		.dev = common->dev,
		.dma_dir = DMA_BIDIRECTIONAL,
		/* .napi set dynamically */
	};
	struct am65_cpsw_rx_flow *flow;
	struct xdp_rxq_info *rxq;
	struct page_pool *pool;
	struct page *page;
	int port, ret, i;

	flow = &rx_chn->flows[id];
	pp_params.napi = &flow->napi_rx;
	pool = page_pool_create(&pp_params);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		return ret;
	}

	flow->page_pool = pool;

	/* using same page pool is allowed as no running rx handlers
	 * simultaneously for both ndevs
	 */
	for (port = 0; port < common->port_num; port++) {
		if (!common->ports[port].ndev)
		/* FIXME should we BUG here? */
			continue;

		rxq = &common->ports[port].xdp_rxq[id];
		ret = xdp_rxq_info_reg(rxq, common->ports[port].ndev,
				       id, flow->napi_rx.napi_id);
		if (ret)
			goto err;

		ret = xdp_rxq_info_reg_mem_model(rxq,
						 MEM_TYPE_PAGE_POOL,
						 pool);
		if (ret)
			goto err;
	}

	for (i = 0; i < AM65_CPSW_MAX_RX_DESC; i++) {
		page = page_pool_dev_alloc_pages(flow->page_pool);
		if (!page) {
			dev_err(common->dev, "cannot allocate page in flow %d\n",
				id);
			ret = -ENOMEM;
			goto err;
		}

		ret = am65_cpsw_nuss_rx_push(common, page, id);
		if (ret < 0) {
			dev_err(common->dev,
				"cannot submit page to rx channel flow %d, error %d\n",
				id, ret);
			am65_cpsw_put_page(flow, page, false);
			goto err;
		}
	}

	napi_enable(&flow->napi_rx);
	return 0;

err:
	am65_cpsw_destroy_rxq(common, id);
	return ret;
}

static int am65_cpsw_create_rxqs(struct am65_cpsw_common *common)
{
	int id, ret;

	for (id = 0; id < common->rx_ch_num_flows; id++) {
		ret = am65_cpsw_create_rxq(common, id);
		if (ret) {
			dev_err(common->dev, "couldn't create rxq %d: %d\n",
				id, ret);
			goto err;
		}
	}

	ret = k3_udma_glue_enable_rx_chn(common->rx_chns.rx_chn);
	if (ret) {
		dev_err(common->dev, "couldn't enable rx chn: %d\n", ret);
		goto err;
	}

	return 0;

err:
	for (--id; id >= 0; id--)
		am65_cpsw_destroy_rxq(common, id);

	return ret;
}

static void am65_cpsw_destroy_txq(struct am65_cpsw_common *common, int id)
{
	struct am65_cpsw_tx_chn *tx_chn = &common->tx_chns[id];

	napi_disable(&tx_chn->napi_tx);
	hrtimer_cancel(&tx_chn->tx_hrtimer);
	k3_udma_glue_reset_tx_chn(tx_chn->tx_chn, tx_chn,
				  am65_cpsw_nuss_tx_cleanup);
	k3_udma_glue_disable_tx_chn(tx_chn->tx_chn);
}

static void am65_cpsw_destroy_txqs(struct am65_cpsw_common *common)
{
	struct am65_cpsw_tx_chn *tx_chn = common->tx_chns;
	int id;

	/* shutdown tx channels */
	atomic_set(&common->tdown_cnt, common->tx_ch_num);
	/* ensure new tdown_cnt value is visible */
	smp_mb__after_atomic();
	reinit_completion(&common->tdown_complete);

	for (id = 0; id < common->tx_ch_num; id++)
		k3_udma_glue_tdown_tx_chn(tx_chn[id].tx_chn, false);

	id = wait_for_completion_timeout(&common->tdown_complete,
					 msecs_to_jiffies(1000));
	if (!id)
		dev_err(common->dev, "tx teardown timeout\n");

	for (id = common->tx_ch_num - 1; id >= 0; id--)
		am65_cpsw_destroy_txq(common, id);
}

static int am65_cpsw_create_txq(struct am65_cpsw_common *common, int id)
{
	struct am65_cpsw_tx_chn *tx_chn = &common->tx_chns[id];
	int ret;

	ret = k3_udma_glue_enable_tx_chn(tx_chn->tx_chn);
	if (ret)
		return ret;

	napi_enable(&tx_chn->napi_tx);

	return 0;
}

static int am65_cpsw_create_txqs(struct am65_cpsw_common *common)
{
	int id, ret;

	for (id = 0; id < common->tx_ch_num; id++) {
		ret = am65_cpsw_create_txq(common, id);
		if (ret) {
			dev_err(common->dev, "couldn't create txq %d: %d\n",
				id, ret);
			goto err;
		}
	}

	return 0;

err:
	for (--id; id >= 0; id--)
		am65_cpsw_destroy_txq(common, id);

	return ret;
}

static int am65_cpsw_nuss_desc_idx(struct k3_cppi_desc_pool *desc_pool,
				   void *desc,
				   unsigned char dsize_log2)
{
	void *pool_addr = k3_cppi_desc_pool_cpuaddr(desc_pool);

	return (desc - pool_addr) >> dsize_log2;
}

static void am65_cpsw_nuss_set_buf_type(struct am65_cpsw_tx_chn *tx_chn,
					struct cppi5_host_desc_t *desc,
					enum am65_cpsw_tx_buf_type buf_type)
{
	int desc_idx;

	desc_idx = am65_cpsw_nuss_desc_idx(tx_chn->desc_pool, desc,
					   tx_chn->dsize_log2);
	k3_cppi_desc_pool_desc_info_set(tx_chn->desc_pool, desc_idx,
					(void *)buf_type);
}

static enum am65_cpsw_tx_buf_type am65_cpsw_nuss_buf_type(struct am65_cpsw_tx_chn *tx_chn,
							  dma_addr_t desc_dma)
{
	struct cppi5_host_desc_t *desc_tx;
	int desc_idx;

	desc_tx = k3_cppi_desc_pool_dma2virt(tx_chn->desc_pool, desc_dma);
	desc_idx = am65_cpsw_nuss_desc_idx(tx_chn->desc_pool, desc_tx,
					   tx_chn->dsize_log2);

	return (enum am65_cpsw_tx_buf_type)k3_cppi_desc_pool_desc_info(tx_chn->desc_pool,
								       desc_idx);
}

static inline void am65_cpsw_put_page(struct am65_cpsw_rx_flow *flow,
				      struct page *page,
				      bool allow_direct)
{
	page_pool_put_full_page(flow->page_pool, page, allow_direct);
}

static void am65_cpsw_nuss_rx_cleanup(void *data, dma_addr_t desc_dma)
{
	struct am65_cpsw_rx_chn *rx_chn = data;
	struct cppi5_host_desc_t *desc_rx;
	struct am65_cpsw_swdata *swdata;
	dma_addr_t buf_dma;
	struct page *page;
	u32 buf_dma_len;
	u32 flow_id;

	desc_rx = k3_cppi_desc_pool_dma2virt(rx_chn->desc_pool, desc_dma);
	swdata = cppi5_hdesc_get_swdata(desc_rx);
	page = swdata->page;
	flow_id = swdata->flow_id;
	cppi5_hdesc_get_obuf(desc_rx, &buf_dma, &buf_dma_len);
	k3_udma_glue_rx_cppi5_to_dma_addr(rx_chn->rx_chn, &buf_dma);
	dma_unmap_single(rx_chn->dma_dev, buf_dma, buf_dma_len, DMA_FROM_DEVICE);
	k3_cppi_desc_pool_free(rx_chn->desc_pool, desc_rx);
	am65_cpsw_put_page(&rx_chn->flows[flow_id], page, false);
}

static void am65_cpsw_nuss_xmit_free(struct am65_cpsw_tx_chn *tx_chn,
				     struct cppi5_host_desc_t *desc)
{
	struct cppi5_host_desc_t *first_desc, *next_desc;
	dma_addr_t buf_dma, next_desc_dma;
	u32 buf_dma_len;

	first_desc = desc;
	next_desc = first_desc;

	cppi5_hdesc_get_obuf(first_desc, &buf_dma, &buf_dma_len);
	k3_udma_glue_tx_cppi5_to_dma_addr(tx_chn->tx_chn, &buf_dma);

	dma_unmap_single(tx_chn->dma_dev, buf_dma, buf_dma_len, DMA_TO_DEVICE);

	next_desc_dma = cppi5_hdesc_get_next_hbdesc(first_desc);
	k3_udma_glue_tx_cppi5_to_dma_addr(tx_chn->tx_chn, &next_desc_dma);
	while (next_desc_dma) {
		next_desc = k3_cppi_desc_pool_dma2virt(tx_chn->desc_pool,
						       next_desc_dma);
		cppi5_hdesc_get_obuf(next_desc, &buf_dma, &buf_dma_len);
		k3_udma_glue_tx_cppi5_to_dma_addr(tx_chn->tx_chn, &buf_dma);

		dma_unmap_page(tx_chn->dma_dev, buf_dma, buf_dma_len,
			       DMA_TO_DEVICE);

		next_desc_dma = cppi5_hdesc_get_next_hbdesc(next_desc);
		k3_udma_glue_tx_cppi5_to_dma_addr(tx_chn->tx_chn, &next_desc_dma);

		k3_cppi_desc_pool_free(tx_chn->desc_pool, next_desc);
	}

	k3_cppi_desc_pool_free(tx_chn->desc_pool, first_desc);
}

static void am65_cpsw_nuss_tx_cleanup(void *data, dma_addr_t desc_dma)
{
	struct am65_cpsw_tx_chn *tx_chn = data;
	enum am65_cpsw_tx_buf_type buf_type;
	struct am65_cpsw_tx_swdata *swdata;
	struct cppi5_host_desc_t *desc_tx;
	struct xdp_frame *xdpf;
	struct sk_buff *skb;

	desc_tx = k3_cppi_desc_pool_dma2virt(tx_chn->desc_pool, desc_dma);
	swdata = cppi5_hdesc_get_swdata(desc_tx);
	buf_type = am65_cpsw_nuss_buf_type(tx_chn, desc_dma);
	if (buf_type == AM65_CPSW_TX_BUF_TYPE_SKB) {
		skb = swdata->skb;
		dev_kfree_skb_any(skb);
	} else {
		xdpf = swdata->xdpf;
		xdp_return_frame(xdpf);
	}

	am65_cpsw_nuss_xmit_free(tx_chn, desc_tx);
}

static struct sk_buff *am65_cpsw_build_skb(void *page_addr,
					   struct net_device *ndev,
					   unsigned int len,
					   unsigned int headroom)
{
	struct sk_buff *skb;

	skb = build_skb(page_addr, len);
	if (unlikely(!skb))
		return NULL;

	skb_reserve(skb, headroom);
	skb->dev = ndev;

	return skb;
}

static int am65_cpsw_nuss_common_open(struct am65_cpsw_common *common)
{
	struct am65_cpsw_host *host_p = am65_common_get_host(common);
	u32 val, port_mask;
	int port_idx, ret;

	if (common->usage_count)
		return 0;

	/* Control register */
	writel(AM65_CPSW_CTL_P0_ENABLE | AM65_CPSW_CTL_P0_TX_CRC_REMOVE |
	       AM65_CPSW_CTL_VLAN_AWARE | AM65_CPSW_CTL_P0_RX_PAD,
	       common->cpsw_base + AM65_CPSW_REG_CTL);
	/* Max length register */
	writel(AM65_CPSW_MAX_PACKET_SIZE,
	       host_p->port_base + AM65_CPSW_PORT_REG_RX_MAXLEN);
	/* set base flow_id */
	writel(common->rx_flow_id_base,
	       host_p->port_base + AM65_CPSW_PORT0_REG_FLOW_ID_OFFSET);
	writel(AM65_CPSW_P0_REG_CTL_RX_CHECKSUM_EN | AM65_CPSW_P0_REG_CTL_RX_REMAP_VLAN,
	       host_p->port_base + AM65_CPSW_P0_REG_CTL);

	am65_cpsw_nuss_set_p0_ptype(common);

	/* enable statistic */
	val = BIT(HOST_PORT_NUM);
	for (port_idx = 0; port_idx < common->port_num; port_idx++) {
		struct am65_cpsw_port *port = &common->ports[port_idx];

		if (!port->disabled)
			val |=  BIT(port->port_id);
	}
	writel(val, common->cpsw_base + AM65_CPSW_REG_STAT_PORT_EN);

	/* disable priority elevation */
	writel(0, common->cpsw_base + AM65_CPSW_REG_PTYPE);

	cpsw_ale_start(common->ale);

	/* limit to one RX flow only */
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM,
			     ALE_DEFAULT_THREAD_ID, 0);
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM,
			     ALE_DEFAULT_THREAD_ENABLE, 1);
	/* switch to vlan aware mode */
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM, ALE_VLAN_AWARE, 1);
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM,
			     ALE_PORT_STATE, ALE_PORT_STATE_FORWARD);

	/* default vlan cfg: create mask based on enabled ports */
	port_mask = GENMASK(common->port_num, 0) &
		    ~common->disabled_ports_mask;

	cpsw_ale_add_vlan(common->ale, 0, port_mask,
			  port_mask, port_mask,
			  port_mask & ~ALE_PORT_HOST);

	if (common->is_emac_mode)
		am65_cpsw_init_host_port_emac(common);
	else
		am65_cpsw_init_host_port_switch(common);

	am65_cpsw_qos_tx_p0_rate_init(common);

	ret = am65_cpsw_create_rxqs(common);
	if (ret)
		return ret;

	ret = am65_cpsw_create_txqs(common);
	if (ret)
		goto cleanup_rx;

	dev_dbg(common->dev, "cpsw_nuss started\n");
	return 0;

cleanup_rx:
	am65_cpsw_destroy_rxqs(common);

	return ret;
}

static int am65_cpsw_nuss_common_stop(struct am65_cpsw_common *common)
{
	if (common->usage_count != 1)
		return 0;

	cpsw_ale_control_set(common->ale, HOST_PORT_NUM,
			     ALE_PORT_STATE, ALE_PORT_STATE_DISABLE);

	am65_cpsw_destroy_txqs(common);
	am65_cpsw_destroy_rxqs(common);
	cpsw_ale_stop(common->ale);

	writel(0, common->cpsw_base + AM65_CPSW_REG_CTL);
	writel(0, common->cpsw_base + AM65_CPSW_REG_STAT_PORT_EN);

	dev_dbg(common->dev, "cpsw_nuss stopped\n");
	return 0;
}

static int am65_cpsw_nuss_ndo_slave_stop(struct net_device *ndev)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	int ret;

	phylink_stop(port->slave.phylink);

	netif_tx_stop_all_queues(ndev);

	phylink_disconnect_phy(port->slave.phylink);

	ret = am65_cpsw_nuss_common_stop(common);
	if (ret)
		return ret;

	common->usage_count--;
	pm_runtime_put(common->dev);
	return 0;
}

static int cpsw_restore_vlans(struct net_device *vdev, int vid, void *arg)
{
	struct am65_cpsw_port *port = arg;

	if (!vdev)
		return 0;

	return am65_cpsw_nuss_ndo_slave_add_vid(port->ndev, 0, vid);
}

static int am65_cpsw_nuss_ndo_slave_open(struct net_device *ndev)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	int ret, i;
	u32 reg;

	ret = pm_runtime_resume_and_get(common->dev);
	if (ret < 0)
		return ret;

	/* Idle MAC port */
	cpsw_sl_ctl_set(port->slave.mac_sl, CPSW_SL_CTL_CMD_IDLE);
	cpsw_sl_wait_for_idle(port->slave.mac_sl, 100);
	cpsw_sl_ctl_reset(port->slave.mac_sl);

	/* soft reset MAC */
	cpsw_sl_reg_write(port->slave.mac_sl, CPSW_SL_SOFT_RESET, 1);
	mdelay(1);
	reg = cpsw_sl_reg_read(port->slave.mac_sl, CPSW_SL_SOFT_RESET);
	if (reg) {
		dev_err(common->dev, "soft RESET didn't complete\n");
		ret = -ETIMEDOUT;
		goto runtime_put;
	}

	/* Notify the stack of the actual queue counts. */
	ret = netif_set_real_num_tx_queues(ndev, common->tx_ch_num);
	if (ret) {
		dev_err(common->dev, "cannot set real number of tx queues\n");
		goto runtime_put;
	}

	ret = netif_set_real_num_rx_queues(ndev, common->rx_ch_num_flows);
	if (ret) {
		dev_err(common->dev, "cannot set real number of rx queues\n");
		goto runtime_put;
	}

	for (i = 0; i < common->tx_ch_num; i++) {
		struct netdev_queue *txq = netdev_get_tx_queue(ndev, i);

		netdev_tx_reset_queue(txq);
		txq->tx_maxrate =  common->tx_chns[i].rate_mbps;
	}

	ret = am65_cpsw_nuss_common_open(common);
	if (ret)
		goto runtime_put;

	common->usage_count++;

	/* VLAN aware CPSW mode is incompatible with some DSA tagging schemes.
	 * Therefore disable VLAN_AWARE mode if any of the ports is a DSA Port.
	 */
	if (netdev_uses_dsa(ndev)) {
		reg = readl(common->cpsw_base + AM65_CPSW_REG_CTL);
		reg &= ~AM65_CPSW_CTL_VLAN_AWARE;
		writel(reg, common->cpsw_base + AM65_CPSW_REG_CTL);
	}

	am65_cpsw_port_set_sl_mac(port, ndev->dev_addr);
	am65_cpsw_port_enable_dscp_map(port);

	if (common->is_emac_mode)
		am65_cpsw_init_port_emac_ale(port);
	else
		am65_cpsw_init_port_switch_ale(port);

	/* mac_sl should be configured via phy-link interface */
	am65_cpsw_sl_ctl_reset(port);

	ret = phylink_of_phy_connect(port->slave.phylink, port->slave.port_np, 0);
	if (ret)
		goto error_cleanup;

	/* restore vlan configurations */
	vlan_for_each(ndev, cpsw_restore_vlans, port);

	phylink_start(port->slave.phylink);

	return 0;

error_cleanup:
	am65_cpsw_nuss_ndo_slave_stop(ndev);
	return ret;

runtime_put:
	pm_runtime_put(common->dev);
	return ret;
}

static int am65_cpsw_xdp_tx_frame(struct net_device *ndev,
				  struct am65_cpsw_tx_chn *tx_chn,
				  struct xdp_frame *xdpf,
				  enum am65_cpsw_tx_buf_type buf_type)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	struct cppi5_host_desc_t *host_desc;
	struct am65_cpsw_tx_swdata *swdata;
	struct netdev_queue *netif_txq;
	dma_addr_t dma_desc, dma_buf;
	u32 pkt_len = xdpf->len;
	int ret;

	host_desc = k3_cppi_desc_pool_alloc(tx_chn->desc_pool);
	if (unlikely(!host_desc)) {
		ndev->stats.tx_dropped++;
		return AM65_CPSW_XDP_CONSUMED;	/* drop */
	}

	am65_cpsw_nuss_set_buf_type(tx_chn, host_desc, buf_type);

	dma_buf = dma_map_single(tx_chn->dma_dev, xdpf->data,
				 pkt_len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(tx_chn->dma_dev, dma_buf))) {
		ndev->stats.tx_dropped++;
		ret = AM65_CPSW_XDP_CONSUMED;	/* drop */
		goto pool_free;
	}

	cppi5_hdesc_init(host_desc, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 AM65_CPSW_NAV_PS_DATA_SIZE);
	cppi5_hdesc_set_pkttype(host_desc, AM65_CPSW_CPPI_TX_PKT_TYPE);
	cppi5_hdesc_set_pktlen(host_desc, pkt_len);
	cppi5_desc_set_pktids(&host_desc->hdr, 0, AM65_CPSW_CPPI_TX_FLOW_ID);
	cppi5_desc_set_tags_ids(&host_desc->hdr, 0, port->port_id);

	k3_udma_glue_tx_dma_to_cppi5_addr(tx_chn->tx_chn, &dma_buf);
	cppi5_hdesc_attach_buf(host_desc, dma_buf, pkt_len, dma_buf, pkt_len);

	swdata = cppi5_hdesc_get_swdata(host_desc);
	swdata->ndev = ndev;
	swdata->xdpf = xdpf;

	/* Report BQL before sending the packet */
	netif_txq = netdev_get_tx_queue(ndev, tx_chn->id);
	netdev_tx_sent_queue(netif_txq, pkt_len);

	dma_desc = k3_cppi_desc_pool_virt2dma(tx_chn->desc_pool, host_desc);
	if (AM65_CPSW_IS_CPSW2G(common)) {
		ret = k3_udma_glue_push_tx_chn(tx_chn->tx_chn, host_desc,
					       dma_desc);
	} else {
		spin_lock_bh(&tx_chn->lock);
		ret = k3_udma_glue_push_tx_chn(tx_chn->tx_chn, host_desc,
					       dma_desc);
		spin_unlock_bh(&tx_chn->lock);
	}
	if (ret) {
		/* Inform BQL */
		netdev_tx_completed_queue(netif_txq, 1, pkt_len);
		ndev->stats.tx_errors++;
		ret = AM65_CPSW_XDP_CONSUMED; /* drop */
		goto dma_unmap;
	}

	return 0;

dma_unmap:
	k3_udma_glue_tx_cppi5_to_dma_addr(tx_chn->tx_chn, &dma_buf);
	dma_unmap_single(tx_chn->dma_dev, dma_buf, pkt_len, DMA_TO_DEVICE);
pool_free:
	k3_cppi_desc_pool_free(tx_chn->desc_pool, host_desc);
	return ret;
}

static int am65_cpsw_run_xdp(struct am65_cpsw_rx_flow *flow,
			     struct am65_cpsw_port *port,
			     struct xdp_buff *xdp, int *len)
{
	struct am65_cpsw_common *common = flow->common;
	struct net_device *ndev = port->ndev;
	int ret = AM65_CPSW_XDP_CONSUMED;
	struct am65_cpsw_tx_chn *tx_chn;
	struct netdev_queue *netif_txq;
	int cpu = smp_processor_id();
	struct xdp_frame *xdpf;
	struct bpf_prog *prog;
	int pkt_len;
	u32 act;
	int err;

	pkt_len = *len;
	prog = READ_ONCE(port->xdp_prog);
	if (!prog)
		return AM65_CPSW_XDP_PASS;

	act = bpf_prog_run_xdp(prog, xdp);
	/* XDP prog might have changed packet data and boundaries */
	*len = xdp->data_end - xdp->data;

	switch (act) {
	case XDP_PASS:
		return AM65_CPSW_XDP_PASS;
	case XDP_TX:
		tx_chn = &common->tx_chns[cpu % AM65_CPSW_MAX_QUEUES];
		netif_txq = netdev_get_tx_queue(ndev, tx_chn->id);

		xdpf = xdp_convert_buff_to_frame(xdp);
		if (unlikely(!xdpf)) {
			ndev->stats.tx_dropped++;
			goto drop;
		}

		__netif_tx_lock(netif_txq, cpu);
		err = am65_cpsw_xdp_tx_frame(ndev, tx_chn, xdpf,
					     AM65_CPSW_TX_BUF_TYPE_XDP_TX);
		__netif_tx_unlock(netif_txq);
		if (err)
			goto drop;

		dev_sw_netstats_rx_add(ndev, pkt_len);
		return AM65_CPSW_XDP_TX;
	case XDP_REDIRECT:
		if (unlikely(xdp_do_redirect(ndev, xdp, prog)))
			goto drop;

		dev_sw_netstats_rx_add(ndev, pkt_len);
		return AM65_CPSW_XDP_REDIRECT;
	default:
		bpf_warn_invalid_xdp_action(ndev, prog, act);
		fallthrough;
	case XDP_ABORTED:
drop:
		trace_xdp_exception(ndev, prog, act);
		fallthrough;
	case XDP_DROP:
		ndev->stats.rx_dropped++;
	}

	return ret;
}

/* RX psdata[2] word format - checksum information */
#define AM65_CPSW_RX_PSD_CSUM_ADD	GENMASK(15, 0)
#define AM65_CPSW_RX_PSD_CSUM_ERR	BIT(16)
#define AM65_CPSW_RX_PSD_IS_FRAGMENT	BIT(17)
#define AM65_CPSW_RX_PSD_IS_TCP		BIT(18)
#define AM65_CPSW_RX_PSD_IPV6_VALID	BIT(19)
#define AM65_CPSW_RX_PSD_IPV4_VALID	BIT(20)

static void am65_cpsw_nuss_rx_csum(struct sk_buff *skb, u32 csum_info)
{
	/* HW can verify IPv4/IPv6 TCP/UDP packets checksum
	 * csum information provides in psdata[2] word:
	 * AM65_CPSW_RX_PSD_CSUM_ERR bit - indicates csum error
	 * AM65_CPSW_RX_PSD_IPV6_VALID and AM65_CPSW_RX_PSD_IPV4_VALID
	 * bits - indicates IPv4/IPv6 packet
	 * AM65_CPSW_RX_PSD_IS_FRAGMENT bit - indicates fragmented packet
	 * AM65_CPSW_RX_PSD_CSUM_ADD has value 0xFFFF for non fragmented packets
	 * or csum value for fragmented packets if !AM65_CPSW_RX_PSD_CSUM_ERR
	 */
	skb_checksum_none_assert(skb);

	if (unlikely(!(skb->dev->features & NETIF_F_RXCSUM)))
		return;

	if ((csum_info & (AM65_CPSW_RX_PSD_IPV6_VALID |
			  AM65_CPSW_RX_PSD_IPV4_VALID)) &&
			  !(csum_info & AM65_CPSW_RX_PSD_CSUM_ERR)) {
		/* csum for fragmented packets is unsupported */
		if (!(csum_info & AM65_CPSW_RX_PSD_IS_FRAGMENT))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
	}
}

static int am65_cpsw_nuss_rx_packets(struct am65_cpsw_rx_flow *flow,
				     int *xdp_state)
{
	struct am65_cpsw_rx_chn *rx_chn = &flow->common->rx_chns;
	u32 buf_dma_len, pkt_len, port_id = 0, csum_info;
	struct am65_cpsw_common *common = flow->common;
	struct am65_cpsw_ndev_priv *ndev_priv;
	struct cppi5_host_desc_t *desc_rx;
	struct device *dev = common->dev;
	struct am65_cpsw_swdata *swdata;
	struct page *page, *new_page;
	dma_addr_t desc_dma, buf_dma;
	struct am65_cpsw_port *port;
	struct net_device *ndev;
	u32 flow_idx = flow->id;
	struct sk_buff *skb;
	struct xdp_buff	xdp;
	int headroom, ret;
	void *page_addr;
	u32 *psdata;

	*xdp_state = AM65_CPSW_XDP_PASS;
	ret = k3_udma_glue_pop_rx_chn(rx_chn->rx_chn, flow_idx, &desc_dma);
	if (ret) {
		if (ret != -ENODATA)
			dev_err(dev, "RX: pop chn fail %d\n", ret);
		return ret;
	}

	if (cppi5_desc_is_tdcm(desc_dma)) {
		dev_dbg(dev, "%s RX tdown flow: %u\n", __func__, flow_idx);
		if (common->pdata.quirks & AM64_CPSW_QUIRK_DMA_RX_TDOWN_IRQ)
			complete(&common->tdown_complete);
		return 0;
	}

	desc_rx = k3_cppi_desc_pool_dma2virt(rx_chn->desc_pool, desc_dma);
	dev_dbg(dev, "%s flow_idx: %u desc %pad\n",
		__func__, flow_idx, &desc_dma);

	swdata = cppi5_hdesc_get_swdata(desc_rx);
	page = swdata->page;
	page_addr = page_address(page);
	cppi5_hdesc_get_obuf(desc_rx, &buf_dma, &buf_dma_len);
	k3_udma_glue_rx_cppi5_to_dma_addr(rx_chn->rx_chn, &buf_dma);
	pkt_len = cppi5_hdesc_get_pktlen(desc_rx);
	cppi5_desc_get_tags_ids(&desc_rx->hdr, &port_id, NULL);
	dev_dbg(dev, "%s rx port_id:%d\n", __func__, port_id);
	port = am65_common_get_port(common, port_id);
	ndev = port->ndev;
	psdata = cppi5_hdesc_get_psdata(desc_rx);
	csum_info = psdata[2];
	dev_dbg(dev, "%s rx csum_info:%#x\n", __func__, csum_info);

	dma_unmap_single(rx_chn->dma_dev, buf_dma, buf_dma_len, DMA_FROM_DEVICE);
	k3_cppi_desc_pool_free(rx_chn->desc_pool, desc_rx);

	if (port->xdp_prog) {
		xdp_init_buff(&xdp, PAGE_SIZE, &port->xdp_rxq[flow->id]);
		xdp_prepare_buff(&xdp, page_addr, AM65_CPSW_HEADROOM,
				 pkt_len, false);
		*xdp_state = am65_cpsw_run_xdp(flow, port, &xdp, &pkt_len);
		if (*xdp_state == AM65_CPSW_XDP_CONSUMED) {
			page = virt_to_head_page(xdp.data);
			am65_cpsw_put_page(flow, page, true);
			goto allocate;
		}

		if (*xdp_state != AM65_CPSW_XDP_PASS)
			goto allocate;

		headroom = xdp.data - xdp.data_hard_start;
	} else {
		headroom = AM65_CPSW_HEADROOM;
	}

	skb = am65_cpsw_build_skb(page_addr, ndev,
				  PAGE_SIZE, headroom);
	if (unlikely(!skb)) {
		new_page = page;
		goto requeue;
	}

	ndev_priv = netdev_priv(ndev);
	am65_cpsw_nuss_set_offload_fwd_mark(skb, ndev_priv->offload_fwd_mark);
	skb_put(skb, pkt_len);
	if (port->rx_ts_enabled)
		am65_cpts_rx_timestamp(common->cpts, skb);
	skb_mark_for_recycle(skb);
	skb->protocol = eth_type_trans(skb, ndev);
	am65_cpsw_nuss_rx_csum(skb, csum_info);
	napi_gro_receive(&flow->napi_rx, skb);

	dev_sw_netstats_rx_add(ndev, pkt_len);

allocate:
	new_page = page_pool_dev_alloc_pages(flow->page_pool);
	if (unlikely(!new_page)) {
		dev_err(dev, "page alloc failed\n");
		return -ENOMEM;
	}

	if (netif_dormant(ndev)) {
		am65_cpsw_put_page(flow, new_page, true);
		ndev->stats.rx_dropped++;
		return 0;
	}

requeue:
	ret = am65_cpsw_nuss_rx_push(common, new_page, flow_idx);
	if (WARN_ON(ret < 0)) {
		am65_cpsw_put_page(flow, new_page, true);
		ndev->stats.rx_errors++;
		ndev->stats.rx_dropped++;
	}

	return ret;
}

static enum hrtimer_restart am65_cpsw_nuss_rx_timer_callback(struct hrtimer *timer)
{
	struct am65_cpsw_rx_flow *flow = container_of(timer,
						      struct am65_cpsw_rx_flow,
						      rx_hrtimer);

	enable_irq(flow->irq);
	return HRTIMER_NORESTART;
}

static int am65_cpsw_nuss_rx_poll(struct napi_struct *napi_rx, int budget)
{
	struct am65_cpsw_rx_flow *flow = am65_cpsw_napi_to_rx_flow(napi_rx);
	struct am65_cpsw_common *common = flow->common;
	int xdp_state_or = 0;
	int cur_budget, ret;
	int xdp_state;
	int num_rx = 0;

	/* process only this flow */
	cur_budget = budget;
	while (cur_budget--) {
		ret = am65_cpsw_nuss_rx_packets(flow, &xdp_state);
		xdp_state_or |= xdp_state;
		if (ret)
			break;
		num_rx++;
	}

	if (xdp_state_or & AM65_CPSW_XDP_REDIRECT)
		xdp_do_flush();

	dev_dbg(common->dev, "%s num_rx:%d %d\n", __func__, num_rx, budget);

	if (num_rx < budget && napi_complete_done(napi_rx, num_rx)) {
		if (flow->irq_disabled) {
			flow->irq_disabled = false;
			if (unlikely(flow->rx_pace_timeout)) {
				hrtimer_start(&flow->rx_hrtimer,
					      ns_to_ktime(flow->rx_pace_timeout),
					      HRTIMER_MODE_REL_PINNED);
			} else {
				enable_irq(flow->irq);
			}
		}
	}

	return num_rx;
}

static void am65_cpsw_nuss_tx_wake(struct am65_cpsw_tx_chn *tx_chn, struct net_device *ndev,
				   struct netdev_queue *netif_txq)
{
	if (netif_tx_queue_stopped(netif_txq)) {
		/* Check whether the queue is stopped due to stalled
		 * tx dma, if the queue is stopped then wake the queue
		 * as we have free desc for tx
		 */
		__netif_tx_lock(netif_txq, smp_processor_id());
		if (netif_running(ndev) &&
		    (k3_cppi_desc_pool_avail(tx_chn->desc_pool) >= MAX_SKB_FRAGS))
			netif_tx_wake_queue(netif_txq);

		__netif_tx_unlock(netif_txq);
	}
}

static int am65_cpsw_nuss_tx_compl_packets(struct am65_cpsw_common *common,
					   int chn, unsigned int budget, bool *tdown)
{
	bool single_port = AM65_CPSW_IS_CPSW2G(common);
	enum am65_cpsw_tx_buf_type buf_type;
	struct am65_cpsw_tx_swdata *swdata;
	struct cppi5_host_desc_t *desc_tx;
	struct device *dev = common->dev;
	struct am65_cpsw_tx_chn *tx_chn;
	struct netdev_queue *netif_txq;
	unsigned int total_bytes = 0;
	struct net_device *ndev;
	struct xdp_frame *xdpf;
	unsigned int pkt_len;
	struct sk_buff *skb;
	dma_addr_t desc_dma;
	int res, num_tx = 0;

	tx_chn = &common->tx_chns[chn];

	while (true) {
		if (!single_port)
			spin_lock(&tx_chn->lock);
		res = k3_udma_glue_pop_tx_chn(tx_chn->tx_chn, &desc_dma);
		if (!single_port)
			spin_unlock(&tx_chn->lock);

		if (res == -ENODATA)
			break;

		if (cppi5_desc_is_tdcm(desc_dma)) {
			if (atomic_dec_and_test(&common->tdown_cnt))
				complete(&common->tdown_complete);
			*tdown = true;
			break;
		}

		desc_tx = k3_cppi_desc_pool_dma2virt(tx_chn->desc_pool,
						     desc_dma);
		swdata = cppi5_hdesc_get_swdata(desc_tx);
		ndev = swdata->ndev;
		buf_type = am65_cpsw_nuss_buf_type(tx_chn, desc_dma);
		if (buf_type == AM65_CPSW_TX_BUF_TYPE_SKB) {
			skb = swdata->skb;
			am65_cpts_tx_timestamp(tx_chn->common->cpts, skb);
			pkt_len = skb->len;
			napi_consume_skb(skb, budget);
		} else {
			xdpf = swdata->xdpf;
			pkt_len = xdpf->len;
			if (buf_type == AM65_CPSW_TX_BUF_TYPE_XDP_TX)
				xdp_return_frame_rx_napi(xdpf);
			else
				xdp_return_frame(xdpf);
		}

		total_bytes += pkt_len;
		num_tx++;
		am65_cpsw_nuss_xmit_free(tx_chn, desc_tx);
		dev_sw_netstats_tx_add(ndev, 1, pkt_len);
		if (!single_port) {
			/* as packets from multi ports can be interleaved
			 * on the same channel, we have to figure out the
			 * port/queue at every packet and report it/wake queue.
			 */
			netif_txq = netdev_get_tx_queue(ndev, chn);
			netdev_tx_completed_queue(netif_txq, 1, pkt_len);
			am65_cpsw_nuss_tx_wake(tx_chn, ndev, netif_txq);
		}
	}

	if (single_port) {
		netif_txq = netdev_get_tx_queue(ndev, chn);
		netdev_tx_completed_queue(netif_txq, num_tx, total_bytes);
		am65_cpsw_nuss_tx_wake(tx_chn, ndev, netif_txq);
	}

	dev_dbg(dev, "%s:%u pkt:%d\n", __func__, chn, num_tx);

	return num_tx;
}

static enum hrtimer_restart am65_cpsw_nuss_tx_timer_callback(struct hrtimer *timer)
{
	struct am65_cpsw_tx_chn *tx_chns =
			container_of(timer, struct am65_cpsw_tx_chn, tx_hrtimer);

	enable_irq(tx_chns->irq);
	return HRTIMER_NORESTART;
}

static int am65_cpsw_nuss_tx_poll(struct napi_struct *napi_tx, int budget)
{
	struct am65_cpsw_tx_chn *tx_chn = am65_cpsw_napi_to_tx_chn(napi_tx);
	bool tdown = false;
	int num_tx;

	num_tx = am65_cpsw_nuss_tx_compl_packets(tx_chn->common,
						 tx_chn->id, budget, &tdown);
	if (num_tx >= budget)
		return budget;

	if (napi_complete_done(napi_tx, num_tx)) {
		if (unlikely(tx_chn->tx_pace_timeout && !tdown)) {
			hrtimer_start(&tx_chn->tx_hrtimer,
				      ns_to_ktime(tx_chn->tx_pace_timeout),
				      HRTIMER_MODE_REL_PINNED);
		} else {
			enable_irq(tx_chn->irq);
		}
	}

	return 0;
}

static irqreturn_t am65_cpsw_nuss_rx_irq(int irq, void *dev_id)
{
	struct am65_cpsw_rx_flow *flow = dev_id;

	flow->irq_disabled = true;
	disable_irq_nosync(irq);
	napi_schedule(&flow->napi_rx);

	return IRQ_HANDLED;
}

static irqreturn_t am65_cpsw_nuss_tx_irq(int irq, void *dev_id)
{
	struct am65_cpsw_tx_chn *tx_chn = dev_id;

	disable_irq_nosync(irq);
	napi_schedule(&tx_chn->napi_tx);

	return IRQ_HANDLED;
}

static netdev_tx_t am65_cpsw_nuss_ndo_slave_xmit(struct sk_buff *skb,
						 struct net_device *ndev)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct cppi5_host_desc_t *first_desc, *next_desc, *cur_desc;
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	struct am65_cpsw_tx_swdata *swdata;
	struct device *dev = common->dev;
	struct am65_cpsw_tx_chn *tx_chn;
	struct netdev_queue *netif_txq;
	dma_addr_t desc_dma, buf_dma;
	int ret, q_idx, i;
	u32 *psdata;
	u32 pkt_len;

	/* padding enabled in hw */
	pkt_len = skb_headlen(skb);

	/* SKB TX timestamp */
	if (port->tx_ts_enabled)
		am65_cpts_prep_tx_timestamp(common->cpts, skb);

	q_idx = skb_get_queue_mapping(skb);
	dev_dbg(dev, "%s skb_queue:%d\n", __func__, q_idx);

	tx_chn = &common->tx_chns[q_idx];
	netif_txq = netdev_get_tx_queue(ndev, q_idx);

	/* Map the linear buffer */
	buf_dma = dma_map_single(tx_chn->dma_dev, skb->data, pkt_len,
				 DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(tx_chn->dma_dev, buf_dma))) {
		dev_err(dev, "Failed to map tx skb buffer\n");
		ndev->stats.tx_errors++;
		goto err_free_skb;
	}

	first_desc = k3_cppi_desc_pool_alloc(tx_chn->desc_pool);
	if (!first_desc) {
		dev_dbg(dev, "Failed to allocate descriptor\n");
		dma_unmap_single(tx_chn->dma_dev, buf_dma, pkt_len,
				 DMA_TO_DEVICE);
		goto busy_stop_q;
	}

	am65_cpsw_nuss_set_buf_type(tx_chn, first_desc,
				    AM65_CPSW_TX_BUF_TYPE_SKB);

	cppi5_hdesc_init(first_desc, CPPI5_INFO0_HDESC_EPIB_PRESENT,
			 AM65_CPSW_NAV_PS_DATA_SIZE);
	cppi5_desc_set_pktids(&first_desc->hdr, 0, AM65_CPSW_CPPI_TX_FLOW_ID);
	cppi5_hdesc_set_pkttype(first_desc, AM65_CPSW_CPPI_TX_PKT_TYPE);
	cppi5_desc_set_tags_ids(&first_desc->hdr, 0, port->port_id);

	k3_udma_glue_tx_dma_to_cppi5_addr(tx_chn->tx_chn, &buf_dma);
	cppi5_hdesc_attach_buf(first_desc, buf_dma, pkt_len, buf_dma, pkt_len);
	swdata = cppi5_hdesc_get_swdata(first_desc);
	swdata->ndev = ndev;
	swdata->skb = skb;
	psdata = cppi5_hdesc_get_psdata(first_desc);

	/* HW csum offload if enabled */
	psdata[2] = 0;
	if (likely(skb->ip_summed == CHECKSUM_PARTIAL)) {
		unsigned int cs_start, cs_offset;

		cs_start = skb_transport_offset(skb);
		cs_offset = cs_start + skb->csum_offset;
		/* HW numerates bytes starting from 1 */
		psdata[2] = ((cs_offset + 1) << 24) |
			    ((cs_start + 1) << 16) | (skb->len - cs_start);
		dev_dbg(dev, "%s tx psdata:%#x\n", __func__, psdata[2]);
	}

	if (!skb_is_nonlinear(skb))
		goto done_tx;

	dev_dbg(dev, "fragmented SKB\n");

	/* Handle the case where skb is fragmented in pages */
	cur_desc = first_desc;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 frag_size = skb_frag_size(frag);

		next_desc = k3_cppi_desc_pool_alloc(tx_chn->desc_pool);
		if (!next_desc) {
			dev_err(dev, "Failed to allocate descriptor\n");
			goto busy_free_descs;
		}

		am65_cpsw_nuss_set_buf_type(tx_chn, next_desc,
					    AM65_CPSW_TX_BUF_TYPE_SKB);

		buf_dma = skb_frag_dma_map(tx_chn->dma_dev, frag, 0, frag_size,
					   DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(tx_chn->dma_dev, buf_dma))) {
			dev_err(dev, "Failed to map tx skb page\n");
			k3_cppi_desc_pool_free(tx_chn->desc_pool, next_desc);
			ndev->stats.tx_errors++;
			goto err_free_descs;
		}

		cppi5_hdesc_reset_hbdesc(next_desc);
		k3_udma_glue_tx_dma_to_cppi5_addr(tx_chn->tx_chn, &buf_dma);
		cppi5_hdesc_attach_buf(next_desc,
				       buf_dma, frag_size, buf_dma, frag_size);

		desc_dma = k3_cppi_desc_pool_virt2dma(tx_chn->desc_pool,
						      next_desc);
		k3_udma_glue_tx_dma_to_cppi5_addr(tx_chn->tx_chn, &desc_dma);
		cppi5_hdesc_link_hbdesc(cur_desc, desc_dma);

		pkt_len += frag_size;
		cur_desc = next_desc;
	}
	WARN_ON(pkt_len != skb->len);

done_tx:
	skb_tx_timestamp(skb);

	/* report bql before sending packet */
	netdev_tx_sent_queue(netif_txq, pkt_len);

	cppi5_hdesc_set_pktlen(first_desc, pkt_len);
	desc_dma = k3_cppi_desc_pool_virt2dma(tx_chn->desc_pool, first_desc);
	if (AM65_CPSW_IS_CPSW2G(common)) {
		ret = k3_udma_glue_push_tx_chn(tx_chn->tx_chn, first_desc, desc_dma);
	} else {
		spin_lock_bh(&tx_chn->lock);
		ret = k3_udma_glue_push_tx_chn(tx_chn->tx_chn, first_desc, desc_dma);
		spin_unlock_bh(&tx_chn->lock);
	}
	if (ret) {
		dev_err(dev, "can't push desc %d\n", ret);
		/* inform bql */
		netdev_tx_completed_queue(netif_txq, 1, pkt_len);
		ndev->stats.tx_errors++;
		goto err_free_descs;
	}

	if (k3_cppi_desc_pool_avail(tx_chn->desc_pool) < MAX_SKB_FRAGS) {
		netif_tx_stop_queue(netif_txq);
		/* Barrier, so that stop_queue visible to other cpus */
		smp_mb__after_atomic();
		dev_dbg(dev, "netif_tx_stop_queue %d\n", q_idx);

		/* re-check for smp */
		if (k3_cppi_desc_pool_avail(tx_chn->desc_pool) >=
		    MAX_SKB_FRAGS) {
			netif_tx_wake_queue(netif_txq);
			dev_dbg(dev, "netif_tx_wake_queue %d\n", q_idx);
		}
	}

	return NETDEV_TX_OK;

err_free_descs:
	am65_cpsw_nuss_xmit_free(tx_chn, first_desc);
err_free_skb:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;

busy_free_descs:
	am65_cpsw_nuss_xmit_free(tx_chn, first_desc);
busy_stop_q:
	netif_tx_stop_queue(netif_txq);
	return NETDEV_TX_BUSY;
}

static int am65_cpsw_nuss_ndo_slave_set_mac_address(struct net_device *ndev,
						    void *addr)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	struct sockaddr *sockaddr = (struct sockaddr *)addr;
	int ret;

	ret = eth_prepare_mac_addr_change(ndev, addr);
	if (ret < 0)
		return ret;

	ret = pm_runtime_resume_and_get(common->dev);
	if (ret < 0)
		return ret;

	cpsw_ale_del_ucast(common->ale, ndev->dev_addr,
			   HOST_PORT_NUM, 0, 0);
	cpsw_ale_add_ucast(common->ale, sockaddr->sa_data,
			   HOST_PORT_NUM, ALE_SECURE, 0);

	am65_cpsw_port_set_sl_mac(port, addr);
	eth_commit_mac_addr_change(ndev, sockaddr);

	pm_runtime_put(common->dev);

	return 0;
}

static int am65_cpsw_nuss_hwtstamp_set(struct net_device *ndev,
				       struct ifreq *ifr)
{
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	u32 ts_ctrl, seq_id, ts_ctrl_ltype2, ts_vlan_ltype;
	struct hwtstamp_config cfg;

	if (!IS_ENABLED(CONFIG_TI_K3_AM65_CPTS))
		return -EOPNOTSUPP;

	if (copy_from_user(&cfg, ifr->ifr_data, sizeof(cfg)))
		return -EFAULT;

	/* TX HW timestamp */
	switch (cfg.tx_type) {
	case HWTSTAMP_TX_OFF:
	case HWTSTAMP_TX_ON:
		break;
	default:
		return -ERANGE;
	}

	switch (cfg.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		port->rx_ts_enabled = false;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		port->rx_ts_enabled = true;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_SOME:
	case HWTSTAMP_FILTER_NTP_ALL:
		return -EOPNOTSUPP;
	default:
		return -ERANGE;
	}

	port->tx_ts_enabled = (cfg.tx_type == HWTSTAMP_TX_ON);

	/* cfg TX timestamp */
	seq_id = (AM65_CPSW_TS_SEQ_ID_OFFSET <<
		  AM65_CPSW_PN_TS_SEQ_ID_OFFSET_SHIFT) | ETH_P_1588;

	ts_vlan_ltype = ETH_P_8021Q;

	ts_ctrl_ltype2 = ETH_P_1588 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_107 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_129 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_130 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_131 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_132 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_319 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_320 |
			 AM65_CPSW_PN_TS_CTL_LTYPE2_TS_TTL_NONZERO;

	ts_ctrl = AM65_CPSW_TS_EVENT_MSG_TYPE_BITS <<
		  AM65_CPSW_PN_TS_CTL_MSG_TYPE_EN_SHIFT;

	if (port->tx_ts_enabled)
		ts_ctrl |= AM65_CPSW_TS_TX_ANX_ALL_EN |
			   AM65_CPSW_PN_TS_CTL_TX_VLAN_LT1_EN;

	if (port->rx_ts_enabled)
		ts_ctrl |= AM65_CPSW_TS_RX_ANX_ALL_EN |
			   AM65_CPSW_PN_TS_CTL_RX_VLAN_LT1_EN;

	writel(seq_id, port->port_base + AM65_CPSW_PORTN_REG_TS_SEQ_LTYPE_REG);
	writel(ts_vlan_ltype, port->port_base +
	       AM65_CPSW_PORTN_REG_TS_VLAN_LTYPE_REG);
	writel(ts_ctrl_ltype2, port->port_base +
	       AM65_CPSW_PORTN_REG_TS_CTL_LTYPE2);
	writel(ts_ctrl, port->port_base + AM65_CPSW_PORTN_REG_TS_CTL);

	return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static int am65_cpsw_nuss_hwtstamp_get(struct net_device *ndev,
				       struct ifreq *ifr)
{
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	struct hwtstamp_config cfg;

	if (!IS_ENABLED(CONFIG_TI_K3_AM65_CPTS))
		return -EOPNOTSUPP;

	cfg.flags = 0;
	cfg.tx_type = port->tx_ts_enabled ?
		      HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF;
	cfg.rx_filter = port->rx_ts_enabled ?
			HWTSTAMP_FILTER_PTP_V2_EVENT : HWTSTAMP_FILTER_NONE;

	return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static int am65_cpsw_nuss_ndo_slave_ioctl(struct net_device *ndev,
					  struct ifreq *req, int cmd)
{
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);

	if (!netif_running(ndev))
		return -EINVAL;

	switch (cmd) {
	case SIOCSHWTSTAMP:
		return am65_cpsw_nuss_hwtstamp_set(ndev, req);
	case SIOCGHWTSTAMP:
		return am65_cpsw_nuss_hwtstamp_get(ndev, req);
	}

	return phylink_mii_ioctl(port->slave.phylink, req, cmd);
}

static void am65_cpsw_nuss_ndo_get_stats(struct net_device *dev,
					 struct rtnl_link_stats64 *stats)
{
	dev_fetch_sw_netstats(stats, dev->tstats);

	stats->rx_errors	= dev->stats.rx_errors;
	stats->rx_dropped	= dev->stats.rx_dropped;
	stats->tx_dropped	= dev->stats.tx_dropped;
}

static int am65_cpsw_xdp_prog_setup(struct net_device *ndev,
				    struct bpf_prog *prog)
{
	struct am65_cpsw_port *port = am65_ndev_to_port(ndev);
	bool running = netif_running(ndev);
	struct bpf_prog *old_prog;

	if (running)
		am65_cpsw_nuss_ndo_slave_stop(ndev);

	old_prog = xchg(&port->xdp_prog, prog);
	if (old_prog)
		bpf_prog_put(old_prog);

	if (running)
		return am65_cpsw_nuss_ndo_slave_open(ndev);

	return 0;
}

static int am65_cpsw_ndo_bpf(struct net_device *ndev, struct netdev_bpf *bpf)
{
	switch (bpf->command) {
	case XDP_SETUP_PROG:
		return am65_cpsw_xdp_prog_setup(ndev, bpf->prog);
	default:
		return -EINVAL;
	}
}

static int am65_cpsw_ndo_xdp_xmit(struct net_device *ndev, int n,
				  struct xdp_frame **frames, u32 flags)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_tx_chn *tx_chn;
	struct netdev_queue *netif_txq;
	int cpu = smp_processor_id();
	int i, nxmit = 0;

	tx_chn = &common->tx_chns[cpu % common->tx_ch_num];
	netif_txq = netdev_get_tx_queue(ndev, tx_chn->id);

	__netif_tx_lock(netif_txq, cpu);
	for (i = 0; i < n; i++) {
		if (am65_cpsw_xdp_tx_frame(ndev, tx_chn, frames[i],
					   AM65_CPSW_TX_BUF_TYPE_XDP_NDO))
			break;
		nxmit++;
	}
	__netif_tx_unlock(netif_txq);

	return nxmit;
}

static const struct net_device_ops am65_cpsw_nuss_netdev_ops = {
	.ndo_open		= am65_cpsw_nuss_ndo_slave_open,
	.ndo_stop		= am65_cpsw_nuss_ndo_slave_stop,
	.ndo_start_xmit		= am65_cpsw_nuss_ndo_slave_xmit,
	.ndo_set_rx_mode	= am65_cpsw_nuss_ndo_slave_set_rx_mode,
	.ndo_get_stats64        = am65_cpsw_nuss_ndo_get_stats,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= am65_cpsw_nuss_ndo_slave_set_mac_address,
	.ndo_tx_timeout		= am65_cpsw_nuss_ndo_host_tx_timeout,
	.ndo_vlan_rx_add_vid	= am65_cpsw_nuss_ndo_slave_add_vid,
	.ndo_vlan_rx_kill_vid	= am65_cpsw_nuss_ndo_slave_kill_vid,
	.ndo_eth_ioctl		= am65_cpsw_nuss_ndo_slave_ioctl,
	.ndo_setup_tc           = am65_cpsw_qos_ndo_setup_tc,
	.ndo_set_tx_maxrate	= am65_cpsw_qos_ndo_tx_p0_set_maxrate,
	.ndo_bpf		= am65_cpsw_ndo_bpf,
	.ndo_xdp_xmit		= am65_cpsw_ndo_xdp_xmit,
};

static void am65_cpsw_disable_phy(struct phy *phy)
{
	phy_power_off(phy);
	phy_exit(phy);
}

static int am65_cpsw_enable_phy(struct phy *phy)
{
	int ret;

	ret = phy_init(phy);
	if (ret < 0)
		return ret;

	ret = phy_power_on(phy);
	if (ret < 0) {
		phy_exit(phy);
		return ret;
	}

	return 0;
}

static void am65_cpsw_disable_serdes_phy(struct am65_cpsw_common *common)
{
	struct am65_cpsw_port *port;
	struct phy *phy;
	int i;

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		phy = port->slave.serdes_phy;
		if (phy)
			am65_cpsw_disable_phy(phy);
	}
}

static int am65_cpsw_init_serdes_phy(struct device *dev, struct device_node *port_np,
				     struct am65_cpsw_port *port)
{
	const char *name = "serdes";
	struct phy *phy;
	int ret;

	phy = devm_of_phy_optional_get(dev, port_np, name);
	if (IS_ERR_OR_NULL(phy))
		return PTR_ERR_OR_ZERO(phy);

	/* Serdes PHY exists. Store it. */
	port->slave.serdes_phy = phy;

	ret =  am65_cpsw_enable_phy(phy);
	if (ret < 0)
		goto err_phy;

	return 0;

err_phy:
	devm_phy_put(dev, phy);
	return ret;
}

static void am65_cpsw_nuss_mac_config(struct phylink_config *config, unsigned int mode,
				      const struct phylink_link_state *state)
{
	struct am65_cpsw_slave_data *slave = container_of(config, struct am65_cpsw_slave_data,
							  phylink_config);
	struct am65_cpsw_port *port = container_of(slave, struct am65_cpsw_port, slave);
	struct am65_cpsw_common *common = port->common;

	if (common->pdata.extra_modes & BIT(state->interface)) {
		if (state->interface == PHY_INTERFACE_MODE_SGMII) {
			writel(ADVERTISE_SGMII,
			       port->sgmii_base + AM65_CPSW_SGMII_MR_ADV_ABILITY_REG);
			cpsw_sl_ctl_set(port->slave.mac_sl, CPSW_SL_CTL_EXT_EN);
		} else {
			cpsw_sl_ctl_clr(port->slave.mac_sl, CPSW_SL_CTL_EXT_EN);
		}

		if (state->interface == PHY_INTERFACE_MODE_USXGMII) {
			cpsw_sl_ctl_set(port->slave.mac_sl,
					CPSW_SL_CTL_XGIG | CPSW_SL_CTL_XGMII_EN);
		} else {
			cpsw_sl_ctl_clr(port->slave.mac_sl,
					CPSW_SL_CTL_XGIG | CPSW_SL_CTL_XGMII_EN);
		}

		writel(AM65_CPSW_SGMII_CONTROL_MR_AN_ENABLE,
		       port->sgmii_base + AM65_CPSW_SGMII_CONTROL_REG);
	}
}

static void am65_cpsw_nuss_mac_link_down(struct phylink_config *config, unsigned int mode,
					 phy_interface_t interface)
{
	struct am65_cpsw_slave_data *slave = container_of(config, struct am65_cpsw_slave_data,
							  phylink_config);
	struct am65_cpsw_port *port = container_of(slave, struct am65_cpsw_port, slave);
	struct am65_cpsw_common *common = port->common;
	struct net_device *ndev = port->ndev;
	u32 mac_control;
	int tmo;

	/* disable forwarding */
	cpsw_ale_control_set(common->ale, port->port_id, ALE_PORT_STATE, ALE_PORT_STATE_DISABLE);

	cpsw_sl_ctl_set(port->slave.mac_sl, CPSW_SL_CTL_CMD_IDLE);

	tmo = cpsw_sl_wait_for_idle(port->slave.mac_sl, 100);
	dev_dbg(common->dev, "down msc_sl %08x tmo %d\n",
		cpsw_sl_reg_read(port->slave.mac_sl, CPSW_SL_MACSTATUS), tmo);

	/* All the bits that am65_cpsw_nuss_mac_link_up() can possibly set */
	mac_control = CPSW_SL_CTL_GMII_EN | CPSW_SL_CTL_GIG | CPSW_SL_CTL_IFCTL_A |
		      CPSW_SL_CTL_FULLDUPLEX | CPSW_SL_CTL_RX_FLOW_EN | CPSW_SL_CTL_TX_FLOW_EN;
	/* If interface mode is RGMII, CPSW_SL_CTL_EXT_EN might have been set for 10 Mbps */
	if (phy_interface_mode_is_rgmii(interface))
		mac_control |= CPSW_SL_CTL_EXT_EN;
	/* Only clear those bits that can be set by am65_cpsw_nuss_mac_link_up() */
	cpsw_sl_ctl_clr(port->slave.mac_sl, mac_control);

	am65_cpsw_qos_link_down(ndev);
	netif_tx_stop_all_queues(ndev);
}

static void am65_cpsw_nuss_mac_link_up(struct phylink_config *config, struct phy_device *phy,
				       unsigned int mode, phy_interface_t interface, int speed,
				       int duplex, bool tx_pause, bool rx_pause)
{
	struct am65_cpsw_slave_data *slave = container_of(config, struct am65_cpsw_slave_data,
							  phylink_config);
	struct am65_cpsw_port *port = container_of(slave, struct am65_cpsw_port, slave);
	struct am65_cpsw_common *common = port->common;
	u32 mac_control = CPSW_SL_CTL_GMII_EN;
	struct net_device *ndev = port->ndev;

	/* Bring the port out of idle state */
	cpsw_sl_ctl_clr(port->slave.mac_sl, CPSW_SL_CTL_CMD_IDLE);

	if (speed == SPEED_1000)
		mac_control |= CPSW_SL_CTL_GIG;
	/* TODO: Verify whether in-band is necessary for 10 Mbps RGMII */
	if (speed == SPEED_10 && phy_interface_mode_is_rgmii(interface))
		/* Can be used with in band mode only */
		mac_control |= CPSW_SL_CTL_EXT_EN;
	if (speed == SPEED_100 && interface == PHY_INTERFACE_MODE_RMII)
		mac_control |= CPSW_SL_CTL_IFCTL_A;
	if (duplex)
		mac_control |= CPSW_SL_CTL_FULLDUPLEX;

	/* rx_pause/tx_pause */
	if (rx_pause)
		mac_control |= CPSW_SL_CTL_TX_FLOW_EN;

	if (tx_pause)
		mac_control |= CPSW_SL_CTL_RX_FLOW_EN;

	cpsw_sl_ctl_set(port->slave.mac_sl, mac_control);

	/* enable forwarding */
	cpsw_ale_control_set(common->ale, port->port_id, ALE_PORT_STATE, ALE_PORT_STATE_FORWARD);

	am65_cpsw_qos_link_up(ndev, speed);
	netif_tx_wake_all_queues(ndev);
}

static const struct phylink_mac_ops am65_cpsw_phylink_mac_ops = {
	.mac_config = am65_cpsw_nuss_mac_config,
	.mac_link_down = am65_cpsw_nuss_mac_link_down,
	.mac_link_up = am65_cpsw_nuss_mac_link_up,
};

static void am65_cpsw_nuss_slave_disable_unused(struct am65_cpsw_port *port)
{
	struct am65_cpsw_common *common = port->common;

	if (!port->disabled)
		return;

	cpsw_ale_control_set(common->ale, port->port_id,
			     ALE_PORT_STATE, ALE_PORT_STATE_DISABLE);

	cpsw_sl_reset(port->slave.mac_sl, 100);
	cpsw_sl_ctl_reset(port->slave.mac_sl);
}

static void am65_cpsw_nuss_free_tx_chns(void *data)
{
	struct am65_cpsw_common *common = data;
	int i;

	for (i = 0; i < common->tx_ch_num; i++) {
		struct am65_cpsw_tx_chn *tx_chn = &common->tx_chns[i];

		if (!IS_ERR_OR_NULL(tx_chn->desc_pool))
			k3_cppi_desc_pool_destroy(tx_chn->desc_pool);

		if (!IS_ERR_OR_NULL(tx_chn->tx_chn))
			k3_udma_glue_release_tx_chn(tx_chn->tx_chn);

		memset(tx_chn, 0, sizeof(*tx_chn));
	}
}

static void am65_cpsw_nuss_remove_tx_chns(struct am65_cpsw_common *common)
{
	struct device *dev = common->dev;
	int i;

	common->tx_ch_rate_msk = 0;
	for (i = 0; i < common->tx_ch_num; i++) {
		struct am65_cpsw_tx_chn *tx_chn = &common->tx_chns[i];

		if (tx_chn->irq > 0)
			devm_free_irq(dev, tx_chn->irq, tx_chn);

		netif_napi_del(&tx_chn->napi_tx);
	}

	am65_cpsw_nuss_free_tx_chns(common);
}

static int am65_cpsw_nuss_ndev_add_tx_napi(struct am65_cpsw_common *common)
{
	struct device *dev = common->dev;
	struct am65_cpsw_tx_chn *tx_chn;
	int i, ret = 0;

	for (i = 0; i < common->tx_ch_num; i++) {
		tx_chn = &common->tx_chns[i];

		hrtimer_setup(&tx_chn->tx_hrtimer, &am65_cpsw_nuss_tx_timer_callback,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);

		netif_napi_add_tx(common->dma_ndev, &tx_chn->napi_tx,
				  am65_cpsw_nuss_tx_poll);

		ret = devm_request_irq(dev, tx_chn->irq,
				       am65_cpsw_nuss_tx_irq,
				       IRQF_TRIGGER_HIGH,
				       tx_chn->tx_chn_name, tx_chn);
		if (ret) {
			dev_err(dev, "failure requesting tx%u irq %u, %d\n",
				tx_chn->id, tx_chn->irq, ret);
			goto err;
		}
	}

	return 0;

err:
	netif_napi_del(&tx_chn->napi_tx);
	for (--i; i >= 0; i--) {
		tx_chn = &common->tx_chns[i];
		devm_free_irq(dev, tx_chn->irq, tx_chn);
		netif_napi_del(&tx_chn->napi_tx);
	}

	return ret;
}

static int am65_cpsw_nuss_init_tx_chns(struct am65_cpsw_common *common)
{
	u32  max_desc_num = ALIGN(AM65_CPSW_MAX_TX_DESC, MAX_SKB_FRAGS);
	struct k3_udma_glue_tx_channel_cfg tx_cfg = { 0 };
	struct device *dev = common->dev;
	struct k3_ring_cfg ring_cfg = {
		.elm_size = K3_RINGACC_RING_ELSIZE_8,
		.mode = K3_RINGACC_RING_MODE_RING,
		.flags = 0
	};
	u32 hdesc_size, hdesc_size_out;
	int i, ret = 0;

	hdesc_size = cppi5_hdesc_calc_size(true, AM65_CPSW_NAV_PS_DATA_SIZE,
					   AM65_CPSW_NAV_SW_DATA_SIZE);

	tx_cfg.swdata_size = AM65_CPSW_NAV_SW_DATA_SIZE;
	tx_cfg.tx_cfg = ring_cfg;
	tx_cfg.txcq_cfg = ring_cfg;
	tx_cfg.tx_cfg.size = max_desc_num;
	tx_cfg.txcq_cfg.size = max_desc_num;

	for (i = 0; i < common->tx_ch_num; i++) {
		struct am65_cpsw_tx_chn *tx_chn = &common->tx_chns[i];

		snprintf(tx_chn->tx_chn_name,
			 sizeof(tx_chn->tx_chn_name), "tx%d", i);

		spin_lock_init(&tx_chn->lock);
		tx_chn->common = common;
		tx_chn->id = i;
		tx_chn->descs_num = max_desc_num;

		tx_chn->tx_chn =
			k3_udma_glue_request_tx_chn(dev,
						    tx_chn->tx_chn_name,
						    &tx_cfg);
		if (IS_ERR(tx_chn->tx_chn)) {
			ret = dev_err_probe(dev, PTR_ERR(tx_chn->tx_chn),
					    "Failed to request tx dma channel\n");
			goto err;
		}
		tx_chn->dma_dev = k3_udma_glue_tx_get_dma_device(tx_chn->tx_chn);

		tx_chn->desc_pool = k3_cppi_desc_pool_create_name(tx_chn->dma_dev,
								  tx_chn->descs_num,
								  hdesc_size,
								  tx_chn->tx_chn_name);
		if (IS_ERR(tx_chn->desc_pool)) {
			ret = PTR_ERR(tx_chn->desc_pool);
			dev_err(dev, "Failed to create poll %d\n", ret);
			goto err;
		}

		hdesc_size_out = k3_cppi_desc_pool_desc_size(tx_chn->desc_pool);
		tx_chn->dsize_log2 = __fls(hdesc_size_out);
		WARN_ON(hdesc_size_out != (1 << tx_chn->dsize_log2));

		tx_chn->irq = k3_udma_glue_tx_get_irq(tx_chn->tx_chn);
		if (tx_chn->irq < 0) {
			dev_err(dev, "Failed to get tx dma irq %d\n",
				tx_chn->irq);
			ret = tx_chn->irq;
			goto err;
		}

		snprintf(tx_chn->tx_chn_name,
			 sizeof(tx_chn->tx_chn_name), "%s-tx%d",
			 dev_name(dev), tx_chn->id);
	}

	ret = am65_cpsw_nuss_ndev_add_tx_napi(common);
	if (ret) {
		dev_err(dev, "Failed to add tx NAPI %d\n", ret);
		goto err;
	}

	return 0;

err:
	am65_cpsw_nuss_free_tx_chns(common);

	return ret;
}

static void am65_cpsw_nuss_free_rx_chns(void *data)
{
	struct am65_cpsw_common *common = data;
	struct am65_cpsw_rx_chn *rx_chn;

	rx_chn = &common->rx_chns;

	if (!IS_ERR_OR_NULL(rx_chn->desc_pool))
		k3_cppi_desc_pool_destroy(rx_chn->desc_pool);

	if (!IS_ERR_OR_NULL(rx_chn->rx_chn))
		k3_udma_glue_release_rx_chn(rx_chn->rx_chn);
}

static void am65_cpsw_nuss_remove_rx_chns(struct am65_cpsw_common *common)
{
	struct device *dev = common->dev;
	struct am65_cpsw_rx_chn *rx_chn;
	struct am65_cpsw_rx_flow *flows;
	int i;

	rx_chn = &common->rx_chns;
	flows = rx_chn->flows;

	for (i = 0; i < common->rx_ch_num_flows; i++) {
		if (!(flows[i].irq < 0))
			devm_free_irq(dev, flows[i].irq, &flows[i]);
		netif_napi_del(&flows[i].napi_rx);
	}

	am65_cpsw_nuss_free_rx_chns(common);

	common->rx_flow_id_base = -1;
}

static int am65_cpsw_nuss_init_rx_chns(struct am65_cpsw_common *common)
{
	struct am65_cpsw_rx_chn *rx_chn = &common->rx_chns;
	struct k3_udma_glue_rx_channel_cfg rx_cfg = { 0 };
	u32  max_desc_num = AM65_CPSW_MAX_RX_DESC;
	struct device *dev = common->dev;
	struct am65_cpsw_rx_flow *flow;
	u32 hdesc_size, hdesc_size_out;
	u32 fdqring_id;
	int i, ret = 0;

	hdesc_size = cppi5_hdesc_calc_size(true, AM65_CPSW_NAV_PS_DATA_SIZE,
					   AM65_CPSW_NAV_SW_DATA_SIZE);

	rx_cfg.swdata_size = AM65_CPSW_NAV_SW_DATA_SIZE;
	rx_cfg.flow_id_num = common->rx_ch_num_flows;
	rx_cfg.flow_id_base = common->rx_flow_id_base;

	/* init all flows */
	rx_chn->dev = dev;
	rx_chn->descs_num = max_desc_num * rx_cfg.flow_id_num;

	for (i = 0; i < common->rx_ch_num_flows; i++) {
		flow = &rx_chn->flows[i];
		flow->page_pool = NULL;
	}

	rx_chn->rx_chn = k3_udma_glue_request_rx_chn(dev, "rx", &rx_cfg);
	if (IS_ERR(rx_chn->rx_chn)) {
		ret = dev_err_probe(dev, PTR_ERR(rx_chn->rx_chn),
				    "Failed to request rx dma channel\n");
		goto err;
	}
	rx_chn->dma_dev = k3_udma_glue_rx_get_dma_device(rx_chn->rx_chn);

	rx_chn->desc_pool = k3_cppi_desc_pool_create_name(rx_chn->dma_dev,
							  rx_chn->descs_num,
							  hdesc_size, "rx");
	if (IS_ERR(rx_chn->desc_pool)) {
		ret = PTR_ERR(rx_chn->desc_pool);
		dev_err(dev, "Failed to create rx poll %d\n", ret);
		goto err;
	}

	hdesc_size_out = k3_cppi_desc_pool_desc_size(rx_chn->desc_pool);
	rx_chn->dsize_log2 = __fls(hdesc_size_out);
	WARN_ON(hdesc_size_out != (1 << rx_chn->dsize_log2));

	common->rx_flow_id_base =
			k3_udma_glue_rx_get_flow_id_base(rx_chn->rx_chn);
	dev_info(dev, "set new flow-id-base %u\n", common->rx_flow_id_base);

	fdqring_id = K3_RINGACC_RING_ID_ANY;
	for (i = 0; i < rx_cfg.flow_id_num; i++) {
		struct k3_ring_cfg rxring_cfg = {
			.elm_size = K3_RINGACC_RING_ELSIZE_8,
			.mode = K3_RINGACC_RING_MODE_RING,
			.flags = 0,
		};
		struct k3_ring_cfg fdqring_cfg = {
			.elm_size = K3_RINGACC_RING_ELSIZE_8,
			.flags = K3_RINGACC_RING_SHARED,
		};
		struct k3_udma_glue_rx_flow_cfg rx_flow_cfg = {
			.rx_cfg = rxring_cfg,
			.rxfdq_cfg = fdqring_cfg,
			.ring_rxq_id = K3_RINGACC_RING_ID_ANY,
			.src_tag_lo_sel =
				K3_UDMA_GLUE_SRC_TAG_LO_USE_REMOTE_SRC_TAG,
		};

		flow = &rx_chn->flows[i];
		flow->id = i;
		flow->common = common;
		flow->irq = -EINVAL;

		rx_flow_cfg.ring_rxfdq0_id = fdqring_id;
		rx_flow_cfg.rx_cfg.size = max_desc_num;
		/* share same FDQ for all flows */
		rx_flow_cfg.rxfdq_cfg.size = max_desc_num * rx_cfg.flow_id_num;
		rx_flow_cfg.rxfdq_cfg.mode = common->pdata.fdqring_mode;

		ret = k3_udma_glue_rx_flow_init(rx_chn->rx_chn,
						i, &rx_flow_cfg);
		if (ret) {
			dev_err(dev, "Failed to init rx flow%d %d\n", i, ret);
			goto err_flow;
		}
		if (!i)
			fdqring_id =
				k3_udma_glue_rx_flow_get_fdq_id(rx_chn->rx_chn,
								i);

		flow->irq = k3_udma_glue_rx_get_irq(rx_chn->rx_chn, i);
		if (flow->irq <= 0) {
			dev_err(dev, "Failed to get rx dma irq %d\n",
				flow->irq);
			ret = flow->irq;
			goto err_flow;
		}

		snprintf(flow->name,
			 sizeof(flow->name), "%s-rx%d",
			 dev_name(dev), i);
		hrtimer_setup(&flow->rx_hrtimer, &am65_cpsw_nuss_rx_timer_callback, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_PINNED);

		netif_napi_add(common->dma_ndev, &flow->napi_rx,
			       am65_cpsw_nuss_rx_poll);

		ret = devm_request_irq(dev, flow->irq,
				       am65_cpsw_nuss_rx_irq,
				       IRQF_TRIGGER_HIGH,
				       flow->name, flow);
		if (ret) {
			dev_err(dev, "failure requesting rx %d irq %u, %d\n",
				i, flow->irq, ret);
			flow->irq = -EINVAL;
			goto err_request_irq;
		}
	}

	/* setup classifier to route priorities to flows */
	cpsw_ale_classifier_setup_default(common->ale, common->rx_ch_num_flows);

	return 0;

err_request_irq:
	netif_napi_del(&flow->napi_rx);

err_flow:
	for (--i; i >= 0; i--) {
		flow = &rx_chn->flows[i];
		devm_free_irq(dev, flow->irq, flow);
		netif_napi_del(&flow->napi_rx);
	}

err:
	am65_cpsw_nuss_free_rx_chns(common);

	return ret;
}

static int am65_cpsw_nuss_init_host_p(struct am65_cpsw_common *common)
{
	struct am65_cpsw_host *host_p = am65_common_get_host(common);

	host_p->common = common;
	host_p->port_base = common->cpsw_base + AM65_CPSW_NU_PORTS_BASE;
	host_p->stat_base = common->cpsw_base + AM65_CPSW_NU_STATS_BASE;

	return 0;
}

static int am65_cpsw_am654_get_efuse_macid(struct device_node *of_node,
					   int slave, u8 *mac_addr)
{
	u32 mac_lo, mac_hi, offset;
	struct regmap *syscon;

	syscon = syscon_regmap_lookup_by_phandle_args(of_node, "ti,syscon-efuse",
						      1, &offset);
	if (IS_ERR(syscon)) {
		if (PTR_ERR(syscon) == -ENODEV)
			return 0;
		return PTR_ERR(syscon);
	}

	regmap_read(syscon, offset, &mac_lo);
	regmap_read(syscon, offset + 4, &mac_hi);

	mac_addr[0] = (mac_hi >> 8) & 0xff;
	mac_addr[1] = mac_hi & 0xff;
	mac_addr[2] = (mac_lo >> 24) & 0xff;
	mac_addr[3] = (mac_lo >> 16) & 0xff;
	mac_addr[4] = (mac_lo >> 8) & 0xff;
	mac_addr[5] = mac_lo & 0xff;

	return 0;
}

static int am65_cpsw_init_cpts(struct am65_cpsw_common *common)
{
	struct device *dev = common->dev;
	struct device_node *node;
	struct am65_cpts *cpts;
	void __iomem *reg_base;

	if (!IS_ENABLED(CONFIG_TI_K3_AM65_CPTS))
		return 0;

	node = of_get_child_by_name(dev->of_node, "cpts");
	if (!node) {
		dev_err(dev, "%s cpts not found\n", __func__);
		return -ENOENT;
	}

	reg_base = common->cpsw_base + AM65_CPSW_NU_CPTS_BASE;
	cpts = am65_cpts_create(dev, reg_base, node);
	if (IS_ERR(cpts)) {
		int ret = PTR_ERR(cpts);

		of_node_put(node);
		dev_err(dev, "cpts create err %d\n", ret);
		return ret;
	}
	common->cpts = cpts;
	/* Forbid PM runtime if CPTS is running.
	 * K3 CPSWxG modules may completely lose context during ON->OFF
	 * transitions depending on integration.
	 * AM65x/J721E MCU CPSW2G: false
	 * J721E MAIN_CPSW9G: true
	 */
	pm_runtime_forbid(dev);

	return 0;
}

static int am65_cpsw_nuss_init_slave_ports(struct am65_cpsw_common *common)
{
	struct device_node *node, *port_np;
	struct device *dev = common->dev;
	int ret;

	node = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!node)
		return -ENOENT;

	for_each_child_of_node(node, port_np) {
		struct am65_cpsw_port *port;
		u32 port_id;

		/* it is not a slave port node, continue */
		if (strcmp(port_np->name, "port"))
			continue;

		ret = of_property_read_u32(port_np, "reg", &port_id);
		if (ret < 0) {
			dev_err(dev, "%pOF error reading port_id %d\n",
				port_np, ret);
			goto of_node_put;
		}

		if (!port_id || port_id > common->port_num) {
			dev_err(dev, "%pOF has invalid port_id %u %s\n",
				port_np, port_id, port_np->name);
			ret = -EINVAL;
			goto of_node_put;
		}

		port = am65_common_get_port(common, port_id);
		port->port_id = port_id;
		port->common = common;
		port->port_base = common->cpsw_base + AM65_CPSW_NU_PORTS_BASE +
				  AM65_CPSW_NU_PORTS_OFFSET * (port_id);
		if (common->pdata.extra_modes)
			port->sgmii_base = common->ss_base + AM65_CPSW_SGMII_BASE * (port_id);
		port->stat_base = common->cpsw_base + AM65_CPSW_NU_STATS_BASE +
				  (AM65_CPSW_NU_STATS_PORT_OFFSET * port_id);
		port->name = of_get_property(port_np, "label", NULL);
		port->fetch_ram_base =
				common->cpsw_base + AM65_CPSW_NU_FRAM_BASE +
				(AM65_CPSW_NU_FRAM_PORT_OFFSET * (port_id - 1));

		port->slave.mac_sl = cpsw_sl_get("am65", dev, port->port_base);
		if (IS_ERR(port->slave.mac_sl)) {
			ret = PTR_ERR(port->slave.mac_sl);
			goto of_node_put;
		}

		port->disabled = !of_device_is_available(port_np);
		if (port->disabled) {
			common->disabled_ports_mask |= BIT(port->port_id);
			continue;
		}

		port->slave.ifphy = devm_of_phy_get(dev, port_np, NULL);
		if (IS_ERR(port->slave.ifphy)) {
			ret = PTR_ERR(port->slave.ifphy);
			dev_err(dev, "%pOF error retrieving port phy: %d\n",
				port_np, ret);
			goto of_node_put;
		}

		/* Initialize the Serdes PHY for the port */
		ret = am65_cpsw_init_serdes_phy(dev, port_np, port);
		if (ret)
			goto of_node_put;

		port->slave.mac_only =
				of_property_read_bool(port_np, "ti,mac-only");

		/* get phy/link info */
		port->slave.port_np = of_node_get(port_np);
		ret = of_get_phy_mode(port_np, &port->slave.phy_if);
		if (ret) {
			dev_err(dev, "%pOF read phy-mode err %d\n",
				port_np, ret);
			goto of_node_put;
		}

		ret = phy_set_mode_ext(port->slave.ifphy, PHY_MODE_ETHERNET, port->slave.phy_if);
		if (ret)
			goto of_node_put;

		ret = of_get_mac_address(port_np, port->slave.mac_addr);
		if (ret == -EPROBE_DEFER) {
			goto of_node_put;
		} else if (ret) {
			am65_cpsw_am654_get_efuse_macid(port_np,
							port->port_id,
							port->slave.mac_addr);
			if (!is_valid_ether_addr(port->slave.mac_addr)) {
				eth_random_addr(port->slave.mac_addr);
				dev_info(dev, "Use random MAC address\n");
			}
		}

		/* Reset all Queue priorities to 0 */
		writel(0, port->port_base + AM65_CPSW_PN_REG_TX_PRI_MAP);
	}
	of_node_put(node);

	/* is there at least one ext.port */
	if (!(~common->disabled_ports_mask & GENMASK(common->port_num, 1))) {
		dev_err(dev, "No Ext. port are available\n");
		return -ENODEV;
	}

	return 0;

of_node_put:
	of_node_put(port_np);
	of_node_put(node);
	return ret;
}

static void am65_cpsw_nuss_phylink_cleanup(struct am65_cpsw_common *common)
{
	struct am65_cpsw_port *port;
	int i;

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		if (port->slave.phylink)
			phylink_destroy(port->slave.phylink);
	}
}

static void am65_cpsw_remove_dt(struct am65_cpsw_common *common)
{
	struct am65_cpsw_port *port;
	int i;

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		of_node_put(port->slave.port_np);
	}
}

static int
am65_cpsw_nuss_init_port_ndev(struct am65_cpsw_common *common, u32 port_idx)
{
	struct am65_cpsw_ndev_priv *ndev_priv;
	struct device *dev = common->dev;
	struct am65_cpsw_port *port;
	struct phylink *phylink;

	port = &common->ports[port_idx];

	if (port->disabled)
		return 0;

	/* alloc netdev */
	port->ndev = alloc_etherdev_mqs(sizeof(struct am65_cpsw_ndev_priv),
					AM65_CPSW_MAX_QUEUES,
					AM65_CPSW_MAX_QUEUES);
	if (!port->ndev) {
		dev_err(dev, "error allocating slave net_device %u\n",
			port->port_id);
		return -ENOMEM;
	}

	ndev_priv = netdev_priv(port->ndev);
	ndev_priv->port = port;
	ndev_priv->msg_enable = AM65_CPSW_DEBUG;
	mutex_init(&ndev_priv->mm_lock);
	port->qos.link_speed = SPEED_UNKNOWN;
	SET_NETDEV_DEV(port->ndev, dev);
	port->ndev->dev.of_node = port->slave.port_np;

	eth_hw_addr_set(port->ndev, port->slave.mac_addr);

	port->ndev->min_mtu = AM65_CPSW_MIN_PACKET_SIZE;
	port->ndev->max_mtu = AM65_CPSW_MAX_PACKET_SIZE -
			      (VLAN_ETH_HLEN + ETH_FCS_LEN);
	port->ndev->hw_features = NETIF_F_SG |
				  NETIF_F_RXCSUM |
				  NETIF_F_HW_CSUM |
				  NETIF_F_HW_TC;
	port->ndev->features = port->ndev->hw_features |
			       NETIF_F_HW_VLAN_CTAG_FILTER;
	port->ndev->xdp_features = NETDEV_XDP_ACT_BASIC |
				   NETDEV_XDP_ACT_REDIRECT |
				   NETDEV_XDP_ACT_NDO_XMIT;
	port->ndev->vlan_features |=  NETIF_F_SG;
	port->ndev->netdev_ops = &am65_cpsw_nuss_netdev_ops;
	port->ndev->ethtool_ops = &am65_cpsw_ethtool_ops_slave;

	/* Configuring Phylink */
	port->slave.phylink_config.dev = &port->ndev->dev;
	port->slave.phylink_config.type = PHYLINK_NETDEV;
	port->slave.phylink_config.mac_capabilities = MAC_SYM_PAUSE | MAC_10 | MAC_100 |
						      MAC_1000FD | MAC_5000FD;
	port->slave.phylink_config.mac_managed_pm = true; /* MAC does PM */

	switch (port->slave.phy_if) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		phy_interface_set_rgmii(port->slave.phylink_config.supported_interfaces);
		break;

	case PHY_INTERFACE_MODE_RMII:
		__set_bit(PHY_INTERFACE_MODE_RMII,
			  port->slave.phylink_config.supported_interfaces);
		break;

	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_USXGMII:
		if (common->pdata.extra_modes & BIT(port->slave.phy_if)) {
			__set_bit(port->slave.phy_if,
				  port->slave.phylink_config.supported_interfaces);
		} else {
			dev_err(dev, "selected phy-mode is not supported\n");
			return -EOPNOTSUPP;
		}
		break;

	default:
		dev_err(dev, "selected phy-mode is not supported\n");
		return -EOPNOTSUPP;
	}

	phylink = phylink_create(&port->slave.phylink_config,
				 of_fwnode_handle(port->slave.port_np),
				 port->slave.phy_if,
				 &am65_cpsw_phylink_mac_ops);
	if (IS_ERR(phylink))
		return PTR_ERR(phylink);

	port->slave.phylink = phylink;

	/* Disable TX checksum offload by default due to HW bug */
	if (common->pdata.quirks & AM65_CPSW_QUIRK_I2027_NO_TX_CSUM)
		port->ndev->features &= ~NETIF_F_HW_CSUM;

	port->ndev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	port->xdp_prog = NULL;

	if (!common->dma_ndev)
		common->dma_ndev = port->ndev;

	return 0;
}

static int am65_cpsw_nuss_init_ndevs(struct am65_cpsw_common *common)
{
	int ret;
	int i;

	for (i = 0; i < common->port_num; i++) {
		ret = am65_cpsw_nuss_init_port_ndev(common, i);
		if (ret)
			return ret;
	}

	return ret;
}

static void am65_cpsw_nuss_cleanup_ndev(struct am65_cpsw_common *common)
{
	struct am65_cpsw_port *port;
	int i;

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		if (!port->ndev)
			continue;
		if (port->ndev->reg_state == NETREG_REGISTERED)
			unregister_netdev(port->ndev);
		free_netdev(port->ndev);
		port->ndev = NULL;
	}
}

static void am65_cpsw_port_offload_fwd_mark_update(struct am65_cpsw_common *common)
{
	int set_val = 0;
	int i;

	if (common->br_members == (GENMASK(common->port_num, 1) & ~common->disabled_ports_mask))
		set_val = 1;

	dev_dbg(common->dev, "set offload_fwd_mark %d\n", set_val);

	for (i = 1; i <= common->port_num; i++) {
		struct am65_cpsw_port *port = am65_common_get_port(common, i);
		struct am65_cpsw_ndev_priv *priv;

		if (!port->ndev)
			continue;

		priv = am65_ndev_to_priv(port->ndev);
		priv->offload_fwd_mark = set_val;
	}
}

bool am65_cpsw_port_dev_check(const struct net_device *ndev)
{
	if (ndev->netdev_ops == &am65_cpsw_nuss_netdev_ops) {
		struct am65_cpsw_common *common = am65_ndev_to_common(ndev);

		return !common->is_emac_mode;
	}

	return false;
}

static int am65_cpsw_netdevice_port_link(struct net_device *ndev,
					 struct net_device *br_ndev,
					 struct netlink_ext_ack *extack)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_ndev_priv *priv = am65_ndev_to_priv(ndev);
	int err;

	if (!common->br_members) {
		common->hw_bridge_dev = br_ndev;
	} else {
		/* This is adding the port to a second bridge, this is
		 * unsupported
		 */
		if (common->hw_bridge_dev != br_ndev)
			return -EOPNOTSUPP;
	}

	err = switchdev_bridge_port_offload(ndev, ndev, NULL, NULL, NULL,
					    false, extack);
	if (err)
		return err;

	common->br_members |= BIT(priv->port->port_id);

	am65_cpsw_port_offload_fwd_mark_update(common);

	return NOTIFY_DONE;
}

static void am65_cpsw_netdevice_port_unlink(struct net_device *ndev)
{
	struct am65_cpsw_common *common = am65_ndev_to_common(ndev);
	struct am65_cpsw_ndev_priv *priv = am65_ndev_to_priv(ndev);

	switchdev_bridge_port_unoffload(ndev, NULL, NULL, NULL);

	common->br_members &= ~BIT(priv->port->port_id);

	am65_cpsw_port_offload_fwd_mark_update(common);

	if (!common->br_members)
		common->hw_bridge_dev = NULL;
}

/* netdev notifier */
static int am65_cpsw_netdevice_event(struct notifier_block *unused,
				     unsigned long event, void *ptr)
{
	struct netlink_ext_ack *extack = netdev_notifier_info_to_extack(ptr);
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct netdev_notifier_changeupper_info *info;
	int ret = NOTIFY_DONE;

	if (!am65_cpsw_port_dev_check(ndev))
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_CHANGEUPPER:
		info = ptr;

		if (netif_is_bridge_master(info->upper_dev)) {
			if (info->linking)
				ret = am65_cpsw_netdevice_port_link(ndev,
								    info->upper_dev,
								    extack);
			else
				am65_cpsw_netdevice_port_unlink(ndev);
		}
		break;
	default:
		return NOTIFY_DONE;
	}

	return notifier_from_errno(ret);
}

static int am65_cpsw_register_notifiers(struct am65_cpsw_common *cpsw)
{
	int ret = 0;

	if (AM65_CPSW_IS_CPSW2G(cpsw) ||
	    !IS_REACHABLE(CONFIG_TI_K3_AM65_CPSW_SWITCHDEV))
		return 0;

	cpsw->am65_cpsw_netdevice_nb.notifier_call = &am65_cpsw_netdevice_event;
	ret = register_netdevice_notifier(&cpsw->am65_cpsw_netdevice_nb);
	if (ret) {
		dev_err(cpsw->dev, "can't register netdevice notifier\n");
		return ret;
	}

	ret = am65_cpsw_switchdev_register_notifiers(cpsw);
	if (ret)
		unregister_netdevice_notifier(&cpsw->am65_cpsw_netdevice_nb);

	return ret;
}

static void am65_cpsw_unregister_notifiers(struct am65_cpsw_common *cpsw)
{
	if (AM65_CPSW_IS_CPSW2G(cpsw) ||
	    !IS_REACHABLE(CONFIG_TI_K3_AM65_CPSW_SWITCHDEV))
		return;

	am65_cpsw_switchdev_unregister_notifiers(cpsw);
	unregister_netdevice_notifier(&cpsw->am65_cpsw_netdevice_nb);
}

static const struct devlink_ops am65_cpsw_devlink_ops = {};

static void am65_cpsw_init_stp_ale_entry(struct am65_cpsw_common *cpsw)
{
	cpsw_ale_add_mcast(cpsw->ale, eth_stp_addr, ALE_PORT_HOST, ALE_SUPER, 0,
			   ALE_MCAST_BLOCK_LEARN_FWD);
}

static void am65_cpsw_init_host_port_switch(struct am65_cpsw_common *common)
{
	struct am65_cpsw_host *host = am65_common_get_host(common);

	writel(common->default_vlan, host->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);

	am65_cpsw_init_stp_ale_entry(common);

	cpsw_ale_control_set(common->ale, HOST_PORT_NUM, ALE_P0_UNI_FLOOD, 1);
	dev_dbg(common->dev, "Set P0_UNI_FLOOD\n");
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM, ALE_PORT_NOLEARN, 0);
}

static void am65_cpsw_init_host_port_emac(struct am65_cpsw_common *common)
{
	struct am65_cpsw_host *host = am65_common_get_host(common);

	writel(0, host->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);

	cpsw_ale_control_set(common->ale, HOST_PORT_NUM, ALE_P0_UNI_FLOOD, 0);
	dev_dbg(common->dev, "unset P0_UNI_FLOOD\n");

	/* learning make no sense in multi-mac mode */
	cpsw_ale_control_set(common->ale, HOST_PORT_NUM, ALE_PORT_NOLEARN, 1);
}

static int am65_cpsw_dl_switch_mode_get(struct devlink *dl, u32 id,
					struct devlink_param_gset_ctx *ctx)
{
	struct am65_cpsw_devlink *dl_priv = devlink_priv(dl);
	struct am65_cpsw_common *common = dl_priv->common;

	dev_dbg(common->dev, "%s id:%u\n", __func__, id);

	if (id != AM65_CPSW_DL_PARAM_SWITCH_MODE)
		return -EOPNOTSUPP;

	ctx->val.vbool = !common->is_emac_mode;

	return 0;
}

static void am65_cpsw_init_port_emac_ale(struct  am65_cpsw_port *port)
{
	struct am65_cpsw_slave_data *slave = &port->slave;
	struct am65_cpsw_common *common = port->common;
	u32 port_mask;

	writel(slave->port_vlan, port->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);

	if (slave->mac_only)
		/* enable mac-only mode on port */
		cpsw_ale_control_set(common->ale, port->port_id,
				     ALE_PORT_MACONLY, 1);

	cpsw_ale_control_set(common->ale, port->port_id, ALE_PORT_NOLEARN, 1);

	port_mask = BIT(port->port_id) | ALE_PORT_HOST;

	cpsw_ale_add_ucast(common->ale, port->ndev->dev_addr,
			   HOST_PORT_NUM, ALE_SECURE, slave->port_vlan);
	cpsw_ale_add_mcast(common->ale, port->ndev->broadcast,
			   port_mask, ALE_VLAN, slave->port_vlan, ALE_MCAST_FWD_2);
}

static void am65_cpsw_init_port_switch_ale(struct am65_cpsw_port *port)
{
	struct am65_cpsw_slave_data *slave = &port->slave;
	struct am65_cpsw_common *cpsw = port->common;
	u32 port_mask;

	cpsw_ale_control_set(cpsw->ale, port->port_id,
			     ALE_PORT_NOLEARN, 0);

	cpsw_ale_add_ucast(cpsw->ale, port->ndev->dev_addr,
			   HOST_PORT_NUM, ALE_SECURE | ALE_BLOCKED | ALE_VLAN,
			   slave->port_vlan);

	port_mask = BIT(port->port_id) | ALE_PORT_HOST;

	cpsw_ale_add_mcast(cpsw->ale, port->ndev->broadcast,
			   port_mask, ALE_VLAN, slave->port_vlan,
			   ALE_MCAST_FWD_2);

	writel(slave->port_vlan, port->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);

	cpsw_ale_control_set(cpsw->ale, port->port_id,
			     ALE_PORT_MACONLY, 0);
}

static int am65_cpsw_dl_switch_mode_set(struct devlink *dl, u32 id,
					struct devlink_param_gset_ctx *ctx,
					struct netlink_ext_ack *extack)
{
	struct am65_cpsw_devlink *dl_priv = devlink_priv(dl);
	struct am65_cpsw_common *cpsw = dl_priv->common;
	bool switch_en = ctx->val.vbool;
	bool if_running = false;
	int i;

	dev_dbg(cpsw->dev, "%s id:%u\n", __func__, id);

	if (id != AM65_CPSW_DL_PARAM_SWITCH_MODE)
		return -EOPNOTSUPP;

	if (switch_en == !cpsw->is_emac_mode)
		return 0;

	if (!switch_en && cpsw->br_members) {
		dev_err(cpsw->dev, "Remove ports from bridge before disabling switch mode\n");
		return -EINVAL;
	}

	rtnl_lock();

	cpsw->is_emac_mode = !switch_en;

	for (i = 0; i < cpsw->port_num; i++) {
		struct net_device *sl_ndev = cpsw->ports[i].ndev;

		if (!sl_ndev || !netif_running(sl_ndev))
			continue;

		if_running = true;
	}

	if (!if_running) {
		/* all ndevs are down */
		for (i = 0; i < cpsw->port_num; i++) {
			struct net_device *sl_ndev = cpsw->ports[i].ndev;
			struct am65_cpsw_slave_data *slave;

			if (!sl_ndev)
				continue;

			slave = am65_ndev_to_slave(sl_ndev);
			if (switch_en)
				slave->port_vlan = cpsw->default_vlan;
			else
				slave->port_vlan = 0;
		}

		goto exit;
	}

	cpsw_ale_control_set(cpsw->ale, 0, ALE_BYPASS, 1);
	/* clean up ALE table */
	cpsw_ale_control_set(cpsw->ale, HOST_PORT_NUM, ALE_CLEAR, 1);
	cpsw_ale_control_get(cpsw->ale, HOST_PORT_NUM, ALE_AGEOUT);

	if (switch_en) {
		dev_info(cpsw->dev, "Enable switch mode\n");

		am65_cpsw_init_host_port_switch(cpsw);

		for (i = 0; i < cpsw->port_num; i++) {
			struct net_device *sl_ndev = cpsw->ports[i].ndev;
			struct am65_cpsw_slave_data *slave;
			struct am65_cpsw_port *port;

			if (!sl_ndev)
				continue;

			port = am65_ndev_to_port(sl_ndev);
			slave = am65_ndev_to_slave(sl_ndev);
			slave->port_vlan = cpsw->default_vlan;

			if (netif_running(sl_ndev))
				am65_cpsw_init_port_switch_ale(port);
		}

	} else {
		dev_info(cpsw->dev, "Disable switch mode\n");

		am65_cpsw_init_host_port_emac(cpsw);

		for (i = 0; i < cpsw->port_num; i++) {
			struct net_device *sl_ndev = cpsw->ports[i].ndev;
			struct am65_cpsw_port *port;

			if (!sl_ndev)
				continue;

			port = am65_ndev_to_port(sl_ndev);
			port->slave.port_vlan = 0;
			if (netif_running(sl_ndev))
				am65_cpsw_init_port_emac_ale(port);
		}
	}
	cpsw_ale_control_set(cpsw->ale, HOST_PORT_NUM, ALE_BYPASS, 0);
exit:
	rtnl_unlock();

	return 0;
}

static const struct devlink_param am65_cpsw_devlink_params[] = {
	DEVLINK_PARAM_DRIVER(AM65_CPSW_DL_PARAM_SWITCH_MODE, "switch_mode",
			     DEVLINK_PARAM_TYPE_BOOL,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     am65_cpsw_dl_switch_mode_get,
			     am65_cpsw_dl_switch_mode_set, NULL),
};

static int am65_cpsw_nuss_register_devlink(struct am65_cpsw_common *common)
{
	struct devlink_port_attrs attrs = {};
	struct am65_cpsw_devlink *dl_priv;
	struct device *dev = common->dev;
	struct devlink_port *dl_port;
	struct am65_cpsw_port *port;
	int ret = 0;
	int i;

	common->devlink =
		devlink_alloc(&am65_cpsw_devlink_ops, sizeof(*dl_priv), dev);
	if (!common->devlink)
		return -ENOMEM;

	dl_priv = devlink_priv(common->devlink);
	dl_priv->common = common;

	/* Provide devlink hook to switch mode when multiple external ports
	 * are present NUSS switchdev driver is enabled.
	 */
	if (!AM65_CPSW_IS_CPSW2G(common) &&
	    IS_ENABLED(CONFIG_TI_K3_AM65_CPSW_SWITCHDEV)) {
		ret = devlink_params_register(common->devlink,
					      am65_cpsw_devlink_params,
					      ARRAY_SIZE(am65_cpsw_devlink_params));
		if (ret) {
			dev_err(dev, "devlink params reg fail ret:%d\n", ret);
			goto dl_unreg;
		}
	}

	for (i = 1; i <= common->port_num; i++) {
		port = am65_common_get_port(common, i);
		dl_port = &port->devlink_port;

		if (port->ndev)
			attrs.flavour = DEVLINK_PORT_FLAVOUR_PHYSICAL;
		else
			attrs.flavour = DEVLINK_PORT_FLAVOUR_UNUSED;
		attrs.phys.port_number = port->port_id;
		attrs.switch_id.id_len = sizeof(resource_size_t);
		memcpy(attrs.switch_id.id, common->switch_id, attrs.switch_id.id_len);
		devlink_port_attrs_set(dl_port, &attrs);

		ret = devlink_port_register(common->devlink, dl_port, port->port_id);
		if (ret) {
			dev_err(dev, "devlink_port reg fail for port %d, ret:%d\n",
				port->port_id, ret);
			goto dl_port_unreg;
		}
	}
	devlink_register(common->devlink);
	return ret;

dl_port_unreg:
	for (i = i - 1; i >= 1; i--) {
		port = am65_common_get_port(common, i);
		dl_port = &port->devlink_port;

		devlink_port_unregister(dl_port);
	}
dl_unreg:
	devlink_free(common->devlink);
	return ret;
}

static void am65_cpsw_unregister_devlink(struct am65_cpsw_common *common)
{
	struct devlink_port *dl_port;
	struct am65_cpsw_port *port;
	int i;

	devlink_unregister(common->devlink);

	for (i = 1; i <= common->port_num; i++) {
		port = am65_common_get_port(common, i);
		dl_port = &port->devlink_port;

		devlink_port_unregister(dl_port);
	}

	if (!AM65_CPSW_IS_CPSW2G(common) &&
	    IS_ENABLED(CONFIG_TI_K3_AM65_CPSW_SWITCHDEV))
		devlink_params_unregister(common->devlink,
					  am65_cpsw_devlink_params,
					  ARRAY_SIZE(am65_cpsw_devlink_params));

	devlink_free(common->devlink);
}

static int am65_cpsw_nuss_register_ndevs(struct am65_cpsw_common *common)
{
	struct am65_cpsw_rx_chn *rx_chan = &common->rx_chns;
	struct am65_cpsw_tx_chn *tx_chan = common->tx_chns;
	struct device *dev = common->dev;
	struct am65_cpsw_port *port;
	int ret = 0, i;

	/* init tx channels */
	ret = am65_cpsw_nuss_init_tx_chns(common);
	if (ret)
		return ret;
	ret = am65_cpsw_nuss_init_rx_chns(common);
	if (ret)
		goto err_remove_tx;

	/* The DMA Channels are not guaranteed to be in a clean state.
	 * Reset and disable them to ensure that they are back to the
	 * clean state and ready to be used.
	 */
	for (i = 0; i < common->tx_ch_num; i++) {
		k3_udma_glue_reset_tx_chn(tx_chan[i].tx_chn, &tx_chan[i],
					  am65_cpsw_nuss_tx_cleanup);
		k3_udma_glue_disable_tx_chn(tx_chan[i].tx_chn);
	}

	for (i = 0; i < common->rx_ch_num_flows; i++)
		k3_udma_glue_reset_rx_chn(rx_chan->rx_chn, i,
					  rx_chan,
					  am65_cpsw_nuss_rx_cleanup);

	k3_udma_glue_disable_rx_chn(rx_chan->rx_chn);

	ret = am65_cpsw_nuss_register_devlink(common);
	if (ret)
		goto err_remove_rx;

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];

		if (!port->ndev)
			continue;

		SET_NETDEV_DEVLINK_PORT(port->ndev, &port->devlink_port);

		ret = register_netdev(port->ndev);
		if (ret) {
			dev_err(dev, "error registering slave net device%i %d\n",
				i, ret);
			goto err_cleanup_ndev;
		}
	}

	ret = am65_cpsw_register_notifiers(common);
	if (ret)
		goto err_cleanup_ndev;

	/* can't auto unregister ndev using devm_add_action() due to
	 * devres release sequence in DD core for DMA
	 */

	return 0;

err_cleanup_ndev:
	am65_cpsw_nuss_cleanup_ndev(common);
	am65_cpsw_unregister_devlink(common);
err_remove_rx:
	am65_cpsw_nuss_remove_rx_chns(common);
err_remove_tx:
	am65_cpsw_nuss_remove_tx_chns(common);

	return ret;
}

int am65_cpsw_nuss_update_tx_rx_chns(struct am65_cpsw_common *common,
				     int num_tx, int num_rx)
{
	int ret;

	am65_cpsw_nuss_remove_tx_chns(common);
	am65_cpsw_nuss_remove_rx_chns(common);

	common->tx_ch_num = num_tx;
	common->rx_ch_num_flows = num_rx;
	ret = am65_cpsw_nuss_init_tx_chns(common);
	if (ret)
		return ret;

	ret = am65_cpsw_nuss_init_rx_chns(common);
	if (ret)
		am65_cpsw_nuss_remove_tx_chns(common);

	return ret;
}

struct am65_cpsw_soc_pdata {
	u32	quirks_dis;
};

static const struct am65_cpsw_soc_pdata am65x_soc_sr2_0 = {
	.quirks_dis = AM65_CPSW_QUIRK_I2027_NO_TX_CSUM,
};

static const struct soc_device_attribute am65_cpsw_socinfo[] = {
	{ .family = "AM65X",
	  .revision = "SR2.0",
	  .data = &am65x_soc_sr2_0
	},
	{/* sentinel */}
};

static const struct am65_cpsw_pdata am65x_sr1_0 = {
	.quirks = AM65_CPSW_QUIRK_I2027_NO_TX_CSUM,
	.ale_dev_id = "am65x-cpsw2g",
	.fdqring_mode = K3_RINGACC_RING_MODE_MESSAGE,
};

static const struct am65_cpsw_pdata j721e_pdata = {
	.quirks = 0,
	.ale_dev_id = "am65x-cpsw2g",
	.fdqring_mode = K3_RINGACC_RING_MODE_MESSAGE,
};

static const struct am65_cpsw_pdata am64x_cpswxg_pdata = {
	.quirks = AM64_CPSW_QUIRK_DMA_RX_TDOWN_IRQ,
	.ale_dev_id = "am64-cpswxg",
	.fdqring_mode = K3_RINGACC_RING_MODE_RING,
};

static const struct am65_cpsw_pdata j7200_cpswxg_pdata = {
	.quirks = 0,
	.ale_dev_id = "am64-cpswxg",
	.fdqring_mode = K3_RINGACC_RING_MODE_RING,
	.extra_modes = BIT(PHY_INTERFACE_MODE_QSGMII) | BIT(PHY_INTERFACE_MODE_SGMII) |
		       BIT(PHY_INTERFACE_MODE_USXGMII),
};

static const struct am65_cpsw_pdata j721e_cpswxg_pdata = {
	.quirks = 0,
	.ale_dev_id = "am64-cpswxg",
	.fdqring_mode = K3_RINGACC_RING_MODE_MESSAGE,
	.extra_modes = BIT(PHY_INTERFACE_MODE_QSGMII) | BIT(PHY_INTERFACE_MODE_SGMII),
};

static const struct am65_cpsw_pdata j784s4_cpswxg_pdata = {
	.quirks = 0,
	.ale_dev_id = "am64-cpswxg",
	.fdqring_mode = K3_RINGACC_RING_MODE_MESSAGE,
	.extra_modes = BIT(PHY_INTERFACE_MODE_QSGMII) | BIT(PHY_INTERFACE_MODE_SGMII) |
		       BIT(PHY_INTERFACE_MODE_USXGMII),
};

static const struct of_device_id am65_cpsw_nuss_of_mtable[] = {
	{ .compatible = "ti,am654-cpsw-nuss", .data = &am65x_sr1_0},
	{ .compatible = "ti,j721e-cpsw-nuss", .data = &j721e_pdata},
	{ .compatible = "ti,am642-cpsw-nuss", .data = &am64x_cpswxg_pdata},
	{ .compatible = "ti,j7200-cpswxg-nuss", .data = &j7200_cpswxg_pdata},
	{ .compatible = "ti,j721e-cpswxg-nuss", .data = &j721e_cpswxg_pdata},
	{ .compatible = "ti,j784s4-cpswxg-nuss", .data = &j784s4_cpswxg_pdata},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, am65_cpsw_nuss_of_mtable);

static void am65_cpsw_nuss_apply_socinfo(struct am65_cpsw_common *common)
{
	const struct soc_device_attribute *soc;

	soc = soc_device_match(am65_cpsw_socinfo);
	if (soc && soc->data) {
		const struct am65_cpsw_soc_pdata *socdata = soc->data;

		/* disable quirks */
		common->pdata.quirks &= ~socdata->quirks_dis;
	}
}

static int am65_cpsw_nuss_probe(struct platform_device *pdev)
{
	struct cpsw_ale_params ale_params = { 0 };
	const struct of_device_id *of_id;
	struct device *dev = &pdev->dev;
	struct am65_cpsw_common *common;
	struct device_node *node;
	struct resource *res;
	struct clk *clk;
	int ale_entries;
	__be64 id_temp;
	int ret, i;

	BUILD_BUG_ON_MSG(sizeof(struct am65_cpsw_tx_swdata) > AM65_CPSW_NAV_SW_DATA_SIZE,
			 "TX SW_DATA size exceeds AM65_CPSW_NAV_SW_DATA_SIZE");
	BUILD_BUG_ON_MSG(sizeof(struct am65_cpsw_swdata) > AM65_CPSW_NAV_SW_DATA_SIZE,
			 "SW_DATA size exceeds AM65_CPSW_NAV_SW_DATA_SIZE");
	common = devm_kzalloc(dev, sizeof(struct am65_cpsw_common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;
	common->dev = dev;

	of_id = of_match_device(am65_cpsw_nuss_of_mtable, dev);
	if (!of_id)
		return -EINVAL;
	common->pdata = *(const struct am65_cpsw_pdata *)of_id->data;

	am65_cpsw_nuss_apply_socinfo(common);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cpsw_nuss");
	common->ss_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(common->ss_base))
		return PTR_ERR(common->ss_base);
	common->cpsw_base = common->ss_base + AM65_CPSW_CPSW_NU_BASE;
	/* Use device's physical base address as switch id */
	id_temp = cpu_to_be64(res->start);
	memcpy(common->switch_id, &id_temp, sizeof(res->start));

	node = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!node)
		return -ENOENT;
	common->port_num = of_get_child_count(node);
	of_node_put(node);
	if (common->port_num < 1 || common->port_num > AM65_CPSW_MAX_PORTS)
		return -ENOENT;

	common->rx_flow_id_base = -1;
	init_completion(&common->tdown_complete);
	common->tx_ch_num = AM65_CPSW_DEFAULT_TX_CHNS;
	common->rx_ch_num_flows = AM65_CPSW_DEFAULT_RX_CHN_FLOWS;
	common->pf_p0_rx_ptype_rrobin = true;
	common->default_vlan = 1;

	common->ports = devm_kcalloc(dev, common->port_num,
				     sizeof(*common->ports),
				     GFP_KERNEL);
	if (!common->ports)
		return -ENOMEM;

	clk = devm_clk_get(dev, "fck");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "getting fck clock\n");
	common->bus_freq = clk_get_rate(clk);

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		return ret;
	}

	am65_cpsw_nuss_get_ver(common);

	ret = am65_cpsw_nuss_init_host_p(common);
	if (ret)
		goto err_pm_clear;

	ret = am65_cpsw_nuss_init_slave_ports(common);
	if (ret)
		goto err_pm_clear;

	node = of_get_child_by_name(dev->of_node, "mdio");
	if (!node) {
		dev_warn(dev, "MDIO node not found\n");
	} else if (of_device_is_available(node)) {
		struct platform_device *mdio_pdev;

		mdio_pdev = of_platform_device_create(node, NULL, dev);
		if (!mdio_pdev) {
			ret = -ENODEV;
			goto err_pm_clear;
		}

		common->mdio_dev =  &mdio_pdev->dev;
	}
	of_node_put(node);

	/* init common data */
	ale_params.dev = dev;
	ale_params.ale_ageout = AM65_CPSW_ALE_AGEOUT_DEFAULT;
	ale_params.ale_ports = common->port_num + 1;
	ale_params.ale_regs = common->cpsw_base + AM65_CPSW_NU_ALE_BASE;
	ale_params.dev_id = common->pdata.ale_dev_id;
	ale_params.bus_freq = common->bus_freq;

	common->ale = cpsw_ale_create(&ale_params);
	if (IS_ERR(common->ale)) {
		dev_err(dev, "error initializing ale engine\n");
		ret = PTR_ERR(common->ale);
		goto err_of_clear;
	}

	ale_entries = common->ale->params.ale_entries;
	common->ale_context = devm_kzalloc(dev,
					   ale_entries * ALE_ENTRY_WORDS * sizeof(u32),
					   GFP_KERNEL);
	ret = am65_cpsw_init_cpts(common);
	if (ret)
		goto err_of_clear;

	/* init ports */
	for (i = 0; i < common->port_num; i++)
		am65_cpsw_nuss_slave_disable_unused(&common->ports[i]);

	dev_set_drvdata(dev, common);

	common->is_emac_mode = true;

	ret = am65_cpsw_nuss_init_ndevs(common);
	if (ret)
		goto err_ndevs_clear;

	ret = am65_cpsw_nuss_register_ndevs(common);
	if (ret)
		goto err_ndevs_clear;

	pm_runtime_put(dev);
	return 0;

err_ndevs_clear:
	am65_cpsw_nuss_cleanup_ndev(common);
	am65_cpsw_nuss_phylink_cleanup(common);
	am65_cpts_release(common->cpts);
	am65_cpsw_remove_dt(common);
err_of_clear:
	if (common->mdio_dev)
		of_platform_device_destroy(common->mdio_dev, NULL);
err_pm_clear:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	return ret;
}

static void am65_cpsw_nuss_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct am65_cpsw_common *common;
	int ret;

	common = dev_get_drvdata(dev);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) {
		/* Note, if this error path is taken, we're leaking some
		 * resources.
		 */
		dev_err(&pdev->dev, "Failed to resume device (%pe)\n",
			ERR_PTR(ret));
		return;
	}

	am65_cpsw_unregister_notifiers(common);

	/* must unregister ndevs here because DD release_driver routine calls
	 * dma_deconfigure(dev) before devres_release_all(dev)
	 */
	am65_cpsw_nuss_cleanup_ndev(common);
	am65_cpsw_unregister_devlink(common);
	am65_cpsw_nuss_remove_rx_chns(common);
	am65_cpsw_nuss_remove_tx_chns(common);
	am65_cpsw_nuss_phylink_cleanup(common);
	am65_cpts_release(common->cpts);
	am65_cpsw_disable_serdes_phy(common);
	am65_cpsw_remove_dt(common);

	if (common->mdio_dev)
		of_platform_device_destroy(common->mdio_dev, NULL);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static int am65_cpsw_nuss_suspend(struct device *dev)
{
	struct am65_cpsw_common *common = dev_get_drvdata(dev);
	struct am65_cpsw_host *host_p = am65_common_get_host(common);
	struct am65_cpsw_port *port;
	struct net_device *ndev;
	int i, ret;

	cpsw_ale_dump(common->ale, common->ale_context);
	host_p->vid_context = readl(host_p->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);
	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		ndev = port->ndev;

		if (!ndev)
			continue;

		port->vid_context = readl(port->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);
		netif_device_detach(ndev);
		if (netif_running(ndev)) {
			rtnl_lock();
			ret = am65_cpsw_nuss_ndo_slave_stop(ndev);
			rtnl_unlock();
			if (ret < 0) {
				netdev_err(ndev, "failed to stop: %d", ret);
				return ret;
			}
		}
	}

	am65_cpts_suspend(common->cpts);

	am65_cpsw_nuss_remove_rx_chns(common);
	am65_cpsw_nuss_remove_tx_chns(common);

	return 0;
}

static int am65_cpsw_nuss_resume(struct device *dev)
{
	struct am65_cpsw_common *common = dev_get_drvdata(dev);
	struct am65_cpsw_host *host_p = am65_common_get_host(common);
	struct am65_cpsw_port *port;
	struct net_device *ndev;
	int i, ret;

	ret = am65_cpsw_nuss_init_tx_chns(common);
	if (ret)
		return ret;
	ret = am65_cpsw_nuss_init_rx_chns(common);
	if (ret) {
		am65_cpsw_nuss_remove_tx_chns(common);
		return ret;
	}

	/* If RX IRQ was disabled before suspend, keep it disabled */
	for (i = 0; i < common->rx_ch_num_flows; i++) {
		if (common->rx_chns.flows[i].irq_disabled)
			disable_irq(common->rx_chns.flows[i].irq);
	}

	am65_cpts_resume(common->cpts);

	for (i = 0; i < common->port_num; i++) {
		port = &common->ports[i];
		ndev = port->ndev;

		if (!ndev)
			continue;

		if (netif_running(ndev)) {
			rtnl_lock();
			ret = am65_cpsw_nuss_ndo_slave_open(ndev);
			rtnl_unlock();
			if (ret < 0) {
				netdev_err(ndev, "failed to start: %d", ret);
				return ret;
			}
		}

		netif_device_attach(ndev);
		writel(port->vid_context, port->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);
	}

	writel(host_p->vid_context, host_p->port_base + AM65_CPSW_PORT_VLAN_REG_OFFSET);
	cpsw_ale_restore(common->ale, common->ale_context);

	return 0;
}

static const struct dev_pm_ops am65_cpsw_nuss_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(am65_cpsw_nuss_suspend, am65_cpsw_nuss_resume)
};

static struct platform_driver am65_cpsw_nuss_driver = {
	.driver = {
		.name	 = AM65_CPSW_DRV_NAME,
		.of_match_table = am65_cpsw_nuss_of_mtable,
		.pm = &am65_cpsw_nuss_dev_pm_ops,
	},
	.probe = am65_cpsw_nuss_probe,
	.remove = am65_cpsw_nuss_remove,
};

module_platform_driver(am65_cpsw_nuss_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Grygorii Strashko <grygorii.strashko@ti.com>");
MODULE_DESCRIPTION("TI AM65 CPSW Ethernet driver");
