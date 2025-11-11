/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * LUKS (Linux Unified Key Setup) internal interfaces
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#ifndef __LUKS_INTERNAL_H__
#define __LUKS_INTERNAL_H__

#include <hash.h>

/**
 * af_merge() - Merge anti-forensic split key into original key
 *
 * This performs the LUKS AF-merge operation to recover the original key from
 * its AF-split representation. The algorithm XORs all stripes together,
 * applying diffusion between each stripe. Used by both LUKS1 and LUKS2.
 *
 * @src:	AF-split key material (key_size * stripes bytes)
 * @dst:	Output buffer for merged key (key_size bytes)
 * @key_size:	Size of the original key
 * @stripes:	Number of anti-forensic stripes
 * @hash_spec:	Hash algorithm name (e.g., "sha256")
 * Return:	0 on success, -ve on error
 */
int af_merge(const u8 *src, u8 *dst, size_t key_size, uint stripes,
	     const char *hash_spec);

/**
 * unlock_luks2() - Unlock a LUKS2 partition with a passphrase
 *
 * @blk:	Block device
 * @pinfo:	Partition information
 * @pass:	Passphrase to unlock the partition
 * @master_key:	Buffer to receive the decrypted master key
 * @key_sizep:	Returns the key size
 * Return:	0 on success, -ve on error
 */
int unlock_luks2(struct udevice *blk, struct disk_partition *pinfo,
		 const char *pass, u8 *master_key, uint *key_sizep);

#endif /* __LUKS_INTERNAL_H__ */
