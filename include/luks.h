/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * LUKS (Linux Unified Key Setup) filesystem support
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#ifndef __LUKS_H__
#define __LUKS_H__

#include <linux/types.h>

struct udevice;
struct disk_partition;

/* LUKS magic bytes: "LUKS" followed by 0xba 0xbe */
#define LUKS_MAGIC		"LUKS\xba\xbe"
#define LUKS_MAGIC_LEN		6

/* LUKS versions */
#define LUKS_VERSION_1		1
#define LUKS_VERSION_2		2

/* LUKS constants */
#define LUKS_DIGESTSIZE		20
#define LUKS_SALTSIZE		32
#define LUKS_NUMKEYS		8
#define LUKS_KEY_DISABLED	0x0000dead
#define LUKS_KEY_ENABLED	0x00ac71f3
#define LUKS_STRIPES		4000

/**
 * struct luks1_keyslot - LUKS1 key slot
 *
 * @active:		Key slot state (LUKS_KEY_ENABLED/DISABLED)
 * @iterations:		PBKDF2 iteration count
 * @salt:		Salt for PBKDF2
 * @key_material_offset: Start sector of key material
 * @stripes:		Number of anti-forensic stripes
 */
struct luks1_keyslot {
	__be32		active;
	__be32		iterations;
	char		salt[LUKS_SALTSIZE];
	__be32		key_material_offset;
	__be32		stripes;
} __packed;

/**
 * struct luks1_phdr - LUKS1 header structure
 *
 * @magic:		LUKS magic bytes
 * @version:		LUKS version
 * @cipher_name:	Cipher name
 * @cipher_mode:	Cipher mode
 * @hash_spec:		Hash specification
 * @payload_offset:	Payload offset in sectors
 * @key_bytes:		Key length in bytes
 * @mk_digest:		Master key digest
 * @mk_digest_salt:	Salt for master key digest
 * @mk_digest_iter:	Iterations for master key digest
 * @uuid:		Partition UUID
 * @key_slot:		Key slots (8 total)
 */
struct luks1_phdr {
	char		magic[LUKS_MAGIC_LEN];
	__be16		version;
	char		cipher_name[32];
	char		cipher_mode[32];
	char		hash_spec[32];
	__be32		payload_offset;
	__be32		key_bytes;
	char		mk_digest[LUKS_DIGESTSIZE];
	char		mk_digest_salt[LUKS_SALTSIZE];
	__be32		mk_digest_iter;
	char		uuid[40];
	struct luks1_keyslot key_slot[LUKS_NUMKEYS];
} __packed;

/**
 * struct luks2_hdr - LUKS2 binary header
 *
 * @magic:	LUKS magic bytes
 * @version:	LUKS version
 * @hdr_size:	Header size (includes binary header + JSON area)
 * @seqid:	Sequence ID
 * @label:	Label string
 * @csum_alg:	Checksum algorithm
 * @salt:	Salt for header checksum
 * @uuid:	Partition UUID
 * @subsystem:	Subsystem identifier
 * @hdr_offset:	Offset of this header
 * @_padding:	Reserved padding
 * @csum:	Header checksum
 * @_padding4096: Padding to 4096 bytes
 */
struct luks2_hdr {
	char		magic[LUKS_MAGIC_LEN];
	__be16		version;
	__be64		hdr_size;
	__be64		seqid;
	char		label[48];
	char		csum_alg[32];
	u8		salt[64];
	char		uuid[40];
	char		subsystem[48];
	__be64		hdr_offset;
	u8		_padding[184];
	u8		csum[64];
	u8		_padding4096[3584];
} __packed;

/**
 * luks_detect() - Detect if a partition is LUKS encrypted
 *
 * @blk:	Block device
 * @pinfo:	Partition information
 * Return:	0 if LUKS partition detected, -ve on error
 */
int luks_detect(struct udevice *blk, struct disk_partition *pinfo);

/**
 * luks_get_version() - Get LUKS version of a partition
 *
 * @blk:	Block device
 * @pinfo:	Partition information
 * Return:	LUKS version (1 or 2) if detected, -ve on error
 */
int luks_get_version(struct udevice *blk, struct disk_partition *pinfo);

/**
 * luks_show_info() - Display LUKS header information
 *
 * @blk:	Block device
 * @pinfo:	Partition information
 * Return:	0 on success, -ve on error
 */
int luks_show_info(struct udevice *blk, struct disk_partition *pinfo);

/**
 * luks_unlock() - Unlock a LUKS partition with a passphrase
 *
 * This attempts to decrypt the master key using the provided passphrase.
 * Currently only supports LUKS1 with PBKDF2 and AES-CBC.
 *
 * @blk:	Block device
 * @pinfo:	Partition information
 * @pass:	Passphrase to unlock the partition
 * @master_key:	Buffer to receive the decrypted master key
 * @key_size:	Size of the master_key buffer
 * Return:	0 on success, -ve on error
 */
int luks_unlock(struct udevice *blk, struct disk_partition *pinfo,
		const char *pass, u8 *master_key, u32 *key_size);

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
 * @blkmap_dev:	Output pointer for created blkmap device
 * Return:	0 on success, -ve on error
 */
int luks_create_blkmap(struct udevice *blk, struct disk_partition *pinfo,
		       const u8 *master_key, u32 key_size, const char *label,
		       struct udevice **blkmap_dev);

#endif /* __LUKS_H__ */
