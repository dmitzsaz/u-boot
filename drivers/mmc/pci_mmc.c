// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015, Google, Inc
 * Copyright (C) 2014, Bin Meng <bmeng.cn@gmail.com>
 */

#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <pci.h>
#include <sdhci.h>
#include <acpi/acpigen.h>
#include <acpi/acpi_device.h>
#include <acpi/acpi_dp.h>
#include <asm-generic/gpio.h>
#include <dm/acpi.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>

/* Type of MMC device */
enum {
	TYPE_SD,
	TYPE_EMMC,
};

struct pci_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct pci_mmc_priv {
	struct sdhci_host host;
	void *base;
	struct gpio_desc cd_gpio;
};

#define PCI_VENDOR_ID_GLI		0x17a0
#define PCI_DEVICE_ID_GLI_9755		0x9755

/* GL9755 vendor configuration registers, as in Linux's sdhci-pci-gli.c */
#define GL9755_PECONF			0x44
#define  GL9755_LFCLK			GENMASK(14, 12)
#define  GL9755_DMACLK			BIT(29)
#define  GL9755_INVERT_CD		BIT(30)
#define  GL9755_INVERT_WP		BIT(31)
#define GL9755_PLL			0x64
#define  GL9755_PLL_DIR			BIT(15)
#define  GL9755_PLLSSC_EN		BIT(31)
#define GL9755_WT			0x800
#define  GL9755_WT_EN			BIT(0)

/* Card detection reports a seated card about 1.2 s after a polarity change */
#define GL9755_CD_SETTLE_US		2000000

/* The GL975x registers are read in whole 32-bit words, as Linux does */
static u16 gl9755_readw(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> (8 * (reg & 2));
}

static u8 gl9755_readb(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> (8 * (reg & 3));
}

static const struct sdhci_ops gl9755_ops = {
	.read_w = gl9755_readw,
	.read_b = gl9755_readb,
};

/*
 * Apply the board's card-detect and write-protect polarity in the GL9755
 * itself (Apple wires both inverted), which is also what makes it power
 * the slot's pull-ups. Then clear the low-frequency and DMA clock options
 * and turn off the spread-spectrum PLL, as Linux does, and start the
 * internal clock, without which the controller does not finish a full
 * reset.
 */
static int gl9755_init(struct udevice *dev, struct pci_mmc_plat *plat,
		       struct sdhci_host *host)
{
	u32 conf, prev, state, clk;
	int ret;

	dm_pci_clrset_config32(dev, GL9755_WT, GL9755_WT_EN, GL9755_WT_EN);
	dm_pci_read_config32(dev, GL9755_PECONF, &prev);
	conf = prev & ~(GL9755_LFCLK | GL9755_DMACLK | GL9755_INVERT_CD |
			GL9755_INVERT_WP);
	if (dev_read_bool(dev, "cd-inverted"))
		conf |= GL9755_INVERT_CD;
	if (dev_read_bool(dev, "wp-inverted"))
		conf |= GL9755_INVERT_WP;
	dm_pci_write_config32(dev, GL9755_PECONF, conf);
	dm_pci_clrset_config32(dev, GL9755_PLL,
			       GL9755_PLL_DIR | GL9755_PLLSSC_EN, 0);
	dm_pci_clrset_config32(dev, GL9755_WT, GL9755_WT_EN, 0);

	/* The present state already has the board's polarity */
	plat->cfg.host_caps &= ~MMC_CAP_CD_ACTIVE_HIGH;

	host->ops = &gl9755_ops;
	sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);
	/* Word read: SDHCI_CLOCK_CONTROL is the low half of its word */
	ret = readl_poll_timeout(host->ioaddr + SDHCI_CLOCK_CONTROL, clk,
				 clk & SDHCI_CLOCK_INT_STABLE, 20000);
	if (ret)
		return ret;

	/* An empty slot only costs time when the polarity changed */
	if ((conf ^ prev) & GL9755_INVERT_CD)
		readl_poll_timeout(host->ioaddr + SDHCI_PRESENT_STATE, state,
				   state & SDHCI_CARD_PRESENT,
				   GL9755_CD_SETTLE_US);

	return 0;
}

static int pci_mmc_probe(struct udevice *dev)
{
	struct pci_child_plat *pplat = dev_get_parent_plat(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct pci_mmc_plat *plat = dev_get_plat(dev);
	struct pci_mmc_priv *priv = dev_get_priv(dev);
	struct sdhci_host *host = &priv->host;
	struct blk_desc *desc;
	int ret;

	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;
	desc = mmc_get_blk_desc(&plat->mmc);
	desc->removable = !(plat->cfg.host_caps & MMC_CAP_NONREMOVABLE);

	host->ioaddr = dm_pci_map_bar(dev, PCI_BASE_ADDRESS_0, 0, 0,
				      PCI_REGION_TYPE, PCI_REGION_MEM);
	if (pplat->vendor == PCI_VENDOR_ID_GLI &&
	    pplat->device == PCI_DEVICE_ID_GLI_9755) {
		ret = gl9755_init(dev, plat, host);
		if (ret)
			return ret;
	}
	host->name = dev->name;
	host->cd_gpio = priv->cd_gpio;
	host->mmc = &plat->mmc;
	host->mmc->dev = dev;
	ret = sdhci_setup_cfg(&plat->cfg, host, 0, 0);
	if (ret)
		return ret;
	host->mmc->priv = &priv->host;
	upriv->mmc = host->mmc;

	return sdhci_probe(dev);
}

static int pci_mmc_of_to_plat(struct udevice *dev)
{
	if (CONFIG_IS_ENABLED(DM_GPIO)) {
		struct pci_mmc_priv *priv = dev_get_priv(dev);
		int ret;

		ret = gpio_request_by_name(dev, "cd-gpios", 0, &priv->cd_gpio,
					   GPIOD_IS_IN);
		log_debug("cd-gpio %s done, ret=%d\n", dev->name, ret);
	}

	return 0;
}

static int pci_mmc_bind(struct udevice *dev)
{
	struct pci_mmc_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

__maybe_unused
static int pci_mmc_acpi_fill_ssdt(const struct udevice *dev,
				  struct acpi_ctx *ctx)
{
	struct pci_mmc_priv *priv = dev_get_priv(dev);
	char path[ACPI_PATH_MAX];
	struct acpi_gpio gpio;
	struct acpi_dp *dp;
	int ret;

	if (!dev_has_ofnode(dev))
		return 0;
	if (dev_get_driver_data(dev) == TYPE_EMMC)
		return 0;

	ret = gpio_get_acpi(&priv->cd_gpio, &gpio);
	if (ret)
		return log_msg_ret("gpio", ret);
	gpio.type = ACPI_GPIO_TYPE_INTERRUPT;
	gpio.pull = ACPI_GPIO_PULL_NONE;
	gpio.irq.mode = ACPI_IRQ_EDGE_TRIGGERED;
	gpio.irq.polarity = ACPI_IRQ_ACTIVE_BOTH;
	gpio.irq.shared = ACPI_IRQ_SHARED;
	gpio.irq.wake = ACPI_IRQ_WAKE;
	gpio.interrupt_debounce_timeout = 10000; /* 100ms */

	/* Use device path as the Scope for the SSDT */
	ret = acpi_device_path(dev, path, sizeof(path));
	if (ret)
		return log_msg_ret("path", ret);
	acpigen_write_scope(ctx, path);
	acpigen_write_name(ctx, "_CRS");

	/* Write GpioInt() as default (if set) or custom from devicetree */
	acpigen_write_resourcetemplate_header(ctx);
	acpi_device_write_gpio(ctx, &gpio);
	acpigen_write_resourcetemplate_footer(ctx);

	/* Bind the cd-gpio name to the GpioInt() resource */
	dp = acpi_dp_new_table("_DSD");
	if (!dp)
		return -ENOMEM;
	acpi_dp_add_gpio(dp, "cd-gpio", path, 0, 0, 1);
	ret = acpi_dp_write(ctx, dp);
	if (ret)
		return log_msg_ret("cd", ret);

	acpigen_pop_len(ctx);

	return 0;
}

struct acpi_ops pci_mmc_acpi_ops = {
#ifdef CONFIG_ACPIGEN
	.fill_ssdt	= pci_mmc_acpi_fill_ssdt,
#endif
};

static const struct udevice_id pci_mmc_match[] = {
	{ .compatible = "intel,apl-sd", .data = TYPE_SD },
	{ .compatible = "intel,apl-emmc", .data = TYPE_EMMC },
	{ }
};

U_BOOT_DRIVER(pci_mmc) = {
	.name	= "pci_mmc",
	.id	= UCLASS_MMC,
	.of_match = pci_mmc_match,
	.bind	= pci_mmc_bind,
	.of_to_plat	= pci_mmc_of_to_plat,
	.probe	= pci_mmc_probe,
	.ops	= &sdhci_ops,
	.priv_auto	= sizeof(struct pci_mmc_priv),
	.plat_auto	= sizeof(struct pci_mmc_plat),
	ACPI_OPS_PTR(&pci_mmc_acpi_ops)
};

static struct pci_device_id mmc_supported[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_SYSTEM_SDHCI << 8, 0xffff00) },
	{},
};

U_BOOT_PCI_DEVICE(pci_mmc, mmc_supported);
