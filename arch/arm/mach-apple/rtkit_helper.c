// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <cpu_func.h>
#include <lmb.h>
#include <mailbox.h>
#include <mapmem.h>
#include <reset.h>
#include <linux/libfdt.h>

#include <asm/io.h>
#include <asm/arch/dart.h>
#include <asm/arch/rtkit.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>

/* ASC registers */
#define REG_CPU_CTRL		0x0044
#define  REG_CPU_CTRL_RUN	BIT(4)

#define APPLE_RTKIT_EP_OSLOG 8

/*
 * Value of "apple,rtkit-handoff" on a helper the OS must adopt while it runs:
 * T8140 MTP firmware cannot be restarted after QUIESCED or SLEEP.
 */
#define RTKIT_HELPER_HANDOFF	"mtp-live-v1"
#define RTKIT_HELPER_MAX_BUFS	8

struct rtkit_helper_buf {
	u32 endpoint;
	bool is_mapped;
	u64 dva;
	size_t size;
};

struct rtkit_helper_priv {
	void *asc;		/* ASC registers */
	struct mbox_chan chan;
	struct apple_rtkit *rtk;
	bool sram_stolen;
	/*
	 * DMA window of the helper's DART ("apple,dma-range").  Buffers are
	 * handed to the firmware by physical address (the DART is in bypass),
	 * so they must lie inside the window the coprocessor can address.
	 */
	u64 dma_start, dma_end;
	/* The helper's DART and stream, for a live OS handoff. */
	struct udevice *dart;
	u32 dart_sid;
	/* DRAM buffers the running firmware holds. */
	struct rtkit_helper_buf bufs[RTKIT_HELPER_MAX_BUFS];
	int nbufs;
	/* The OS device tree describes the running session; see remove(). */
	bool handoff_published;
};

static bool rtkit_helper_is_j704(struct udevice *dev)
{
	return of_machine_is_compatible("apple,j704") &&
	       device_is_compatible(dev, "apple,t8142-rtk-helper-asc4");
}

static bool rtkit_helper_uses_dram(struct udevice *dev)
{
	return IS_ENABLED(CONFIG_APPLE_MTP_KEYB) &&
	       (device_is_compatible(dev, "apple,t8140-rtk-helper-asc4") ||
		rtkit_helper_is_j704(dev));
}

static void rtkit_helper_read_dma_window(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	u64 range[2];

	if (dev_read_phandle_with_args(dev, "iommus", "#iommu-cells", 0, 0,
				       &args))
		return;
	if (ofnode_read_u64_array(args.node, "apple,dma-range", range, 2) ||
	    !range[1])
		return;

	if (rtkit_helper_is_j704(dev)) {
		ofnode memory = ofnode_path("/memory");

		/* Buffers are physical addresses while the helper DART bypasses.
		 * Use only the live RAM bank Aura provided; LMB excludes every
		 * existing reservation, payload and U-Boot allocation.
		 */
		if (!ofnode_valid(memory) ||
		    ofnode_read_u64_array(memory, "reg", range, 2) ||
		    range[0] < 0x10000000000ULL || !range[1] ||
		    range[0] > 0x10800000000ULL ||
		    range[1] > 0x10800000000ULL - range[0])
			return;
	}
	priv->dma_start = range[0];
	priv->dma_end = range[0] + range[1];

	/* A live handoff additionally needs the DART left in bypass. */
	if (args.args_count == 1 &&
	    !uclass_get_device_by_ofnode(UCLASS_IOMMU, args.node, &priv->dart))
		priv->dart_sid = args.args[0];
}

static void *rtkit_helper_alloc(struct rtkit_helper_priv *priv, size_t size)
{
	phys_addr_t addr;

	if (!priv->dma_end)
		return memalign(SZ_16K, ALIGN(size, SZ_16K));

	/* U-Boot's heap sits at the top of RAM, above a 4 GiB window. */
	addr = priv->dma_end - 1;
	if (lmb_alloc_mem(LMB_MEM_ALLOC_MAX, SZ_16K, &addr,
			  ALIGN(size, SZ_16K), LMB_NOOVERWRITE))
		return NULL;
	if (addr < priv->dma_start) {
		lmb_free(addr, ALIGN(size, SZ_16K), LMB_NOOVERWRITE);
		return NULL;
	}

	return map_sysmem(addr, size);
}

static int shmem_setup(void *cookie, struct apple_rtkit_buffer *buf) {
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	fdt_size_t sram_size;
	fdt_addr_t sram;

	if (!buf->is_mapped) {
		/* Older MTP firmware retains OSLog in SRAM across OS handoff. */
		if (buf->endpoint == APPLE_RTKIT_EP_OSLOG &&
		    !rtkit_helper_uses_dram(dev)) {
			if (priv->sram_stolen)
				return -EBUSY;

			sram = dev_read_addr_size_name(dev, "sram", &sram_size);
			if (sram == FDT_ADDR_T_NONE || !buf->size ||
			    buf->size > sram_size || sram_size > (u64)-1 - sram)
				return -EFAULT;

			buf->dva = ALIGN_DOWN(sram + sram_size - buf->size, SZ_16K);
			if (buf->dva < sram)
				return -EFAULT;

			priv->sram_stolen = true;
			return 0;
		}

		/* DRAM OSLog is only for the unsafe, opt-in T8140 MTP path. */
		buf->buffer = rtkit_helper_alloc(priv, buf->size);
		if (!buf->buffer)
			return -ENOMEM;

		memset(buf->buffer, 0, buf->size);
		flush_dcache_range((ulong)buf->buffer,
				   (ulong)buf->buffer + ALIGN(buf->size, SZ_16K));
		buf->dva = map_to_sysmem(buf->buffer);

		/* An untracked buffer rules out a live handoff. */
		if (priv->nbufs < RTKIT_HELPER_MAX_BUFS) {
			priv->bufs[priv->nbufs].endpoint = buf->endpoint;
			priv->bufs[priv->nbufs].is_mapped = false;
			priv->bufs[priv->nbufs].dva = buf->dva;
			priv->bufs[priv->nbufs].size = ALIGN(buf->size, SZ_16K);
		}
		priv->nbufs++;
		return 0;
	}

	/* Linux accepts firmware-owned mappings only inside the helper SRAM. */
	sram = dev_read_addr_size_name(dev, "sram", &sram_size);
	if (sram == FDT_ADDR_T_NONE || buf->dva < sram ||
	    buf->size > sram_size || buf->dva - sram > sram_size - buf->size) {
		dev_err(dev, "RTKit mapped buffer outside SRAM: %#llx+%#zx\n",
			buf->dva, buf->size);
		return -EFAULT;
	}
	if (rtkit_helper_is_j704(dev)) {
		if (priv->nbufs >= RTKIT_HELPER_MAX_BUFS)
			return -E2BIG;
		priv->bufs[priv->nbufs].endpoint = buf->endpoint;
		priv->bufs[priv->nbufs].is_mapped = true;
		priv->bufs[priv->nbufs].dva = buf->dva;
		priv->bufs[priv->nbufs].size = buf->size;
		priv->nbufs++;
	}
	return 0;
}

static void shmem_destroy(void *cookie, struct apple_rtkit_buffer *buf) {
	struct udevice *dev = cookie;
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	/* Firmware keeps OSLog even after QUIESCED and ASC RUN is cleared. */
	if (!buf->buffer || buf->endpoint == APPLE_RTKIT_EP_OSLOG)
		return;

	/* A live session keeps every buffer; the OS reserves them. */
	if (priv->handoff_published)
		return;

	if (priv->dma_end)
		lmb_free(map_to_sysmem(buf->buffer), ALIGN(buf->size, SZ_16K),
			 LMB_NOOVERWRITE);
	else
		free(buf->buffer);
}

/* Keep T8140 MTP and its DART untouched in the default build. */
bool apple_rtkit_helper_allowed(ofnode node)
{
	return !ofnode_device_is_compatible(node, "apple,t8140-rtk-helper-asc4") ||
	       IS_ENABLED(CONFIG_APPLE_MTP_KEYB);
}

static int rtkit_helper_bind(struct udevice *dev)
{
	return apple_rtkit_helper_allowed(dev_ofnode(dev)) ? 0 : -ENODEV;
}

static int rtkit_helper_probe(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	u32 ctrl;
	int ret;

	priv->asc = dev_read_addr_ptr(dev);
	if (!priv->asc)
		return -EINVAL;

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	if (rtkit_helper_uses_dram(dev))
		rtkit_helper_read_dma_window(dev);
	if (rtkit_helper_is_j704(dev) && (!priv->dart || !priv->dma_end))
		return -EINVAL;

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	priv->rtk = apple_rtkit_init(&priv->chan, dev, shmem_setup, shmem_destroy);
	if (!priv->rtk)
		return -ENOMEM;

	ret = rtkit_helper_is_j704(dev) ? apple_rtkit_boot_early_ap(priv->rtk) :
		apple_rtkit_boot(priv->rtk);
	if (ret < 0) {
		printf("%s: Helper apple_rtkit_boot returned: %d\n", __func__, ret);
		goto err_stop;
	}

	ret = apple_rtkit_set_ap_power(priv->rtk, APPLE_RTKIT_PWR_STATE_ON);
	if (ret < 0) {
		printf("%s: Helper apple_rtkit_set_ap_power returned: %d\n", __func__, ret);
		goto err_stop;
	}

	return 0;

err_stop:
	/* A failed probe will not get an OS-prepare remove callback. */
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);
	readl(priv->asc + REG_CPU_CTRL);
	apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;
	return ret;
}

static int rtkit_helper_remove(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	/*
	 * The OS was told to adopt this session: leave the coprocessor running
	 * with its buffers and its DART in bypass.  Only the U-Boot side of the
	 * transport is torn down.
	 */
	if (priv->handoff_published && !apple_dart_retain_bypass(priv->dart)) {
		printf("%s: leaving RTKit running for the OS\n", dev->name);
		apple_rtkit_free(priv->rtk);
		priv->rtk = NULL;
		return 0;
	}

	apple_rtkit_shutdown(priv->rtk, APPLE_RTKIT_PWR_STATE_QUIESCED);

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;

	return 0;
}

int apple_rtkit_helper_poll(struct udevice *dev, ulong timeout)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	return apple_rtkit_poll(priv->rtk, timeout);
}

/* Node offsets move as the blob grows, so always look them up by path. */
static int rtkit_helper_fdt_node(void *blob, struct udevice *dev)
{
	char path[256];
	int ret;

	ret = ofnode_get_path(dev_ofnode(dev), path, sizeof(path));
	if (ret)
		return ret;
	return fdt_path_offset(blob, path);
}

/*
 * Describe the running session to the OS: one no-map reservation per
 * buffer, identity-mapped for the helper through its DART, the stream the
 * DART must keep in bypass until the OS maps those regions, and finally the
 * handoff contract itself.
 */
static int rtkit_helper_fdt_publish(struct udevice *dev, void *blob)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	int resv, node, sub, parent, i, ret;
	char name[48];
	u32 phandle, rphandle;
	fdt64_t reg[2];

	if (!priv->rtk || !priv->dart || !priv->nbufs ||
	    priv->nbufs > RTKIT_HELPER_MAX_BUFS)
		return -ENODEV;
	if (!(readl(priv->asc + REG_CPU_CTRL) & REG_CPU_CTRL_RUN))
		return -ENODEV;
	ret = apple_dart_retain_bypass(priv->dart);
	if (ret)
		return ret;

	resv = fdt_path_offset(blob, "/reserved-memory");
	node = rtkit_helper_fdt_node(blob, dev);
	if (resv < 0 || node < 0)
		return -ENOENT;
	parent = fdt_parent_offset(blob, node);
	if (fdt_address_cells(blob, resv) != 2 || fdt_size_cells(blob, resv) != 2 ||
	    fdt_address_cells(blob, parent) != 2 || fdt_size_cells(blob, parent) != 2)
		return -EINVAL;

	phandle = fdt_get_phandle(blob, node);
	if (!phandle) {
		ret = fdt_generate_phandle(blob, &phandle);
		if (!ret)
			ret = fdt_setprop_u32(blob, node, "phandle", phandle);
		if (ret)
			return -ENOSPC;
	}

	if (rtkit_helper_is_j704(dev)) {
		fdt_delprop(blob, node, "memory-region");
		fdt_delprop(blob, node, "apple,rtkit-buffers");
	}
	for (i = 0; i < priv->nbufs; i++) {
		struct rtkit_helper_buf *b = &priv->bufs[i];

		snprintf(name, sizeof(name), "mtp-rtkit@%llx", b->dva);
		resv = fdt_path_offset(blob, "/reserved-memory");
		sub = fdt_add_subnode(blob, resv, name);
		if (sub == -FDT_ERR_EXISTS && rtkit_helper_is_j704(dev))
			sub = fdt_subnode_offset(blob, resv, name);
		if (sub < 0)
			return -ENOSPC;

		reg[0] = cpu_to_fdt64(b->dva);
		reg[1] = cpu_to_fdt64(b->size);
		ret = fdt_generate_phandle(blob, &rphandle);
		ret = ret ?: fdt_setprop_u32(blob, sub, "phandle", rphandle);
		ret = ret ?: fdt_setprop(blob, sub, "reg", reg, sizeof(reg));
		ret = ret ?: fdt_setprop_empty(blob, sub, "no-map");
		/* The DART passed these addresses through unchanged: IOVA == PA. */
		ret = ret ?: fdt_setprop_u32(blob, sub, "iommu-addresses", phandle);
		ret = ret ?: fdt_appendprop(blob, sub, "iommu-addresses", reg,
					    sizeof(reg));
		if (ret)
			return -ENOSPC;

		node = rtkit_helper_fdt_node(blob, dev);
		if (node < 0 ||
		    fdt_appendprop_u32(blob, node, "memory-region", rphandle))
			return -ENOSPC;
		if (rtkit_helper_is_j704(dev)) {
			u32 inventory[] = { cpu_to_fdt32(b->endpoint),
				cpu_to_fdt32(b->dva >> 32), cpu_to_fdt32(b->dva),
				cpu_to_fdt32(b->size), cpu_to_fdt32(b->is_mapped) };

			if (fdt_appendprop(blob, node, "apple,rtkit-buffers",
					   inventory, sizeof(inventory)))
				return -ENOSPC;
		}
	}

	node = rtkit_helper_fdt_node(blob, priv->dart);
	if (node < 0)
		return node;
	if (rtkit_helper_is_j704(dev)) {
		/* The adopted stream uses physical addresses for its buffers.
		 * Make Linux's allocator use the same, validated high DMA window.
		 */
		reg[0] = cpu_to_fdt64(priv->dma_start);
		reg[1] = cpu_to_fdt64(priv->dma_end - priv->dma_start);
		ret = fdt_setprop(blob, node, "apple,dma-range", reg, sizeof(reg));
		ret = ret ?: fdt_setprop_u32(blob, node, "apple,bypass-handoff-streams",
					 priv->dart_sid);
	} else {
		ret = fdt_appendprop_u32(blob, node, "apple,bypass-handoff-streams",
					priv->dart_sid);
	}
	if (ret)
		return -ENOSPC;

	node = rtkit_helper_fdt_node(blob, dev);
	if (node < 0 ||
	    fdt_setprop_string(blob, node, "apple,rtkit-handoff",
			       RTKIT_HELPER_HANDOFF))
		return -ENOSPC;

	priv->handoff_published = true;
	printf("%s: live OS handoff published (%d buffers, DART stream %u)\n",
	       dev->name, priv->nbufs, priv->dart_sid);
	return 0;
}

int apple_rtkit_helper_fdt_fixup(void *blob)
{
	struct udevice *dev;
	struct uclass *uc;
	int ret;

	uclass_id_foreach_dev(UCLASS_MISC, dev, uc) {
		struct rtkit_helper_priv *priv;

		if (dev->driver != DM_DRIVER_GET(rtkit_helper) ||
		    !device_active(dev) || !rtkit_helper_uses_dram(dev))
			continue;

		priv = dev_get_priv(dev);
		/* A second fixup describes the same session again. */
		priv->handoff_published = false;
		ret = rtkit_helper_fdt_publish(dev, blob);
		if (ret) {
			priv->handoff_published = false;
			apple_dart_release_bypass(priv->dart);
			if (rtkit_helper_is_j704(dev)) {
				dev_err(dev, "Refusing J704 boot without a live MTP handoff (%d)\n", ret);
				return ret;
			}
			dev_warn(dev, "no live OS handoff (%d); stopping RTKit at boot\n",
				 ret);
		}
	}

	return 0;
}

static const struct udevice_id rtkit_helper_ids[] = {
	{ .compatible = "apple,rtk-helper-asc4" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(rtkit_helper) = {
	.name = "rtkit_helper",
	.id = UCLASS_MISC,
	.of_match = rtkit_helper_ids,
	.bind = rtkit_helper_bind,
	.priv_auto = sizeof(struct rtkit_helper_priv),
	.probe = rtkit_helper_probe,
	.remove = rtkit_helper_remove,
	.flags = DM_FLAG_OS_PREPARE,
};
