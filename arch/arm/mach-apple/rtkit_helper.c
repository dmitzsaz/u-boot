// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <dm.h>
#include <vsprintf.h>
#include <dm/device_compat.h>
#include <cpu_func.h>
#include <lmb.h>
#include <mailbox.h>
#include <malloc.h>
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
	const void *published_fdt;
	bool input_ready;
};

static bool rtkit_helper_uses_dram(struct udevice *dev)
{
	return IS_ENABLED(CONFIG_APPLE_MTP_KEYB) &&
	       (device_is_compatible(dev, "apple,t8140-rtk-helper-asc4") ||
		(of_machine_is_compatible("apple,j614s") &&
		 device_is_compatible(dev, "apple,t6041-rtk-helper-asc4")));
}

/*
 * The T8142 MTP cannot be started again once stopped. U-Boot only uses it
 * when it hands the running session on to the OS.
 */
static bool rtkit_helper_t8142_handoff(struct udevice *dev)
{
	return IS_ENABLED(CONFIG_APPLE_MTP_KEYB_T8142_HANDOFF) &&
	       device_is_compatible(dev, "apple,t8142-rtk-helper-asc4");
}

/* J713 and J614s Linux consume the initialized session without owning its RTKit epoch. */
static bool rtkit_helper_j713_handoff(struct udevice *dev)
{
	return (of_machine_is_compatible("apple,j713") &&
		device_is_compatible(dev, "apple,t8132-rtk-helper-asc4")) ||
	       (of_machine_is_compatible("apple,j614s") &&
		device_is_compatible(dev, "apple,t6041-rtk-helper-asc4")) ||
	       rtkit_helper_t8142_handoff(dev);
}

static void rtkit_helper_read_dma_window(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	u64 range[2];

	if (dev_read_phandle_with_args(dev, "iommus", "#iommu-cells", 0, 0,
				       &args))
		return;
	if (!ofnode_read_u64_array(args.node, "apple,dma-range", range, 2) &&
	    range[1] && range[0] <= (u64)-1 - range[1]) {
		priv->dma_start = range[0];
		priv->dma_end = range[0] + range[1];
	}

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

	/* The published inventory and reservations are immutable until reset. */
	if (priv->handoff_published)
		return -EBUSY;

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

/*
 * Keep T8140 MTP and its DART untouched in the default build.  Never touch
 * T6040 MTP except for the J614s retained handoff.
 */
bool apple_rtkit_helper_allowed(ofnode node)
{
	if (ofnode_device_is_compatible(node, "apple,t6041-rtk-helper-asc4"))
		return IS_ENABLED(CONFIG_APPLE_MTP_KEYB) &&
		       of_machine_is_compatible("apple,j614s");
	if (ofnode_device_is_compatible(node, "apple,t8142-rtk-helper-asc4"))
		return IS_ENABLED(CONFIG_APPLE_MTP_KEYB_T8142_HANDOFF);

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

	if (rtkit_helper_uses_dram(dev) || rtkit_helper_j713_handoff(dev))
		rtkit_helper_read_dma_window(dev);
	/*
	 * The T8142 MTP DART's apple,dma-range is the OS's translated IOVA
	 * window. The session runs with the DART in bypass, on physical
	 * addresses, so it does not bound the buffers.
	 */
	if (rtkit_helper_t8142_handoff(dev))
		priv->dma_start = priv->dma_end = 0;

	if (of_machine_is_compatible("apple,j614s") &&
	    device_is_compatible(dev, "apple,t6041-rtk-helper-asc4")) {
		ret = priv->dart ? apple_dart_retain_bypass(priv->dart) : -ENODEV;
		if (ret) {
			printf("J614s: MTP DART bypass unavailable (%d)\n", ret);
			return ret;
		}
	}

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	priv->rtk = apple_rtkit_init(&priv->chan, dev, shmem_setup, shmem_destroy);
	if (!priv->rtk) {
		ret = -ENOMEM;
		goto err_stop;
	}

	/* Stable kernels do not service logging after the OS boundary. */
	if (rtkit_helper_t8142_handoff(dev))
		ret = apple_rtkit_boot_with_options(priv->rtk,
			~(u32)(BIT(2) | BIT(3) | BIT(10)), APPLE_RTKIT_BOOT_AP_FIRST);
	else if (rtkit_helper_j713_handoff(dev))
		ret = apple_rtkit_boot_with_sys_ep_mask(priv->rtk,
							~(u32)(BIT(2) | BIT(3) | BIT(10)));
	else
		ret = apple_rtkit_boot(priv->rtk);
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
	if (priv->rtk)
		apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;
	return ret;
}

static void rtkit_helper_stop(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	if (priv->rtk)
		apple_rtkit_shutdown(priv->rtk, APPLE_RTKIT_PWR_STATE_QUIESCED);
	/* Do not release buffers or the bypass stream while the ASC can DMA. */
	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);
	readl(priv->asc + REG_CPU_CTRL);
	priv->handoff_published = false;
	priv->published_fdt = NULL;
	priv->input_ready = false;
	if (priv->rtk)
		apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;
	priv->nbufs = 0;
	apple_dart_release_bypass(priv->dart);
}

static int rtkit_helper_remove(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	/*
	 * The OS was told to adopt this session: leave the coprocessor running
	 * with its buffers and its DART in bypass.  Only the U-Boot side of the
	 * transport is torn down.
	 */
	if (priv->handoff_published) {
		int ret = apple_dart_retain_bypass(priv->dart);

		/* A published J713 inventory already promises a running session.
		 * Removal errors alone cannot stop EFI/booti from entering Linux. */
		if (ret && rtkit_helper_j713_handoff(dev))
			panic("MTP: refusing OS entry after DART retention error %d", ret);
		if (ret)
			goto stop;

		debug("%s: leaving RTKit running for the OS\n", dev->name);
		apple_rtkit_free(priv->rtk);
		priv->rtk = NULL;
		return 0;
	}

stop:
	rtkit_helper_stop(dev);

	return 0;
}

int apple_rtkit_helper_poll(struct udevice *dev, ulong timeout)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	if (!priv || !priv->rtk)
		return -ENODEV;
	return apple_rtkit_poll(priv->rtk, timeout);
}

bool apple_rtkit_helper_input_prepared(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	return device_active(dev) && priv && priv->rtk && priv->input_ready;
}

bool apple_rtkit_helper_input_retained(struct udevice *dev)
{
	/* Device flags outlive private transport teardown at OS prepare. */
	return dev_get_flags(dev) & DM_FLAG_LEAVE_PD_ON;
}

void apple_rtkit_helper_input_failed(struct udevice *dev)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);

	/* A prior EFI table still owns a published session. Refuse new boots
	 * in the keyboard remove path, without revoking its reservations. */
	if (priv && !priv->handoff_published)
		priv->input_ready = false;
}

void apple_rtkit_helper_input_ready(struct udevice *dev)
{
	((struct rtkit_helper_priv *)dev_get_priv(dev))->input_ready = true;
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


/* J713 preserves its HID startup state and exports every live RTKit buffer. */
static int rtkit_helper_fdt_publish_j713(struct udevice *dev, void *blob)
{
	struct rtkit_helper_priv *priv = dev_get_priv(dev);
	int node, resv, sub, ret;
	char name[48];
	fdt64_t reg[2];
	fdt32_t regions[RTKIT_HELPER_MAX_BUFS], inventory[4 * 5];
	struct apple_rtkit_buffer buffers[4];
	int nbufs;

	if (!priv->input_ready || !priv->rtk || !priv->dart ||
	    priv->nbufs > RTKIT_HELPER_MAX_BUFS ||
	    !(readl(priv->asc + REG_CPU_CTRL) & REG_CPU_CTRL_RUN))
		return -ENODEV;
	ret = apple_dart_retain_bypass(priv->dart);
	if (ret)
		return ret;

	resv = fdt_path_offset(blob, "/reserved-memory");
	if (resv < 0)
		return resv;
	if (fdt_address_cells(blob, resv) != 2 || fdt_size_cells(blob, resv) != 2)
		return -EINVAL;
	nbufs = apple_rtkit_get_buffers(priv->rtk, buffers, ARRAY_SIZE(buffers));
	if (nbufs < 0)
		return nbufs;
	for (int i = 0; i < nbufs; i++) {
		struct apple_rtkit_buffer *b = &buffers[i];

		if (b->size > (u32)-1)
			return -EOVERFLOW;
		inventory[5 * i] = cpu_to_fdt32(b->endpoint);
		inventory[5 * i + 1] = cpu_to_fdt32(b->dva >> 32);
		inventory[5 * i + 2] = cpu_to_fdt32(b->dva);
		inventory[5 * i + 3] = cpu_to_fdt32(b->size);
		/* Original request supplied the address; independent of SRAM/DRAM. */
		inventory[5 * i + 4] = cpu_to_fdt32(b->is_mapped);
	}

	for (int i = 0; i < priv->nbufs; i++) {
		/* EFI consumes no-map reserved-memory nodes as reserved memory;
		 * Linux retains the same explicit ownership record after EFI boot. */
		snprintf(name, sizeof(name), "mtp-rtkit@%llx", priv->bufs[i].dva);
		resv = fdt_path_offset(blob, "/reserved-memory");
		sub = fdt_subnode_offset(blob, resv, name);
		if (sub == -FDT_ERR_NOTFOUND)
			sub = fdt_add_subnode(blob, resv, name);
		if (sub < 0)
			return sub;
		reg[0] = cpu_to_fdt64(priv->bufs[i].dva);
		reg[1] = cpu_to_fdt64(priv->bufs[i].size);
		ret = fdt_setprop(blob, sub, "reg", reg, sizeof(reg));
		ret = ret ?: fdt_setprop_empty(blob, sub, "no-map");
		if (ret)
			return ret;
		u32 phandle = fdt_get_phandle(blob, sub);

		if (!phandle) {
			ret = fdt_generate_phandle(blob, &phandle);
			ret = ret ?: fdt_setprop_u32(blob, sub, "phandle", phandle);
			if (ret)
				return ret;
		}
		regions[i] = cpu_to_fdt32(phandle);
	}
	node = rtkit_helper_fdt_node(blob, dev);
	if (node < 0)
		return node;
	if (priv->nbufs) {
		ret = fdt_setprop(blob, node, "memory-region", regions,
				  priv->nbufs * sizeof(*regions));
	} else {
		ret = fdt_delprop(blob, node, "memory-region");
		if (ret == -FDT_ERR_NOTFOUND)
			ret = 0;
	}
	ret = ret ?: fdt_setprop(blob, node, "apple,rtkit-buffers", inventory,
				 nbufs * 5 * sizeof(*inventory));
	ret = ret ?: fdt_setprop_string(blob, node, "apple,rtkit-handoff",
					RTKIT_HELPER_HANDOFF);
	/* Older stable kernels understand only the legacy initialized-HID marker. */
	ret = ret ?: fdt_setprop_u32(blob, node, "linux-enablement-mac,rtkit-inherited", 1);
	if (ret)
		return ret;
	priv->handoff_published = true;
	dev_or_flags(dev, DM_FLAG_LEAVE_PD_ON);
	debug("%s: initialized session retained for Linux (%d buffers)\n",
	      dev->name, priv->nbufs);
	return 0;
}

/* Failed optional input must not leave a completed-handoff claim behind. */
static void rtkit_helper_fdt_disable_j713(struct udevice *dev, void *blob)
{
	int node = rtkit_helper_fdt_node(blob, dev);
	int input;

	if (node >= 0) {
		fdt_delprop(blob, node, "linux-enablement-mac,rtkit-inherited");
		fdt_delprop(blob, node, "apple,rtkit-handoff");
		fdt_delprop(blob, node, "apple,rtkit-buffers");
		if (fdt_setprop_string(blob, node, "status", "disabled"))
			fdt_nop_node(blob, node); /* No space needed for the fallback. */
	}
	input = fdt_path_offset(blob, "keyboard");
	if (input >= 0) {
		input = fdt_parent_offset(blob, input);
		if (input > 0 && fdt_setprop_string(blob, input, "status", "disabled"))
			fdt_nop_node(blob, input);
	}
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

	for (i = 0; i < priv->nbufs; i++) {
		struct rtkit_helper_buf *b = &priv->bufs[i];

		snprintf(name, sizeof(name), "mtp-rtkit@%llx", b->dva);
		resv = fdt_path_offset(blob, "/reserved-memory");
		sub = fdt_add_subnode(blob, resv, name);
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
	}

	node = rtkit_helper_fdt_node(blob, priv->dart);
	if (node < 0 ||
	    fdt_appendprop_u32(blob, node, "apple,bypass-handoff-streams",
			       priv->dart_sid))
		return -ENOSPC;

	node = rtkit_helper_fdt_node(blob, dev);
	if (node < 0 ||
	    fdt_setprop_string(blob, node, "apple,rtkit-handoff",
			       RTKIT_HELPER_HANDOFF))
		return -ENOSPC;

	priv->handoff_published = true;
	debug("%s: live OS handoff published (%d buffers, DART stream %u)\n",
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
		    !device_active(dev) ||
		    (!rtkit_helper_uses_dram(dev) && !rtkit_helper_j713_handoff(dev)))
			continue;

		priv = dev_get_priv(dev);
		bool was_published = priv->handoff_published;
		size_t capacity = fdt_totalsize(blob);
		void *scratch = malloc(capacity);

		ret = scratch ? fdt_open_into(blob, scratch, capacity) : -ENOMEM;
		if (!ret && rtkit_helper_j713_handoff(dev)) {
			int node = rtkit_helper_fdt_node(scratch, dev);

			if (node >= 0) {
				fdt_delprop(scratch, node, "linux-enablement-mac,rtkit-inherited");
				fdt_delprop(scratch, node, "apple,rtkit-handoff");
			}
		}
		if (!ret)
			ret = rtkit_helper_j713_handoff(dev) ?
				rtkit_helper_fdt_publish_j713(dev, scratch) :
				rtkit_helper_fdt_publish(dev, scratch);
		if (!ret) {
			memcpy(blob, scratch, capacity);
			if (!was_published)
				priv->published_fdt = blob;
		}
		free(scratch);
		if (ret && was_published) {
			/* The prior EFI table may still be selected after a later
			 * candidate fixup fails. Its buffers/DART/session stay live. */
			dev_warn(dev, "candidate handoff refused (%d); keeping prior RTKit session\n", ret);
			if (rtkit_helper_j713_handoff(dev) && blob != priv->published_fdt) {
				rtkit_helper_fdt_disable_j713(dev, blob);
				/* Refuse this candidate so EFI keeps the prior table. */
				return ret;
			}
		} else if (ret) {
			dev_warn(dev, "no live OS handoff (%d); stopping RTKit at boot\n", ret);
			if (rtkit_helper_j713_handoff(dev)) {
				rtkit_helper_stop(dev);
				rtkit_helper_fdt_disable_j713(dev, blob);
			} else {
				apple_dart_release_bypass(priv->dart);
			}
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
