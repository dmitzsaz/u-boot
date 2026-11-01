// SPDX-License-Identifier: GPL-2.0+
/*
 * J704 (MacBook Pro M5, T8142) built-in SD card reader.
 *
 * The reader is a Genesys GL9755 SDHCI endpoint behind APCIe port 2. The
 * bring-up below follows the verified J704 sequence: PMGR gates, common
 * PHY, port 2 lane clocks and RUN, root port
 * tunables, reader power through the board's SMC key gP19, PERST# release and
 * link training. The endpoint is then configured through ECAM by hand and
 * driven as a plain SDHCI host with 64-bit ADMA2; its DMA goes through port 2's
 * DART with owned identity tables (PIO if that DART is not dormant).
 *
 * Port 2 stays powered and linked for the OS, whose host driver adopts a
 * bootloader-linked root port. Before handing over, the controller's clock
 * and card power are switched off, the DMA path is returned to the dormant
 * state Linux checks, and the root port's bus numbers and memory window are
 * cleared, so the OS enumerates both ports as on a cold boot.
 *
 * Port 2's register bank is powered down until its lane clocks run; touching
 * it earlier raises an asynchronous SError. The order below must be kept.
 */

#include <command.h>
#include <cpu_func.h>
#include <dm.h>
#include <env.h>
#include <lmb.h>
#include <mailbox.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <pci.h>
#include <sdhci.h>
#include <time.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <dm/ofnode.h>
#include <dm/root.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch/j704_sd.h>
#include <asm/arch/rtkit.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/sizes.h>

#include "j704_pcie_data.h"

/* ADT windows plus 0x210000000, as used by the Linux staging modules. */
#define J704_PMGR		0x380700000UL
#define J704_RC			0x48c000000UL	/* RC common */
#define J704_GLUE		0x48f000000UL	/* PHY glue */
#define J704_REFCLK		(J704_GLUE + 0x4000)
#define J704_LANE2		(J704_GLUE + 0x18000)
#define J704_PHY		(J704_GLUE + 0x20000)
#define J704_PHY_IP		0x48f040000UL
#define J704_AXI		0x48e000000UL
#define J704_PORT2		0x48a028000UL
#define J704_GPIO0		0x39a000000UL
#define J704_PERST_PIN		194
#define J704_CLKREQ_PIN		166

/* ECAM: root port 2 is bus 0 device 2; the reader gets bus 2, as under Linux. */
#define J704_ECAM		0x1cb0000000UL
#define J704_ROOT2		(J704_ECAM + (2 << 15))
#define J704_READER_BUS		2
#define J704_READER		(J704_ECAM + (J704_READER_BUS << 20))
#define J704_READER_ID		0x975517a0	/* 17a0:9755 */

/* PCI memory window: bus address 0x80000000 is CPU 0xb80000000. */
#define J704_PCI_MEM		0x80000000U
#define J704_PCI_MEM_CPU	0xb80000000UL

#define PORT_LTSSMCTL		0x080
#define PORT_LINKSTS		0x208
#define PORT_APPCLK		0x800
#define PORT_STATUS		0x804
#define PORT_RID2SID		0x3000	/* indexed by SID on T8142 */
#define  PORT_RID2SID_VALID	BIT(31)

/*
 * Port 2's DART (T8110 layout, v2.4, 19 streams, 16 KiB pages, 42-bit IOVA and
 * PA). Linux maps the reader as RID 0x200 -> SID 1 with four-level tables
 * (TCR 9). Its bypass bits read back as zero on this machine, so U-Boot uses
 * the same four-level format with an identity map of RAM: SDHCI's ADMA hands
 * physical addresses to the controller unchanged.
 */
#define J704_DART2		0x48a000000UL
#define J704_DART2_ID		0x2a2a0204
#define J704_READER_SID		1
#define J704_READER_RID		(J704_READER_BUS << 8)
#define DART_PARAMS3		0x008
#define DART_PARAMS4		0x00c
#define DART_TLB_CMD		0x080
#define  DART_TLB_CMD_BUSY	BIT(31)
#define  DART_TLB_CMD_V2	BIT(15)
#define  DART_TLB_CMD_FLUSH_SID	(1 << 8)
#define DART_ERROR		0x100
#define  DART_ERROR_FLAG	BIT(31)
#define DART_ERROR_STREAMS	0x1c0
#define DART_BLOCK_ERROR	0x704
#define DART_EXCEPTION0		0x4000
#define DART_PROTECT		0x200
#define DART_ENABLE_STREAMS	0xc00
#define DART_DISABLE_STREAMS	0xc20
#define DART_TCR(sid)		(0x1000 + 4 * (sid))
#define  DART_TCR_TRANSLATE	BIT(0)
#define  DART_TCR_FOUR_LEVEL	BIT(3)
#define DART_TTBR(sid)		(0x1400 + 4 * (sid))
#define  DART_TTBR_VALID	BIT(0)

/* DART v2 tables: 16 KiB, 2048 eight-byte entries, 11 index bits per level. */
#define DART_PAGE_SHIFT		14
#define DART_TABLE_SIZE		SZ_16K
#define DART_PTE_VALID		BIT_ULL(0)
#define DART_PTE_SUBPAGE_END	GENMASK_ULL(51, 40)
#define DART_PTE_PADDR(pa)	(((u64)(pa) >> 4) & GENMASK_ULL(37, 10))
/* Linux io-pgtable-dart: level 1 is the leaf (bits 24:14), 2 bits 35:25, 3 bits 46:36. */
#define DART_INDEX(pa, level)	(((u64)(pa) >> (3 + 11 * (level))) & 0x7ff)

DECLARE_GLOBAL_DATA_PTR;

/* GL9755 vendor configuration (Linux sdhci-pci-gli.c). */
#define GL9755_PECONF		0x44
#define  GL9755_LFCLK		GENMASK(14, 12)
#define  GL9755_DMACLK		BIT(29)
#define  GL9755_INVERT_CD	BIT(30)
#define  GL9755_INVERT_WP	BIT(31)
#define GL9755_PLL		0x64
#define  GL9755_PLL_DIR		BIT(15)
#define  GL9755_PLLSSC_EN	BIT(31)
#define GL9755_WT		0x800
#define  GL9755_WT_EN		BIT(0)

/* SMC application protocol (Linux macsmc, m1n1 smc.c). */
#define SMC_EP			0x20
#define SMC_MSG_TYPE		GENMASK_ULL(7, 0)
#define SMC_MSG_ID		GENMASK_ULL(15, 12)
#define SMC_MSG_SIZE		GENMASK_ULL(23, 16)
#define SMC_MSG_KEY		GENMASK_ULL(63, 32)
#define SMC_WRITE_KEY		0x11
#define SMC_GET_KEY_INFO	0x13
#define SMC_INITIALIZE		0x17
#define SMC_NOTIFICATION	0x18
#define SMC_KEY_SD_POWER	0x67503139	/* "gP19": board SD power enable */
#define SMC_TIMEOUT_US		1000000

static struct udevice *j704_sd_dev;
static bool j704_sd_dma;
static bool j704_sd_started;
static u32 j704_sd_tcr;		/* the reader stream's TCR before U-Boot */
static phys_addr_t j704_dart_tables;	/* L3, L2 and leaf tables, one block */
static phys_size_t j704_dart_size;

static void j704_change(ulong reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static void j704_table(ulong base, const struct j704_pcie_table *table)
{
	for (unsigned int i = 0; i < table->count; i++) {
		const fdt32_t *row = table->words + 3 * i;
		j704_change(base + fdt32_to_cpu(row[0]), fdt32_to_cpu(row[1]),
			    fdt32_to_cpu(row[2]));
	}
}

static int j704_wait(ulong reg, u32 bit, ulong timeout_us, const char *what)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(reg, v, v & bit, timeout_us);
	if (ret)
		printf("J704 SD: %s not ready (%#x)\n", what, v);
	return ret;
}

/* Shared PMGR gates, with the same bounded transition as Aura. */
static int j704_sd_power(void)
{
	static const u16 gates[] = {
		0x300, 0x2f0, 0x408, 0x410, 0x520, 0x560, 0x578, 0x580,
	};
	ulong reg;
	u32 v;
	int i, ret;

	v = readl(J704_PMGR + 0x280);
	if ((v & 0xff) != 0xff) {
		printf("J704 SD: PMGR 0x280 is not active (%#x)\n", v);
		return -EIO;
	}
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		reg = J704_PMGR + gates[i];
		v = readl(reg);
		if (((v >> 4) & 0xf) == 0xf && !(v & BIT(28)))
			continue;
		j704_change(reg, BIT(28) | BIT(9) | BIT(8) | GENMASK(3, 0), 0xf);
		ret = readl_poll_timeout(reg, v, ((v >> 4) & 0xf) == 0xf, 10000);
		if (ret) {
			printf("J704 SD: PMGR %#x refused active (%#x)\n",
			       gates[i], v);
			return ret;
		}
	}
	return 0;
}

/* SD and ANS share these gates; complete the existing producer only once. */
int j704_pcie_prepare_power(void)
{
	static bool attempted;
	static int result;

	if (!of_machine_is_compatible("apple,j704") ||
	    !of_machine_is_compatible("apple,t8142"))
		return -ENODEV;
	if (!attempted) {
		attempted = true;
		result = j704_sd_power();
	}
	return result;
}

/* Common PHY initialization (pcie-cold); skipped once it already runs. */
static int j704_sd_phy(void)
{
	int ret;

	if ((readl(J704_RC + 0x58) & BIT(0)) && (readl(J704_PHY + 8) & BIT(0)))
		return 0;

	j704_table(J704_AXI, &j704_pcie_tables[J704_TABLE_AXI]);
	writel(0, J704_RC + 4);
	j704_table(J704_GLUE, &j704_pcie_tables[J704_TABLE_PHY]);
	ret = j704_wait(J704_REFCLK, BIT(31), 250000, "PHY reference clock");
	if (ret)
		return ret;

	j704_change(J704_PHY, 0, BIT(0));
	ret = j704_wait(J704_PHY, BIT(2), 50000, "PHY clock 0");
	if (ret)
		return ret;
	udelay(1);
	j704_change(J704_PHY, 0, BIT(1));
	ret = j704_wait(J704_PHY, BIT(3), 50000, "PHY clock 1");
	if (ret)
		return ret;
	j704_change(J704_PHY, BIT(4), 0);
	udelay(1);
	j704_change(J704_PHY + 4, 0, BIT(0));
	j704_table(J704_PHY_IP, &j704_pcie_tables[J704_TABLE_PLL]);
	j704_table(J704_PHY_IP, &j704_pcie_tables[J704_TABLE_AUSPMA]);
	j704_change(J704_PHY_IP + 0x90, BIT(0), 0);
	j704_change(J704_PHY + 4, 0, BIT(4));
	ret = j704_wait(J704_PHY + 8, BIT(0), 250000, "PHY");
	if (ret)
		return ret;
	j704_change(J704_REFCLK, BIT(0), BIT(0));
	j704_change(J704_PHY, BIT(27), BIT(27));

	writel(0x140, J704_RC + 0x54);
	j704_change(J704_RC + 0x50, 0, BIT(0));
	return j704_wait(J704_RC + 0x58, BIT(0), 250000, "PCIe RC");
}

/* Port 2 lane clocks and register-bank RUN. */
static int j704_sd_port(void)
{
	/* The port-0 stage's traced reset defaults; all port banks share them. */
	static const struct { u16 off; u32 val; } reset[] = {
		{ 0x88, 0x110 }, { 0x80, 0 }, { 0x84, 0 }, { 0x104, 0xfffffff0 },
		{ 0x124, 0x100 }, { 0x16c, 0 }, { 0x13c, 0 }, { 0x800, 0x100100 },
		{ 0x808, 0x1001ff }, { 0x82c, 0x10000 }, { 0x130, 0x3020000 },
		{ 0x140, 0x10 }, { 0x144, 0x253770 }, { 0x21c, 0 }, { 0x834, 0 },
		{ 0x83c, 0 },
	};

	int i, ret;

	if (!(readl(J704_RC + 0x58) & BIT(0)) ||
	    (readl(J704_PHY) & 0x1f) != 0xf ||
	    (readl(J704_PHY + 4) & 0x11) != 0x11) {
		printf("J704 SD: shared PHY is not running\n");
		return -EIO;
	}

	j704_change(J704_LANE2, 0, BIT(0));
	ret = j704_wait(J704_LANE2, BIT(2), 50000, "port 2 lane clock 0");
	if (ret)
		return ret;
	j704_change(J704_LANE2, 0, BIT(1));
	ret = j704_wait(J704_LANE2, BIT(3), 50000, "port 2 lane clock 1");
	if (ret)
		return ret;
	j704_change(J704_LANE2, BIT(4), 0);
	udelay(1);
	j704_change(J704_LANE2, 0, BIT(9) | BIT(10));

	/* The bank answers from here on. A second run finds it linked. */
	if (readl(J704_PORT2 + PORT_LINKSTS) & BIT(0)) {
		if (readl(J704_PORT2 + PORT_STATUS) & BIT(0))
			return 0;
		printf("J704 SD: port 2 linked but not running\n");
		return -EBUSY;
	}
	for (i = 0; i < 16; i++) {
		if (readl(J704_PORT2 + PORT_RID2SID + 4 * i)) {
			printf("J704 SD: port 2 RID2SID %d populated\n", i);
			return -EBUSY;
		}
	}
	for (i = 0; i < ARRAY_SIZE(reset); i++)
		writel(reset[i].val, J704_PORT2 + reset[i].off);
	j704_table(J704_PORT2, &j704_pcie_tables[J704_TABLE_CONFIG]);
	j704_change(J704_PORT2 + PORT_APPCLK, 0, BIT(0));
	j704_change(J704_PORT2 + 0x82c, 0, BIT(0));
	return j704_wait(J704_PORT2 + PORT_STATUS, BIT(0), 250000, "port 2 RUN");
}

/* The SMC keeps its RTKit buffers in its own SRAM, as Linux macsmc requires. */
static int j704_smc_shmem_setup(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct resource *sram = cookie;

	if (!buf->dva || buf->dva < sram->start ||
	    buf->dva + buf->size - 1 > sram->end)
		return -EINVAL;
	buf->buffer = map_sysmem(buf->dva, buf->size);
	return 0;
}

static void j704_smc_shmem_destroy(void *cookie, struct apple_rtkit_buffer *buf)
{
	buf->buffer = NULL;
}

static int j704_smc_cmd(struct apple_rtkit *rtk, u64 msg, u8 id, u64 *reply)
{
	u64 rx;
	int ret;

	ret = apple_rtkit_send_app(rtk, SMC_EP, msg | FIELD_PREP(SMC_MSG_ID, id));
	if (ret)
		return ret;
	for (;;) {
		ret = apple_rtkit_recv_app(rtk, SMC_EP, &rx, SMC_TIMEOUT_US);
		if (ret)
			return ret;
		if (FIELD_GET(SMC_MSG_TYPE, rx) == SMC_NOTIFICATION)
			continue;
		if (FIELD_GET(SMC_MSG_ID, rx) == id)
			break;
	}
	if (reply)
		*reply = rx;
	return FIELD_GET(SMC_MSG_TYPE, rx) ? -EIO : 0;
}

/*
 * Write one 32-bit SMC key, then leave the SMC quiesced as m1n1 does; the OS
 * wakes it again with its own RTKit handshake.
 */
static int j704_smc_write_u32(u32 key, u32 value)
{
	static struct mbox_chan chan;
	static struct resource sram;
	struct ofnode_phandle_args args;
	struct apple_rtkit *rtk;
	ulong shmem;
	u64 msg;
	u8 id = 0;
	int i, ret, ret2;
	ofnode smc;

	smc = ofnode_path("/soc/smc@38c600000");
	if (!ofnode_valid(smc) ||
	    ofnode_read_resource_byname(smc, "sram", &sram))
		return -ENODEV;
	ret = ofnode_parse_phandle_with_args(smc, "mboxes", "#mbox-cells", 0, 0,
					     &args);
	if (ret)
		return ret;
	ret = uclass_get_device_by_ofnode(UCLASS_MAILBOX, args.node, &chan.dev);
	if (ret)
		return ret;
	chan.id = 0;

	rtk = apple_rtkit_init(&chan, &sram, j704_smc_shmem_setup,
			       j704_smc_shmem_destroy);
	if (!rtk)
		return -ENOMEM;
	ret = apple_rtkit_boot(rtk);
	if (ret) {
		printf("J704 SD: SMC RTKit boot failed (%d)\n", ret);
		goto out;
	}
	/* Linux apple_rtkit_boot ends the same way before talking to the SMC. */
	ret = apple_rtkit_set_ap_power(rtk, APPLE_RTKIT_PWR_STATE_ON);
	if (ret) {
		printf("J704 SD: SMC AP power on failed (%d)\n", ret);
		goto quiesce;
	}

	/* The first SMC message after INITIALIZE is the shared buffer address. */
	ret = apple_rtkit_send_app(rtk, SMC_EP,
				   FIELD_PREP(SMC_MSG_TYPE, SMC_INITIALIZE) |
				   FIELD_PREP(SMC_MSG_ID, id++));
	if (!ret)
		ret = apple_rtkit_recv_app(rtk, SMC_EP, &msg, SMC_TIMEOUT_US);
	if (ret)
		goto quiesce;
	shmem = msg;
	if (shmem < sram.start || shmem + 8 - 1 > sram.end) {
		printf("J704 SD: SMC buffer %#lx outside its SRAM\n", shmem);
		ret = -EFAULT;
		goto quiesce;
	}

	/* The board key must exist with a 32-bit value. */
	ret = j704_smc_cmd(rtk, FIELD_PREP(SMC_MSG_TYPE, SMC_GET_KEY_INFO) |
			   FIELD_PREP(SMC_MSG_KEY, key), id++, NULL);
	if (ret || readb(shmem) != sizeof(value)) {
		printf("J704 SD: SMC key info failed (%d, size %u)\n", ret,
		       ret ? 0 : readb(shmem));
		ret = ret ?: -EINVAL;
		goto quiesce;
	}

	for (i = 0; i < sizeof(value); i++)
		writeb(value >> (8 * i), shmem + i);
	ret = j704_smc_cmd(rtk, FIELD_PREP(SMC_MSG_TYPE, SMC_WRITE_KEY) |
			   FIELD_PREP(SMC_MSG_SIZE, sizeof(value)) |
			   FIELD_PREP(SMC_MSG_KEY, key), id++, NULL);
	if (ret)
		printf("J704 SD: SMC key write failed (%d)\n", ret);

quiesce:
	ret2 = apple_rtkit_shutdown(rtk, APPLE_RTKIT_PWR_STATE_QUIESCED);
	if (ret2)
		printf("J704 SD: SMC quiesce failed (%d)\n", ret2);
	ret = ret ?: ret2;
out:
	apple_rtkit_free(rtk);
	return ret;
}

/* Root port configuration, reader power, PERST# and link (sd-link). */
static int j704_sd_link(void)
{
	ulong cfg = J704_ROOT2;
	ulong perst = J704_GPIO0 + 4 * J704_PERST_PIN;
	u32 id, class, cap, v;
	int ret;

	id = readl(cfg + PCI_VENDOR_ID);
	class = readl(cfg + PCI_CLASS_REVISION);
	cap = readl(cfg + 0x7c);
	if ((id & 0xffff) != 0x106b || (class >> 16) != 0x604 || !(cap & 0x3f0)) {
		printf("J704 SD: unexpected root port 2 (%#x %#x %#x)\n",
		       id, class, cap);
		return -ENODEV;
	}
	if (readl(J704_PORT2 + PORT_LINKSTS) & BIT(0))
		return 0;
	if ((readl(perst) & 0x7f) != 2) {
		printf("J704 SD: PERST# is not asserted (%#x)\n", readl(perst));
		return -EBUSY;
	}

	j704_change(cfg + 0x8bc, 0, 1);
	j704_table(cfg, &j704_pcie_tables[J704_TABLE_ROOT]);
	j704_table(cfg, &j704_pcie_tables[J704_TABLE_GEN3]);
	j704_table(cfg, &j704_pcie_tables[J704_TABLE_GEN4]);
	j704_change(cfg + 0x7c, 0xf, 2);
	j704_change(cfg + 0x9c, 0x7e, 6);
	writew((readw(cfg + 0xa0) & ~0xf) | 2, cfg + 0xa0);
	j704_change(cfg + 0x710, 0x3f0000, 0x10000);
	j704_change(cfg + 0x80c, 0x1f00, 0x100);
	/* The reader's ADT initial-link-speed is 1: train at Gen1 first. */
	j704_change(cfg + 0x7c, 0x3f0, 0x10);
	j704_change(cfg + 0x80c, 0, BIT(17));
	j704_change(cfg + 0x8bc, 1, 0);
	/* Board CLKREQ# peripheral mux and Apple APPCLK policy, as on port 0. */
	j704_change(J704_GPIO0 + 4 * J704_CLKREQ_PIN, 0x60, 0x40 | BIT(9));
	j704_change(J704_PORT2 + PORT_APPCLK, BIT(8), 0);

	ret = j704_smc_write_u32(SMC_KEY_SD_POWER, 1);
	if (ret)
		return ret;
	mdelay(150);
	j704_change(perst, 0, 1);
	mdelay(100);
	writel(1, J704_PORT2 + PORT_LTSSMCTL);
	ret = readl_poll_sleep_timeout(J704_PORT2 + PORT_LINKSTS, v, v & BIT(0),
				       1000, 1000000);
	if (ret)
		printf("J704 SD: no link (%#x)\n", v);
	return ret;
}

static void j704_gl9755_write_enable(ulong ep, bool on)
{
	j704_change(ep + GL9755_WT, GL9755_WT_EN, on ? GL9755_WT_EN : 0);
}

/* Root port window, reader BAR0 and the GL9755 connector configuration. */
static int j704_sd_endpoint(void)
{
	ulong root = J704_ROOT2, ep = J704_READER;
	u32 v, size;

	/* Primary 0, secondary = subordinate = the reader's bus. */
	writel((readl(root + PCI_PRIMARY_BUS) & 0xff000000) |
	       (J704_READER_BUS << 16) | (J704_READER_BUS << 8),
	       root + PCI_PRIMARY_BUS);
	writel((J704_PCI_MEM >> 16) | (J704_PCI_MEM & 0xfff00000),
	       root + PCI_MEMORY_BASE);
	writel(0x0000fff0, root + PCI_PREF_MEMORY_BASE);
	writel(0, root + PCI_PREF_BASE_UPPER32);
	writel(0, root + PCI_PREF_LIMIT_UPPER32);
	writew(0x00f0, root + PCI_IO_BASE);
	writew(readw(root + PCI_COMMAND) | PCI_COMMAND_MEMORY, root + PCI_COMMAND);

	v = readl(ep + PCI_VENDOR_ID);
	if (v != J704_READER_ID) {
		printf("J704 SD: unexpected endpoint %#x\n", v);
		return -ENODEV;
	}
	v = readl(ep + PCI_BASE_ADDRESS_0);
	if (v & PCI_BASE_ADDRESS_SPACE_IO)
		return -ENODEV;
	writel(0xffffffff, ep + PCI_BASE_ADDRESS_0);
	size = ~(readl(ep + PCI_BASE_ADDRESS_0) & PCI_BASE_ADDRESS_MEM_MASK) + 1;
	if (!size || size > SZ_1M) {
		printf("J704 SD: unexpected BAR0 size %#x\n", size);
		return -ENODEV;
	}
	writel(J704_PCI_MEM, ep + PCI_BASE_ADDRESS_0);
	if (v & PCI_BASE_ADDRESS_MEM_TYPE_64)
		writel(0, ep + PCI_BASE_ADDRESS_1);
	writew(readw(ep + PCI_COMMAND) | PCI_COMMAND_MEMORY, ep + PCI_COMMAND);

	/*
	 * Apple wires the connector with CD and WP polarity 1, applied as the
	 * GL9755's own inversions (Linux gl9755_hw_setting with cd-inverted/
	 * wp-inverted). Spread-spectrum PLL off: the divided base clock is used.
	 */
	j704_gl9755_write_enable(ep, true);
	j704_change(ep + GL9755_PECONF, GL9755_LFCLK | GL9755_DMACLK,
		    GL9755_INVERT_CD | GL9755_INVERT_WP);
	j704_change(ep + GL9755_PLL, GL9755_PLL_DIR | GL9755_PLLSSC_EN, 0);
	j704_gl9755_write_enable(ep, false);
	return 0;
}

static int j704_dart_flush(void)
{
	ulong cmd = J704_DART2 + DART_TLB_CMD;
	u32 v;
	int ret;

	ret = readl_poll_timeout(cmd, v, !(v & DART_TLB_CMD_BUSY), 2000);
	if (!ret) {
		writel(DART_TLB_CMD_V2 | DART_TLB_CMD_FLUSH_SID | J704_READER_SID, cmd);
		ret = readl_poll_timeout(cmd, v, !(v & DART_TLB_CMD_BUSY), 2000);
	}
	if (ret)
		printf("J704 SD: DART2 TLB flush timed out (%#x)\n", v);
	return ret;
}

/* Identity-map RAM bank 0 for the reader's stream in four-level DART tables. */
static int j704_dart_map_ram(void)
{
	phys_addr_t base = ALIGN_DOWN(gd->bd->bi_dram[0].start, DART_TABLE_SIZE);
	phys_addr_t end = gd->bd->bi_dram[0].start + gd->bd->bi_dram[0].size;
	phys_addr_t pa, leaf;
	unsigned int leaves;
	u64 *l3, *l2, *pte;
	int ret;

	/* All of RAM sits under one level-3 entry; one leaf table per 32 MiB. */
	if (DART_INDEX(base, 3) != DART_INDEX(end - 1, 3))
		return -ERANGE;
	leaves = DART_INDEX(end - 1, 2) - DART_INDEX(base, 2) + 1;
	j704_dart_size = (2 + leaves) * DART_TABLE_SIZE;
	ret = lmb_alloc_mem(LMB_MEM_ALLOC_ANY, DART_TABLE_SIZE, &j704_dart_tables,
			    j704_dart_size, LMB_NONE);
	if (ret)
		return ret;
	l3 = map_sysmem(j704_dart_tables, j704_dart_size);
	l2 = l3 + DART_TABLE_SIZE / sizeof(u64);
	memset(l3, 0, j704_dart_size);

	l3[DART_INDEX(base, 3)] = DART_PTE_PADDR(j704_dart_tables + DART_TABLE_SIZE) |
				  DART_PTE_VALID;
	for (pa = base; pa < end; pa += 1UL << DART_PAGE_SHIFT) {
		leaf = j704_dart_tables + (2 + DART_INDEX(pa, 2) - DART_INDEX(base, 2)) *
		       DART_TABLE_SIZE;
		l2[DART_INDEX(pa, 2)] = DART_PTE_PADDR(leaf) | DART_PTE_VALID;
		pte = map_sysmem(leaf, DART_TABLE_SIZE);
		pte[DART_INDEX(pa, 1)] = DART_PTE_SUBPAGE_END | DART_PTE_PADDR(pa) |
					 DART_PTE_VALID;
	}
	flush_dcache_range(j704_dart_tables, j704_dart_tables + j704_dart_size);
	return 0;
}

/*
 * Let the reader master memory: DART2 stream 1 translating through the
 * identity map, port 2's RID-to-SID entry for the reader, bus mastering on the
 * root port and the endpoint. Only a dormant DART2 (as found at power-on and as
 * Linux's port 2 description requires) is touched; otherwise PIO is used.
 */
static bool j704_sd_dma_enable(void)
{
	ulong dart = J704_DART2, root = J704_ROOT2, ep = J704_READER;
	ulong rid2sid = J704_PORT2 + PORT_RID2SID + 4 * J704_READER_SID;
	u32 entry = PORT_RID2SID_VALID | (J704_READER_SID << 16) | J704_READER_RID;
	u32 tcr = DART_TCR_TRANSLATE | DART_TCR_FOUR_LEVEL, ttbr;

	/* Same dormancy test as Linux's port 2 description: TCR bit 0 may be set. */
	j704_sd_tcr = readl(dart + DART_TCR(J704_READER_SID));
	if (readl(dart + DART_PARAMS3) != J704_DART2_ID ||
	    (readl(dart + DART_PARAMS4) & 0x1ff) != 19 ||
	    readl(dart + DART_PROTECT) & 1 || readl(dart + DART_ENABLE_STREAMS) ||
	    readl(dart + DART_ERROR) & DART_ERROR_FLAG || j704_sd_tcr & ~1u ||
	    readl(dart + DART_TTBR(J704_READER_SID)) & 1 || readl(rid2sid)) {
		printf("J704 SD: DART2 is not dormant (id %#x lock %#x enable %#x error %#x tcr %#x ttbr %#x rid2sid %#x); using PIO\n",
		       readl(dart + DART_PARAMS3), readl(dart + DART_PROTECT),
		       readl(dart + DART_ENABLE_STREAMS), readl(dart + DART_ERROR),
		       j704_sd_tcr, readl(dart + DART_TTBR(J704_READER_SID)),
		       readl(rid2sid));
		return false;
	}
	if (j704_dart_map_ram()) {
		printf("J704 SD: no memory for DART2 tables; using PIO\n");
		return false;
	}

	ttbr = DART_TTBR_VALID | (u32)(j704_dart_tables >> DART_PAGE_SHIFT) << 2;
	writel(ttbr, dart + DART_TTBR(J704_READER_SID));
	writel(tcr, dart + DART_TCR(J704_READER_SID));
	writel(BIT(J704_READER_SID), dart + DART_ENABLE_STREAMS);
	writel(entry, rid2sid);
	if (readl(dart + DART_TTBR(J704_READER_SID)) != ttbr ||
	    readl(dart + DART_TCR(J704_READER_SID)) != tcr ||
	    readl(rid2sid) != entry || j704_dart_flush()) {
		printf("J704 SD: DART2 setup did not take (tcr %#x ttbr %#x rid2sid %#x); using PIO\n",
		       readl(dart + DART_TCR(J704_READER_SID)),
		       readl(dart + DART_TTBR(J704_READER_SID)), readl(rid2sid));
		writel(0, rid2sid);
		writel(BIT(J704_READER_SID), dart + DART_DISABLE_STREAMS);
		writel(j704_sd_tcr, dart + DART_TCR(J704_READER_SID));
		writel(0, dart + DART_TTBR(J704_READER_SID));
		lmb_free(j704_dart_tables, j704_dart_size, LMB_NONE);
		return false;
	}
	writew(readw(root + PCI_COMMAND) | PCI_COMMAND_MASTER, root + PCI_COMMAND);
	writew(readw(ep + PCI_COMMAND) | PCI_COMMAND_MASTER, ep + PCI_COMMAND);
	return true;
}

/* Undo j704_sd_dma_enable(): the dormant state Linux's description checks. */
static void j704_sd_dma_disable(void)
{
	ulong dart = J704_DART2;
	u32 error, streams, exception;

	writew(readw(J704_READER + PCI_COMMAND) & ~PCI_COMMAND_MASTER,
	       J704_READER + PCI_COMMAND);
	writew(readw(J704_ROOT2 + PCI_COMMAND) & ~PCI_COMMAND_MASTER,
	       J704_ROOT2 + PCI_COMMAND);
	writel(0, J704_PORT2 + PORT_RID2SID + 4 * J704_READER_SID);
	writel(BIT(J704_READER_SID), dart + DART_DISABLE_STREAMS);
	writel(j704_sd_tcr, dart + DART_TCR(J704_READER_SID));
	writel(0, dart + DART_TTBR(J704_READER_SID));
	j704_dart_flush();
	/* Acknowledge any fault the way Linux's T8110 handler does. */
	error = readl(dart + DART_ERROR);
	streams = readl(dart + DART_ERROR_STREAMS);
	exception = readl(dart + DART_EXCEPTION0);
	if (error & DART_ERROR_FLAG)
		writel(error, dart + DART_ERROR);
	if (streams)
		writel(U32_MAX, dart + DART_ERROR_STREAMS);
	if (exception)
		writel(exception, dart + DART_EXCEPTION0);
	readl(dart + DART_BLOCK_ERROR);
	if (error & DART_ERROR_FLAG || streams || exception)
		printf("J704 SD: DART2 reported error %#x streams %#x exception %#x\n",
		       error, streams, exception);
	lmb_free(j704_dart_tables, j704_dart_size, LMB_NONE);
}

struct j704_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct j704_sdhci_priv {
	struct sdhci_host host;
};

/* Like Linux sdhci_gli_readw/readb: GL975x registers are read as words. */
static u16 j704_sdhci_readw(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> (8 * (reg & 2));
}

static u8 j704_sdhci_readb(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> (8 * (reg & 3));
}

static const struct sdhci_ops j704_sdhci_ops = {
	.read_w = j704_sdhci_readw,
	.read_b = j704_sdhci_readb,
};

static int j704_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct j704_sdhci_plat *plat = dev_get_plat(dev);
	struct j704_sdhci_priv *priv = dev_get_priv(dev);
	struct sdhci_host *host = &priv->host;
	u16 clk;
	int ret;

	host->name = dev->name;
	host->ioaddr = map_sysmem(J704_PCI_MEM_CPU, SZ_4K);
	host->ops = &j704_sdhci_ops;
	host->mmc = &plat->mmc;
	host->mmc->dev = dev;
	host->mmc->priv = host;
	upriv->mmc = host->mmc;

	/* The GL9755 completes a full reset only with its internal clock on. */
	sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);
	ret = read_poll_timeout(sdhci_readw, clk, clk & SDHCI_CLOCK_INT_STABLE,
				10, 20000, host, SDHCI_CLOCK_CONTROL);
	if (ret)
		return ret;

	/* Up to 50 MHz high speed, 3.3 V signalling; ADMA2 64-bit when DART2 allows. */
	ret = sdhci_setup_cfg(&plat->cfg, host, 50000000, 400000);
	if (ret)
		return ret;
	if (!j704_sd_dma)
		host->flags &= ~USE_DMA;
	return sdhci_probe(dev);
}

static int j704_sdhci_bind(struct udevice *dev)
{
	struct j704_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

U_BOOT_DRIVER(j704_sdhci) = {
	.name		= "j704_sdhci",
	.id		= UCLASS_MMC,
	.ops		= &sdhci_ops,
	.bind		= j704_sdhci_bind,
	.probe		= j704_sdhci_probe,
	.priv_auto	= sizeof(struct j704_sdhci_priv),
	.plat_auto	= sizeof(struct j704_sdhci_plat),
};

static int j704_sd_init(void)
{
	ulong start = get_timer(0);
	u32 present;
	int ret;

	if (!of_machine_is_compatible("apple,j704") ||
	    !of_machine_is_compatible("apple,t8142"))
		return -ENODEV;
	if (j704_sd_dev)
		goto card;
	if (j704_sd_started)
		return -EBUSY;

	ret = j704_pcie_load_tables(gd->fdt_blob);
	if (ret)
		return ret;
	/* Never replay a partially completed hardware startup. */
	j704_sd_started = true;
	ret = j704_pcie_prepare_power();
	if (!ret)
		ret = j704_sd_phy();
	if (!ret)
		ret = j704_sd_port();
	if (!ret)
		ret = j704_sd_link();
	if (!ret) {
		/* PCIe asks for 100 ms between link-up and configuration. */
		mdelay(100);
		ret = j704_sd_endpoint();
	}
	if (ret)
		return ret;
	j704_sd_dma = j704_sd_dma_enable();

	ret = device_bind_driver(dm_root(), "j704_sdhci", "j704-sd", &j704_sd_dev);
	if (!ret)
		ret = device_probe(j704_sd_dev);
	if (ret) {
		printf("J704 SD: SDHCI host failed (%d)\n", ret);
		if (j704_sd_dev)
			device_unbind(j704_sd_dev);
		j704_sd_dev = NULL;
		if (j704_sd_dma)
			j704_sd_dma_disable();
		j704_sd_dma = false;
		return ret;
	}

card:
	/*
	 * After the CD inversion is set the GL9755 reports "stable, no card"
	 * and debounces for about 1.1-1.25 s (measured) before it sees a seated
	 * card. Wait for the card itself; an empty slot costs three seconds.
	 */
	readl_poll_timeout(J704_PCI_MEM_CPU + SDHCI_PRESENT_STATE, present,
			   present & SDHCI_CARD_PRESENT, 3000000);
	printf("J704 SD: GL9755 on port 2 link %#x, %s, %s, %lu ms\n",
	       readl(J704_PORT2 + PORT_LINKSTS), j704_sd_dma ? "ADMA" : "PIO",
	       present & SDHCI_CARD_PRESENT ? "card present" : "no card",
	       get_timer(start));
	return present & SDHCI_CARD_PRESENT ? 0 : -ENOMEDIUM;
}

/*
 * Before the OS starts: stop the SD clock and card power, end the reader's DMA
 * (bus mastering, RID-to-SID entry and DART2 stream back to dormant), close
 * its decode and give root port 2 back its reset bus numbers and window. The
 * link and the reader's power stay up for the OS host driver to adopt.
 */
void j704_sd_quiesce(void)
{
	ulong root = J704_ROOT2, ep = J704_READER;

	if (!j704_sd_dev)
		return;
	device_remove(j704_sd_dev, DM_REMOVE_NORMAL);
	device_unbind(j704_sd_dev);
	j704_sd_dev = NULL;

	writew(0, J704_PCI_MEM_CPU + SDHCI_CLOCK_CONTROL);
	writeb(0, J704_PCI_MEM_CPU + SDHCI_POWER_CONTROL);
	if (j704_sd_dma)
		j704_sd_dma_disable();
	j704_sd_dma = false;
	writew(readw(ep + PCI_COMMAND) & ~PCI_COMMAND_MEMORY, ep + PCI_COMMAND);
	writew(readw(root + PCI_COMMAND) & ~PCI_COMMAND_MEMORY, root + PCI_COMMAND);
	writel(0x0000fff0, root + PCI_MEMORY_BASE);
	writel(readl(root + PCI_PRIMARY_BUS) & 0xff000000, root + PCI_PRIMARY_BUS);
}

void j704_sd_late_init(void)
{
	/*
	 * Boot like a regular distribution install: U-Boot's extlinux support
	 * reads /boot/extlinux/extlinux.conf from the card, whose entry names
	 * /boot/Image and /boot/initramfs-linux.img. The live device tree from
	 * Aura is used (no fdt line), and ${j704_bootargs} carries its /chosen
	 * bootargs so the entry only adds its own options.
	 */
	static const char * const env[][2] = {
		{ "j704_sdpart", "0:1" },
		{ "j704_sdconf", "/boot/extlinux/extlinux.conf" },
		{ "j704_sdboot",
		  "if j704sd && fdt addr ${fdtcontroladdr}; then "
		  "setenv j704_bootargs; "
		  "fdt get value j704_bootargs /chosen bootargs; "
		  "sysboot mmc ${j704_sdpart} any ${scriptaddr} ${j704_sdconf}; fi" },
	};
	const char *conf = ofnode_read_string(ofnode_path("/chosen"),
					    "aurora,sd-extlinux-conf");
	int i;

	if (!strcmp(env_get("bootcmd") ?: "", CONFIG_BOOTCOMMAND))
		env_set("bootcmd", conf ? "run j704_sdboot" :
			"run j704_sdboot; bootflow scan -b");

	/* An explicit image contract must not fall back to another kernel/DT
	 * pair. Keep a saved environment override ahead of the loader choice.
	 */
	if (conf && !env_get("j704_sdconf"))
		env_set("j704_sdconf", conf);

	/* A saved or preboot environment may override any of these. */
	for (i = 0; i < ARRAY_SIZE(env); i++) {
		if (!env_get(env[i][0]))
			env_set(env[i][0], env[i][1]);
	}
}

static int do_j704sd(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	int ret = j704_sd_init();

	if (ret) {
		printf("J704 SD: not available (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(j704sd, 1, 0, do_j704sd,
	   "power and link the J704 SD card reader (GL9755, PCIe port 2)",
	   "");
