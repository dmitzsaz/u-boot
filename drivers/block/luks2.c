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
#include <hash.h>
#include <json.h>
#include <log.h>
#include <luks.h>
#include <memalign.h>
#include <part.h>
#include <uboot_aes.h>
#include <asm/unaligned.h>
#include <linux/libfdt.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/constant_time.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <u-boot/sha256.h>
#include <argon2.h>
#include "luks_internal.h"

/* The binary header ahead of the JSON area */
#define LUKS2_HDR_BIN_SIZE	4096

/* Sizes cryptsetup uses for binary header plus JSON area: 16 KiB to 4 MiB */
#define LUKS2_HDR_MIN_SIZE	SZ_16K
#define LUKS2_HDR_MAX_SIZE	SZ_4M

/* Upper bound of a keyslot's key material (key size times AF stripes) */
#define LUKS2_KM_MAX_SIZE	SZ_16M

/* Argon2 memory cost limit of cryptsetup, in KiB */
#define LUKS2_ARGON2_MAX_MEMORY	(4U * 1024 * 1024)

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
 * @hash: PBKDF2 hash algorithm name (valid if type == LUKS2_KDF_PBKDF2)
 */
struct luks2_kdf {
	enum luks2_kdf_type type;
	const char *hash;
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

/*
 * The LUKS2 JSON metadata is converted into a flattened device tree owned by
 * this driver and read with libfdt. The ofnode API reaches a tree other than
 * the control FDT only with OFNODE_MULTI_TREE, which also registers it for
 * the rest of U-Boot's run.
 */

/**
 * luks2_read_string() - Read a string property of a metadata node
 *
 * @fdt: Metadata tree
 * @node: Node offset
 * @name: Property name
 * Return: nul-terminated string, or NULL if absent or not a string
 */
static const char *luks2_read_string(const void *fdt, int node,
				     const char *name)
{
	const char *str;
	int len;

	str = fdt_getprop(fdt, node, name, &len);
	if (!str || len < 1 || str[len - 1])
		return NULL;

	return str;
}

/**
 * luks2_read_u32() - Read a 32-bit number property of a metadata node
 *
 * @fdt: Metadata tree
 * @node: Node offset
 * @name: Property name
 * @valp: Returns the value
 * Return: 0 if OK, -EINVAL if absent or not a 32-bit number
 */
static int luks2_read_u32(const void *fdt, int node, const char *name,
			  u32 *valp)
{
	const fdt32_t *val;
	int len;

	val = fdt_getprop(fdt, node, name, &len);
	if (!val || len != sizeof(*val))
		return -EINVAL;
	*valp = fdt32_to_cpu(*val);

	return 0;
}

/**
 * luks2_read_u64() - Read a number property that LUKS2 stores as a string
 *
 * Offsets and sizes are decimal strings in the LUKS2 JSON.
 *
 * @fdt: Metadata tree
 * @node: Node offset
 * @name: Property name
 * @valp: Returns the value
 * Return: 0 if OK, -EINVAL if absent or not a plain decimal number
 */
static int luks2_read_u64(const void *fdt, int node, const char *name,
			  u64 *valp)
{
	const char *str = luks2_read_string(fdt, node, name);
	char *end;

	/* At most 19 digits, so that the value cannot overflow */
	if (!str || *str < '0' || *str > '9' || strlen(str) > 19)
		return -EINVAL;
	*valp = simple_strtoull(str, &end, 10);
	if (*end)
		return -EINVAL;

	return 0;
}

/**
 * luks2_read_metadata() - Read and check the primary LUKS2 header
 *
 * The checksum covers the binary header, with its checksum field zeroed,
 * and the JSON area. The header is only read: a damaged primary header is
 * refused, never repaired from the secondary copy.
 *
 * @blk: Block device
 * @pinfo: Partition information
 * @fdt_buf: Returns the JSON metadata converted to a flat tree; the caller
 *	must uninit it
 * Return: 0 on success, -EBADMSG on a checksum mismatch, other -ve on error
 */
static int luks2_read_metadata(struct udevice *blk, struct disk_partition *pinfo,
			       struct abuf *fdt_buf)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	u8 expected[FIELD_SIZEOF(struct luks2_hdr, csum)];
	u8 actual[sizeof(expected)];
	struct luks2_hdr *hdr;
	int csum_len, ret;
	lbaint_t blocks;
	u64 hdr_size;
	char *data;

	abuf_init(fdt_buf);
	if (!desc->blksz || desc->blksz > LUKS2_HDR_BIN_SIZE ||
	    LUKS2_HDR_BIN_SIZE % desc->blksz)
		return -EINVAL;
	blocks = LUKS2_HDR_BIN_SIZE / desc->blksz;
	if (pinfo->size < blocks)
		return -EINVAL;

	hdr = malloc_cache_aligned(LUKS2_HDR_BIN_SIZE);
	if (!hdr)
		return -ENOMEM;
	if (blk_read(blk, pinfo->start, blocks, hdr) != blocks) {
		free(hdr);
		return -EIO;
	}
	hdr_size = be64_to_cpu(hdr->hdr_size);
	if (memcmp(hdr->magic, LUKS_MAGIC, LUKS_MAGIC_LEN) ||
	    be16_to_cpu(hdr->version) != LUKS_VERSION_2 ||
	    hdr->hdr_offset || hdr_size < LUKS2_HDR_MIN_SIZE ||
	    hdr_size > LUKS2_HDR_MAX_SIZE || !is_power_of_2(hdr_size) ||
	    hdr_size / desc->blksz > pinfo->size ||
	    !memchr(hdr->csum_alg, '\0', sizeof(hdr->csum_alg))) {
		log_debug("LUKS2: invalid binary header\n");
		free(hdr);
		return -EINVAL;
	}
	free(hdr);

	blocks = hdr_size / desc->blksz;
	data = malloc_cache_aligned(hdr_size);
	if (!data)
		return -ENOMEM;
	if (blk_read(blk, pinfo->start, blocks, data) != blocks) {
		ret = -EIO;
		goto out;
	}

	/* Check the header that was just read, not the first copy */
	hdr = (struct luks2_hdr *)data;
	if (be64_to_cpu(hdr->hdr_size) != hdr_size ||
	    !memchr(hdr->csum_alg, '\0', sizeof(hdr->csum_alg))) {
		ret = -EINVAL;
		goto out;
	}
	memcpy(expected, hdr->csum, sizeof(expected));
	memset(hdr->csum, '\0', sizeof(hdr->csum));
	csum_len = sizeof(actual);
	ret = hash_block(hdr->csum_alg, data, hdr_size, actual, &csum_len);
	if (ret) {
		log_debug("LUKS2: checksum %s not available\n", hdr->csum_alg);
		ret = -ENOTSUPP;
		goto out;
	}
	if (mbedtls_ct_memcmp(expected, actual, csum_len)) {
		log_debug("LUKS2: header checksum mismatch\n");
		ret = -EBADMSG;
		goto out;
	}

	/* The JSON text must end inside its area */
	if (!memchr(data + LUKS2_HDR_BIN_SIZE, '\0',
		    hdr_size - LUKS2_HDR_BIN_SIZE)) {
		ret = -EINVAL;
		goto out;
	}
	if (json_to_fdt(data + LUKS2_HDR_BIN_SIZE, fdt_buf)) {
		log_err("Failed to convert JSON to FDT\n");
		ret = -EINVAL;
	}

out:
	memset(data, '\0', hdr_size);
	free(data);

	return ret;
}

/**
 * luks2_find_digest() - Find the digest that verifies the data segment's key
 *
 * @fdt: Metadata tree
 * Return: node offset of the digest bound to segment "0", -ve if none
 */
static int luks2_find_digest(const void *fdt)
{
	int digests, node;

	digests = fdt_subnode_offset(fdt, 0, "digests");
	if (digests < 0)
		return digests;
	fdt_for_each_subnode(node, fdt, digests) {
		if (fdt_stringlist_search(fdt, node, "segments", "0") >= 0)
			return node;
	}

	return -ENOENT;
}

/**
 * read_digest_info() - Read LUKS2 digest information from the metadata
 *
 * @fdt: Metadata tree
 * @digest_node: Node of the digest (e.g., digest "0")
 * @digest: Pointer to digest structure to fill
 * Return: 0 on success, -ve on error
 */
static int read_digest_info(const void *fdt, int digest_node,
			    struct luks2_digest *digest)
{
	const char *salt_b64, *digest_b64;

	const char *type_str;
	int ret;

	memset(digest, '\0', sizeof(*digest));

	/* Read and convert digest type */
	type_str = luks2_read_string(fdt, digest_node, "type");
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
	digest->hash = luks2_read_string(fdt, digest_node, "hash");
	if (!digest->hash)
		return -EINVAL;

	/* Read KDF-specific parameters */
	if (digest->type == LUKS2_KDF_PBKDF2) {
		/* PBKDF2 */
		if (luks2_read_u32(fdt, digest_node, "iterations",
				   &digest->iters))
			return -EINVAL;
	} else {
		/* Argon2 */
		if (luks2_read_u32(fdt, digest_node, "time", &digest->time) ||
		    luks2_read_u32(fdt, digest_node, "memory",
				   &digest->memory) ||
		    luks2_read_u32(fdt, digest_node, "cpus", &digest->cpus))
			return -EINVAL;
	}

	/* Read and decode salt */
	salt_b64 = luks2_read_string(fdt, digest_node, "salt");
	if (!salt_b64)
		return -EINVAL;
	digest->salt_len = base64_decode(salt_b64, digest->salt,
					 sizeof(digest->salt));
	if (digest->salt_len <= 0)
		return -EINVAL;

	/* Read and decode digest */
	digest_b64 = luks2_read_string(fdt, digest_node, "digest");
	if (!digest_b64)
		return -EINVAL;
	digest->digest_len = base64_decode(digest_b64, digest->digest,
					   sizeof(digest->digest));
	if (digest->digest_len <= 0)
		return -EINVAL;

	if (digest->type == LUKS2_KDF_PBKDF2 ? !digest->iters :
	    !digest->time || !digest->cpus ||
	    digest->memory > LUKS2_ARGON2_MAX_MEMORY)
		return -EINVAL;

	return 0;
}

/**
 * read_keyslot_info() - Read LUKS2 keyslot information from the metadata
 *
 * @fdt: Metadata tree
 * @keyslot_node: Node of the keyslot (e.g., keyslot "0")
 * @keyslot: Pointer to keyslot structure to fill
 * Return: 0 on success, -ve on error
 */
static int read_keyslot_info(const void *fdt, int keyslot_node,
			     struct luks2_keyslot *keyslot)
{
	const char *salt_b64, *offset_str;
	int kdf_node, af_node, area_node;
	int ret;

	memset(keyslot, '\0', sizeof(*keyslot));

	/* Read keyslot type */
	keyslot->type = luks2_read_string(fdt, keyslot_node, "type");
	if (!keyslot->type || strcmp(keyslot->type, "luks2"))
		return -EINVAL;

	/* Read key size */
	if (luks2_read_u32(fdt, keyslot_node, "key_size", &keyslot->key_size))
		return -EINVAL;

	/* Navigate to and read KDF node */
	kdf_node = fdt_subnode_offset(fdt, keyslot_node, "kdf");
	if (kdf_node < 0)
		return -EINVAL;

	offset_str = luks2_read_string(fdt, kdf_node, "type");
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
	salt_b64 = luks2_read_string(fdt, kdf_node, "salt");
	if (!salt_b64)
		return -EINVAL;
	keyslot->kdf.salt_len = base64_decode(salt_b64, keyslot->kdf.salt,
					      sizeof(keyslot->kdf.salt));
	if (keyslot->kdf.salt_len <= 0)
		return -EINVAL;

	/* Read KDF-specific parameters */
	if (keyslot->kdf.type == LUKS2_KDF_PBKDF2) {
		keyslot->kdf.hash = luks2_read_string(fdt, kdf_node, "hash");
		if (!keyslot->kdf.hash ||
		    luks2_read_u32(fdt, kdf_node, "iterations",
				   &keyslot->kdf.iters) || !keyslot->kdf.iters)
			return -EINVAL;
	} else {
		/* Argon2 */
		if (luks2_read_u32(fdt, kdf_node, "time", &keyslot->kdf.time) ||
		    luks2_read_u32(fdt, kdf_node, "memory",
				   &keyslot->kdf.memory) ||
		    luks2_read_u32(fdt, kdf_node, "cpus", &keyslot->kdf.cpus) ||
		    !keyslot->kdf.time || !keyslot->kdf.cpus ||
		    keyslot->kdf.memory > LUKS2_ARGON2_MAX_MEMORY)
			return -EINVAL;
	}

	/* Navigate to and read AF node */
	af_node = fdt_subnode_offset(fdt, keyslot_node, "af");
	if (af_node < 0)
		return -EINVAL;

	if (strcmp(luks2_read_string(fdt, af_node, "type") ?: "", "luks1"))
		return -ENOTSUPP;
	if (luks2_read_u32(fdt, af_node, "stripes", &keyslot->af.stripes) ||
	    !keyslot->af.stripes)
		return -EINVAL;
	keyslot->af.hash = luks2_read_string(fdt, af_node, "hash");
	if (!keyslot->af.hash)
		return -EINVAL;

	/* Navigate to and read area node */
	area_node = fdt_subnode_offset(fdt, keyslot_node, "area");
	if (area_node < 0)
		return -EINVAL;

	if (strcmp(luks2_read_string(fdt, area_node, "type") ?: "", "raw"))
		return -ENOTSUPP;

	/* Read offset and size (strings in LUKS2 JSON) */
	if (luks2_read_u64(fdt, area_node, "offset", &keyslot->area.offset) ||
	    luks2_read_u64(fdt, area_node, "size", &keyslot->area.size))
		return -EINVAL;

	/* Read encryption mode */
	keyslot->area.encryption = luks2_read_string(fdt, area_node,
						     "encryption");
	if (!keyslot->area.encryption)
		return -EINVAL;

	/* Read area key size */
	if (luks2_read_u32(fdt, area_node, "key_size",
			   &keyslot->area.key_size))
		return -EINVAL;

	/* AES-256 or AES-XTS with two 128/256-bit keys */
	if ((keyslot->key_size != 32 && keyslot->key_size != 64) ||
	    (keyslot->area.key_size != 32 && keyslot->area.key_size != 64) ||
	    strncmp(keyslot->area.encryption, "aes-", 4))
		return -ENOTSUPP;

	return 0;
}

/**
 * check_keyslot_area() - Check that a keyslot's key material is on the disk
 *
 * @desc: Block device descriptor
 * @pinfo: Partition information
 * @ks: Keyslot
 * Return: 0 if the key material lies within the partition, -EINVAL if not
 */
static int check_keyslot_area(struct blk_desc *desc,
			      struct disk_partition *pinfo,
			      const struct luks2_keyslot *ks)
{
	u64 size = (u64)ks->key_size * ks->af.stripes;
	u64 part_size = (u64)pinfo->size * desc->blksz;

	if (size > LUKS2_KM_MAX_SIZE || size > ks->area.size ||
	    size % AES_BLOCK_LENGTH || ks->area.offset % desc->blksz ||
	    ks->area.offset > part_size ||
	    ks->area.size > part_size - ks->area.offset)
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
 * @md_type: Pointer to receive mbedtls MD type of the digest
 * @digest_nodep: Pointer to receive the offset of the digest node
 * @keyslots_nodep: Pointer to receive the offset of the keyslots node
 * Return: 0 on success, -ve on error
 */
static int read_luks2_info(struct udevice *blk, struct disk_partition *pinfo,
			   struct abuf *fdt_buf, struct luks2_digest *digest,
			   mbedtls_md_type_t *md_typep, int *digest_nodep,
			   int *keyslots_nodep)
{
	struct hash_algo *hash_algo;
	int digest_node, keyslots_node;
	const void *fdt;
	int ret;

	ret = luks2_read_metadata(blk, pinfo, fdt_buf);
	if (ret)
		return ret;
	fdt = abuf_data(fdt_buf);

	ret = -EINVAL;
	digest_node = luks2_find_digest(fdt);
	if (digest_node < 0)
		goto out;

	/* Read digest information */
	ret = read_digest_info(fdt, digest_node, digest);
	if (ret)
		goto out;

	/* Get hash algorithm */
	ret = hash_lookup_algo(digest->hash, &hash_algo);
	if (ret) {
		log_debug("Unsupported hash: %s\n", digest->hash);
		ret = -ENOTSUPP;
		goto out;
	}

	/* Navigate to keyslots node */
	keyslots_node = fdt_subnode_offset(fdt, 0, "keyslots");
	if (keyslots_node < 0) {
		ret = -EINVAL;
		goto out;
	}

	*md_typep = hash_mbedtls_type(hash_algo);
	*digest_nodep = digest_node;
	*keyslots_nodep = keyslots_node;

out:
	if (ret)
		abuf_uninit(fdt_buf);

	return ret;
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
static int decrypt_km_cbc(const u8 *derived_key, uint key_size,
			  const char *encrypt, u8 *km, u8 *split_key,
			  int size, int km_blocks, int blksz)
{
	u8 expkey[AES256_EXPAND_KEY_LENGTH];

	aes_expand_key(derived_key, key_size, expkey);

	/* Check if ESSIV mode is used */
	if (strstr(encrypt, "essiv")) {
		essiv_decrypt(derived_key, key_size, expkey, km, split_key,
			      km_blocks, blksz);
	} else {
		/* Plain CBC with zero IV */
		u8 iv[AES_BLOCK_LENGTH];

		memset(iv, '\0', sizeof(iv));
		aes_cbc_decrypt_blocks(key_size, expkey, iv, km, split_key,
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
 * @md_type: mbedtls message digest type of the keyslot's PBKDF2 hash
 * @cand_key: Output buffer for the recovered candidate key
 * Return: 0 on success, negative error code on failure
 */
static int try_keyslot_pbkdf2(struct udevice *blk, struct disk_partition *pinfo,
			      const struct luks2_keyslot *ks, const u8 *pass,
			      size_t pass_len, mbedtls_md_type_t md_type,
			      u8 *cand_key)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	int ret, km_blocks, size;
	u8 derived_key[128];
	u8 *split_key = NULL, *km = NULL;

	log_debug("LUKS2: trying keyslot with %u iters\n", ks->kdf.iters);

	/* Derive key from passphrase */
	ret = mbedtls_pkcs5_pbkdf2_hmac_ext(md_type, pass, pass_len,
					    ks->kdf.salt, ks->kdf.salt_len,
					    ks->kdf.iters, ks->area.key_size,
					    derived_key);
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
			      const struct luks2_keyslot *ks, const u8 *pass,
			      size_t pass_len, u8 *cand_key)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	int ret, km_blocks, size;
	u8 derived_key[128];
	u8 *split_key = NULL, *km = NULL;

	log_debug("LUKS2: trying keyslot with Argon2id (t=%u, m=%u, p=%u)\n",
		  ks->kdf.time, ks->kdf.memory, ks->kdf.cpus);

	/* Derive key from passphrase using Argon2id */
	log_debug("LUKS2 Argon2: pass_len=%zu, t=%u, m=%u, p=%u, saltlen=%d, keylen=%u\n",
		  pass_len, ks->kdf.time, ks->kdf.memory, ks->kdf.cpus,
		  ks->kdf.salt_len, ks->area.key_size);
	ret = argon2id_hash_raw(ks->kdf.time, ks->kdf.memory, ks->kdf.cpus,
				pass, pass_len, ks->kdf.salt,
				ks->kdf.salt_len, derived_key,
				ks->area.key_size);
	if (ret) {
		log_err("Argon2id failed: %s (code=%d)\n",
			argon2_error_message(ret), ret);
		if (ret == ARGON2_MEMORY_ALLOCATION_ERROR)
			return -ENOMEM;
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
	ret = mbedtls_ct_memcmp(calculated_digest, digest->digest,
				digest->digest_len);
	memset(calculated_digest, '\0', sizeof(calculated_digest));
	if (ret) {
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
 * 1. Reading keyslot metadata from the metadata tree
 * 2. Deriving the candidate master key using the appropriate KDF (or using
 *    pre-derived key directly)
 * 3. Verifying the candidate key against the stored digest
 *
 * @blk: Block device containing the LUKS partition
 * @pinfo: Partition information
 * @fdt: Metadata tree
 * @keyslot_node: Node of this specific keyslot
 * @digest: Digest information for verification
 * @md_type: mbedtls message digest type (for PBKDF2)
 * @pass: User-provided passphrase or pre-derived key
 * @pass_len: Length of passphrase
 * @pre_derived: True if pass is a pre-derived key, false for passphrase
 * @master_key: Output buffer for verified master key
 * @key_sizep: Returns the key size
 * Return: 0 if unlocked successfully, -EAGAIN to continue trying, -ve on error
 */
static int try_unlock_keyslot(struct udevice *blk, struct disk_partition *pinfo,
			      const void *fdt, int keyslot_node,
			      const struct luks2_digest *digest,
			      mbedtls_md_type_t md_type, const u8 *pass,
			      size_t pass_len, bool pre_derived,
			      u8 *master_key, uint *key_sizep)
{
	struct luks2_keyslot keyslot;
	u8 cand_key[128];
	uint key_size;
	int ret;

	/* Read keyslot information */
	ret = read_keyslot_info(fdt, keyslot_node, &keyslot);
	if (!ret)
		ret = check_keyslot_area(dev_get_uclass_plat(blk), pinfo,
					 &keyslot);
	if (ret) {
		/* Skip unsupported or invalid keyslots */
		return -EAGAIN;
	}

	log_debug("LUKS2: trying keyslot (type=%d)\n", keyslot.kdf.type);

	/* If using pre-derived key, use it directly */
	if (pre_derived) {
		if (pass_len != keyslot.key_size) {
			log_debug("Pre-derived key size mismatch: got %zu, need %u\n",
				  pass_len, keyslot.key_size);
			return -EAGAIN;
		}
		memcpy(cand_key, pass, pass_len);
		ret = 0;
	} else {
		/* Try the keyslot using the appropriate KDF */
		if (keyslot.kdf.type == LUKS2_KDF_PBKDF2) {
			struct hash_algo *algo;

			/* The keyslot's PBKDF2 hash need not be the digest's */
			if (hash_lookup_algo(keyslot.kdf.hash, &algo))
				return -EAGAIN;
			log_debug("LUKS2: calling try_keyslot_pbkdf2\n");
			ret = try_keyslot_pbkdf2(blk, pinfo, &keyslot, pass, pass_len,
						 hash_mbedtls_type(algo),
						 cand_key);
		} else {
			/* Argon2 (already checked for CONFIG_ARGON2 support) */
			log_debug("LUKS2: calling try_keyslot_argon2\n");
			ret = try_keyslot_argon2(blk, pinfo, &keyslot, pass, pass_len,
						 cand_key);
		}
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
		 const u8 *pass, size_t pass_len, bool pre_derived,
		 u8 *master_key, uint *key_sizep)
{
	int digest_node, keyslots_node, keyslot_node;
	struct luks2_digest digest;
	mbedtls_md_type_t md_type;
	struct abuf fdt_buf;
	int ret;

	/* Read and parse LUKS2 header and metadata */
	ret = read_luks2_info(blk, pinfo, &fdt_buf, &digest, &md_type,
			      &digest_node, &keyslots_node);
	if (ret)
		return ret;

	/* Try each keyslot until one succeeds */
	ret = -EACCES;
	fdt_for_each_subnode(keyslot_node, abuf_data(&fdt_buf),
			     keyslots_node) {
		const void *fdt = abuf_data(&fdt_buf);
		const char *id = fdt_get_name(fdt, keyslot_node, NULL);
		u32 priority;

		/* Only keyslots that open the data segment's key are tried */
		if (!id ||
		    fdt_stringlist_search(fdt, digest_node, "keyslots", id) < 0)
			continue;
		/* Priority 0 ("ignore") keyslots are only used on request */
		if (!luks2_read_u32(fdt, keyslot_node, "priority", &priority) &&
		    !priority)
			continue;
		ret = try_unlock_keyslot(blk, pinfo, abuf_data(&fdt_buf),
					 keyslot_node, &digest, md_type, pass,
					 pass_len, pre_derived, master_key,
					 key_sizep);
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

int luks2_get_segment(struct udevice *blk, struct disk_partition *pinfo,
		      struct luks2_segment *seg)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	int segments, segment0, ret;
	const char *encryption;
	struct abuf fdt_buf;
	const void *fdt;
	u64 part_size;

	ret = luks2_read_metadata(blk, pinfo, &fdt_buf);
	if (ret)
		return ret;
	fdt = abuf_data(&fdt_buf);

	ret = -EINVAL;
	segments = fdt_subnode_offset(fdt, 0, "segments");
	if (segments < 0)
		goto out;
	segment0 = fdt_subnode_offset(fdt, segments, "0");
	if (segment0 < 0)
		goto out;

	/*
	 * A second segment means a reencryption is in progress; an integrity
	 * section or flags need handling that is not implemented here.
	 */
	ret = -ENOTSUPP;
	if (fdt_first_subnode(fdt, segments) != segment0 ||
	    fdt_next_subnode(fdt, segment0) >= 0 ||
	    strcmp(luks2_read_string(fdt, segment0, "type") ?: "", "crypt") ||
	    strcmp(luks2_read_string(fdt, segment0, "iv_tweak") ?: "", "0") ||
	    fdt_getprop(fdt, segment0, "flags", NULL) ||
	    fdt_subnode_offset(fdt, segment0, "integrity") >= 0)
		goto out;

	encryption = luks2_read_string(fdt, segment0, "encryption");
	if (!encryption)
		goto out;
	if (!strcmp(encryption, "aes-xts-plain64")) {
		seg->xts = true;
		seg->essiv = false;
	} else if (!strcmp(encryption, "aes-cbc-essiv:sha256")) {
		seg->xts = false;
		seg->essiv = true;
	} else {
		log_debug("LUKS2: unsupported encryption %s\n", encryption);
		goto out;
	}

	ret = -EINVAL;
	if (luks2_read_u32(fdt, segment0, "sector_size", &seg->sector_size) ||
	    seg->sector_size < 512 || seg->sector_size > 4096 ||
	    !is_power_of_2(seg->sector_size) ||
	    seg->sector_size < desc->blksz)
		goto out;

	/* Offset of the data and, unless "dynamic", its size, in bytes */
	part_size = (u64)pinfo->size * desc->blksz;
	if (luks2_read_u64(fdt, segment0, "offset", &seg->offset) ||
	    seg->offset % seg->sector_size || seg->offset >= part_size)
		goto out;
	if (!strcmp(luks2_read_string(fdt, segment0, "size") ?: "",
		    "dynamic"))
		seg->size = part_size - seg->offset;
	else if (luks2_read_u64(fdt, segment0, "size", &seg->size) ||
		 seg->size > part_size - seg->offset)
		goto out;
	seg->size = ALIGN_DOWN(seg->size, seg->sector_size);
	if (!seg->size)
		goto out;
	log_debug("LUKS2: sector_size=%u\n", seg->sector_size);
	ret = 0;

out:
	abuf_uninit(&fdt_buf);

	return ret;
}
