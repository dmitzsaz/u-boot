// SPDX-License-Identifier: GPL-2.0+
/*
 * LUKS2 (Linux Unified Key Setup version 2) support
 *
 * Copyright (C) 2025 Canonical Ltd
 */

/* #define LOG_DEBUG */

#include <abuf.h>
#include <blk.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <hash.h>
#include <json.h>
#include <log.h>
#include <luks.h>
#include <memalign.h>
#include <part.h>
#include <uboot_aes.h>
#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <u-boot/sha256.h>
#include <argon2.h>
#include "luks_internal.h"

/**
 * enum luks2_kdf_type - LUKS2 KDF type
 *
 * @LUKS2_KDF_PBKDF2: PBKDF2 key derivation function
 * @LUKS2_KDF_ARGON2I: Argon2i key derivation function
 * @LUKS2_KDF_ARGON2ID: Argon2id key derivation function
 */
enum luks2_kdf_type {
	LUKS2_KDF_PBKDF2,
	LUKS2_KDF_ARGON2I,
	LUKS2_KDF_ARGON2ID,
};

/**
 * struct luks2_digest - LUKS2 digest information
 *
 * @type: Digest KDF type
 * @hash: Hash algorithm name (e.g., "sha256")
 * @iters: PBKDF2 iteration count (valid if type == LUKS2_KDF_PBKDF2)
 * @time: Argon2 time cost parameter (valid if type == LUKS2_KDF_ARGON2*)
 * @memory: Argon2 memory cost parameter in KB (type == LUKS2_KDF_ARGON2*)
 * @cpus: Argon2 parallelism/lanes parameter (type == LUKS2_KDF_ARGON2*)
 * @salt: Decoded salt value
 * @salt_len: Actual length of decoded salt
 * @digest: Decoded digest (master key verification value)
 * @digest_len: Actual length of decoded digest
 */
struct luks2_digest {
	enum luks2_kdf_type type;
	const char *hash;
	u32 iters;
	u32 time;
	u32 memory;
	u32 cpus;
	u8 salt[LUKS_SALTSIZE];
	int salt_len;
	u8 digest[128];
	int digest_len;
};

/**
 * struct luks2_kdf - LUKS2 keyslot KDF parameters
 * @type: KDF type
 * @salt: Decoded KDF salt
 * @salt_len: Actual length of decoded salt
 * @iters: PBKDF2 iteration count (valid if type == LUKS2_KDF_PBKDF2)
 * @time: Argon2 time cost parameter (valid if type == LUKS2_KDF_ARGON2*)
 * @memory: Argon2 memory cost parameter in KB (type == LUKS2_KDF_ARGON2*)
 * @cpus: Argon2 parallelism/lanes parameter (type == LUKS2_KDF_ARGON2*)
 */
struct luks2_kdf {
	enum luks2_kdf_type type;
	u8 salt[LUKS_SALTSIZE];
	int salt_len;
	u32 iters;
	u32 time;
	u32 memory;
	u32 cpus;
};

/**
 * struct luks2_area - LUKS2 keyslot encrypted area parameters
 * @offset: Byte offset from partition start where key material is stored
 * @size: Size of encrypted key material in bytes
 * @encryption: Encryption mode string (e.g., "aes-xts-plain64")
 * @key_size: Encryption key size in bytes (32 for AES-256, 64 for XTS-512)
 */
struct luks2_area {
	u64 offset;
	u64 size;
	const char *encryption;
	u32 key_size;
};

/**
 * struct luks2_af - LUKS2 keyslot anti-forensic parameters
 * @stripes: Number of anti-forensic stripes (typically 4000)
 * @hash: Hash algorithm name for AF merge operation
 */
struct luks2_af {
	u32 stripes;
	const char *hash;
};

/**
 * struct luks2_keyslot - LUKS2 keyslot information
 * @type: Keyslot type (should be "luks2")
 * @key_size: Size of the master key in bytes
 * @kdf: Key derivation function parameters
 * @af: Anti-forensic parameters
 * @area: Encrypted key material area parameters
 */
struct luks2_keyslot {
	const char *type;
	u32 key_size;
	struct luks2_kdf kdf;
	struct luks2_af af;
	struct luks2_area area;
};

/**
 * str_to_kdf_type() - Convert KDF type string to enum
 *
 * @type_str: KDF type string ("pbkdf2", "argon2i", or "argon2id")
 * Return: enum luks2_kdf_type value, or negative error code if unknown type
 */
static int str_to_kdf_type(const char *type_str)
{
	if (!type_str)
		return -EINVAL;

	if (!strcmp(type_str, "pbkdf2"))
		return LUKS2_KDF_PBKDF2;
	if (!strcmp(type_str, "argon2i"))
		return LUKS2_KDF_ARGON2I;
	if (!strcmp(type_str, "argon2id"))
		return LUKS2_KDF_ARGON2ID;

	return -ENOTSUPP;
}

/* Base64 decode wrapper for LUKS2 */
static int base64_decode(const char *in, u8 *out, int out_len)
{
	size_t olen;
	int ret;

	ret = mbedtls_base64_decode(out, out_len, &olen,
				    (const unsigned char *)in, strlen(in));
	if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
		return -ENOSPC;
	if (ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER)
		return -EINVAL;
	if (ret)
		return -EINVAL;

	return olen;
}

/**
 * read_digest_info() - Read LUKS2 digest information from ofnode
 *
 * @digest_node: ofnode for the digest (e.g., digest "0")
 * @digest: Pointer to digest structure to fill
 * Return: 0 on success, -ve on error
 */
static int read_digest_info(ofnode digest_node, struct luks2_digest *digest)
{
	const char *salt_b64, *digest_b64;

	const char *type_str;
	int ret;

	memset(digest, '\0', sizeof(*digest));

	/* Read and convert digest type */
	type_str = ofnode_read_string(digest_node, "type");
	ret = str_to_kdf_type(type_str);
	if (ret < 0) {
		log_debug("LUKS2: unsupported digest type %s\n", type_str);
		return ret;
	}
	digest->type = ret;

	/* Check if Argon2 is supported if needed */
	if ((digest->type == LUKS2_KDF_ARGON2I ||
	     digest->type == LUKS2_KDF_ARGON2ID) &&
	    !IS_ENABLED(CONFIG_ARGON2)) {
		log_debug("LUKS2: Argon2 not supported\n");
		return -ENOTSUPP;
	}

	/* Read hash algorithm */
	digest->hash = ofnode_read_string(digest_node, "hash");
	if (!digest->hash)
		return -EINVAL;

	/* Read KDF-specific parameters */
	if (digest->type == LUKS2_KDF_PBKDF2) {
		/* PBKDF2 */
		if (ofnode_read_u32(digest_node, "iterations", &digest->iters))
			return -EINVAL;
	} else {
		/* Argon2 */
		if (ofnode_read_u32(digest_node, "time", &digest->time) ||
		    ofnode_read_u32(digest_node, "memory", &digest->memory) ||
		    ofnode_read_u32(digest_node, "cpus", &digest->cpus))
			return -EINVAL;
	}

	/* Read and decode salt */
	salt_b64 = ofnode_read_string(digest_node, "salt");
	if (!salt_b64)
		return -EINVAL;
	digest->salt_len = base64_decode(salt_b64, digest->salt,
					 sizeof(digest->salt));
	if (digest->salt_len <= 0)
		return -EINVAL;

	/* Read and decode digest */
	digest_b64 = ofnode_read_string(digest_node, "digest");
	if (!digest_b64)
		return -EINVAL;
	digest->digest_len = base64_decode(digest_b64, digest->digest,
					   sizeof(digest->digest));
	if (digest->digest_len <= 0)
		return -EINVAL;

	return 0;
}

/**
 * read_keyslot_info() - Read LUKS2 keyslot information from ofnode
 *
 * @keyslot_node: ofnode for the keyslot (e.g., keyslot "0")
 * @keyslot: Pointer to keyslot structure to fill
 * @hash_name: Hash name to use for AF (from digest)
 * Return: 0 on success, -ve on error
 */
static int read_keyslot_info(ofnode keyslot_node, struct luks2_keyslot *keyslot,
			     const char *hash_name)
{
	const char *salt_b64, *offset_str, *size_str;
	ofnode kdf_node, af_node, area_node;
	int ret;

	memset(keyslot, '\0', sizeof(*keyslot));

	/* Read keyslot type */
	keyslot->type = ofnode_read_string(keyslot_node, "type");
	if (!keyslot->type || strcmp(keyslot->type, "luks2"))
		return -EINVAL;

	/* Read key size */
	if (ofnode_read_u32(keyslot_node, "key_size", &keyslot->key_size))
		return -EINVAL;

	/* Navigate to and read KDF node */
	kdf_node = ofnode_find_subnode(keyslot_node, "kdf");
	if (!ofnode_valid(kdf_node))
		return -EINVAL;

	offset_str = ofnode_read_string(kdf_node, "type");
	ret = str_to_kdf_type(offset_str);
	if (ret < 0) {
		log_debug("LUKS2: unsupported KDF type %s\n", offset_str);
		return ret;
	}
	keyslot->kdf.type = ret;

	/* Check if Argon2 is supported if needed */
	if ((keyslot->kdf.type == LUKS2_KDF_ARGON2I ||
	     keyslot->kdf.type == LUKS2_KDF_ARGON2ID) &&
	    !IS_ENABLED(CONFIG_ARGON2)) {
		log_debug("LUKS2: Argon2 not supported\n");
		return -ENOTSUPP;
	}

	/* Read KDF salt */
	salt_b64 = ofnode_read_string(kdf_node, "salt");
	if (!salt_b64)
		return -EINVAL;
	keyslot->kdf.salt_len = base64_decode(salt_b64, keyslot->kdf.salt,
					      sizeof(keyslot->kdf.salt));
	if (keyslot->kdf.salt_len <= 0)
		return -EINVAL;

	/* Read KDF-specific parameters */
	if (keyslot->kdf.type == LUKS2_KDF_PBKDF2) {
		if (ofnode_read_u32(kdf_node, "iterations", &keyslot->kdf.iters))
			return -EINVAL;
	} else {
		/* Argon2 */
		if (ofnode_read_u32(kdf_node, "time", &keyslot->kdf.time) ||
		    ofnode_read_u32(kdf_node, "memory", &keyslot->kdf.memory) ||
		    ofnode_read_u32(kdf_node, "cpus", &keyslot->kdf.cpus))
			return -EINVAL;
	}

	/* Navigate to and read AF node */
	af_node = ofnode_find_subnode(keyslot_node, "af");
	if (!ofnode_valid(af_node))
		return -EINVAL;

	if (ofnode_read_u32(af_node, "stripes", &keyslot->af.stripes))
		keyslot->af.stripes = 4000;  /* Default */
	keyslot->af.hash = hash_name;

	/* Navigate to and read area node */
	area_node = ofnode_find_subnode(keyslot_node, "area");
	if (!ofnode_valid(area_node))
		return -EINVAL;

	/* Read offset and size (strings in LUKS2 JSON) */
	offset_str = ofnode_read_string(area_node, "offset");
	if (!offset_str)
		return -EINVAL;
	keyslot->area.offset = simple_strtoull(offset_str, NULL, 10);

	size_str = ofnode_read_string(area_node, "size");
	if (!size_str)
		return -EINVAL;
	keyslot->area.size = simple_strtoull(size_str, NULL, 10);

	/* Read encryption mode */
	keyslot->area.encryption = ofnode_read_string(area_node, "encryption");
	if (!keyslot->area.encryption)
		return -EINVAL;

	/* Read area key size */
	if (ofnode_read_u32(area_node, "key_size", &keyslot->area.key_size))
		return -EINVAL;

	return 0;
}

/**
 * read_luks2_info() - Read and parse LUKS2 header and metadata
 *
 * @blk: Block device
 * @pinfo: Partition information
 * @fdt_buf: Buffer to hold the converted FDT (caller must uninit)
 * @digest: Pointer to digest structure to fill
 * @md_type: Pointer to receive mbedtls MD type
 * @keyslots_node: Pointer to receive keyslots ofnode
 * Return: 0 on success, -ve on error
 */
static int read_luks2_info(struct udevice *blk, struct disk_partition *pinfo,
			   struct abuf *fdt_buf, struct luks2_digest *digest,
			   mbedtls_md_type_t *md_typep, ofnode *keyslots_nodep)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	ALLOC_CACHE_ALIGN_BUFFER(u8, buffer, desc->blksz);
	ofnode root, digests_node, digest0;
	struct hash_algo *hash_algo;
	mbedtls_md_type_t md_type;
	struct luks2_hdr *hdr;
	ofnode keyslots_node;
	char *json_data;
	int count, ret;
	u64 hdr_size;
	oftree tree;

	abuf_init(fdt_buf);

	/* Read LUKS2 header */
	if (blk_read(blk, pinfo->start, 1, buffer) != 1)
		return -EIO;

	hdr = (struct luks2_hdr *)buffer;
	hdr_size = be64_to_cpu(hdr->hdr_size);

	log_debug("LUKS2: header size %llu bytes\n", hdr_size);

	/* Allocate and read full header with JSON */
	count = (hdr_size + desc->blksz - 1) / desc->blksz;
	json_data = malloc_cache_aligned(count * desc->blksz);
	if (!json_data)
		return -ENOMEM;

	if (blk_read(blk, pinfo->start, count, json_data) != count) {
		ret = -EIO;
		goto out;
	}

	ret = -EINVAL;

	/* JSON starts after a 4K binary header: convert to FDT */
	if (json_to_fdt(json_data + 4096, fdt_buf)) {
		log_err("Failed to convert JSON to FDT\n");
		goto out;
	}

	/* Create oftree from FDT */
	tree = oftree_from_fdt(abuf_data(fdt_buf));
	if (!oftree_valid(tree))
		goto out;

	/* Get root node */
	root = oftree_root(tree);
	if (!ofnode_valid(root))
		goto out;

	/* Navigate to digests node and get digest 0 */
	digests_node = ofnode_find_subnode(root, "digests");
	if (!ofnode_valid(digests_node))
		goto out;

	digest0 = ofnode_find_subnode(digests_node, "0");
	if (!ofnode_valid(digest0))
		goto out;

	/* Read digest information */
	ret = read_digest_info(digest0, digest);
	if (ret)
		goto out;

	/* Get hash algorithm */
	ret = hash_lookup_algo(digest->hash, &hash_algo);
	if (ret) {
		log_debug("Unsupported hash: %s\n", digest->hash);
		ret = -ENOTSUPP;
		goto out;
	}
	md_type = hash_mbedtls_type(hash_algo);

	/* Navigate to keyslots node */
	keyslots_node = ofnode_find_subnode(root, "keyslots");
	if (!ofnode_valid(keyslots_node)) {
		ret = -EINVAL;
		goto out;
	}

	*md_typep = md_type;
	*keyslots_nodep = keyslots_node;

out:
	memset(json_data, '\0', count * desc->blksz);
	free(json_data);
	if (ret)
		abuf_uninit(fdt_buf);

	return ret;
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
static void essiv_decrypt(u8 *derived_key, uint key_size, u8 *expkey,
			  u8 *km, u8 *split_key, uint km_blocks, uint blksz)
{
	u8 essiv_expkey[AES256_EXPAND_KEY_LENGTH];
	u8 essiv_key_material[SHA256_SUM_LEN];
	u32 num_sectors = km_blocks;
	u8 iv[AES_BLOCK_LENGTH];
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

		/* Create IV: little-endian sector number padded to 16 bytes */
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
 * decrypt_km_xts() - Decrypt key material using XTS mode
 *
 * Decrypts LUKS2 keyslot key material encrypted with AES-XTS mode.
 * XTS mode uses 512-byte sectors with sector numbers as tweaks.
 *
 * @derived_key: Key derived from passphrase using KDF
 * @key_size: Size of the derived key in bytes (32 or 64 for XTS)
 * @km: Encrypted key material buffer
 * @split_key: Output buffer for decrypted split key
 * @size: Size of the split key in bytes
 * Return: 0 on success, negative error code on failure
 */
static int decrypt_km_xts(const u8 *derived_key, uint key_size, const u8 *km,
			  u8 *split_key, int size)
{
	mbedtls_aes_xts_context ctx;
	const int blksize = 512;
	u8 data_unit[16];
	u64 sector;
	int ret;

	/* Verify key size is valid for XTS (32 or 64 bytes) */
	if (key_size != 32 && key_size != 64) {
		log_err("Unsupported XTS key size: %u\n", key_size);
		return -EINVAL;
	}

	mbedtls_aes_xts_init(&ctx);
	ret = mbedtls_aes_xts_setkey_dec(&ctx, derived_key, key_size * 8);
	if (ret) {
		log_err("Failed to set XTS key: %d\n", ret);
		mbedtls_aes_xts_free(&ctx);
		return -EINVAL;
	}

	/*
	 * XTS uses data unit (sector) as tweak
	 * LUKS2 uses 512-byte sectors for keyslot area
	 * Sector number is relative to start of keyslot area (not absolute)
	 */
	sector = 0;

	/*
	 * Decrypt in chunks (XTS requires whole sectors)
	 * Each sector has its own data_unit/tweak value
	 */
	for (u64 pos = 0; pos < size; pos += blksize) {
		uint todo;

		todo = (size - pos > blksize) ? blksize : (size - pos);

		/* Prepare data_unit (sector number in little-endian) */
		memset(data_unit, '\0', sizeof(data_unit));
		for (int i = 0; i < 8; i++)
			data_unit[i] = (sector >> (i * 8)) & 0xFF;

		ret = mbedtls_aes_crypt_xts(&ctx, MBEDTLS_AES_DECRYPT, todo,
					    data_unit, km + pos,
					    split_key + pos);
		if (ret) {
			log_err("XTS decryption failed at sector %llu: %d\n",
				sector, ret);
			mbedtls_aes_xts_free(&ctx);
			return -EINVAL;
		}
		sector++;
	}

	mbedtls_aes_xts_free(&ctx);

	return 0;
}

/**
 * decrypt_km_cbc() - Decrypt key material using CBC mode
 *
 * Decrypts LUKS keyslot key material encrypted with AES-CBC mode.
 * Supports both ESSIV mode and plain CBC with zero IV.
 *
 * @derived_key: Key derived from passphrase using KDF
 * @key_size: Size of the derived key in bytes
 * @encrypt: Encryption-specification string (may contain "essiv")
 * @km: Encrypted key material buffer
 * @split_key: Output buffer for decrypted split key
 * @size: Size of the split key in bytes
 * @km_blocks: Number of blocks in key material
 * @blksz: Block size in bytes
 * Return: 0 on success, negative error code on failure
 */
static int decrypt_km_cbc(u8 *derived_key, uint key_size, const char *encrypt,
			  u8 *km, u8 *split_key, int size, int km_blocks,
			  int blksz)
{
	u8 expkey[AES256_EXPAND_KEY_LENGTH];

	aes_expand_key(derived_key, key_size * 8, expkey);

	/* Check if ESSIV mode is used */
	if (strstr(encrypt, "essiv")) {
		essiv_decrypt(derived_key, key_size, expkey, km, split_key,
			      km_blocks, blksz);
	} else {
		/* Plain CBC with zero IV */
		u8 iv[AES_BLOCK_LENGTH];

		memset(iv, '\0', sizeof(iv));
		aes_cbc_decrypt_blocks(key_size * 8, expkey, iv, km, split_key,
				       size / AES_BLOCK_LENGTH);
	}

	return 0;
}

/* LUKS2-specific: Unlock using PBKDF2 keyslot */
/**
 * try_keyslot_pbkdf2() - Try to decrypt a LUKS2 keyslot using PBKDF2
 *
 * Attempts to decrypt a LUKS2 keyslot using the PBKDF2 key derivation function.
 * This involves deriving a key from the passphrase, reading the encrypted key
 * material from disk, decrypting it (using either XTS or CBC mode), and
 * recovering the candidate key through anti-forensic splitting.
 *
 * @blk: Block device containing the LUKS2 volume
 * @pinfo: Partition information for the LUKS2 volume
 * @ks: Keyslot information including KDF parameters and encryption area
 * @pass: User passphrase to try
 * @md_type: mbedtls message digest type for PBKDF2
 * @cand_key: Output buffer for the recovered candidate key
 * Return: 0 on success, negative error code on failure
 */
static int try_keyslot_pbkdf2(struct udevice *blk, struct disk_partition *pinfo,
			      const struct luks2_keyslot *ks, const char *pass,
			      mbedtls_md_type_t md_type, u8 *cand_key)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	int ret, km_blocks, size;
	u8 derived_key[128];
	u8 *split_key, *km;

	log_debug("LUKS2: trying keyslot with %u iters\n", ks->kdf.iters);

	/* Derive key from passphrase */
	ret = mbedtls_pkcs5_pbkdf2_hmac_ext(md_type, (const u8 *)pass,
					    strlen(pass), ks->kdf.salt,
					    ks->kdf.salt_len, ks->kdf.iters,
					    ks->area.key_size, derived_key);
	if (ret)
		return -EPROTO;

	size = ks->key_size * ks->af.stripes;
	km_blocks = (size + desc->blksz - 1) / desc->blksz;

	/* Allocate buffers */
	split_key = malloc(size);
	km = malloc_cache_aligned(km_blocks * desc->blksz);
	if (!split_key || !km) {
		ret = -ENOMEM;
		goto out;
	}

	/* Read encrypted key material */
	ret = blk_read(blk, pinfo->start + (ks->area.offset / desc->blksz),
		       km_blocks, km);
	if (ret != km_blocks) {
		ret = -EIO;
		goto out;
	}

	/* Decrypt key material */
	if (strstr(ks->area.encryption, "xts"))
		ret = decrypt_km_xts(derived_key, ks->area.key_size, km,
				     split_key, size);
	else
		ret = decrypt_km_cbc(derived_key, ks->area.key_size,
				     ks->area.encryption, km, split_key, size,
				     km_blocks, desc->blksz);

	if (ret)
		goto out;

	/* AF-merge to recover candidate key */
	ret = af_merge(split_key, cand_key, ks->key_size, ks->af.stripes,
		       ks->af.hash);

out:
	if (split_key) {
		memset(split_key, '\0', size);
		free(split_key);
	}
	if (km) {
		memset(km, '\0', km_blocks * desc->blksz);
		free(km);
	}
	memset(derived_key, '\0', sizeof(derived_key));

	return ret;
}

/* Unlock using Argon2 keyslot */
static int try_keyslot_argon2(struct udevice *blk, struct disk_partition *pinfo,
			      const struct luks2_keyslot *ks, const char *pass,
			      u8 *cand_key)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	int ret, km_blocks, size;
	u8 derived_key[128];
	u8 *split_key, *km;

	log_debug("LUKS2: trying keyslot with Argon2id (t=%u, m=%u, p=%u)\n",
		  ks->kdf.time, ks->kdf.memory, ks->kdf.cpus);

	/* Derive key from passphrase using Argon2id */
	log_debug("LUKS2 Argon2: passphrase='%s', t=%u, m=%u, p=%u, saltlen=%d, keylen=%u\n",
		  pass, ks->kdf.time, ks->kdf.memory, ks->kdf.cpus,
		  ks->kdf.salt_len, ks->area.key_size);
	ret = argon2id_hash_raw(ks->kdf.time, ks->kdf.memory, ks->kdf.cpus,
				pass, strlen(pass), ks->kdf.salt,
				ks->kdf.salt_len, derived_key,
				ks->area.key_size);
	if (ret) {
		log_err("Argon2id failed: %s\n", argon2_error_message(ret));
		return -EPROTO;
	}
	log_debug("LUKS2 Argon2: key derivation succeeded\n");

	size = ks->key_size * ks->af.stripes;
	km_blocks = (size + desc->blksz - 1) / desc->blksz;

	/* Allocate buffers */
	split_key = malloc(size);
	km = malloc_cache_aligned(km_blocks * desc->blksz);
	if (!split_key || !km) {
		ret = -ENOMEM;
		goto out;
	}

	/* Read encrypted key material */
	ret = blk_read(blk, pinfo->start + (ks->area.offset / desc->blksz),
		       km_blocks, km);
	if (ret != km_blocks) {
		ret = -EIO;
		goto out;
	}

	log_debug("LUKS2 Argon2: read %d blocks from offset %llu, encryption=%s\n",
		  km_blocks, ks->area.offset, ks->area.encryption);

	/* Decrypt key material */
	if (strstr(ks->area.encryption, "xts"))
		ret = decrypt_km_xts(derived_key, ks->area.key_size,
				     km, split_key, size);
	else
		ret = decrypt_km_cbc(derived_key, ks->area.key_size,
				     ks->area.encryption, km, split_key,
				     size, km_blocks, desc->blksz);

	if (ret)
		goto out;
	log_debug("LUKS2 Argon2: decryption completed successfully\n");

	/* AF-merge to recover candidate key */
	log_debug("LUKS2 Argon2: calling AF-merge with key_size=%u, stripes=%u, hash=%s\n",
		  ks->key_size, ks->af.stripes, ks->af.hash);
	ret = af_merge(split_key, cand_key, ks->key_size, ks->af.stripes,
		       ks->af.hash);
	log_debug("LUKS2 Argon2: AF-merge returned %d\n", ret);

out:
	if (split_key) {
		memset(split_key, '\0', size);
		free(split_key);
	}
	if (km) {
		memset(km, '\0', km_blocks * desc->blksz);
		free(km);
	}
	memset(derived_key, '\0', sizeof(derived_key));

	return ret;
}

/**
 * verify_master_key() - Verify a candidate master key against the digest
 *
 * This function takes a candidate master key (successfully derived from a
 * keyslot) and verifies it matches the stored digest using the appropriate KDF.
 *
 * @digest: Digest information (KDF type, parameters, expected digest value)
 * @md_type: mbedtls message digest type (for PBKDF2)
 * @cand_key: The candidate master key to verify
 * @key_size: Size of the candidate key
 * @master_key: Output buffer for verified master key
 * @key_sizep: Output pointer for key size
 * Return: 0 if verified and copied to master_key, -EACCES if mismatch, -ve on
 * error
 */
static int verify_master_key(const struct luks2_digest *digest,
			     mbedtls_md_type_t md_type,
			     const u8 *cand_key, uint key_size, u8 *master_key,
			     uint *key_sizep)
{
	u8 calculated_digest[128];
	int ret;

	log_debug("LUKS2: keyslot unlock succeeded, verifying digest (type=%d)\n",
		  digest->type);

	/* Verify against digest using the appropriate KDF */
	if (digest->type == LUKS2_KDF_PBKDF2) {
		/* PBKDF2 digest verification */
		log_debug("LUKS2: verifying with PBKDF2 (iters=%u, saltlen=%d, digestlen=%d)\n",
			  digest->iters, digest->salt_len, digest->digest_len);
		ret = mbedtls_pkcs5_pbkdf2_hmac_ext(md_type, cand_key,
						    key_size, digest->salt,
						    digest->salt_len,
						    digest->iters,
						    digest->digest_len,
						    calculated_digest);
		if (ret) {
			log_debug("PBKDF2 digest hash failed: %d\n", ret);
			return -EACCES;
		}
	} else {
		/* Argon2 digest verification */
		log_debug("LUKS2: verifying with Argon2 (t=%u, m=%u, p=%u)\n",
			  digest->time, digest->memory, digest->cpus);
		ret = argon2id_hash_raw(digest->time, digest->memory,
					digest->cpus, cand_key, key_size,
					digest->salt, digest->salt_len,
					calculated_digest, digest->digest_len);
		if (ret) {
			log_debug("Argon2 digest hash failed: %s\n",
				  argon2_error_message(ret));
			return -EACCES;
		}
	}

	log_debug("LUKS2: digest calculated, comparing...\n");
	if (memcmp(calculated_digest, digest->digest, digest->digest_len)) {
		log_debug("LUKS2: digest mismatch!\n");
		return -EACCES;
	}

	log_debug("LUKS2: digest match, unlock successful\n");
	memcpy(master_key, cand_key, key_size);
	*key_sizep = key_size;

	return 0; /* Success! */
}

/**
 * try_unlock_keyslot() - Try to unlock a single keyslot and verify master key
 *
 * This function attempts to unlock one keyslot by:
 * 1. Reading keyslot metadata from ofnode
 * 2. Deriving the candidate master key using the appropriate KDF
 * 3. Verifying the candidate key against the stored digest
 *
 * @blk: Block device containing the LUKS partition
 * @pinfo: Partition information
 * @keyslot_node: ofnode for this specific keyslot
 * @digest: Digest information for verification
 * @md_type: mbedtls message digest type (for PBKDF2)
 * @pass: User-provided passphrase
 * @master_key: Output buffer for verified master key
 * @key_sizep: Returns the key size
 * Return: 0 if unlocked successfully, -EAGAIN to continue trying, -ve on error
 */
static int try_unlock_keyslot(struct udevice *blk, struct disk_partition *pinfo,
			      ofnode keyslot_node,
			      const struct luks2_digest *digest,
			      mbedtls_md_type_t md_type, const char *pass,
			      u8 *master_key, uint *key_sizep)
{
	struct luks2_keyslot keyslot;
	u8 cand_key[128];
	uint key_size;
	int ret;

	/* Read keyslot information */
	ret = read_keyslot_info(keyslot_node, &keyslot, digest->hash);
	if (ret) {
		/* Skip unsupported or invalid keyslots */
		return -EAGAIN;
	}

	log_debug("LUKS2: trying keyslot (type=%d)\n", keyslot.kdf.type);

	/* Try the keyslot using the appropriate KDF */
	if (keyslot.kdf.type == LUKS2_KDF_PBKDF2) {
		log_debug("LUKS2: calling try_keyslot_pbkdf2\n");
		ret = try_keyslot_pbkdf2(blk, pinfo, &keyslot, pass, md_type,
					 cand_key);
	} else {
		/* Argon2 (already checked for CONFIG_ARGON2 support) */
		log_debug("LUKS2: calling try_keyslot_argon2\n");
		ret = try_keyslot_argon2(blk, pinfo, &keyslot, pass, cand_key);
	}
	log_debug("LUKS2: keyslot try returned %d\n", ret);

	if (!ret) {
		/* Verify the candidate key against the digest */
		ret = verify_master_key(digest, md_type, cand_key,
					keyslot.key_size, master_key,
					&key_size);
		memset(cand_key, '\0', sizeof(cand_key));
		if (!ret) {
			*key_sizep = key_size;
			return 0; /* Success! */
		}
		/* Verification failed, continue trying */
	}

	memset(cand_key, '\0', sizeof(cand_key));

	return -EAGAIN; /* Continue trying other keyslots */
}

int unlock_luks2(struct udevice *blk, struct disk_partition *pinfo,
		 const char *pass, u8 *master_key, uint *key_sizep)
{
	ofnode keyslots_node, keyslot_node;
	struct luks2_digest digest;
	mbedtls_md_type_t md_type;
	struct abuf fdt_buf;
	int ret;

	/* Read and parse LUKS2 header and metadata */
	ret = read_luks2_info(blk, pinfo, &fdt_buf, &digest, &md_type,
			      &keyslots_node);
	if (ret)
		return ret;

	/* Try each keyslot until one succeeds */
	ret = -EACCES;
	ofnode_for_each_subnode(keyslot_node, keyslots_node) {
		ret = try_unlock_keyslot(blk, pinfo, keyslot_node, &digest,
					 md_type, pass, master_key, key_sizep);
		if (!ret)  /* Successfully unlocked! */
			break;

		/* -EAGAIN means skip, other errors also continue trying */
	}
	abuf_uninit(&fdt_buf);
	if (ret) {
		if (ret == -EAGAIN) /* no usable slots */
			log_debug("LUKS2: no supported keyslots found\n");
		else /* no slots worked */
			log_debug("LUKS2: wrong passphrase\n");
		ret = -EACCES;
	}

	return ret;
}
