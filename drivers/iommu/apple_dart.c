// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <cpu_func.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <iommu.h>
#include <linux/bitfield.h>
#include <linux/sizes.h>
#include <lmb.h>
#include <memalign.h>
#include <asm/io.h>
#include <asm/arch/dart.h>

#define DART_PARAMS2		0x0004
#define  DART_PARAMS2_BYPASS_SUPPORT	BIT(0)

#define DART_T8020_TLB_CMD		0x0020
#define  DART_T8020_TLB_CMD_FLUSH		BIT(20)
#define  DART_T8020_TLB_CMD_BUSY		BIT(2)
#define DART_T8020_TLB_SIDMASK		0x0034
#define DART_T8020_ERROR		0x0040
#define DART_T8020_ERROR_ADDR_LO	0x0050
#define DART_T8020_ERROR_ADDR_HI	0x0054
#define DART_T8020_CONFIG		0x0060
#define  DART_T8020_CONFIG_LOCK			BIT(15)
#define DART_T8020_SID_ENABLE		0x00fc
#define DART_T8020_TCR_BASE		0x0100
#define  DART_T8020_TCR_TRANSLATE_ENABLE	BIT(7)
#define  DART_T8020_TCR_BYPASS_DART		BIT(8)
#define  DART_T8020_TCR_BYPASS_DAPF		BIT(12)
#define DART_T8020_TTBR_BASE		0x0200
#define  DART_T8020_TTBR_VALID			BIT(31)

#define DART_T8110_PARAMS3		0x0008
#define  DART_T8110_PARAMS3_VA_WIDTH		GENMASK(21, 16)
#define DART_T8110_PARAMS4		0x000c
#define  DART_T8110_PARAMS4_NSID_MASK		(0x1ff << 0)
#define DART_T8110_TLB_CMD		0x0080
#define  DART_T8110_TLB_CMD_BUSY		BIT(31)
#define  DART_T8110_TLB_CMD_OP		GENMASK(10, 8)
#define  DART_T8110_TLB_CMD_OP_FLUSH_SID	1
#define  DART_T8110_TLB_CMD_STREAM		GENMASK(7, 0)
#define DART_T8110_ERROR		0x0100
#define DART_T8110_ERROR_MASK		0x0104
#define DART_T8110_ERROR_ADDR_LO	0x0170
#define DART_T8110_ERROR_ADDR_HI	0x0174
#define DART_T8110_PROTECT		0x0200
#define  DART_T8110_PROTECT_TTBR_TCR		BIT(0)
#define DART_T8110_SID_ENABLE_BASE	0x0c00
#define DART_T8110_TCR_BASE		0x1000
#define  DART_T8110_TCR_FOUR_LEVEL		BIT(3)
#define  DART_T8110_TCR_BYPASS_DAPF		BIT(2)
#define  DART_T8110_TCR_BYPASS_DART		BIT(1)
#define  DART_T8110_TCR_TRANSLATE_ENABLE	BIT(0)
#define DART_T8110_TTBR_BASE		0x1400
#define  DART_T8110_TTBR_VALID			BIT(0)

#define DART_SID_ENABLE(priv, idx) \
	((priv)->sid_enable_base + 4 * (idx))
#define DART_TCR(priv, sid)	((priv)->tcr_base + 4 * (sid))
#define DART_TTBR(priv, sid, idx)	\
	((priv)->ttbr_base + 4 * (priv)->nttbr * (sid) + 4 * (idx))
#define  DART_TTBR_SHIFT	12

#define DART_ALL_STREAMS(priv)	((1U << (priv)->nsid) - 1)

#define DART_PAGE_SIZE		SZ_16K
#define DART_PAGE_MASK		(DART_PAGE_SIZE - 1)

#define DART_L1_TABLE		0x3
#define DART_T8110_TABLE	BIT(0)
#define DART_L2_INVAL		0
#define DART_L2_VALID		BIT(0)
#define DART_L2_FULL_PAGE	BIT(1)
#define DART_L2_START(addr)	((((addr) & DART_PAGE_MASK) >> 2) << 52)
#define DART_L2_END(addr)	((((addr) & DART_PAGE_MASK) >> 2) << 40)

struct apple_dart_priv {
	void *base;
	u64 *l0, *l1, *l2;
	int bypass, shift;
	/* Leave bypass in place at removal: a coprocessor still uses it. */
	bool retain_bypass;

	struct lmb io_lmb;

	dma_addr_t dvabase;
	dma_addr_t dvaend;

	int nsid;
	int nttbr;
	int sid_enable_base;
	int tcr_base;
	u32 tcr_translate_enable;
	u32 tcr_four_level;
	u32 tcr_bypass;
	/* Bypass was set but its TCR bits do not read back; see apple_dart_map(). */
	bool bypass_unverified;
	u64 table_valid;
	int ttbr_base;
	u32 ttbr_valid;
	void (*flush_tlb)(struct apple_dart_priv *priv);
};

static void apple_dart_t8020_flush_tlb(struct apple_dart_priv *priv)
{
	dsb();

	writel(DART_ALL_STREAMS(priv), priv->base + DART_T8020_TLB_SIDMASK);
	writel(DART_T8020_TLB_CMD_FLUSH, priv->base + DART_T8020_TLB_CMD);
	while (readl(priv->base + DART_T8020_TLB_CMD) &
	       DART_T8020_TLB_CMD_BUSY)
		continue;
}

static void apple_dart_t8110_flush_tlb(struct apple_dart_priv *priv)
{
	int sid;

	dsb();

	/* OP=1 flushes one SID, not all streams. Match Linux's sequence. */
	for (sid = 0; sid < priv->nsid; sid++) {
		writel(FIELD_PREP(DART_T8110_TLB_CMD_OP,
				  DART_T8110_TLB_CMD_OP_FLUSH_SID) |
		       FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid),
		       priv->base + DART_T8110_TLB_CMD);
		while (readl(priv->base + DART_T8110_TLB_CMD) &
		       DART_T8110_TLB_CMD_BUSY)
			continue;
	}
}

static int apple_dart_setup_tables(struct udevice *dev);

static dma_addr_t apple_dart_map(struct udevice *dev, void *addr, size_t size)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	phys_addr_t paddr, dva;
	phys_size_t psize, off;
	int i, idx;

	if (priv->bypass && priv->bypass_unverified && !priv->retain_bypass) {
		dev_dbg(dev, "bypass not verified, translating\n");
		priv->bypass = 0;
		priv->bypass_unverified = false;
		if (apple_dart_setup_tables(dev))
			return 0;
	}
	if (priv->bypass)
		return (phys_addr_t)addr;

	paddr = ALIGN_DOWN((phys_addr_t)addr, DART_PAGE_SIZE);
	off = (phys_addr_t)addr - paddr;
	psize = ALIGN(size + off, DART_PAGE_SIZE);

	dva = io_lmb_alloc(&priv->io_lmb, psize, DART_PAGE_SIZE);

	idx = dva / DART_PAGE_SIZE;
	for (i = 0; i < psize / DART_PAGE_SIZE; i++) {
		priv->l2[idx + i] = (paddr  >> priv->shift) | DART_L2_VALID |
			DART_L2_START(0LL) | DART_L2_END(~0LL);
		paddr += DART_PAGE_SIZE;
	}
	flush_dcache_range((unsigned long)&priv->l2[idx],
			   (unsigned long)&priv->l2[idx + i]);
	priv->flush_tlb(priv);

	return dva + off;
}

static void apple_dart_unmap(struct udevice *dev, dma_addr_t addr, size_t size)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	phys_addr_t dva;
	phys_size_t psize;
	int i, idx;

	if (priv->bypass)
		return;

	dva = ALIGN_DOWN(addr, DART_PAGE_SIZE);
	psize = size + (addr - dva);
	psize = ALIGN(psize, DART_PAGE_SIZE);

	idx = dva / DART_PAGE_SIZE;
	for (i = 0; i < psize / DART_PAGE_SIZE; i++)
		priv->l2[idx + i] = DART_L2_INVAL;
	flush_dcache_range((unsigned long)&priv->l2[idx],
			   (unsigned long)&priv->l2[idx + i]);
	priv->flush_tlb(priv);

	io_lmb_free(&priv->io_lmb, dva, psize);
}

static struct iommu_ops apple_dart_ops = {
	.map = apple_dart_map,
	.unmap = apple_dart_unmap,
};

static int apple_dart_probe(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	int ret, sid, i;
	u32 params2, params3, params4;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	if (device_is_compatible(dev, "apple,t8110-dart")) {
		/*
		 * A DART the firmware locked (e.g. the display DARTs) keeps
		 * translation tables the firmware depends on and ignores or
		 * faults on writes to them.  Never reset one.
		 */
		if (readl(priv->base + DART_T8110_PROTECT) &
		    DART_T8110_PROTECT_TTBR_TCR) {
			dev_err(dev, "DART is locked by firmware\n");
			return -EPERM;
		}

		params3 = readl(priv->base + DART_T8110_PARAMS3);
		params4 = readl(priv->base + DART_T8110_PARAMS4);
		priv->nsid = params4 & DART_T8110_PARAMS4_NSID_MASK;
		/*
		 * With more than 36 address bits the tables have a third
		 * level, as in Linux's io-pgtable-dart. U-Boot's addresses
		 * stay below 4 GiB and use its first entry only.
		 */
		if (FIELD_GET(DART_T8110_PARAMS3_VA_WIDTH, params3) > 36)
			priv->tcr_four_level = DART_T8110_TCR_FOUR_LEVEL;
		priv->nttbr = 1;
		priv->sid_enable_base = DART_T8110_SID_ENABLE_BASE;
		priv->tcr_base = DART_T8110_TCR_BASE;
		priv->tcr_translate_enable = DART_T8110_TCR_TRANSLATE_ENABLE;
		priv->tcr_bypass =
		    DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART;
		priv->ttbr_base = DART_T8110_TTBR_BASE;
		priv->ttbr_valid = DART_T8110_TTBR_VALID;
		priv->table_valid = DART_T8110_TABLE;
		priv->flush_tlb = apple_dart_t8110_flush_tlb;
	} else {
		priv->nsid = 16;
		priv->nttbr = 4;
		priv->sid_enable_base = DART_T8020_SID_ENABLE;
		priv->tcr_base = DART_T8020_TCR_BASE;
		priv->tcr_translate_enable = DART_T8020_TCR_TRANSLATE_ENABLE;
		priv->tcr_bypass =
		    DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART;
		priv->ttbr_base = DART_T8020_TTBR_BASE;
		priv->ttbr_valid = DART_T8020_TTBR_VALID;
		priv->table_valid = DART_L1_TABLE;
		priv->flush_tlb = apple_dart_t8020_flush_tlb;
	}

	if (device_is_compatible(dev, "apple,t6000-dart") ||
	    device_is_compatible(dev, "apple,t8110-dart"))
		priv->shift = 4;

	priv->dvabase = DART_PAGE_SIZE;
	priv->dvaend = SZ_4G - DART_PAGE_SIZE;

	ret = io_lmb_setup(&priv->io_lmb);
	if (ret)
		return ret;
	ret = io_lmb_add(&priv->io_lmb, priv->dvabase,
			 priv->dvaend - priv->dvabase);
	if (ret)
		return -EINVAL;

	/* Disable translations. */
	for (sid = 0; sid < priv->nsid; sid++)
		writel(0, priv->base + DART_TCR(priv, sid));

	/* Remove page tables. */
	for (sid = 0; sid < priv->nsid; sid++) {
		for (i = 0; i < priv->nttbr; i++)
			writel(0, priv->base + DART_TTBR(priv, sid, i));
	}
	priv->flush_tlb(priv);

	/*
	 * Some DARTs advertise bypass, but their TCR bypass bits do not
	 * stick (the T8142 PCIe DARTs); those need translation as well.
	 */
	params2 = readl(priv->base + DART_PARAMS2);
	if (params2 & DART_PARAMS2_BYPASS_SUPPORT) {
		for (sid = 0; sid < priv->nsid; sid++) {
			writel(priv->tcr_bypass,
			       priv->base + DART_TCR(priv, sid));
		}
		priv->bypass = 1;
		/*
		 * On some DARTs the bypass bits do not read back. A firmware
		 * client that addresses its buffers physically (the T8142 MTP)
		 * still works through them, while a device U-Boot maps for DMA
		 * (the T8142 PCIe SD reader) does not, so switch to page
		 * tables only when something is mapped.
		 */
		priv->bypass_unverified =
			(readl(priv->base + DART_TCR(priv, 0)) & priv->tcr_bypass) !=
			priv->tcr_bypass;
		return 0;
	}

	return apple_dart_setup_tables(dev);
}

static int apple_dart_setup_tables(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	dma_addr_t addr;
	phys_addr_t l2;
	int ntte, nl1, nl2;
	int sid, i;

	for (sid = 0; sid < priv->nsid; sid++)
		writel(0, priv->base + DART_TCR(priv, sid));

	ntte = DIV_ROUND_UP(priv->dvaend, DART_PAGE_SIZE);
	nl2 = DIV_ROUND_UP(ntte, DART_PAGE_SIZE / sizeof(u64));
	nl1 = DIV_ROUND_UP(nl2, DART_PAGE_SIZE / sizeof(u64));

	priv->l2 = memalign(DART_PAGE_SIZE, nl2 * DART_PAGE_SIZE);
	memset(priv->l2, 0, nl2 * DART_PAGE_SIZE);
	flush_dcache_range((unsigned long)priv->l2,
			   (unsigned long)priv->l2 + nl2 * DART_PAGE_SIZE);

	priv->l1 = memalign(DART_PAGE_SIZE, nl1 * DART_PAGE_SIZE);
	memset(priv->l1, 0, nl1 * DART_PAGE_SIZE);
	l2 = (phys_addr_t)priv->l2;
	for (i = 0; i < nl2; i++) {
		priv->l1[i] = (l2 >> priv->shift) | priv->table_valid;
		l2 += DART_PAGE_SIZE;
	}
	flush_dcache_range((unsigned long)priv->l1,
			   (unsigned long)priv->l1 + nl1 * DART_PAGE_SIZE);

	if (priv->tcr_four_level) {
		/* The first entry covers 64 GiB, which holds all L1 tables */
		priv->l0 = memalign(DART_PAGE_SIZE, DART_PAGE_SIZE);
		if (!priv->l0 || nl1 != 1)
			return -ENOMEM;
		memset(priv->l0, 0, DART_PAGE_SIZE);
		priv->l0[0] = ((phys_addr_t)priv->l1 >> priv->shift) |
			      priv->table_valid;
		flush_dcache_range((unsigned long)priv->l0,
				   (unsigned long)priv->l0 + DART_PAGE_SIZE);
	}

	/* Install page tables. */
	for (sid = 0; sid < priv->nsid; sid++) {
		addr = priv->l0 ? (phys_addr_t)priv->l0 : (phys_addr_t)priv->l1;
		for (i = 0; i < (priv->l0 ? 1 : nl1); i++) {
			writel(addr >> DART_TTBR_SHIFT | priv->ttbr_valid,
			       priv->base + DART_TTBR(priv, sid, i));
			addr += DART_PAGE_SIZE;
		}
	}
	priv->flush_tlb(priv);

	/* Enable all streams. */
	for (i = 0; i < DIV_ROUND_UP(priv->nsid, 32); i++)
		writel(~0, priv->base + DART_SID_ENABLE(priv, i));

	/* Enable translations. */
	for (sid = 0; sid < priv->nsid; sid++) {
		writel(priv->tcr_translate_enable | priv->tcr_four_level,
		       priv->base + DART_TCR(priv, sid));
	}

	return 0;
}

static int apple_dart_remove(struct udevice *dev)
{
	struct apple_dart_priv *priv = dev_get_priv(dev);
	int sid, i;

	/* Bypass leaves no tables behind that would outlive U-Boot. */
	if (priv->retain_bypass) {
		io_lmb_teardown(&priv->io_lmb);
		return 0;
	}

	/* Disable translations. */
	for (sid = 0; sid < priv->nsid; sid++)
		writel(0, priv->base + DART_TCR(priv, sid));

	/* Remove page tables. */
	for (sid = 0; sid < priv->nsid; sid++) {
		for (i = 0; i < priv->nttbr; i++)
			writel(0, priv->base + DART_TTBR(priv, sid, i));
	}
	priv->flush_tlb(priv);

	io_lmb_teardown(&priv->io_lmb);

	return 0;
}

int apple_dart_retain_bypass(struct udevice *dev)
{
	struct apple_dart_priv *priv;

	if (!dev || dev->driver != DM_DRIVER_GET(apple_dart) ||
	    !device_active(dev))
		return -ENODEV;

	priv = dev_get_priv(dev);
	if (!priv->bypass)
		return -EINVAL;

	priv->retain_bypass = true;
	return 0;
}

void apple_dart_release_bypass(struct udevice *dev)
{
	if (dev && dev->driver == DM_DRIVER_GET(apple_dart) && device_active(dev))
		((struct apple_dart_priv *)dev_get_priv(dev))->retain_bypass = false;
}

static const struct udevice_id apple_dart_ids[] = {
	{ .compatible = "apple,t8103-dart" },
	{ .compatible = "apple,t6000-dart" },
	{ .compatible = "apple,t8110-dart" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_dart) = {
	.name = "apple_dart",
	.id = UCLASS_IOMMU,
	.of_match = apple_dart_ids,
	.priv_auto = sizeof(struct apple_dart_priv),
	.ops = &apple_dart_ops,
	.probe = apple_dart_probe,
	.remove = apple_dart_remove,
	.flags	= DM_FLAG_OS_PREPARE | DM_FLAG_VITAL
};
