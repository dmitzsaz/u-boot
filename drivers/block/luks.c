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
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, buffer, desc->blksz);
	int version;


	/* Read first block of the partition */
	if (blk_read(blk, pinfo->start, 1, buffer) != 1) {
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

int luks_show_info(struct udevice *blk, struct disk_partition *pinfo)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, buffer, desc->blksz);
	int version;

	/* Read first block of the partition */
	if (blk_read(blk, pinfo->start, 1, buffer) != 1) {
		printf("Error: failed to read LUKS header\n");
		return -EIO;
	}

	/* Check for LUKS magic bytes */
	if (memcmp(buffer, LUKS_MAGIC, LUKS_MAGIC_LEN)) {
		printf("Not a LUKS partition\n");
		return -ENOENT;
	}

	/* Read version field */
	version = be16_to_cpu(*(__be16 *)(buffer + LUKS_MAGIC_LEN));

	printf("Version:        %d\n", version);
	if (version == LUKS_VERSION_1) {
		struct luks1_phdr *luks1_hdr = (struct luks1_phdr *)buffer;

		printf("Cipher name:    %.32s\n", luks1_hdr->cipher_name);
		printf("Cipher mode:    %.32s\n", luks1_hdr->cipher_mode);
		printf("Hash spec:      %.32s\n", luks1_hdr->hash_spec);
		printf("Payload offset: %u sectors\n",
		       be32_to_cpu(luks1_hdr->payload_offset));
		printf("Key bytes:      %u\n",
		       be32_to_cpu(luks1_hdr->key_bytes));
	} else if (version == LUKS_VERSION_2) {
		struct luks2_hdr *luks2_hdr = (struct luks2_hdr *)buffer;
		u64 hdr_size;

		hdr_size = be64_to_cpu(luks2_hdr->hdr_size);

		printf("Header size:    %llu bytes\n", hdr_size);
		printf("Sequence ID:    %llu\n", be64_to_cpu(luks2_hdr->seqid));
		printf("UUID:           %.40s\n", luks2_hdr->uuid);
		printf("Label:          %.48s\n", luks2_hdr->label);
		printf("Checksum alg:   %.32s\n", luks2_hdr->csum_alg);
	} else {
		printf("Unknown LUKS version\n");
		return -EPROTONOSUPPORT;
	}

	return 0;
}
