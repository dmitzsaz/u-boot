// SPDX-License-Identifier: GPL-2.0+ AND BSD-2-Clause
/*
 * Copyright (C) 2022 Mark Kettenis <kettenis@openbsd.org>
 *
 * Copyright (C) The Asahi Linux Contributors
 *
 * The A18 Pro (T8140) USB2 register definitions and power sequences are derived
 * from the Linux Apple Type-C PHY driver (drivers/phy/apple/atc.c, author Sven
 * Peter), used under the BSD-2-Clause licence, and from the Aurora Silicon T8140
 * additions to that driver. See Licenses/bsd-2-clause.txt for the redistribution
 * conditions and disclaimer, which also apply to binary distributions.
 */

#include <dm.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <generic-phy.h>
#include <reset-uclass.h>
#include <asm/io.h>
#include <dt-bindings/phy/phy.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

/* Core */
#define ACIOPHY_CFG0			0x08
#define  ACIOPHY_CFG0_COMMON_BIG	BIT(0)
#define  ACIOPHY_CFG0_COMMON_BIG_OV	BIT(1)
#define  ACIOPHY_CFG0_COMMON_SMALL	BIT(2)
#define  ACIOPHY_CFG0_COMMON_SMALL_OV	BIT(3)
#define  ACIOPHY_CFG0_COMMON_CLAMP	BIT(4)
#define  ACIOPHY_CFG0_COMMON_CLAMP_OV	BIT(5)
#define ACIOPHY_LANE_MODE_T8122		0x60
#define  ACIOPHY_LANE_MODE_RX0		GENMASK(2, 0)
#define  ACIOPHY_LANE_MODE_TX0		GENMASK(5, 3)
#define  ACIOPHY_LANE_MODE_RX1		GENMASK(8, 6)
#define  ACIOPHY_LANE_MODE_TX1		GENMASK(11, 9)
#define  ACIOPHY_LANE_MODE_OFF		3
#define ACIOPHY_CROSSBAR_T8122		0x64
#define  ACIOPHY_CROSSBAR_PROTOCOL	GENMASK(4, 0)
#define  ACIOPHY_CROSSBAR_PROTOCOL_USB3	0xa
#define  ACIOPHY_CROSSBAR_DP_SINGLE_PMA	GENMASK(16, 5)
#define  ACIOPHY_CROSSBAR_DP_BOTH_PMA	BIT(17)
#define AUS_COMMON_DIG_RCAL1		0x804
#define  AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE BIT(0)
#define AUS_COMMON_SHIM_BLK_BIAS_REG	0x0a00
#define  AUS_COMMON_SHIM_BLK_BIAS_REG_BGBIAS_OV BIT(1)
#define ATCPHY_EVT_USB2_CTL		0x0
#define  ATCPHY_EVT_USB2_CTL_EVT_EN	0x1
#define  ATCPHY_EVT_USB2_CTL_LOAD_CNT	0x8
#define ATCPHY_POWER_CTRL		0x20000
#define ATCPHY_POWER_STAT		0x20004
#define  ATCPHY_POWER_SLEEP_SMALL	BIT(0)
#define  ATCPHY_POWER_SLEEP_BIG		BIT(1)
#define  ATCPHY_POWER_CLAMP_EN		BIT(2)
#define  ATCPHY_POWER_APB_RESET_N	BIT(3)
#define  ATCPHY_POWER_PHY_RESET_N	BIT(4)
#define ATCPHY_MISC			0x20008
#define  ATCPHY_MISC_RESET_N		BIT(0)
#define  ATCPHY_MISC_LANE_SWAP		BIT(2)

/* Pipe handler */
#define PIPEHANDLER_OVERRIDE		0x00
#define  PIPEHANDLER_OVERRIDE_RXVALID	BIT(0)
#define  PIPEHANDLER_OVERRIDE_RXDETECT	BIT(2)
#define PIPEHANDLER_OVERRIDE_VALUES	0x04
#define  PIPEHANDLER_OVERRIDE_VAL_RXDETECT0 BIT(1)
#define  PIPEHANDLER_OVERRIDE_VAL_RXDETECT1 BIT(2)
#define PIPEHANDLER_MUX_CTRL		0x0c
#define  PIPEHANDLER_MUX_CTRL_CLK	GENMASK(5, 3)
#define  PIPEHANDLER_MUX_CTRL_DATA	GENMASK(2, 0)
#define  PIPEHANDLER_MUX_CTRL_CLK_OFF	0
#define  PIPEHANDLER_MUX_CTRL_CLK_DUMMY	4
#define  PIPEHANDLER_MUX_CTRL_DATA_DUMMY 2
#define PIPEHANDLER_AON_GEN		0x1c
#define  PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN BIT(4)
#define  PIPEHANDLER_AON_GEN_DWC3_RESET_N BIT(0)
#define PIPEHANDLER_NONSELECTED_OVERRIDE 0x20
#define  PIPEHANDLER_DUMMY_PHY_EN	BIT(15)

/* USB2 PHY */
#define USB2PHY_USBCTL			0x00
#define  USB2PHY_USBCTL_USB_MODE	0x7
#define  USB2PHY_USBCTL_RUN		2
#define  USB2PHY_USBCTL_ISOLATION	4
#define USB2PHY_CTL			0x04
#define  USB2PHY_CTL_RESET		BIT(0)
#define  USB2PHY_CTL_PORT_RESET		BIT(1)
#define  USB2PHY_CTL_APB_RESET_N	BIT(2)
#define  USB2PHY_CTL_SIDDQ		BIT(3)
#define USB2PHY_SIG			0x08
#define  USB2PHY_SIG_VBUSDET_FORCE_VAL	BIT(0)
#define  USB2PHY_SIG_VBUSDET_FORCE_EN	BIT(1)
#define  USB2PHY_SIG_VBUSVLDEXT_FORCE_VAL BIT(2)
#define  USB2PHY_SIG_VBUSVLDEXT_FORCE_EN BIT(3)
#define  USB2PHY_SIG_HOST		(7 << 12)
#define USB2PHY_MISCTUNE		0x1c
#define  USB2PHY_MISCTUNE_APBCLK_GATE_OFF BIT(29)
#define  USB2PHY_MISCTUNE_REFCLK_GATE_OFF BIT(30)

struct apple_atcphy_priv {
	bool usb2_only;		/* Bounded bootloader USB2 host mode */
	bool t8142;		/* T8142 has different PIPE mux fields */
	void __iomem *core;
	void __iomem *pipehandler;
	void __iomem *usb2phy;
	void __iomem *usb2phy_reg;
	fdt_size_t usb2phy_reg_size;
	bool powered;		/* May be powered; false only after full shutdown */
	bool configured;	/* Core and dummy path are safe for fast restart */
};

static void mask32(void __iomem *reg, u32 mask, u32 set)
{
	writel((readl(reg) & ~mask) | set, reg);
}

static void set32(void __iomem *reg, u32 set)
{
	mask32(reg, 0, set);
}

static void clear32(void __iomem *reg, u32 clear)
{
	mask32(reg, clear, 0);
}

/* Apply an "apple,tunable-*" property: <offset mask value> triplets. */
static int atcphy_apply_tunable(struct udevice *dev, void __iomem *regs,
				fdt_size_t size, const char *name)
{
	const fdt32_t *p;
	int len, i;

	p = dev_read_prop(dev, name, &len);
	if (!p)
		return -ENOENT;
	if (len <= 0 || len % (3 * sizeof(*p)) || size < sizeof(u32) ||
	    !regs || (uintptr_t)regs % sizeof(u32))
		return -EINVAL;

	/* Validate the entire property before changing any register. */
	for (i = 0; i < len / 4; i += 3) {
		u32 off = fdt32_to_cpu(p[i]);

		if (off % sizeof(u32) || off > size - sizeof(u32))
			return -EINVAL;
	}

	for (i = 0; i < len / 4; i += 3) {
		u32 off = fdt32_to_cpu(p[i]);

		mask32(regs + off, fdt32_to_cpu(p[i + 1]),
		       fdt32_to_cpu(p[i + 2]));
	}

	return 0;
}

static void atcphy_dwc3_reset_assert(struct apple_atcphy_priv *priv)
{
	clear32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		PIPEHANDLER_AON_GEN_DWC3_RESET_N);
	set32(priv->pipehandler + PIPEHANDLER_AON_GEN,
	      PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
}

static void atcphy_dwc3_reset_deassert(struct apple_atcphy_priv *priv)
{
	clear32(priv->pipehandler + PIPEHANDLER_AON_GEN,
		PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
	set32(priv->pipehandler + PIPEHANDLER_AON_GEN,
	      PIPEHANDLER_AON_GEN_DWC3_RESET_N);
}

static void atcphy_usb2_power_off(struct apple_atcphy_priv *priv)
{
	mask32(priv->usb2phy + USB2PHY_USBCTL, USB2PHY_USBCTL_USB_MODE,
	       USB2PHY_USBCTL_ISOLATION);
	udelay(10);
	set32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
	udelay(10);
	set32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
	udelay(10);
	set32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET);
	udelay(10);
	clear32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
	udelay(10);
	set32(priv->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
	set32(priv->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
}

static int atcphy_usb2_power_on(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	int ret;

	set32(priv->usb2phy + USB2PHY_SIG,
	      USB2PHY_SIG_VBUSDET_FORCE_VAL | USB2PHY_SIG_VBUSDET_FORCE_EN |
	      USB2PHY_SIG_VBUSVLDEXT_FORCE_VAL | USB2PHY_SIG_VBUSVLDEXT_FORCE_EN);
	udelay(10);
	clear32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
	udelay(10);
	clear32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_RESET);
	udelay(10);
	clear32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
	udelay(10);
	set32(priv->core + ATCPHY_EVT_USB2_CTL,
	      ATCPHY_EVT_USB2_CTL_LOAD_CNT | ATCPHY_EVT_USB2_CTL_EVT_EN);
	udelay(10);
	set32(priv->usb2phy + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
	udelay(10);
	clear32(priv->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_APBCLK_GATE_OFF);
	clear32(priv->usb2phy + USB2PHY_MISCTUNE, USB2PHY_MISCTUNE_REFCLK_GATE_OFF);

	/* Only T8140 has the separate eUSB2 repeater register bank. */
	if (!priv->t8142)
		mdelay(5);

	writel(USB2PHY_USBCTL_RUN, priv->usb2phy + USB2PHY_USBCTL);

	if (priv->t8142)
		return 0;

	/* Per-device eUSB2 defaults; the resets above cleared them. */
	ret = atcphy_apply_tunable(dev, priv->usb2phy_reg,
				   priv->usb2phy_reg_size,
				   "apple,tunable-usb2phy-reg-dflt");
	if (ret)
		dev_err(dev, "failed to apply required eUSB2 defaults (%d)\n", ret);

	return ret;
}

static int atcphy_power_off(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	u32 reg;
	int ret;

	clear32(priv->core + ATCPHY_POWER_CTRL, ATCPHY_POWER_PHY_RESET_N);
	set32(priv->core + ATCPHY_POWER_CTRL, ATCPHY_POWER_CLAMP_EN);
	clear32(priv->core + ATCPHY_MISC, ATCPHY_MISC_RESET_N | ATCPHY_MISC_LANE_SWAP);
	clear32(priv->core + ATCPHY_POWER_CTRL, ATCPHY_POWER_APB_RESET_N);

	clear32(priv->core + ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_BIG);
	ret = readl_poll_timeout(priv->core + ATCPHY_POWER_STAT, reg,
				 !(reg & ATCPHY_POWER_SLEEP_BIG), 1000);
	if (ret) {
		dev_err(dev, "failed to put the PHY to sleep (big)\n");
		return ret;
	}

	clear32(priv->core + ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_SMALL);
	ret = readl_poll_timeout(priv->core + ATCPHY_POWER_STAT, reg,
				 !(reg & ATCPHY_POWER_SLEEP_SMALL), 1000);
	if (ret) {
		dev_err(dev, "failed to put the PHY to sleep (small)\n");
		return ret;
	}

	priv->powered = false;
	return 0;
}

static u32 atcphy_pipe_clk_mask(struct apple_atcphy_priv *priv)
{
	return priv->t8142 ? GENMASK(6, 4) : PIPEHANDLER_MUX_CTRL_CLK;
}

static void atcphy_pipe_clk(struct apple_atcphy_priv *priv, u32 value)
{
	mask32(priv->pipehandler + PIPEHANDLER_MUX_CTRL,
	       atcphy_pipe_clk_mask(priv), value << (priv->t8142 ? 4 : 3));
}

static void atcphy_pipe_data(struct apple_atcphy_priv *priv, u32 value)
{
	mask32(priv->pipehandler + PIPEHANDLER_MUX_CTRL,
	       priv->t8142 ? GENMASK(1, 0) : PIPEHANDLER_MUX_CTRL_DATA, value);
}

static void atcphy_setup_pipehandler_dummy(struct apple_atcphy_priv *priv);

static int atcphy_stop(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);

	/* The fast restart path is unsafe as soon as teardown begins. */
	priv->configured = false;
	atcphy_dwc3_reset_assert(priv);
	atcphy_usb2_power_off(priv);
	if (priv->t8142) {
		/* The next OS needs the dummy clock for its PIPE lock handshake.
		 * DWC3 is held in reset, so park the backend without waiting on
		 * that stopped controller. Keep the AON dummy source enabled.
		 */
		atcphy_setup_pipehandler_dummy(priv);
	} else {
		atcphy_pipe_clk(priv, PIPEHANDLER_MUX_CTRL_CLK_OFF);
		clear32(priv->pipehandler + PIPEHANDLER_NONSELECTED_OVERRIDE,
			PIPEHANDLER_DUMMY_PHY_EN);
	}

	return atcphy_power_off(dev);
}

static int atcphy_startup_failed(struct udevice *dev, int ret)
{
	int cleanup_ret = atcphy_stop(dev);

	if (cleanup_ret)
		dev_err(dev, "failed to unwind PHY startup (%d)\n", cleanup_ret);

	return ret;
}

static void atcphy_setup_pipehandler_dummy(struct apple_atcphy_priv *priv)
{
	/* DWC3 is held in reset: park PIPE before enabling its dummy backend. */
	if (priv->t8142) {
		clear32(priv->pipehandler + PIPEHANDLER_OVERRIDE_VALUES,
			PIPEHANDLER_OVERRIDE_VAL_RXDETECT0 | PIPEHANDLER_OVERRIDE_VAL_RXDETECT1);
		set32(priv->pipehandler + PIPEHANDLER_OVERRIDE,
		      PIPEHANDLER_OVERRIDE_RXVALID | PIPEHANDLER_OVERRIDE_RXDETECT);
	}
	atcphy_pipe_clk(priv, PIPEHANDLER_MUX_CTRL_CLK_OFF);
	udelay(10);
	atcphy_pipe_data(priv, PIPEHANDLER_MUX_CTRL_DATA_DUMMY);
	udelay(10);
	atcphy_pipe_clk(priv, PIPEHANDLER_MUX_CTRL_CLK_DUMMY);
	udelay(10);
	set32(priv->pipehandler + PIPEHANDLER_NONSELECTED_OVERRIDE,
	      PIPEHANDLER_DUMMY_PHY_EN);
}

/* Bring the block up in USB2 mode: SuperSpeed lanes off, PIPE on the dummy PHY. */
static int atcphy_configure_usb2(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	void __iomem *core = priv->core;
	u32 reg;
	int ret;

	/* Stay conservative if startup and the subsequent power-off both fail. */
	priv->powered = true;
	priv->configured = false;
	atcphy_setup_pipehandler_dummy(priv);
	ret = atcphy_usb2_power_on(dev);
	if (ret)
		goto err_power_off;

	set32(core + ATCPHY_MISC, ATCPHY_MISC_RESET_N);

	set32(core + ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_SMALL);
	ret = readl_poll_timeout(core + ATCPHY_POWER_STAT, reg,
				 reg & ATCPHY_POWER_SLEEP_SMALL, 100000);
	if (ret) {
		dev_err(dev, "failed to wake the PHY (small)\n");
		goto err_power_off;
	}

	set32(core + ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_BIG);
	ret = readl_poll_timeout(core + ATCPHY_POWER_STAT, reg,
				 reg & ATCPHY_POWER_SLEEP_BIG, 100000);
	if (ret) {
		dev_err(dev, "failed to wake the PHY (big)\n");
		goto err_power_off;
	}

	clear32(core + ATCPHY_POWER_CTRL, ATCPHY_POWER_CLAMP_EN);
	set32(core + ATCPHY_POWER_CTRL, ATCPHY_POWER_APB_RESET_N);

	/* The SuperSpeed tunables are left for the OS in USB2-only mode. */

	set32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_SMALL);
	udelay(10);
	set32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_SMALL_OV);
	udelay(10);
	set32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_BIG);
	udelay(10);
	set32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_BIG_OV);
	udelay(10);
	clear32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_CLAMP);
	udelay(10);
	set32(core + ACIOPHY_CFG0, ACIOPHY_CFG0_COMMON_CLAMP_OV);
	udelay(10);
	set32(core + AUS_COMMON_SHIM_BLK_BIAS_REG,
	      AUS_COMMON_SHIM_BLK_BIAS_REG_BGBIAS_OV);
	udelay(10);

	/* Both lanes off, USB3 crossbar, no lane swap, no DP */
	mask32(core + ACIOPHY_LANE_MODE_T8122,
	       ACIOPHY_LANE_MODE_RX0 | ACIOPHY_LANE_MODE_TX0 |
	       ACIOPHY_LANE_MODE_RX1 | ACIOPHY_LANE_MODE_TX1,
	       FIELD_PREP(ACIOPHY_LANE_MODE_RX0, ACIOPHY_LANE_MODE_OFF) |
	       FIELD_PREP(ACIOPHY_LANE_MODE_TX0, ACIOPHY_LANE_MODE_OFF) |
	       FIELD_PREP(ACIOPHY_LANE_MODE_RX1, ACIOPHY_LANE_MODE_OFF) |
	       FIELD_PREP(ACIOPHY_LANE_MODE_TX1, ACIOPHY_LANE_MODE_OFF));
	mask32(core + ACIOPHY_CROSSBAR_T8122, ACIOPHY_CROSSBAR_PROTOCOL,
	       FIELD_PREP(ACIOPHY_CROSSBAR_PROTOCOL,
			  ACIOPHY_CROSSBAR_PROTOCOL_USB3));
	clear32(core + ATCPHY_MISC, ATCPHY_MISC_LANE_SWAP);
	clear32(core + ACIOPHY_CROSSBAR_T8122,
		ACIOPHY_CROSSBAR_DP_SINGLE_PMA | ACIOPHY_CROSSBAR_DP_BOTH_PMA);

	set32(core + ATCPHY_POWER_CTRL, ATCPHY_POWER_PHY_RESET_N);
	ret = readl_poll_timeout(core + AUS_COMMON_DIG_RCAL1, reg,
				 reg & AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE,
				 100000);
	if (ret) {
		dev_err(dev, "resistor calibration did not finish\n");
		goto err_power_off;
	}

	priv->configured = true;
	return 0;

err_power_off:
	return atcphy_startup_failed(dev, ret);
}

static int apple_atcphy_usb2_init(struct phy *phy)
{
	struct udevice *rdev = dev_get_parent(phy->dev);
	struct apple_atcphy_priv *priv = dev_get_priv(rdev);
	int ret;

	if (!priv->usb2_only || phy->id != PHY_TYPE_USB2)
		return 0;

	/* dwc3 releasing its reset powered the USB2 PHY off; bring it back. */
	set32(priv->usb2phy + USB2PHY_SIG, USB2PHY_SIG_HOST);
	if (!priv->configured) {
		/* Finish an earlier incomplete shutdown before reconfiguration. */
		if (priv->powered) {
			ret = atcphy_stop(rdev);
			if (ret)
				return ret;
		}

		return atcphy_configure_usb2(rdev);
	}

	ret = atcphy_usb2_power_on(rdev);
	if (ret)
		return atcphy_startup_failed(rdev, ret);

	return 0;
}

static int apple_atcphy_of_xlate(struct phy *phy,
				 struct ofnode_phandle_args *args)
{
	if (args->args_count != 1)
		return -EINVAL;

	phy->id = args->args[0];
	return 0;
}

static const struct phy_ops apple_atcphy_ops = {
	.of_xlate = apple_atcphy_of_xlate,
	.init = apple_atcphy_usb2_init,
};

static struct driver apple_atcphy_driver = {
	.name = "apple-atcphy",
	.id = UCLASS_PHY,
	.ops = &apple_atcphy_ops,
};

static int apple_atcphy_reset_of_xlate(struct reset_ctl *reset_ctl,
				       struct ofnode_phandle_args *args)
{
	if (args->args_count != 0)
		return -EINVAL;

	return 0;
}

static int apple_atcphy_rst_assert(struct reset_ctl *reset_ctl)
{
	struct apple_atcphy_priv *priv = dev_get_priv(reset_ctl->dev);

	if (!priv->usb2_only)
		return 0;

	atcphy_dwc3_reset_assert(priv);
	atcphy_usb2_power_off(priv);

	return 0;
}

static int apple_atcphy_rst_deassert(struct reset_ctl *reset_ctl)
{
	struct apple_atcphy_priv *priv = dev_get_priv(reset_ctl->dev);

	if (priv->usb2_only)
		atcphy_dwc3_reset_deassert(priv);

	return 0;
}

static const struct reset_ops apple_atcphy_reset_ops = {
	.of_xlate = apple_atcphy_reset_of_xlate,
	.rst_assert = apple_atcphy_rst_assert,
	.rst_deassert = apple_atcphy_rst_deassert,
};

/*
 * The A18 Pro's USB-C port reaches dwc3 only as USB2, through a hub on the
 * board; nothing else brings this PHY up before U-Boot runs.  Reset whatever
 * the boot firmware left, then establish USB2 host mode while dwc3 is still
 * held in reset.  SuperSpeed, DisplayPort and the Type-C controller are left
 * to the OS.
 */
static int apple_atcphy_t8140_probe(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	int ret;

	priv->core = dev_read_addr_name_ptr(dev, "core");
	priv->pipehandler = dev_read_addr_name_ptr(dev, "pipehandler");
	priv->usb2phy = dev_read_addr_name_ptr(dev, "usb2phy");
	priv->usb2phy_reg = dev_read_addr_size_name_ptr(dev, "usb2phy-reg",
							&priv->usb2phy_reg_size);
	if (!priv->core || !priv->pipehandler || !priv->usb2phy ||
	    !priv->usb2phy_reg || priv->usb2phy_reg_size < sizeof(u32))
		return -EINVAL;

	priv->powered = true;
	ret = atcphy_stop(dev);
	if (ret)
		return ret;

	set32(priv->usb2phy + USB2PHY_SIG, USB2PHY_SIG_HOST);
	ret = atcphy_configure_usb2(dev);
	if (ret)
		return ret;

	priv->usb2_only = true;
	return 0;
}

/*
 * Initial J704 USB2 host path. Registers and sequencing follow the T8142
 * Linux driver. Firmware/HPM retains Type-C power policy; no SuperSpeed or
 * alternate-mode ownership is claimed here. All three external ports use
 * this USB2 host path, including the port used for UART during bring-up.
 */
static int apple_atcphy_t8142_probe(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	fdt_addr_t core, pipe, usb2;
	fdt_size_t core_size, pipe_size, usb2_size;
	int ret;

	if (!of_machine_is_compatible("apple,j704"))
		return -ENODEV;
	core = dev_read_addr_size_name(dev, "core", &core_size);
	pipe = dev_read_addr_size_name(dev, "pipehandler", &pipe_size);
	usb2 = dev_read_addr_size_name(dev, "usb2phy", &usb2_size);
	if ((core != 0x403000000ULL && core != 0x40b000000ULL &&
	     core != 0x41b000000ULL) ||
	    pipe != core - 0x57c000 || usb2 != core - 0x570000 ||
	    core_size != 0x4c000 || pipe_size != 0x4000 || usb2_size != 0x4000)
		return -EINVAL;
	priv->core = (void __iomem *)(uintptr_t)core;
	priv->pipehandler = (void __iomem *)(uintptr_t)pipe;
	priv->usb2phy = (void __iomem *)(uintptr_t)usb2;
	priv->t8142 = true;
	priv->usb2_only = true;
	priv->powered = true;
	ret = atcphy_stop(dev);
	if (ret)
		return ret;
	set32(priv->usb2phy + USB2PHY_SIG, USB2PHY_SIG_HOST);
	ret = atcphy_configure_usb2(dev);
	if (!ret)
		dev_info(dev, "J704 T8142 USB2 host PHY ready\n");
	return ret;
}

static int apple_atcphy_reset_probe(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);
	struct udevice *child;
	int ret;

	if (device_is_compatible(dev, "apple,t8142-atcphy")) {
		ret = apple_atcphy_t8142_probe(dev);
		if (ret)
			return ret;
	} else if (device_is_compatible(dev, "apple,t8140-atcphy")) {
		ret = apple_atcphy_t8140_probe(dev);
		if (ret)
			return ret;
	}

	ret = device_bind(dev, &apple_atcphy_driver, "apple-atcphy", NULL,
			  dev_ofnode(dev), &child);
	if (ret) {
		if (priv->usb2_only)
			return atcphy_startup_failed(dev, ret);
		return ret;
	}

	return 0;
}

static int apple_atcphy_reset_remove(struct udevice *dev)
{
	struct apple_atcphy_priv *priv = dev_get_priv(dev);

	/* Leave dwc3 in reset and the PHY off for the OS driver. */
	if (priv->usb2_only)
		return atcphy_stop(dev);

	return 0;
}

static const struct udevice_id apple_atcphy_ids[] = {
	{ .compatible = "apple,t6000-atcphy" },
	{ .compatible = "apple,t8103-atcphy" },
	{ .compatible = "apple,t8122-atcphy" },
	{ .compatible = "apple,t8140-atcphy" },
	{ .compatible = "apple,t8142-atcphy" },
	{ }
};

U_BOOT_DRIVER(apple_atcphy_reset) = {
	.name = "apple-atcphy-reset",
	.id = UCLASS_RESET,
	.of_match = apple_atcphy_ids,
	.ops = &apple_atcphy_reset_ops,
	.probe = apple_atcphy_reset_probe,
	.remove = apple_atcphy_reset_remove,
	.priv_auto = sizeof(struct apple_atcphy_priv),
	.flags = DM_FLAG_OS_PREPARE,
};
