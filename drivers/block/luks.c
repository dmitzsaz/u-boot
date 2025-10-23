// SPDX-License-Identifier: GPL-2.0+
/*
 * LUKS (Linux Unified Key Setup) filesystem support
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#include <blk.h>
#include <dm.h>
#include <hexdump.h>
#include <log.h>
#include <luks.h>
#include <memalign.h>
#include <part.h>
#include <uboot_aes.h>
#include <linux/byteorder/generic.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

int luks_get_version(struct udevice *blk, struct disk_partition *pinfo)
{
	struct blk_desc *desc;
	int version;

	desc = dev_get_uclass_plat(blk);

	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, buffer, desc->blksz);

	/* Read first block of the partition */
	if (blk_dread(desc, pinfo->start, 1, buffer) != 1) {
		log_debug("Error: failed to read LUKS header\n");
		return -EIO;
	}

	/* Check for LUKS magic bytes */
	if (memcmp(buffer, LUKS_MAGIC, LUKS_MAGIC_LEN))
		return -ENOENT;

	/* Read version field (16-bit big-endian at offset 6) */
	version = be16_to_cpu(*(__be16 *)(buffer + LUKS_MAGIC_LEN));

	/* Validate version */
	if (version != LUKS_VERSION_1 && version != LUKS_VERSION_2) {
		log_debug("Warning: unknown LUKS version %d\n", version);
		return -EPROTONOSUPPORT;
	}

	return version;
}

int luks_detect(struct udevice *blk, struct disk_partition *pinfo)
{
	int version;

	version = luks_get_version(blk, pinfo);
	if (IS_ERR_VALUE(version))
		return version;

	return 0;
}
