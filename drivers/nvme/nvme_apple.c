// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021 Mark Kettenis <kettenis@openbsd.org>
 */

#include <dm.h>
#include <fdt_support.h>
#include <lmb.h>
#include <mapmem.h>
#include <asm/arch/nvme_apple.h>
#include <mailbox.h>
#include "nvme.h"
#include <reset.h>
#include <time.h>
#include <dm/device_compat.h>

#include <asm/io.h>
#include <asm/arch/rtkit.h>
#include <asm/arch/sart.h>
#include <linux/apple-mailbox.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>

/* ASC registers */
#define REG_CPU_CTRL		0x0044
#define  REG_CPU_CTRL_RUN	BIT(4)

/* Apple NVMe registers */
#define ANS_IOSQ_BASE		0x01200	/* post-M4 */
#define ANS_IOCQ_BASE		0x01208	/* post-M4 */
#define ANS_MAX_PEND_CMDS_CTRL	0x01210	/* IOQA on post-M4 */
#define  ANS_MAX_QUEUE_DEPTH	64
#define ANS_BOOT_STATUS		0x01300
#define  ANS_BOOT_STATUS_OK	0xde71ce55
#define ANS_MODESEL		0x01304
#define ANS_UNKNOWN_CTRL	0x24008
#define  ANS_PRP_NULL_CHECK	(1 << 11)
#define ANS_LINEAR_SQ_CTRL	0x24908
#define  ANS_LINEAR_SQ_CTRL_EN	(1 << 0)
#define ANS_ASQ_DB		0x2490c
#define ANS_IOSQ_DB		0x24910
#define ANS_NVMMU_NUM		0x28100
#define ANS_NVMMU_BASE_ASQ	0x28108
#define ANS_NVMMU_BASE_IOSQ	0x28110
#define ANS_NVMMU_TCB_INVAL	0x28118
#define ANS_NVMMU_TCB_STAT	0x28120

#define ANS_NVMMU_TCB_SIZE	0x4000
#define ANS_NVMMU_TCB_PITCH	0x80

/*
 * RTKit messages that a post-M4 ANS sends while U-Boot drives it.  The
 * session belongs to the boot loader that started the firmware and passes
 * on to the OS; U-Boot only answers what the firmware waits for, as an OS
 * does for a session it adopts.
 */
#define ANS_RTKIT_EP_CRASHLOG	1
#define ANS_RTKIT_EP_SYSLOG	2
#define ANS_RTKIT_EP_IOREPORT	4
#define ANS_RTKIT_MSG_TYPE	GENMASK_ULL(59, 52)
#define  ANS_RTKIT_SYSLOG_LOG	5
#define ANS_MBOX_POLL_MAX	16

/*
 * The Apple NVMe controller includes an IOMMU known as NVMMU.  The
 * NVMMU is programmed through an array of TCBs. These TCBs are paired
 * with the corresponding slot in the submission queues and need to be
 * configured with the command details before a command is allowed to
 * execute. This is necessary even for commands that don't do DMA.
 */
struct ans_nvmmu_tcb {
	u8 opcode;
	u8 flags;
	u8 slot;
	u8 pad0;
	u32 prpl_len;
	u8 pad1[16];
	u64 prp1;
	u64 prp2;
};

/* DMA direction, named from the device's point of view */
#define ANS_NVMMU_TCB_WRITE	BIT(0)
#define ANS_NVMMU_TCB_READ	BIT(1)

/*
 * On post-M4 controllers (A18 Pro/T8140 and M4/T8132 onwards) the
 * NVMMU has its own register window, the I/O queue addresses are also
 * programmed through dedicated registers, and the ANS coprocessor cannot
 * be reset or restarted once running: clearing CPU_CTRL.RUN is one-way
 * and asserting its PMGR reset raises an SError.  The boot loader that
 * started the firmware (m1n1) hands it over with its RTKit session live,
 * and every later stage adopts that session instead of booting it.
 */
struct apple_nvme_hw {
	bool post_m4;
	/*
	 * LINEAR_SQ_CTRL and the linear submission queue doorbells sit in the
	 * NVMMU block (at the same offsets), and the firmware has enabled the
	 * linear queues itself.
	 */
	bool lsq_in_nvmmu;
	/*
	 * The ANS may arrive straight from iBoot, before any OS-side RTKit
	 * handshake: run that (one-time) handshake here, then hand the live
	 * session on as on adoption.  See apple_nvme_needs_handshake().
	 */
	bool wake_disabled;
	/* Highest namespace ID to expose, 0 for all */
	u32 max_ns_id;
};

static const struct apple_nvme_hw apple_nvme_t8103_hw = {
	.post_m4 = false,
};

/*
 * The post-M4 (T8132 and later) register layout and I/O queue setup follow
 * Yureka Lilian's T8132 support in m1n1 (commit 53f8ee9b54ba, "nvme: support
 * T8132").
 */
static const struct apple_nvme_hw apple_nvme_t8132_hw = {
	.post_m4 = true,
	/*
	 * Only namespace 1 holds anything a boot loader can use.  The others
	 * (2 to 4 on the A18 Pro) are small system namespaces that the
	 * firmware handles specially: a Flush to one of them is fatal to it.
	 * Without block devices for them, nothing in U-Boot or a boot loader
	 * on top of it (GRUB searching every disk for a file system, say)
	 * sends them commands.
	 */
	.max_ns_id = 1,
};

/*
 * T6040 (M4 Pro) with the 26.x ANS firmware: the post-M4 controller, but its
 * "nvme" register block ends at 0x10000 and the linear submission queue
 * registers are in the NVMMU block, as in m1n1 (0d2ded8b, "nvme: use the
 * NVMMU block for the T6040 linear queue registers") and Linux's
 * apple,t6041-nvme-ans2 entry.
 */
static const struct apple_nvme_hw apple_nvme_t6040_hw = {
	.post_m4 = true,
	.lsq_in_nvmmu = true,
	.wake_disabled = true,
	.max_ns_id = 1,
};

/*
 * T8142 (M5): the same controller and handover as T6040.  iBoot leaves the
 * ANS running with the controller disabled and before the OS-side RTKit
 * handshake. The DT register window selects the linear queue aperture.
 */
static const struct apple_nvme_hw apple_nvme_t8142_hw = {
	.post_m4 = true,
	.wake_disabled = true,
	.max_ns_id = 1,
};

struct apple_nvme_priv {
	struct nvme_dev ndev;
	const struct apple_nvme_hw *hw;
	bool lsq_in_nvmmu;
	void *base;		/* NVMe registers */
	void *nvmmu;		/* NVMMU registers */
	void *asc;		/* ASC registers */
	struct reset_ctl_bulk resets; /* ASC reset */
	struct mbox_chan chan;
	bool adopted_mbox;	/* chan carries an adopted RTKit session */
	bool woken;		/* we woke the session with a handshake */
	struct apple_sart *sart;
	struct apple_rtkit *rtk;
	struct ans_nvmmu_tcb *tcbs[NVME_Q_NUM]; /* Submission queue TCBs */
	u32 __iomem *q_db[NVME_Q_NUM]; /* Submission queue doorbell */
};

/* The block holding LINEAR_SQ_CTRL and the linear SQ doorbells */
static void __iomem *apple_nvme_lsq(struct apple_nvme_priv *priv)
{
	return priv->lsq_in_nvmmu ? priv->nvmmu : priv->base;
}

/* A queue's TCB array, allocated once; NVMMU wants it 16 KiB aligned. */
static void *apple_nvme_tcbs(struct apple_nvme_priv *priv, u16 qid)
{
	if (!priv->tcbs[qid]) {
		priv->tcbs[qid] = memalign(SZ_16K, ANS_NVMMU_TCB_SIZE);
		if (!priv->tcbs[qid])
			return NULL;
		memset(priv->tcbs[qid], 0, ANS_NVMMU_TCB_SIZE);
		flush_dcache_range((ulong)priv->tcbs[qid],
				   (ulong)priv->tcbs[qid] + ANS_NVMMU_TCB_SIZE);
	}

	return priv->tcbs[qid];
}

static int apple_nvme_setup_queue(struct nvme_queue *nvmeq)
{
	struct apple_nvme_priv *priv =
		container_of(nvmeq->dev, struct apple_nvme_priv, ndev);

	switch (nvmeq->qid) {
	case NVME_ADMIN_Q:
		if (!apple_nvme_tcbs(priv, NVME_ADMIN_Q))
			return -ENOMEM;
		priv->q_db[nvmeq->qid] = apple_nvme_lsq(priv) + ANS_ASQ_DB;
		nvme_writeq((ulong)priv->tcbs[NVME_ADMIN_Q],
			    priv->nvmmu + ANS_NVMMU_BASE_ASQ);
		/*
		 * Post-M4 firmware takes both TCB arrays before the controller
		 * is enabled, as m1n1 and Linux program them; the T6040 does
		 * not become ready without the I/O one.
		 */
		if (priv->hw->post_m4) {
			if (!apple_nvme_tcbs(priv, NVME_IO_Q))
				return -ENOMEM;
			nvme_writeq((ulong)priv->tcbs[NVME_IO_Q],
				    priv->nvmmu + ANS_NVMMU_BASE_IOSQ);
		}
		break;
	case NVME_IO_Q:
		if (!apple_nvme_tcbs(priv, NVME_IO_Q))
			return -ENOMEM;
		priv->q_db[nvmeq->qid] = apple_nvme_lsq(priv) + ANS_IOSQ_DB;
		nvme_writeq((ulong)priv->tcbs[NVME_IO_Q],
			    priv->nvmmu + ANS_NVMMU_BASE_IOSQ);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void apple_nvme_submit_cmd(struct nvme_queue *nvmeq,
				  struct nvme_command *cmd)
{
	struct apple_nvme_priv *priv =
		container_of(nvmeq->dev, struct apple_nvme_priv, ndev);
	struct ans_nvmmu_tcb *tcb;
	u16 tail = nvmeq->sq_tail;

	tcb = ((void *)priv->tcbs[nvmeq->qid]) + tail * ANS_NVMMU_TCB_PITCH;
	memset(tcb, 0, sizeof(*tcb));
	if (priv->hw->post_m4) {
		/* Post-M4 firmware checks the DMA direction against the command. */
		tcb->opcode = 0;
		if (!cmd->common.prp1)
			tcb->flags = 0;
		else if (cmd->common.opcode & 1)
			tcb->flags = ANS_NVMMU_TCB_READ;
		else
			tcb->flags = ANS_NVMMU_TCB_WRITE;
	} else {
		tcb->opcode = cmd->common.opcode;
		tcb->flags = ANS_NVMMU_TCB_WRITE | ANS_NVMMU_TCB_READ;
	}
	tcb->slot = tail;
	tcb->prpl_len = cmd->rw.length;
	tcb->prp1 = cmd->common.prp1;
	tcb->prp2 = cmd->common.prp2;
	flush_dcache_range((ulong)tcb, (ulong)tcb + ANS_NVMMU_TCB_PITCH);

	writel(tail, priv->q_db[nvmeq->qid]);
}

static void apple_nvme_complete_cmd(struct nvme_queue *nvmeq,
				    struct nvme_command *cmd)
{
	struct apple_nvme_priv *priv =
		container_of(nvmeq->dev, struct apple_nvme_priv, ndev);
	struct ans_nvmmu_tcb *tcb;
	u16 tail = nvmeq->sq_tail;

	tcb = ((void *)priv->tcbs[nvmeq->qid]) + tail * ANS_NVMMU_TCB_PITCH;
	memset(tcb, 0, sizeof(*tcb));
	writel(tail, priv->nvmmu + ANS_NVMMU_TCB_INVAL);
	readl(priv->nvmmu + ANS_NVMMU_TCB_STAT);

	if (++tail == nvmeq->q_depth)
		tail = 0;
	nvmeq->sq_tail = tail;
}

/*
 * RTKit buffers of a session that U-Boot started and hands on live (see
 * apple_nvme_wake()).  The firmware keeps writing them after U-Boot exits,
 * so they come from LMB, never return to it, and are published to the OS
 * as no-map reserved memory by apple_nvme_fdt_fixup().
 */
#define ANS_HANDOFF_MAX_BUFS	8

static struct {
	phys_addr_t addr;
	phys_size_t size;
} ans_handoff_bufs[ANS_HANDOFF_MAX_BUFS];
static int ans_handoff_nbufs;

static int nvme_handoff_shmem_setup(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct apple_nvme_priv *priv = (struct apple_nvme_priv *)cookie;
	phys_size_t size;
	phys_addr_t addr;

	if (!buf || buf->dva || !buf->size ||
	    ans_handoff_nbufs >= ANS_HANDOFF_MAX_BUFS)
		return -1;

	size = ALIGN(buf->size, SZ_16K);
	if (lmb_alloc_mem(LMB_MEM_ALLOC_ANY, SZ_16K, &addr, size,
			  LMB_NOOVERWRITE))
		return -ENOMEM;
	buf->buffer = map_sysmem(addr, size);
	memset(buf->buffer, 0, size);
	flush_dcache_range((ulong)buf->buffer, (ulong)buf->buffer + size);

	if (!sart_add_allowed_region(priv->sart, buf->buffer, buf->size)) {
		lmb_free(addr, size, LMB_NOOVERWRITE);
		buf->buffer = NULL;
		return -1;
	}

	buf->dva = addr;
	ans_handoff_bufs[ans_handoff_nbufs].addr = addr;
	ans_handoff_bufs[ans_handoff_nbufs].size = size;
	ans_handoff_nbufs++;

	return 0;
}

int apple_nvme_fdt_fixup(void *blob)
{
	int resv, sub, i, ret;
	char name[40];
	fdt64_t reg[2];

	if (!ans_handoff_nbufs)
		return 0;

	resv = fdt_path_offset(blob, "/reserved-memory");
	if (resv < 0)
		return resv;
	if (fdt_address_cells(blob, resv) != 2 || fdt_size_cells(blob, resv) != 2)
		return -EINVAL;

	for (i = 0; i < ans_handoff_nbufs; i++) {
		snprintf(name, sizeof(name), "ans-rtkit@%llx",
			 (unsigned long long)ans_handoff_bufs[i].addr);
		resv = fdt_path_offset(blob, "/reserved-memory");
		sub = fdt_subnode_offset(blob, resv, name);
		if (sub == -FDT_ERR_NOTFOUND)
			sub = fdt_add_subnode(blob, resv, name);
		if (sub < 0)
			return sub;
		reg[0] = cpu_to_fdt64(ans_handoff_bufs[i].addr);
		reg[1] = cpu_to_fdt64(ans_handoff_bufs[i].size);
		ret = fdt_setprop(blob, sub, "reg", reg, sizeof(reg));
		ret = ret ?: fdt_setprop_empty(blob, sub, "no-map");
		if (ret)
			return ret;
	}

	return 0;
}

static int nvme_shmem_setup(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct apple_nvme_priv *priv = (struct apple_nvme_priv *)cookie;

	if (!buf || buf->dva || !buf->size)
		return -1;

	buf->buffer = memalign(SZ_16K, ALIGN(buf->size, SZ_16K));
	if (!buf->buffer)
		return -ENOMEM;

	if (!sart_add_allowed_region(priv->sart, buf->buffer, buf->size)) {
		free(buf->buffer);
		buf->buffer = NULL;
		buf->size = 0;
		return -1;
	}

	buf->dva = (u64)buf->buffer;

	return 0;
}

static void nvme_shmem_destroy(void *cookie, struct apple_rtkit_buffer *buf)
{
	struct apple_nvme_priv *priv = (struct apple_nvme_priv *)cookie;

	if (!buf)
		return;

	if (buf->buffer) {
		sart_remove_allowed_region(priv->sart, buf->buffer, buf->size);
		free(buf->buffer);
		buf->buffer = NULL;
		buf->size = 0;
		buf->dva = 0;
	}
}

static int apple_nvme_queue_created(struct nvme_queue *nvmeq)
{
	struct apple_nvme_priv *priv =
		container_of(nvmeq->dev, struct apple_nvme_priv, ndev);

	if (!priv->hw->post_m4 || nvmeq->qid != NVME_IO_Q)
		return 0;

	/*
	 * Post-M4 firmware also takes the I/O queue addresses through
	 * dedicated registers, after the queues have been created.  Without
	 * them it crashes on the first I/O command.
	 */
	nvme_writeq((ulong)nvmeq->cqes, priv->base + ANS_IOCQ_BASE);
	nvme_writeq((ulong)nvmeq->sq_cmds, priv->base + ANS_IOSQ_BASE);

	return 0;
}

/*
 * Answer the firmware of an adopted session while commands are outstanding.
 * It writes log records to a ring that its previous owner set up, and once
 * every record is waiting for an acknowledgment it stops processing
 * commands.  Acknowledge them unread.
 */
static int apple_nvme_poll(struct nvme_dev *ndev)
{
	struct apple_nvme_priv *priv =
		container_of(ndev, struct apple_nvme_priv, ndev);
	struct apple_mbox_msg msg;
	bool ack;
	u64 type;
	int i;

	if (!priv->adopted_mbox)
		return 0;

	/* Take what has arrived; the FIFO holds only a few messages. */
	for (i = 0; i < ANS_MBOX_POLL_MAX; i++) {
		if (mbox_recv(&priv->chan, &msg, 0))
			return 0;

		type = FIELD_GET(ANS_RTKIT_MSG_TYPE, msg.msg0);
		switch (msg.msg1) {
		case ANS_RTKIT_EP_CRASHLOG:
			/* Its buffer was set up long ago: this is a crash. */
			printf("%s: ANS firmware crashed (%#llx)\n",
			       ndev->udev->name, msg.msg0);
			priv->adopted_mbox = false;
			return -EIO;
		case ANS_RTKIT_EP_SYSLOG:
			ack = type == ANS_RTKIT_SYSLOG_LOG;
			break;
		case ANS_RTKIT_EP_IOREPORT:
			/* Two notifications that must be acknowledged */
			ack = type == 0x8 || type == 0xc;
			break;
		default:
			/* Nothing else waits for an answer from us. */
			ack = false;
			break;
		}

		if (ack && mbox_send(&priv->chan, &msg)) {
			printf("%s: ANS does not take messages\n",
			       ndev->udev->name);
			priv->adopted_mbox = false;
			return -EIO;
		}
	}

	return 0;
}

/*
 * Wait for a controller whose CC.EN was cleared to stop, answering its
 * firmware meanwhile.  Only then has it given up every command and stopped
 * its DMA.
 */
static int apple_nvme_wait_disabled(struct apple_nvme_priv *priv, u32 *csts)
{
	/* CAP.TO is in 500 ms units */
	ulong timeout = NVME_CAP_TIMEOUT(nvme_readq(&priv->ndev.bar->cap)) * 500;
	ulong start = get_timer(0);

	for (;;) {
		*csts = readl(&priv->ndev.bar->csts);
		if (!(*csts & NVME_CSTS_RDY))
			return 0;
		if (apple_nvme_poll(&priv->ndev))
			return -EIO;
		if (get_timer(start) >= (timeout ?: 500))
			return -ETIMEDOUT;
		udelay(100);
	}
}

static bool apple_nvme_ans_running(struct apple_nvme_priv *priv)
{
	return (readl(priv->asc + REG_CPU_CTRL) & REG_CPU_CTRL_RUN) &&
	       readl(priv->base + ANS_BOOT_STATUS) == ANS_BOOT_STATUS_OK;
}

/*
 * Stop a post-M4 controller: only a completed disable stops its DMA and
 * gives up the commands it holds.  Retire the I/O queue addresses first,
 * as for a handoff: the disable must not meet a controller that still
 * advertises live I/O queues.
 */
static int apple_nvme_disable_post_m4(struct apple_nvme_priv *priv)
{
	u32 csts;

	nvme_writeq(0, priv->base + ANS_IOSQ_BASE);
	nvme_writeq(0, priv->base + ANS_IOCQ_BASE);
	writel(readl(&priv->ndev.bar->cc) & ~NVME_CC_ENABLE,
	       &priv->ndev.bar->cc);
	if (apple_nvme_wait_disabled(priv, &csts)) {
		printf("%s: controller did not disable (CSTS %#x)\n",
		       priv->ndev.udev->name, csts);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Program a disabled post-M4 controller for our queues. */
static int apple_nvme_setup_post_m4(struct apple_nvme_priv *priv)
{
	struct udevice *dev = priv->ndev.udev;
	u32 ioqa;

	if (!priv->lsq_in_nvmmu)
		writel(ANS_LINEAR_SQ_CTRL_EN, priv->base + ANS_LINEAR_SQ_CTRL);
	if (!(readl(apple_nvme_lsq(priv) + ANS_LINEAR_SQ_CTRL) &
	      ANS_LINEAR_SQ_CTRL_EN)) {
		dev_err(dev, "failed to enable linear submission queues\n");
		return -EIO;
	}

	/*
	 * On post-M4 this register is IOQA: both fields hold the zero-based
	 * I/O queue size, like AQA.  Programming the entry count instead makes
	 * the firmware reject the CQ head once the queue wraps.
	 */
	ioqa = (ANS_MAX_QUEUE_DEPTH - 1) | ((ANS_MAX_QUEUE_DEPTH - 1) << 16);
	writel(ioqa, priv->base + ANS_MAX_PEND_CMDS_CTRL);
	if (readl(priv->base + ANS_MAX_PEND_CMDS_CTRL) != ioqa) {
		dev_err(dev, "failed to program the I/O queue aperture\n");
		return -EIO;
	}

	writel(ANS_MAX_QUEUE_DEPTH - 1, priv->nvmmu + ANS_NVMMU_NUM);

	return 0;
}

/*
 * Post-M4: take over the ANS session the previous boot stage left running.
 * HELLO and the endpoint map are one-time messages of an RTKit session, so
 * the firmware is left alone -- no RTKit handshake, no SART, no reset; we
 * only answer what it waits for -- and only the NVMe controller and its
 * queues are reprogrammed.  An enabled controller is disabled first,
 * exactly as Linux does when it adopts the session.
 */
static int apple_nvme_adopt(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	u32 csts, cc;
	int ret;

	if (!apple_nvme_ans_running(priv)) {
		dev_err(dev, "ANS is not running (CPU_CTRL %#x, boot status %#x): post-M4 ANS can only be adopted from the boot loader\n",
			readl(priv->asc + REG_CPU_CTRL),
			readl(priv->base + ANS_BOOT_STATUS));
		return -ENODEV;
	}

	csts = readl(&priv->ndev.bar->csts);
	if (csts & NVME_CSTS_CFS) {
		dev_err(dev, "inherited controller is in a fatal state (CSTS %#x)\n",
			csts);
		return -EIO;
	}

	cc = readl(&priv->ndev.bar->cc);
	debug("%s: adopting the live ANS session (CC %#x CSTS %#x)\n",
	      dev->name, cc, csts);

	/* Only answers to the firmware; the session itself is left alone. */
	if (mbox_get_by_index(dev, 0, &priv->chan))
		dev_warn(dev, "no ANS mailbox: firmware messages go unanswered\n");
	else
		priv->adopted_mbox = true;

	/*
	 * Like Linux, disable an enabled controller (the previous owner's
	 * queues may still be live) before touching any queue register.
	 */
	if (cc & NVME_CC_ENABLE) {
		ret = apple_nvme_disable_post_m4(priv);
		if (ret)
			return ret;
	}

	ret = apple_nvme_setup_post_m4(priv);
	if (ret)
		return ret;
	priv->ndev.max_q_depth = ANS_MAX_QUEUE_DEPTH;

	return 0;
}

/*
 * T6040: the firmware takes the OS-side RTKit handshake once per boot; every
 * later owner must adopt the live session.  An m1n1 stage that ran the
 * handshake leaves the controller disabled but programmed (NVMMU TCB bases,
 * I/O queue aperture); straight from iBoot (a chain whose m1n1 stages never
 * used NVMe) both read zero and the controller will not become ready until
 * someone runs the handshake.
 */
static bool apple_nvme_needs_handshake(struct apple_nvme_priv *priv)
{
	if (!priv->hw->wake_disabled ||
	    (readl(&priv->ndev.bar->cc) & NVME_CC_ENABLE))
		return false;

	return !nvme_readq(priv->nvmmu + ANS_NVMMU_BASE_ASQ) &&
	       !readl(priv->base + ANS_MAX_PEND_CMDS_CTRL);
}

/*
 * Post-M4, T6040: the boot loader hands the ANS over running, with the
 * controller disabled and before the OS-side RTKit handshake (Linux in the
 * same chain logs "RTKit: Initializing" and a cold "boot: mode normal").
 * The controller never becomes ready until that handshake has run, and the
 * firmware takes it once per run: after U-Boot it can neither be repeated
 * nor be undone by sleeping or stopping the IOP.  So run it here the way
 * Linux would (IOP power state INIT, then the boot handshake), with buffers
 * that stay reserved for the firmware, and hand the live session on as on
 * adoption: apple_nvme_remove() leaves the controller enabled and ready for
 * Linux to adopt, and apple_nvme_fdt_fixup() reserves the buffers.  The
 * coprocessor is never reset or restarted.
 */
static int apple_nvme_wake(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	ofnode of_sart;
	u32 phandle, stat;
	int ret;

	if (!apple_nvme_ans_running(priv)) {
		dev_err(dev, "ANS is not running (CPU_CTRL %#x, boot status %#x)\n",
			readl(priv->asc + REG_CPU_CTRL),
			readl(priv->base + ANS_BOOT_STATUS));
		return -ENODEV;
	}
	if (readl(&priv->ndev.bar->csts) & NVME_CSTS_CFS) {
		dev_err(dev, "inherited controller is in a fatal state\n");
		return -EIO;
	}

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;
	ret = dev_read_u32(dev, "apple,sart", &phandle);
	if (ret < 0)
		return ret;
	of_sart = ofnode_get_by_phandle(phandle);
	priv->sart = sart_init(of_sart);
	if (!priv->sart)
		return -EINVAL;

	priv->rtk = apple_rtkit_init(&priv->chan, priv, nvme_handoff_shmem_setup,
				     NULL);
	if (!priv->rtk)
		return -ENOMEM;

	debug("%s: starting the RTKit session of the ANS (CC %#x CSTS %#x)\n",
	      dev->name, readl(&priv->ndev.bar->cc),
	      readl(&priv->ndev.bar->csts));
	ret = apple_rtkit_boot(priv->rtk);
	if (ret < 0) {
		printf("%s: ANS RTKit handshake failed (%d)\n", dev->name, ret);
		return ret;
	}
	priv->woken = true;

	ret = readl_poll_sleep_timeout(priv->base + ANS_BOOT_STATUS, stat,
				       stat == ANS_BOOT_STATUS_OK, 100, 500000);
	if (ret < 0) {
		printf("%s: ANS did not report ready after the handshake\n", dev->name);
		return -ETIMEDOUT;
	}

	/* Answer its log traffic while commands are outstanding. */
	priv->adopted_mbox = true;

	ret = apple_nvme_setup_post_m4(priv);
	if (ret)
		return ret;
	priv->ndev.max_q_depth = ANS_MAX_QUEUE_DEPTH;

	return 0;
}

/*
 * Recover from a command that never completed.  Post-M4 firmware cannot be
 * restarted, but stopping the controller gives up its commands and stops
 * their DMA, as Linux does on a timeout.  Then set up the controller as
 * when adopting it.  U-Boot never resets the session it starts on older
 * controllers, so their stuck queue stays stuck.
 */
static int apple_nvme_reset_ctrl(struct nvme_dev *ndev)
{
	struct apple_nvme_priv *priv =
		container_of(ndev, struct apple_nvme_priv, ndev);
	int qid, ret;

	if (!priv->hw->post_m4)
		return -EOPNOTSUPP;

	ret = apple_nvme_disable_post_m4(priv);
	if (ret)
		return ret;

	ret = apple_nvme_setup_post_m4(priv);
	if (ret)
		return ret;

	/* The commands the TCBs describe are gone. */
	for (qid = 0; qid < NVME_Q_NUM; qid++) {
		if (!priv->tcbs[qid])
			continue;
		memset(priv->tcbs[qid], 0, ANS_NVMMU_TCB_SIZE);
		flush_dcache_range((ulong)priv->tcbs[qid],
				   (ulong)priv->tcbs[qid] + ANS_NVMMU_TCB_SIZE);
	}
	nvme_writeq((ulong)priv->tcbs[NVME_ADMIN_Q],
		    priv->nvmmu + ANS_NVMMU_BASE_ASQ);
	nvme_writeq((ulong)priv->tcbs[NVME_IO_Q],
		    priv->nvmmu + ANS_NVMMU_BASE_IOSQ);

	return 0;
}

static int apple_nvme_boot(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	ofnode of_sart;
	u32 ctrl, stat, phandle;
	int ret;

	ret = reset_get_bulk(dev, &priv->resets);
	if (ret < 0)
		return ret;

	ret = mbox_get_by_index(dev, 0, &priv->chan);
	if (ret < 0)
		return ret;

	ret = dev_read_u32(dev, "apple,sart", &phandle);
	if (ret < 0)
		return ret;

	of_sart = ofnode_get_by_phandle(phandle);
	priv->sart = sart_init(of_sart);
	if (!priv->sart)
		return -EINVAL;

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl | REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	priv->rtk = apple_rtkit_init(&priv->chan, priv, nvme_shmem_setup, nvme_shmem_destroy);
	if (!priv->rtk)
		return -ENOMEM;

	ret = apple_rtkit_boot(priv->rtk);
	if (ret < 0) {
		printf("%s: NVMe apple_rtkit_boot returned: %d\n", __func__, ret);
		return ret;
	}

	ret = readl_poll_sleep_timeout(priv->base + ANS_BOOT_STATUS, stat,
				       (stat == ANS_BOOT_STATUS_OK), 100,
				       500000);
	if (ret < 0) {
		printf("%s: NVMe firmware didn't boot\n", __func__);
		return -ETIMEDOUT;
	}

	writel(ANS_LINEAR_SQ_CTRL_EN, priv->base + ANS_LINEAR_SQ_CTRL);
	writel(((ANS_MAX_QUEUE_DEPTH << 16) | ANS_MAX_QUEUE_DEPTH),
	       priv->base + ANS_MAX_PEND_CMDS_CTRL);

	writel(readl(priv->base + ANS_UNKNOWN_CTRL) & ~ANS_PRP_NULL_CHECK,
	       priv->base + ANS_UNKNOWN_CTRL);

	writel((ANS_NVMMU_TCB_SIZE / ANS_NVMMU_TCB_PITCH) - 1,
	       priv->nvmmu + ANS_NVMMU_NUM);
	writel(0, priv->base + ANS_MODESEL);

	return 0;
}

/*
 * A post-M4 session that could not be adopted will not become adoptable by
 * trying again, and each attempt can wait for controller timeouts.  Every
 * block device scan probes again, so remember the failure.
 */
static bool apple_nvme_adopt_failed;

static int apple_nvme_probe(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	fdt_addr_t addr;
	fdt_size_t nvme_size = 0;
	int ret;

	if (apple_nvme_adopt_failed)
		return -ENODEV;

	priv->hw = (const struct apple_nvme_hw *)dev_get_driver_data(dev);

	if (priv->hw->post_m4) {
		addr = dev_read_addr_size_name(dev, "nvme", &nvme_size);
		if (addr == FDT_ADDR_T_NONE || !nvme_size)
			return -EINVAL;
		priv->base = map_sysmem(addr, nvme_size);
		priv->asc = dev_read_addr_name_ptr(dev, "ans");
		priv->nvmmu = dev_read_addr_name_ptr(dev, "nvmmu");
		if (!priv->base || !priv->asc || !priv->nvmmu)
			return -EINVAL;
	} else {
		priv->base = dev_read_addr_ptr(dev);
		if (!priv->base)
			return -EINVAL;

		addr = dev_read_addr_index(dev, 1);
		if (addr == FDT_ADDR_T_NONE)
			return -EINVAL;
		priv->asc = map_sysmem(addr, 0);
		priv->nvmmu = priv->base;
	}
	/* 26.x firmware ends the NVMe window before the linear doorbells. */
	priv->lsq_in_nvmmu = priv->hw->lsq_in_nvmmu ||
		(priv->hw->post_m4 && nvme_size < ANS_IOSQ_DB + sizeof(u32));
	priv->ndev.bar = priv->base;
	priv->ndev.udev = dev;

	if (priv->hw->post_m4 && apple_nvme_needs_handshake(priv))
		ret = apple_nvme_wake(dev);
	else if (priv->hw->post_m4)
		ret = apple_nvme_adopt(dev);
	else
		ret = apple_nvme_boot(dev);
	if (ret < 0) {
		if (priv->hw->post_m4)
			apple_nvme_adopt_failed = true;
		return ret;
	}

	strcpy(priv->ndev.vendor, "Apple");
	priv->ndev.max_ns_id = priv->hw->max_ns_id;

	ret = nvme_init(dev);
	if (ret && priv->hw->post_m4)
		apple_nvme_adopt_failed = true;

	return ret;
}

static int apple_nvme_remove(struct udevice *dev)
{
	struct apple_nvme_priv *priv = dev_get_priv(dev);
	u32 ctrl;

	if (priv->hw->post_m4) {
		/*
		 * Hand the session over with RTKit live and the controller
		 * enabled and ready, which is what Linux's nvme-apple adopts:
		 * it disables the controller and builds its own queues.
		 * Delete our I/O queues first so that disable never hits a
		 * controller with live I/O queues, and clear their addresses.
		 * Shutting the firmware down instead would leave the machine
		 * without storage until the next reset.
		 *
		 * Without a shutdown nothing commits the controller's volatile
		 * write cache, but every write was flushed before it completed
		 * (see nvme_blk_rw()), so the cache holds nothing of ours.  Do
		 * not flush here: on the A18 Pro a Flush to one of Apple's small
		 * system namespaces (2 to 4) is fatal to the firmware.
		 */
		if (nvme_retire_io_queues(dev))
			printf("%s: could not delete the I/O queues\n", dev->name);
		nvme_writeq(0, priv->base + ANS_IOSQ_BASE);
		nvme_writeq(0, priv->base + ANS_IOCQ_BASE);
		debug("%s: handing over the live ANS session (CC %#x CSTS %#x)\n",
		      dev->name, readl(&priv->ndev.bar->cc),
		      readl(&priv->ndev.bar->csts));
		return 0;
	}

	nvme_shutdown(dev);

	apple_rtkit_shutdown(priv->rtk, APPLE_RTKIT_PWR_STATE_SLEEP);

	ctrl = readl(priv->asc + REG_CPU_CTRL);
	writel(ctrl & ~REG_CPU_CTRL_RUN, priv->asc + REG_CPU_CTRL);

	apple_rtkit_free(priv->rtk);
	priv->rtk = NULL;

	sart_free(priv->sart);
	priv->sart = NULL;

	reset_assert_bulk(&priv->resets);
	reset_deassert_bulk(&priv->resets);

	return 0;
}

static const struct nvme_ops apple_nvme_ops = {
	.setup_queue = apple_nvme_setup_queue,
	.submit_cmd = apple_nvme_submit_cmd,
	.complete_cmd = apple_nvme_complete_cmd,
	.queue_created = apple_nvme_queue_created,
	.poll = apple_nvme_poll,
	.reset_ctrl = apple_nvme_reset_ctrl,
};

static const struct udevice_id apple_nvme_ids[] = {
	{ .compatible = "apple,t6041-nvme-ans2",
	  .data = (ulong)&apple_nvme_t6040_hw },
	{ .compatible = "apple,t8103-nvme-ans2",
	  .data = (ulong)&apple_nvme_t8103_hw },
	{ .compatible = "apple,t8132-nvme-ans2",
	  .data = (ulong)&apple_nvme_t8132_hw },
	{ .compatible = "apple,t8142-nvme-ans2",
	  .data = (ulong)&apple_nvme_t8142_hw },
	{ .compatible = "apple,nvme-ans2",
	  .data = (ulong)&apple_nvme_t8103_hw },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(apple_nvme) = {
	.name = "apple_nvme",
	.id = UCLASS_NVME,
	.of_match = apple_nvme_ids,
	.priv_auto = sizeof(struct apple_nvme_priv),
	.probe = apple_nvme_probe,
	.remove = apple_nvme_remove,
	.ops = &apple_nvme_ops,
	.flags = DM_FLAG_OS_PREPARE,
};
