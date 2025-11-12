// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Canonical Ltd
 * Author: Simon Glass <simon.glass@canonical.com>
 *
 * LUKS encryption/decryption support
 */

#include <blk.h>
#include <blkmap.h>
#include <dm.h>
#include <malloc.h>
#include <memalign.h>
#include <uboot_aes.h>
#include <mbedtls/aes.h>
#include <dm/device-internal.h>
#include <linux/string.h>
#include "blkmap_internal.h"

/**
 * struct blkmap_crypt - Encrypted mapping
 *
 * Data associated with an encrypted region of a block device (e.g., LUKS).
 * Provides on-the-fly decryption of data using AES-CBC or AES-XTS modes.
 *
 * @slice: Common slice data (must be first member)
 * @blk: Underlying block device containing encrypted data
 * @blknr: Start block of the underlying block device
 * @master_key: Decrypted master key for decryption
 * @key_size: Size of the master key in bytes (must be <= 128)
 * @payload_offset: Offset in sectors from lblknr to actual encrypted payload
 * @sector_size: Sector size for IV calculation (typically 512 or 4096)
 * @use_essiv: True if ESSIV mode is used for IV generation (CBC only)
 * @essiv_key: ESSIV key (SHA256 hash of master key)
 */
struct blkmap_crypt {
	struct blkmap_slice slice;
	struct udevice *blk;
	lbaint_t blknr;
	u8 master_key[128];
	u32 key_size;
	u32 payload_offset;
	u32 sector_size;
	bool use_essiv;
	u8 essiv_key[32];
};

static ulong blkmap_crypt_read(struct blkmap *bm, struct blkmap_slice *bms,
			       lbaint_t blknr, lbaint_t blkcnt, void *buffer)
{
	struct blkmap_crypt *bmc = container_of(bms, struct blkmap_crypt, slice);
	struct blk_desc *bd = dev_get_uclass_plat(bm->blk);
	struct blk_desc *src_bd = dev_get_uclass_plat(bmc->blk);
	lbaint_t src_blknr, blocks_read;
	u8 *encrypted_buf, *dest = buffer;
	u8 expkey[AES256_EXPAND_KEY_LENGTH];
	u8 iv[AES_BLOCK_LENGTH];
	u64 sector;
	lbaint_t i;

	/* Allocate buffer for encrypted data */
	encrypted_buf = malloc_cache_aligned(blkcnt * src_bd->blksz);
	if (!encrypted_buf)
		return 0;

	/*
	 * Calculate source block number (LUKS payload offset + requested
	 * block)
	 */
	src_blknr = bmc->blknr + bmc->payload_offset + blknr;

	/* Read encrypted data from underlying device */
	blocks_read = blk_read(bmc->blk, src_blknr, blkcnt, encrypted_buf);
	if (blocks_read != blkcnt) {
		free(encrypted_buf);
		return 0;
	}

	/* Expand AES key */
	aes_expand_key(bmc->master_key, bmc->key_size * 8, expkey);

	/* Decrypt each sector */
	for (i = 0; i < blkcnt; i++) {
		/* Calculate sector number for IV */
		sector = blknr + i;

		if (bmc->use_essiv) {
			/*
			 * ESSIV mode:
			 * IV = AES_encrypt(sector_number, SHA256(master_key))
			 */
			u8 essiv_expkey[AES256_EXPAND_KEY_LENGTH];
			u8 sector_iv[AES_BLOCK_LENGTH];

			/* Create sector number as IV input (little-endian) */
			memset(sector_iv, '\0', sizeof(sector_iv));
			*(u64 *)sector_iv = cpu_to_le64(sector);

			/* Expand ESSIV key */
			aes_expand_key(bmc->essiv_key, 256, essiv_expkey);

			/* Encrypt sector number with ESSIV key to get IV */
			aes_encrypt(256, sector_iv, essiv_expkey, iv);
		} else {
			/*
			 * Plain64 mode:
			 * IV is sector number in little-endian format
			 */
			memset(iv, '\0', sizeof(iv));
			*(u64 *)iv = cpu_to_le64(sector);
		}

		/* Decrypt sector using AES-CBC */
		aes_cbc_decrypt_blocks(bmc->key_size * 8, expkey, iv,
				       encrypted_buf + i * bd->blksz,
				       dest + i * bd->blksz,
				       bd->blksz / AES_BLOCK_LENGTH);
	}
	free(encrypted_buf);

	return blkcnt;
}

static void blkmap_crypt_destroy(struct blkmap *bm, struct blkmap_slice *bms)
{
	struct blkmap_crypt *bmc = container_of(bms, struct blkmap_crypt, slice);

	/* Securely wipe master key before freeing */
	memset(bmc->master_key, '\0', sizeof(bmc->master_key));
	free(bmc);
}

int blkmap_map_crypt(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		     struct udevice *lblk, lbaint_t lblknr,
		     const u8 *master_key, u32 key_size, u32 payload_offset,
		     bool use_essiv, const u8 *essiv_key)
{
	struct blkmap *bm = dev_get_plat(dev);
	struct blkmap_crypt *bmc;
	int err;

	if (key_size > 128)
		return -EINVAL;

	bmc = malloc(sizeof(*bmc));
	if (!bmc)
		return -ENOMEM;

	bmc->blk = lblk;
	bmc->blknr = lblknr;
	bmc->key_size = key_size;
	bmc->payload_offset = payload_offset;
	bmc->use_essiv = use_essiv;
	memcpy(bmc->master_key, master_key, key_size);

	if (use_essiv && essiv_key)
		memcpy(bmc->essiv_key, essiv_key, sizeof(bmc->essiv_key));
	else
		memset(bmc->essiv_key, '\0', sizeof(bmc->essiv_key));

	bmc->slice.blknr = blknr;
	bmc->slice.blkcnt = blkcnt;
	bmc->slice.read = blkmap_crypt_read;
	bmc->slice.write = NULL;  /* Read-only for now */
	bmc->slice.destroy = blkmap_crypt_destroy;

	err = blkmap_slice_add(bm, &bmc->slice);
	if (err)
		free(bmc);

	return err;
}
