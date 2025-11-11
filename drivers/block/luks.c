// SPDX-License-Identifier: GPL-2.0+
/*
 * LUKS (Linux Unified Key Setup) filesystem support
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#include <blk.h>
#include <blkmap.h>
#include <dm.h>
#include <hash.h>
#include <hexdump.h>
#include <json.h>
#include <log.h>
#include <luks.h>
#include <memalign.h>
#include <part.h>
#include <uboot_aes.h>
#include <asm/unaligned.h>
#include <linux/byteorder/generic.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <u-boot/sha256.h>
#include <u-boot/sha512.h>
#include "luks_internal.h"

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

		if (IS_ENABLED(CONFIG_JSON)) {
			u64 json_size;
			char *json_start;
			int count;

			/* Read the full header to get JSON area */
			count = (hdr_size + desc->blksz - 1) / desc->blksz;
			ALLOC_CACHE_ALIGN_BUFFER(u8, hdr, count * desc->blksz);

			if (blk_read(blk, pinfo->start, count, hdr) != count) {
				printf("Error: can't read full LUKS2 header\n");
				return -EIO;
			}

			/* JSON starts after the 4096-byte binary header */
			json_start = (char *)(hdr + 4096);
			json_size = hdr_size - 4096;

			printf("\nJSON metadata (%llx bytes):\n", json_size);
			json_print_pretty(json_start, (int)json_size);
		}
	} else {
		printf("Unknown LUKS version\n");
		return -EPROTONOSUPPORT;
	}

	return 0;
}

/**
 * af_hash() - Apply anti-forensic diffusion by hashing each block
 *
 * This applies the LUKS AF-hash diffusion function to a buffer. Each
 * digest-sized chunk is replaced with H(counter || chunk), where H is
 * the specified hash function.
 *
 * @algo:	Hash algorithm to use
 * @key_size:	Size of the buffer to diffuse
 * @block_buf:	Buffer to diffuse in-place
 * Return:	0 on success, -ve on error
 */
static int af_hash(struct hash_algo *algo, size_t key_size, u8 *block_buf)
{
	uint hashcount, finallen, i, digest_size = algo->digest_size;
	u8 input_buf[sizeof(u32) + HASH_MAX_DIGEST_SIZE];
	u8 hash_buf[HASH_MAX_DIGEST_SIZE];

	if (digest_size > HASH_MAX_DIGEST_SIZE)
		return -EINVAL;

	/* Calculate how many full digest blocks fit */
	hashcount = key_size / digest_size;
	finallen = key_size % digest_size;
	if (finallen)
		hashcount++;
	else
		finallen = digest_size;

	/* Hash each chunk with a counter prefix */
	for (i = 0; i < hashcount; i++) {
		size_t chunk_size, input_len;
		u32 iv = cpu_to_be32(i);

		chunk_size = (i == hashcount - 1) ? finallen : digest_size;
		input_len = sizeof(iv) + chunk_size;

		/* Build input: counter || block_chunk */
		memcpy(input_buf, &iv, sizeof(iv));
		memcpy(input_buf + sizeof(iv),
		       block_buf + (i * digest_size), chunk_size);

		/* Hash: H(counter || block_chunk) */
		algo->hash_func_ws(input_buf, input_len, hash_buf,
				   algo->chunk_size);

		/* Replace chunk with its hash */
		memcpy(block_buf + (i * digest_size), hash_buf, chunk_size);
	}

	return 0;
}

/**
 * af_merge() - Merge anti-forensic split key into original key
 *
 * This performs the LUKS AF-merge operation to recover the original key from
 * its AF-split representation. The algorithm XORs all stripes together,
 * applying diffusion between each stripe.
 *
 * @src:	AF-split key material (key_size * stripes bytes)
 * @dst:	Output buffer for merged key (key_size bytes)
 * @key_size:	Size of the original key
 * @stripes:	Number of anti-forensic stripes
 * @hash_spec:	Hash algorithm name (e.g., "sha256")
 * Return:	0 on success, -ve on error
 */
int af_merge(const u8 *src, u8 *dst, size_t key_size, uint stripes,
	     const char *hash_spec)
{
	struct hash_algo *algo;
	u8 block_buf[128];
	int ret;
	uint i;

	/* Look up hash algorithm */
	ret = hash_lookup_algo(hash_spec, &algo);
	if (ret) {
		log_debug("Unsupported hash algorithm: %s\n", hash_spec);
		return -ENOTSUPP;
	}

	if (key_size > sizeof(block_buf))
		return -E2BIG;

	memset(block_buf, '\0', key_size);

	/* Standard LUKS AF-merge algorithm */
	for (i = 0; i < stripes - 1; i++) {
		uint j;

		/* XOR stripe into block_buf */
		for (j = 0; j < key_size; j++)
			block_buf[j] ^= src[i * key_size + j];

		/* Diffuse by hashing */
		ret = af_hash(algo, key_size, block_buf);
		if (ret)
			return ret;
	}

	/* Final XOR with last stripe */
	for (i = 0; i < key_size; i++)
		dst[i] = block_buf[i] ^ src[(stripes - 1) * key_size + i];

	return 0;
}

/**
 * essiv_decrypt() - Decrypt key material using ESSIV mode
 *
 * ESSIV (Encrypted Salt-Sector Initialization Vector) mode generates a unique
 * IV for each sector by encrypting the sector number with a key derived from
 * hashing the encryption key.
 *
 * @derived_key: Key derived from passphrase
 * @key_size: Size of the encryption key in bytes
 * @expkey: Expanded AES key for decryption
 * @km: Encrypted key material buffer
 * @split_key: Output buffer for decrypted key material
 * @km_blocks: Number of blocks of key material
 * @blksz: Block size in bytes
 */
static void essiv_decrypt(u8 *derived_key, uint key_size, u8 *expkey, u8 *km,
			  u8 *split_key, uint km_blocks, uint blksz)
{
	u8 essiv_expkey[AES256_EXPAND_KEY_LENGTH];
	u8 essiv_key_material[SHA256_SUM_LEN];
	u8 iv[AES_BLOCK_LENGTH];
	u32 num_sectors = km_blocks;
	uint rel_sect;

	/* Generate ESSIV key by hashing the encryption key */
	log_debug("using ESSIV mode\n");
	sha256_csum_wd(derived_key, key_size, essiv_key_material,
		       CHUNKSZ_SHA256);

	log_debug_hex("ESSIV key[0-7]:", essiv_key_material, 8);

	/* Expand ESSIV key for AES */
	aes_expand_key(essiv_key_material, 256, essiv_expkey);

	/*
	 * Decrypt each sector with its own IV
	 * NOTE: sector number is relative to the key material buffer,
	 * not an absolute disk sector
	 */
	for (rel_sect = 0; rel_sect < num_sectors; rel_sect++) {
		u8 sector_iv[AES_BLOCK_LENGTH];

		/*
		 * Create IV: little-endian sector number padded to
		 * 16 bytes
		 */
		memset(sector_iv, '\0', AES_BLOCK_LENGTH);
		put_unaligned_le32(rel_sect, sector_iv);

		/* Encrypt sector number with ESSIV key to get IV */
		aes_encrypt(256, sector_iv, essiv_expkey, iv);

		/* Show the first sector for debugging */
		if (!rel_sect) {
			log_debug("rel_sect %x, ", rel_sect);
			log_debug_hex("IV[0-7]:", iv, 8);
		}

		/* Decrypt this sector */
		aes_cbc_decrypt_blocks(key_size * 8, expkey, iv,
				       km + (rel_sect * blksz),
				       split_key + (rel_sect * blksz),
				       blksz / AES_BLOCK_LENGTH);
	}
}

/**
 * try_keyslot() - Unlock a LUKS key slot with a passphrase
 *
 * @blk:		Block device
 * @pinfo:		Partition information
 * @hdr:		LUKS header
 * @slot_idx:		Key slot index to try
 * @pass:		Passphrase to try
 * @md_type:		Hash algorithm type
 * @key_size:		Size of the key
 * @derived_key:	Buffer for derived key (key_size bytes)
 * @km:			Buffer for encrypted key material
 * @km_blocks:		Size of km buffer in blocks
 * @split_key:		Buffer for AF-split key
 * @candidate_key:	Buffer to receive decrypted master key
 *
 * Return: 0 on success (correct passphrase), -EPROTO on mbedtls error, -ve on
 * other error
 */
static int try_keyslot(struct udevice *blk, struct disk_partition *pinfo,
		       struct luks1_phdr *hdr, int slot_idx,
		       const char *pass, mbedtls_md_type_t md_type,
		       uint key_size, u8 *derived_key, u8 *km, uint km_blocks,
		       u8 *split_key, u8 *candidate_key)
{
	struct luks1_keyslot *slot = &hdr->key_slot[slot_idx];
	uint iters, km_offset, stripes, split_key_size;
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	u8 expkey[AES256_EXPAND_KEY_LENGTH];
	u8 key_digest[LUKS_DIGESTSIZE];
	u8 iv[AES_BLOCK_LENGTH];
	int ret;

	/* Check if slot is active */
	if (be32_to_cpu(slot->active) != LUKS_KEY_ENABLED)
		return -ENOENT;

	log_debug("trying key slot %d...\n", slot_idx);

	iters = be32_to_cpu(slot->iterations);
	km_offset = be32_to_cpu(slot->key_material_offset);
	stripes = be32_to_cpu(slot->stripes);
	split_key_size = key_size * stripes;

	/* Derive key from passphrase using PBKDF2 */
	log_debug("PBKDF2(pass '%s'[len %zu], ", pass, strlen(pass));
	log_debug_hex("salt[0-7]", (u8 *)slot->salt, 8);
	log_debug("iter %u, keylen %u)\n", iters, key_size);
	ret = mbedtls_pkcs5_pbkdf2_hmac_ext(md_type, (const u8 *)pass,
					    strlen(pass),
					    (const u8 *)slot->salt,
					    LUKS_SALTSIZE, iters, key_size,
					    derived_key);
	if (ret) {
		log_debug("PBKDF2 failed: %d\n", ret);
		return -EPROTO;
	}

	log_debug_hex("derived_key[0-7]", derived_key, 8);

	/* Read encrypted key material */
	ret = blk_read(blk, pinfo->start + km_offset, km_blocks, km);
	if (ret != km_blocks) {
		log_debug("Failed to read key material\n");
		return -EIO;
	}

	log_debug_hex("km[0-7]", km, 8);

	/* Decrypt key material using derived key */
	log_debug("expand key with key_size*8 %u bits\n", key_size * 8);
	log_debug_hex("input key (derived_key) full:", derived_key, key_size);

	aes_expand_key(derived_key, key_size * 8, expkey);
	log_debug_hex("expanded key [0-15]:", expkey, 16);

	/* Decrypt with CBC mode: first check if ESSIV is used */
	if (strstr(hdr->cipher_mode, "essiv")) {
		essiv_decrypt(derived_key, key_size, expkey, km, split_key,
			      km_blocks, desc->blksz);
	} else {
		/* Plain CBC with zero IV */
		memset(iv, '\0', sizeof(iv));
		log_debug("using plain CBC with zero IV\n");
		log_debug("decrypting %u blocks\n",
			  split_key_size / AES_BLOCK_LENGTH);
		aes_cbc_decrypt_blocks(key_size * 8, expkey, iv, km, split_key,
				       split_key_size / AES_BLOCK_LENGTH);
	}

	log_debug_hex("split_key[0-7]", split_key, 8);

	/* Merge AF-split key */
	ret = af_merge(split_key, candidate_key, key_size, stripes,
		       hdr->hash_spec);
	if (ret) {
		log_debug("af_merge() failed\n");
		return ret;
	}

	log_debug_hex("candidate_key[0-7]", candidate_key, 8);

	/* Verify master key by checking its digest */
	ret = mbedtls_pkcs5_pbkdf2_hmac_ext(md_type, candidate_key, key_size,
					    (const u8 *)hdr->mk_digest_salt,
					    LUKS_SALTSIZE,
					    be32_to_cpu(hdr->mk_digest_iter),
					    LUKS_DIGESTSIZE, key_digest);
	if (ret) {
		log_debug("Master key digest derivation failed\n");
		return EPROTO;
	}

	log_debug_hex("key_digest[0-7]", key_digest, 8);
	log_debug_hex("mk_digest[0-7]", (u8 *)hdr->mk_digest, 8);

	/* Check if the digest matches */
	if (!memcmp(key_digest, hdr->mk_digest, LUKS_DIGESTSIZE)) {
		log_debug("Uunlocked with key slot %d\n", slot_idx);
		return 0;
	}
	log_debug("key slot %d: wrong passphrase\n", slot_idx);

	return -EACCES;
}

int luks_unlock(struct udevice *blk, struct disk_partition *pinfo,
		const char *pass, u8 *master_key, u32 *key_size)
{
	uint version, split_key_size, km_blocks, hdr_blocks;
	u8 *split_key, *derived_key;
	struct hash_algo *hash_algo;
	u8 candidate_key[128], *km;
	mbedtls_md_type_t md_type;
	struct luks1_phdr *hdr;
	struct blk_desc *desc;
	int i, ret;

	if (!blk || !pinfo || !pass || !master_key || !key_size)
		return -EINVAL;

	desc = dev_get_uclass_plat(blk);

	/* LUKS1 header is 592 bytes, calculate blocks needed */
	hdr_blocks = (sizeof(struct luks1_phdr) + desc->blksz - 1) /
			desc->blksz;

	/* Allocate buffer for LUKS header */
	ALLOC_CACHE_ALIGN_BUFFER(u8, buffer, hdr_blocks * desc->blksz);

	/* Read LUKS header */
	if (blk_read(blk, pinfo->start, hdr_blocks, buffer) != hdr_blocks) {
		log_debug("failed to read LUKS header\n");
		return -EIO;
	}

	/* Verify it's LUKS */
	if (memcmp(buffer, LUKS_MAGIC, LUKS_MAGIC_LEN) != 0) {
		log_debug("not a LUKS partition\n");
		return -ENOENT;
	}

	version = be16_to_cpu(*(__be16 *)(buffer + LUKS_MAGIC_LEN));
	if (version != LUKS_VERSION_1) {
		log_debug("unsupported LUKS version %d\n", version);
		return -ENOTSUPP;
	}

	hdr = (struct luks1_phdr *)buffer;

	/* Debug: show what we read from header */
	log_debug("Read header at sector %llu, mk_digest[0-7] ",
		  (unsigned long long)pinfo->start);
	log_debug_hex("", (u8 *)hdr->mk_digest, 8);

	/* Verify cipher mode - only CBC supported */
	if (strncmp(hdr->cipher_mode, "cbc", 3) != 0) {
		log_debug("only CBC mode is currently supported (got: %.32s)\n",
			  hdr->cipher_mode);
		return -ENOTSUPP;
	}

	/* Look up hash algorithm */
	ret = hash_lookup_algo(hdr->hash_spec, &hash_algo);
	if (ret) {
		log_debug("unsupported hash: %.32s\n", hdr->hash_spec);
		return -ENOTSUPP;
	}

	md_type = hash_mbedtls_type(hash_algo);

	*key_size = be32_to_cpu(hdr->key_bytes);

	/* Find the first active slot to get the stripes value */
	u32 stripes = 0;
	for (i = 0; i < LUKS_NUMKEYS; i++) {
		if (be32_to_cpu(hdr->key_slot[i].active) == LUKS_KEY_ENABLED) {
			stripes = be32_to_cpu(hdr->key_slot[i].stripes);
			break;
		}
	}
	if (!stripes) {
		log_debug("no active key slots found\n");
		return -ENOENT;
	}

	split_key_size = *key_size * stripes;

	log_debug("Trying to unlock LUKS partition: key size: %u bytes\n",
		  *key_size);

	/* Allocate buffers */
	derived_key = malloc(*key_size);
	split_key = malloc(split_key_size);
	km_blocks = (split_key_size + desc->blksz - 1) / desc->blksz;
	km = malloc_cache_aligned(km_blocks * desc->blksz);

	if (!derived_key || !split_key || !km) {
		ret = -ENOMEM;
		goto out;
	}

	/* Try each key slot */
	for (i = 0; i < LUKS_NUMKEYS; i++) {
		ret = try_keyslot(blk, pinfo, hdr, i, pass, md_type,
				  *key_size, derived_key, km, km_blocks,
				  split_key, candidate_key);

		if (!ret) {
			/* Successfully unlocked */
			memcpy(master_key, candidate_key, *key_size);
			goto out;
		}
		/* Continue trying other slots on failure */
	}

	log_debug("Failed to unlock: wrong passphrase or no active key slots\n");
	ret = -EACCES;

out:
	if (derived_key) {
		memset(derived_key, '\0', *key_size);
		free(derived_key);
	}
	if (split_key) {
		memset(split_key, '\0', split_key_size);
		free(split_key);
	}
	if (km) {
		memset(km, '\0', km_blocks * desc->blksz);
		free(km);
	}
	memset(candidate_key, '\0', sizeof(candidate_key));

	return ret;
}

/**
 * luks_create_blkmap() - Create a blkmap device for a LUKS partition
 *
 * This creates and configures a blkmap device to provide access to the
 * decrypted contents of a LUKS partition. The master key must already be
 * unlocked using luks_unlock().
 *
 * @blk:	Block device containing the LUKS partition
 * @pinfo:	Partition information
 * @master_key:	Unlocked master key
 * @key_size:	Size of the master key in bytes
 * @label:	Label for the blkmap device
 * @blkmapp:	Output pointer for created blkmap device
 * Return:	0 on success, -ve on error
 */
int luks_create_blkmap(struct udevice *blk, struct disk_partition *pinfo,
		       const u8 *master_key, u32 key_size, const char *label,
		       struct udevice **blkmapp)
{
	u8 essiv_key[SHA256_SUM_LEN];  /* SHA-256 output */
	struct luks1_phdr *hdr;
	struct blk_desc *desc;
	struct udevice *dev;
	uint payload_offset;
	bool use_essiv;
	int ret;

	if (!blk || !pinfo || !master_key || !label || !blkmapp)
		return -EINVAL;

	desc = dev_get_uclass_plat(blk);

	/* Read LUKS header to get payload offset and cipher mode */
	ALLOC_CACHE_ALIGN_BUFFER(u8, buf, desc->blksz);
	if (blk_read(blk, pinfo->start, 1, buf) != 1) {
		log_debug("failed to read LUKS header\n");
		return -EIO;
	}
	hdr = (struct luks1_phdr *)buf;

	/* Create blkmap device */
	ret = blkmap_create(label, &dev);
	if (ret) {
		log_debug("failed to create blkmap device\n");
		return ret;
	}

	/* Check if ESSIV mode is used */
	use_essiv = strstr(hdr->cipher_mode, "essiv");

	if (use_essiv) {
		int hash_size = SHA256_SUM_LEN;

		if (hash_block("sha256", master_key, key_size, essiv_key,
			       &hash_size)) {
			log_debug("SHA256 hash algorithm not available\n");
			blkmap_destroy(dev);
			return -ENOTSUPP;
		}
	}

	/* Map the encrypted partition to the blkmap device */
	payload_offset = be32_to_cpu(hdr->payload_offset);
	log_debug("mapping blkmap: blknr 0 blkcnt %lx payload_offset %x essiv %d\n",
		  (ulong)pinfo->size, payload_offset, use_essiv);
	ret = blkmap_map_crypt(dev, 0, pinfo->size, blk, pinfo->start,
			       master_key, key_size, payload_offset,
			       use_essiv, use_essiv ? essiv_key : NULL);
	if (ret) {
		log_debug("failed to map encrypted partition\n");
		blkmap_destroy(dev);
		return ret;
	}

	/* Wipe ESSIV key from stack */
	if (use_essiv)
		memset(essiv_key, '\0', sizeof(essiv_key));
	*blkmapp = dev;

	return 0;
}
