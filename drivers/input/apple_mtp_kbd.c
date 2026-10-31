// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright The Asahi Linux Contributors
 */

#include <dm.h>
#include <dm/simple_bus.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <mailbox.h>
#include <keyboard.h>
#include <stdio_dev.h>
#include <iomux.h>
#include <asm/arch/rtkit.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <linux/input.h>
#include "apple_kbd.h"

struct apple_mtp_kbd_priv {
	struct apple_kbd_priv kbd;
	struct udevice *helper;
	void *local;
	void *config;
	void *rmt;
	void *irq_base;
	u32 rx_irq_mask;

	u8 *init_data;
	u32 init_size;
	u32 fifo_size;

	/*
	 * Newer MTP firmware (A18 Pro) announces its interfaces on the comm
	 * interface and only streams keyboard reports once the host enables
	 * the keyboard interface.
	 */
	bool announce;
	int kbd_iface;
	u8 tx_seq;
	u8 *pkt;
	bool ready;
};

/*
 * DockChannel HID framing, interface announcements and the enable command
 * are derived from Linux drivers/hid/dockchannel-hid/dockchannel-hid.c,
 * Copyright The Asahi Linux Contributors, licensed GPL-2.0 OR MIT.
 * These definitions are used under its MIT licence.
 */
#define DCHID_CHANNEL_CMD	0x11
#define DCHID_CHANNEL_REPORT	0x12
#define DCHID_IFACE_COMM	0
#define DCHID_MAX_PKT		(0xffff + 4)

struct dchid_subhdr {
	u8 flags;
	u8 unk;
	__le16 length;
	__le32 retcode;
} __packed;

#define DCHID_FLAGS_FEATURE_SET	(2 << 6)	/* HID feature report, SET_REPORT */

#define DCHID_EVENT_INIT	0xf0
#define DCHID_EVENT_READY	0xf1
#define DCHID_CMD_ENABLE_IFACE	0xb4

struct dchid_init_hdr {
	u8 type;
	u8 unk1;
	u8 unk2;
	u8 iface;
	char name[16];
	u8 more_packets;
	u8 unkpad;
} __packed;

#define DATA_TX8		0x4
#define DATA_TX32		0x10
#define DATA_TX_FREE		0x14
#define DATA_RX8		0x1c
#define DATA_RX_COUNT		0x2c
#define DCHID_RX_THRESH	0x4
#define DCHID_IRQ_MASK		0x0
#define DCHID_IRQ_FLAG		0x4

struct dchid_hdr {
	u8 hdr_len;
	u8 channel;
	__le16 length;
	u8 seq;
	u8 iface;
	__le16 pad;
} __packed;

static int dockchannel_read(struct udevice *dev, void *buf, size_t size)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	int ret = 0;
	u8 b;
	u8 *p = buf;

	while (size--) {
		ulong start;
		start = get_timer(0);
		while (get_timer(start) < 100) {
			if (readl(priv->local + DATA_RX_COUNT) != 0)
				break;
		}

		if (readl(priv->local + DATA_RX_COUNT) == 0) {
			return -ETIME;
		}

		b = readl(priv->local + DATA_RX8) >> 8;
		if (buf)
			*p++ = b;

		ret++;
	}

	return ret;
}

static int dockchannel_write(struct apple_mtp_kbd_priv *priv, const void *buf,
			     size_t size)
{
	const u8 *p = buf;

	/*
	 * DCHID pads every transfer to a 32-bit boundary.  Keep it that way on
	 * the FIFO too: T8140 aborts byte writes to DATA_TX8, while Linux sends
	 * these aligned protocol fragments with 32-bit writes to DATA_TX32.
	 */
	if (!IS_ALIGNED(size, sizeof(u32)))
		return -EINVAL;

	while (size) {
		ulong start = get_timer(0);
		u32 value;

		while (readl(priv->local + DATA_TX_FREE) < sizeof(value)) {
			if (get_timer(start) > 100)
				return -ETIME;
		}
		value = get_unaligned_le32(p);
		writel(value, priv->local + DATA_TX32);
		p += sizeof(value);
		size -= sizeof(value);
	}

	return 0;
}

static u32 dchid_sum(const void *buf, size_t size)
{
	const u8 *p = buf;
	u32 sum = 0;

	for (; size >= 4; size -= 4, p += 4)
		sum += get_unaligned_le32(p);

	return sum;
}

/* Send a feature SET_REPORT on the comm interface */
static int dchid_comm_set_report(struct apple_mtp_kbd_priv *priv,
				 const u8 *msg, size_t size)
{
	struct {
		struct dchid_hdr hdr;
		struct dchid_subhdr sub;
		u8 data[8];
	} __packed pkt;
	u32 checksum;
	size_t len = ALIGN(size, 4);
	int ret;

	if (size > sizeof(pkt.data))
		return -EMSGSIZE;

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.hdr_len = sizeof(pkt.hdr);
	pkt.hdr.channel = DCHID_CHANNEL_CMD;
	pkt.hdr.length = cpu_to_le16(len + sizeof(pkt.sub));
	pkt.hdr.seq = priv->tx_seq++;
	pkt.hdr.iface = DCHID_IFACE_COMM;
	pkt.sub.flags = DCHID_FLAGS_FEATURE_SET;
	pkt.sub.length = cpu_to_le16(size);
	memcpy(pkt.data, msg, size);

	checksum = 0xffffffff - dchid_sum(&pkt, sizeof(pkt.hdr) +
					  sizeof(pkt.sub) + len);
	ret = dockchannel_write(priv, &pkt,
				sizeof(pkt.hdr) + sizeof(pkt.sub) + len);
	if (!ret)
		ret = dockchannel_write(priv, &checksum, sizeof(checksum));

	return ret;
}

static void dchid_handle_event(struct udevice *dev, const u8 *p, size_t len)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	const struct dchid_init_hdr *init = (const void *)p;
	u8 msg[2];

	if (!len)
		return;

	switch (p[0]) {
	case DCHID_EVENT_INIT:
		if (len < sizeof(*init))
			return;
		if (strncmp(init->name, "keyboard", sizeof(init->name)))
			return;
		priv->kbd_iface = init->iface;
		if (init->more_packets)
			return;
		msg[0] = DCHID_CMD_ENABLE_IFACE;
		msg[1] = init->iface;
		if (dchid_comm_set_report(priv, msg, sizeof(msg)))
			dev_err(dev, "failed to enable keyboard interface %d\n",
				init->iface);
		break;
	case DCHID_EVENT_READY:
		if (len >= 2 && p[1] == priv->kbd_iface && !priv->ready) {
			priv->ready = true;
			printf("mtpkbd: keyboard ready\n");
		}
		break;
	}
}

/* Announcing firmware: read and check a whole packet, then dispatch it. */
static int apple_mtp_kbd_check_announce(struct input_config *input,
					struct dchid_hdr *hdr)
{
	struct udevice *dev = input->dev;
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	const struct dchid_subhdr *sub = (const void *)priv->pkt;
	u16 length = le16_to_cpu(hdr->length);
	u16 plen;
	int ret;

	ret = dockchannel_read(dev, priv->pkt, length + 4);
	if (ret < 0)
		return ret;

	if (hdr->hdr_len != sizeof(*hdr) ||
	    dchid_sum(hdr, sizeof(*hdr)) + dchid_sum(priv->pkt, length + 4) !=
	    0xffffffff) {
		dev_err(dev, "bad packet (iface %d, channel %#x)\n",
			hdr->iface, hdr->channel);
		return 0;
	}
	if (length < sizeof(*sub))
		return 0;
	if (hdr->channel != DCHID_CHANNEL_REPORT)
		return 0;

	plen = le16_to_cpu(sub->length);
	if (plen + sizeof(*sub) > length)
		return 0;

	if (hdr->iface == DCHID_IFACE_COMM) {
		/* Keep the complete INIT packet for Linux's DockChannel probe. */
		if (plen && priv->pkt[sizeof(*sub)] == DCHID_EVENT_INIT) {
			u32 need = sizeof(*hdr) + length + 4;

			if (!IS_ALIGNED(need, sizeof(u32))) {
				dev_err(dev, "unaligned init packet (%u bytes)\n", need);
			} else if (need > priv->fifo_size - priv->init_size) {
				dev_err(dev, "out of init buffer space (%u > %u)\n",
					need, priv->fifo_size - priv->init_size);
			} else {
				u8 *p = priv->init_data + priv->init_size;

				memcpy(p, hdr, sizeof(*hdr));
				memcpy(p + sizeof(*hdr), priv->pkt, length + 4);
				priv->init_size += need;
			}
		}
		if (priv->helper)
			dchid_handle_event(dev, priv->pkt + sizeof(*sub), plen);
		return 0;
	}

	if (!priv->helper || hdr->iface != priv->kbd_iface)
		return 0;

	return apple_kbd_handle_report(input, &priv->kbd,
				       priv->pkt + sizeof(*sub), plen);
}

static int apple_mtp_kbd_check(struct input_config *input)
{
	struct udevice *dev = input->dev;
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	struct dchid_hdr hdr;
	u32 pending;
	int ret;

	/* Poll for syslogs if RTKit is up */
	if (priv->helper)
		apple_rtkit_helper_poll(priv->helper, 0);

	pending = readl(priv->local + DATA_RX_COUNT);
	if (pending < 8)
		return 0;

	ret = dockchannel_read(dev, &hdr, sizeof(hdr));
	if (ret < 0) {
		dev_err(dev, "failed to read packet header\n");
		return ret;
	}

	if (priv->announce)
		return apple_mtp_kbd_check_announce(input, &hdr);

	/* Save comm init messages for the next stage */
	if (hdr.iface == 0) {
		int space = priv->fifo_size - priv->init_size;
		int need = hdr.length + sizeof(hdr) + 4;

		if (space < need) {
			dev_err(dev, "out of buf space (%d > %d)\n",
				need, space);
			ret = dockchannel_read(dev, NULL, hdr.length + 4);
			if (ret < 0)
				return ret;
		} else {
			u8 *p = &priv->init_data[priv->init_size];

			memcpy(p, &hdr, sizeof(hdr));
			ret = dockchannel_read(dev, p + sizeof(hdr), hdr.length + 4);
			if (ret < 0)
				return ret;
			priv->init_size += need;
		}
	} else if (hdr.channel == 0x12 && hdr.length == 0x14) {
		u8 buf[0x18];

		ret = dockchannel_read(dev, buf, 0x18);
		if (ret < 0)
			return ret;

		if (!priv->helper)
			return 1; /* Ignore if shutting down */

		/* Just assume it's a keyboard report */
		return apple_kbd_handle_report(input, &priv->kbd, buf + 8, 0xc);
	} else {
		ret = dockchannel_read(dev, NULL, hdr.length + 4);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int get_rtkit_helper(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	int ret;
	u32 phandle;
	ofnode of_mtp;

	ret = dev_read_u32(dev, "apple,helper-cpu", &phandle);
	if (ret < 0)
		return ret;

	of_mtp = ofnode_get_by_phandle(phandle);
	ret = uclass_get_device_by_ofnode(UCLASS_MISC, of_mtp, &priv->helper);
	if (ret < 0)
		return ret;

	return 0;
}

static int apple_mtp_kbd_probe(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	int ret;
	u32 rx_irq;
	fdt_addr_t reg;

	/*
	 * J813's MTP firmware cold-boots correctly, but restarting it after
	 * U-Boot's RTKit shutdown crashes while restoring its logging state.
	 * Leave the helper and its FIFOs untouched so Linux owns the first boot.
	 */
	if (of_machine_is_compatible("apple,j813"))
		return -ENODEV;

	reg = dev_read_addr_name(dev, "data");
	if (reg == FDT_ADDR_T_NONE) {
		dev_err(dev, "no reg property for local FIFO data registers\n");
		return -EINVAL;
	}
	priv->local = (void *)reg;
	reg = dev_read_addr_name(dev, "config");
	if (reg == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->config = (void *)reg;

	reg = dev_read_addr_name(dev, "rmt-data");
	if (reg == FDT_ADDR_T_NONE) {
		dev_err(dev, "no reg property for remote FIFO data registers\n");
		return -EINVAL;
	}
	priv->rmt = (void *)reg;
	reg = dev_read_addr_name(dev_get_parent(dev), "irq");
	if (reg == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->irq_base = (void *)reg;
	/*
	 * The second interrupt is RX; the DockChannel parent uses one bit per
	 * child interrupt.  Match Linux's mask/ACK/threshold/unmask sequence.
	 * Polling RX_COUNT still consumes the bytes in U-Boot.
	 */
	ret = dev_read_u32_index(dev, "interrupts", 2, &rx_irq);
	if (ret || rx_irq >= 32)
		return -EINVAL;
	priv->rx_irq_mask = BIT(rx_irq);

	ret = dev_read_u32(dev, "apple,fifo-size", &priv->fifo_size);
	if (ret < 0 || !priv->fifo_size) {
		dev_err(dev, "no apple,fifo-size property\n");
		return ret ?: -EINVAL;
	}

	writel(0, priv->irq_base + DCHID_IRQ_MASK);
	writel(~0U, priv->irq_base + DCHID_IRQ_FLAG);
	writel(sizeof(struct dchid_hdr), priv->config + DCHID_RX_THRESH);
	writel(priv->rx_irq_mask, priv->irq_base + DCHID_IRQ_MASK);

	ret = get_rtkit_helper(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to get helper device (%d)\n", ret);
		writel(0, priv->irq_base + DCHID_IRQ_MASK);
		return ret;
	}

	priv->init_data = malloc(priv->fifo_size);
	if (!priv->init_data)
		return -ENOMEM;

	priv->kbd_iface = -1;
	priv->announce = dev_read_bool(dev, "apple,no-stm") ||
		of_machine_is_compatible("apple,j704");
	if (priv->announce) {
		priv->pkt = malloc(DCHID_MAX_PKT);
		if (!priv->pkt) {
			free(priv->init_data);
			return -ENOMEM;
		}
	}

	input->dev = dev;
	input->read_keys = apple_mtp_kbd_check;
	input_add_tables(input, false);
	strcpy(sdev->name, "mtpkbd");

	return input_stdio_register(sdev);
}

static int apple_mtp_kbd_remove(struct udevice *dev)
{
	struct apple_mtp_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct input_config *input = &uc_priv->input;
	int i, ret;

	if (priv->helper) {
		ret = device_remove(priv->helper, DM_REMOVE_NORMAL);
		if (ret)
			return ret;
		priv->helper = NULL;
	}
	writel(0, priv->irq_base + DCHID_IRQ_MASK);
	writel(priv->rx_irq_mask, priv->irq_base + DCHID_IRQ_FLAG);

	/* Drain the FIFO */
	while (readl(priv->local + DATA_RX_COUNT)) {
		if (apple_mtp_kbd_check(input) < 0) {
			dev_err(dev, "Failed to drain FIFO\n");
			break;
		}
	}

	/* Stuff init messages back into FIFO for the next stage to find */
	if (priv->announce) {
		/* T8140 aborts byte writes to DATA_TX8. */
		for (i = 0; i < priv->init_size; i += sizeof(u32))
			writel(get_unaligned_le32(priv->init_data + i),
			       priv->rmt + DATA_TX32);
	} else {
		for (i = 0; i < priv->init_size; i++)
			writel(priv->init_data[i], priv->rmt + DATA_TX8);
	}
	free(priv->pkt);
	free(priv->init_data);

	return 0;
}

static int apple_mtp_kbd_bind(struct udevice *dev)
{
	u32 phandle;

	/* Only bind where the helper coprocessor may be started. */
	if (dev_read_u32(dev, "apple,helper-cpu", &phandle))
		return 0;

	return apple_rtkit_helper_allowed(ofnode_get_by_phandle(phandle)) ?
	       0 : -ENODEV;
}

static const struct keyboard_ops apple_mtp_kbd_ops = {
};

static const struct udevice_id apple_mtp_kbd_of_match[] = {
	{ .compatible = "apple,dockchannel-hid" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_mtp_kbd) = {
	.name = "apple_mtp_kbd",
	.id = UCLASS_KEYBOARD,
	.of_match = apple_mtp_kbd_of_match,
	.bind = apple_mtp_kbd_bind,
	.probe = apple_mtp_kbd_probe,
	.remove = apple_mtp_kbd_remove,
	.priv_auto = sizeof(struct apple_mtp_kbd_priv),
	.ops = &apple_mtp_kbd_ops,
	.flags = DM_FLAG_OS_PREPARE,
};

/* Treat dockchannel as a simple-bus, since we don't use the IRQ stuff */

static const struct udevice_id dockchannel_bus_ids[] = {
	{ .compatible = "apple,dockchannel" },
	{ }
};

U_BOOT_DRIVER(dockchannel) = {
	.name	= "dockchannel",
	.id	= UCLASS_SIMPLE_BUS,
	.of_match = of_match_ptr(dockchannel_bus_ids),
};

#if CONFIG_IS_ENABLED(CMDLINE)
#include <command.h>
#include <env.h>
#include <dm/lists.h>
#include <dm/uclass-internal.h>

/* Bind (if needed) and probe the device for @node under its parent */
static int mtpkbd_bind_node(ofnode node, struct udevice **devp)
{
	struct udevice *parent;
	int ret;

	if (!device_find_global_by_ofnode(node, devp))
		return 0;

	ret = device_find_global_by_ofnode(ofnode_get_parent(node), &parent);
	if (ret)
		return ret;

	return lists_bind_fdt(parent, node, devp, NULL, false);
}

/* Preserve all configured inputs and avoid duplicate mtpkbd entries. */
static int mtpkbd_add_stdin(void)
{
	const char *input = env_get("stdin") ?: "";
	const char *entry = input;
	char *updated;
	size_t len;
	int ret;

	while (*entry) {
		len = strcspn(entry, ",");
		if (len == strlen("mtpkbd") && !strncmp(entry, "mtpkbd", len))
			return iomux_doenv(stdin, input);
		entry += len;
		if (*entry)
			entry++;
	}

	len = strlen(input) + sizeof(",mtpkbd");
	updated = malloc(len);
	if (!updated)
		return -ENOMEM;
	snprintf(updated, len, "%s%smtpkbd", input, *input ? "," : "");
	ret = env_set("stdin", updated);
	free(updated);

	return ret;
}

/*
 * "mtpkbd start": probe an MTP keyboard compiled into this image and
 * attach it to the active console inputs.
 */
static int do_mtpkbd(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	struct udevice *dev, *kbd;
	ofnode node;
	u32 phandle;
	int ret;

	if (argc != 2)
		return CMD_RET_USAGE;
	if (strcmp(argv[1], "start"))
		return CMD_RET_USAGE;

	node = ofnode_by_compatible(ofnode_null(), "apple,dockchannel-hid");
	if (!ofnode_valid(node)) {
		printf("mtpkbd: no DockChannel keyboard in the devicetree\n");
		return CMD_RET_FAILURE;
	}

	if (!ofnode_read_u32(node, "apple,helper-cpu", &phandle)) {
		ret = mtpkbd_bind_node(ofnode_get_by_phandle(phandle), &dev);
		if (ret) {
			printf("mtpkbd: cannot bind the MTP (%d)\n", ret);
			return CMD_RET_FAILURE;
		}
	}

	ret = mtpkbd_bind_node(node, &kbd);
	if (!ret)
		ret = device_probe(kbd);
	if (ret) {
		printf("mtpkbd: keyboard failed to start (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	ret = mtpkbd_add_stdin();
	if (ret) {
		printf("mtpkbd: cannot update stdin (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(mtpkbd, 2, 0, do_mtpkbd,
	   "start the Apple MTP (internal) keyboard",
	   "start - bind and start the internal keyboard, add it to stdin");
#endif
