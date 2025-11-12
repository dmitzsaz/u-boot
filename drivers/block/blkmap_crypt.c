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
 * @cipher_mode: Cipher mode (CBC or XTS)
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
	enum blkmap_crypt_mode cipher_mode;
	u32 sector_size;
	bool use_essiv;
	u8 essiv_key[32];
};

/**
 * process_xts_sector() - Read, decrypt and copy one XTS sector
 *
 * This reads an encrypted sector from disk, decrypts it using AES-XTS with
 * the plain64 IV mode, and copies the requested portion to the output buffer.
 * Handles partial sector reads at the start and end of the requested range.
 *
 * @bmc: Blkmap crypt context with key and device info
 * @ctx: Initialized AES-XTS context
 * @cur_sector: Current XTS sector number being processed
 * @start_sector: First XTS sector in the requested range
 * @end_sector: Last XTS sector in the requested range
 * @blks_per_sect: Number of 512-byte blocks per XTS sector
 * @offset_in_first_sector: Byte offset within first sector to start copying
 * @blkcnt: Total number of blocks requested
 * @buf: Buffer for reading/decrypting one full sector
 * @dest: Output buffer for decrypted data
 * @blksz: Block-device block size (typically 512 bytes)
 * @blocks_donep: Count of blocks copied so far (updated)
 * Return: 0 on success, -EIO on read failure, other negative on decrypt failure
 */
static int process_xts_sector(struct blkmap_crypt *bmc,
			      mbedtls_aes_xts_context *ctx, lbaint_t cur_sector,
			      lbaint_t start_sector, lbaint_t end_sector,
			      uint blks_per_sect, uint offset_in_first_sector,
			      lbaint_t blkcnt, u8 *buf, u8 *dest, uint blksz,
			      lbaint_t *blocks_donep)
{
	lbaint_t start_blk = cur_sector * blks_per_sect;
	lbaint_t src_blk = bmc->blknr + bmc->payload_offset + start_blk;
	uint copy_offset = 0;
	lbaint_t iv_sector;
	u8 data_unit[16];
	uint copy_len;
	lbaint_t j;
	int ret;

	log_debug("XTS: cur_sector=%lu bmc->blknr=%lu bmc->payload_offset=%u src_blk=%lu\n",
		  cur_sector, bmc->blknr, bmc->payload_offset, src_blk);

	/* Read entire sector from disk */
	if (blk_read(bmc->blk, src_blk, blks_per_sect, buf) !=
	    blks_per_sect) {
		log_err("Failed to read sector %lu\n", cur_sector);
		return -EIO;
	}

	/*
	 * Prepare data_unit (IV) for XTS decryption.
	 * For plain64 IV mode, the IV is the 512-byte sector number,
	 * not the larger XTS sector number. This matches dm-crypt behavior.
	 */
	iv_sector = start_blk;
	memset(data_unit, '\0', sizeof(data_unit));
	for (j = 0; j < 8; j++)
		data_unit[j] = (iv_sector >> (j * 8)) & 0xff;

	/* Decrypt entire sector */
	ret = mbedtls_aes_crypt_xts(ctx, MBEDTLS_AES_DECRYPT, bmc->sector_size,
				    data_unit, buf, buf);
	if (ret) {
		log_err("XTS decrypt sector %lu failed: %d\n", cur_sector, ret);
		return ret;
	}

	/* Calculate which portion of this sector to copy */
	if (cur_sector == start_sector)
		copy_offset = offset_in_first_sector;

	if (cur_sector == end_sector) {
		/* Last sector: copy only up to the end of requested data */
		uint remaining = (blkcnt - *blocks_donep) * blksz;

		copy_len = remaining;
	} else {
		/* Not the last sector: copy from offset to end of sector */
		copy_len = bmc->sector_size - copy_offset;
	}

	/* Copy decrypted data to output buffer */
	memcpy(dest + *blocks_donep * blksz, buf + copy_offset, copy_len);
	*blocks_donep += copy_len / blksz;

	return 0;
}

/**
 * crypt_read_xts() - Decrypt data using AES-XTS cipher mode
 *
 * Decrypts blocks from an encrypted device using AES-XTS with plain64 IV mode.
 * Handles requests that span multiple XTS sectors and partial sector reads.
 * The IV for each XTS sector is the 512-byte block number (not the larger
 * XTS sector number), matching dm-crypt's plain64 IV generation.
 *
 * @bm: Blkmap device context
 * @bmc: Blkmap crypt context with encryption parameters
 * @blknr: Starting block number (relative to decrypted device)
 * @blkcnt: Number of blocks to read
 * @buffer: Output buffer for decrypted data
 * Return: number of blocks successfully decrypted, or negative error code
 *         (-ENOMEM if buffer allocation failed, -EINVAL if key setup failed,
 *         -EIO or other negative on sector read/decrypt failure)
 */
static ulong crypt_read_xts(struct blkmap *bm, struct blkmap_crypt *bmc,
			    lbaint_t blknr, lbaint_t blkcnt, void *out_buf)
{
	struct blk_desc *bd = dev_get_uclass_plat(bm->blk);
	lbaint_t start_sector, end_sector, cur_sect;
	uint offset_in_first_sector;
	mbedtls_aes_xts_context ctx;
	lbaint_t blocks_done;
	uint blks_per_sect;
	u8 *buf;
	int ret;

	blks_per_sect = bmc->sector_size / bd->blksz;

	log_debug("key_size=%u blkcnt=%lu\n", bmc->key_size, blkcnt);
	log_debug("XTS: sector_size=%u blocks_per_sector=%u\n",
		  bmc->sector_size, blks_per_sect);
	log_debug("Master key (all %u bytes):\n", bmc->key_size);
	log_debug_hex("", bmc->master_key, bmc->key_size);

	/* Calculate which encryption sectors we need */
	start_sector = blknr / blks_per_sect;
	end_sector = (blknr + blkcnt - 1) / blks_per_sect;
	offset_in_first_sector = (blknr % blks_per_sect) * bd->blksz;

	log_debug("XTS: blknr=%lu blkcnt=%lu start_sector=%lu end_sector=%lu offset=%u\n",
		  blknr, blkcnt, start_sector, end_sector,
		  offset_in_first_sector);

	/* Allocate buffer for one full sector */
	buf = malloc_cache_aligned(bmc->sector_size);
	if (!buf) {
		log_err("Failed to allocate sector buffer\n");
		return -ENOMEM;
	}

	mbedtls_aes_xts_init(&ctx);
	ret = mbedtls_aes_xts_setkey_dec(&ctx, bmc->master_key,
					 bmc->key_size * 8);
	if (ret) {
		log_err("XTS setkey_dec failed: %d\n", ret);
		mbedtls_aes_xts_free(&ctx);
		free(buf);
		return -EINVAL;
	}

	/* Process each sector */
	blocks_done = 0;
	for (cur_sect = start_sector; cur_sect <= end_sector; cur_sect++) {
		ret = process_xts_sector(bmc, &ctx, cur_sect, start_sector,
					 end_sector, blks_per_sect,
					 offset_in_first_sector, blkcnt,
					 buf, out_buf, bd->blksz, &blocks_done);
		if (ret) {
			mbedtls_aes_xts_free(&ctx);
			free(buf);
			return ret;
		}
	}

	free(buf);
	mbedtls_aes_xts_free(&ctx);

	log_debug("XTS decryption completed successfully for %lu blocks\n", blkcnt);
	if (blknr == 0 && blkcnt >= 1) {
		log_debug("First 32 bytes of decrypted data:\n");
		log_debug_hex("", out_buf, 32);
	}

	return blkcnt;
}

/**
 * crypt_read_cbc() - Decrypt data using AES-CBC cipher mode
 *
 * Decrypts blocks from an encrypted device using AES-CBC. Supports both
 * plain64 mode (IV = sector number) and ESSIV mode (IV = AES_encrypt(sector
 * number, SHA256(master_key))). Used for LUKS1 volumes.
 *
 * @bm: Blkmap device context
 * @bmc: Blkmap crypt context with encryption parameters and ESSIV key
 * @blknr: Starting block number (relative to decrypted device)
 * @blkcnt: Number of blocks to decrypt
 * @encrypted_buf: Buffer containing encrypted data (already read from disk)
 * @buffer: Output buffer for decrypted data
 * Return: number of blocks successfully decrypted
 */
static ulong crypt_read_cbc(struct blkmap *bm, struct blkmap_crypt *bmc,
			    lbaint_t blknr, lbaint_t blkcnt,
			    u8 *encrypted_buf, void *buffer)
{
	struct blk_desc *bd = dev_get_uclass_plat(bm->blk);
	u8 expkey[AES256_EXPAND_KEY_LENGTH];
	u8 iv[AES_BLOCK_LENGTH];
	u8 *dest = buffer;
	u64 sector;
	lbaint_t i;

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

	return blkcnt;
}

static ulong blkmap_crypt_read(struct blkmap *bm, struct blkmap_slice *bms,
			       lbaint_t blknr, lbaint_t blkcnt, void *buffer)
{
	struct blkmap_crypt *bmc = container_of(bms, struct blkmap_crypt, slice);
	struct blk_desc *src_bd = dev_get_uclass_plat(bmc->blk);
	lbaint_t src_blknr, blocks_read;
	u8 *encrypted_buf;
	ulong result;

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

	if (blknr == 0 && blkcnt >= 1) {
		log_debug("First 32 bytes of ENCRYPTED data:\n");
		log_debug_hex("", encrypted_buf, 32);
	}

	if (bmc->cipher_mode == BLKMAP_CRYPT_MODE_XTS) {
		result = crypt_read_xts(bm, bmc, blknr, blkcnt, buffer);
		/* XTS reads its own data, so free encrypted_buf early */
		free(encrypted_buf);
		/* Check for error - result will be negative on failure */
		if ((long)result < 0)
			return 0;
		return result;
	}

	result = crypt_read_cbc(bm, bmc, blknr, blkcnt, encrypted_buf, buffer);
	free(encrypted_buf);

	return result;
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
		     enum blkmap_crypt_mode cipher_mode, u32 sector_size,
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
	bmc->cipher_mode = cipher_mode;
	bmc->sector_size = sector_size;
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
