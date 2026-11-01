/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2023 Addiva Elektronik
 * Author: Tobias Waldekranz <tobias@waldekranz.com>
 */

#ifndef _BLKMAP_H
#define _BLKMAP_H

#include <blk.h>
#include <dm/lists.h>

/**
 * enum blkmap_crypt_mode - Cipher mode for encrypted block devices
 */
enum blkmap_crypt_mode {
	BLKMAP_CRYPT_MODE_CBC = 0,	/* AES-CBC */
	BLKMAP_CRYPT_MODE_XTS = 1,	/* AES-XTS */
};

/**
 * struct blkmap - Block map
 *
 * Data associated with a blkmap.
 *
 * @label: Human readable name of this blkmap
 * @blk: Underlying block device
 * @slices: List of slices associated with this blkmap
 */
struct blkmap {
	char *label;
	struct udevice *blk;
	struct list_head slices;
};

/**
 * blkmap_map_linear() - Map region of other block device
 *
 * @dev: Blkmap to create the mapping on
 * @blknr: Start block number of the mapping
 * @blkcnt: Number of blocks to map
 * @lblk: The target block device of the mapping
 * @lblknr: The start block number of the target device
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_map_linear(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		      struct udevice *lblk, lbaint_t lblknr);

/**
 * blkmap_map_mem() - Map region of memory
 *
 * @dev: Blkmap to create the mapping on
 * @blknr: Start block number of the mapping
 * @blkcnt: Number of blocks to map
 * @addr: The target memory address of the mapping
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_map_mem(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		   void *addr);

/**
 * blkmap_map_pmem() - Map region of physical memory
 *
 * Ensures that a valid physical to virtual memory mapping for the
 * requested region is valid for the lifetime of the mapping, on
 * architectures that require it (sandbox).
 *
 * @dev: Blkmap to create the mapping on
 * @blknr: Start block number of the mapping
 * @blkcnt: Number of blocks to map
 * @paddr: The target physical memory address of the mapping
 * @preserve: Mapping intended to be preserved for subsequent stages,
 *		like the OS (e.g. ISO installer)
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_map_pmem(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		    phys_addr_t paddr, bool preserve);

/**
 * blkmap_map_crypt() - Map region of encrypted device
 *
 * Creates a mapping that performs on-the-fly decryption of the specified
 * region from another block device. Suitable for LUKS and other encrypted
 * block devices.
 *
 * @dev: Blkmap to create the mapping on
 * @blknr: Start block number of the mapping
 * @blkcnt: Number of blocks to map
 * @lblk: The target block device containing encrypted data
 * @lblknr: The start block number of the encrypted partition
 * @master_key: Decrypted master key for decryption
 * @key_size: Size of the master key in bytes (must be <= 128)
 * @payload_offset: Offset in sectors from lblknr to actual encrypted payload
 * @cipher_mode: Cipher mode (CBC or XTS)
 * @sector_size: Sector size for IV calculation (typically 512 or 4096)
 * @use_essiv: True to use ESSIV mode, false for plain64 mode (CBC only)
 * @essiv_key: ESSIV key (SHA256 of master key), or NULL if use_essiv is false
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_map_crypt(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		     struct udevice *lblk, lbaint_t lblknr,
		     const u8 *master_key, u32 key_size, u32 payload_offset,
		     enum blkmap_crypt_mode cipher_mode, u32 sector_size,
		     bool use_essiv, const u8 *essiv_key);

/**
 * blkmap_from_label() - Find blkmap from label
 *
 * @label: Label of the requested blkmap
 * Returns: A pointer to the blkmap on success, NULL on failure
 */
struct udevice *blkmap_from_label(const char *label);

/**
 * blkmap_create() - Create new blkmap
 *
 * @label: Label of the new blkmap
 * @devp: If not NULL, updated with the address of the resulting device
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_create(const char *label, struct udevice **devp);

/**
 * blkmap_destroy() - Destroy blkmap
 *
 * @dev: The blkmap to be destroyed
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_destroy(struct udevice *dev);

/**
 * blkmap_create_ramdisk() - Create new ramdisk with blkmap
 *
 * @label: Label of the new blkmap
 * @image_addr: Target memory start address of this mapping
 * @image_size: Target memory size of this mapping
 * @devp: Updated with the address of the created blkmap device
 * Returns: 0 on success, negative error code on failure
 */
int blkmap_create_ramdisk(const char *label, ulong image_addr, ulong image_size,
			  struct udevice **devp);

/**
 * blkmap_get_preserved_pmem_slices() - Look for memory mapped preserved slices
 * @cb: Callback function to call for the blkmap slice
 * @ctx: Argument to be passed to the callback function
 *
 * The function is used to iterate through all the blkmap slices, looking
 * specifically for memory mapped blkmap mapping which has been
 * created with the preserve attribute. The function looks for such slices
 * with the relevant attributes and then calls the callback function which
 * then does additional configuration as needed. The callback function is
 * invoked for all the discovered slices, unless there is an error returned
 * by the callback, in which case the function returns that error.
 *
 * The callback function has the following arguments
 * @ctx:	Argument to be passed to the callback function
 * @addr:	Start address of the memory mapped slice
 * @size:	Size of the memory mapped slice
 *
 * Typically, the callback will perform some configuration needed for the
 * information passed on to it. An example of this would be setting up the
 * pmem node in a device-tree(passed through the ctx argument) with the
 * parameters passed on to the callback.
 *
 * Return: 0 on success, negative error on failure
 */
int blkmap_get_preserved_pmem_slices(int (*cb)(void *ctx, u64 addr,
					       u64 size), void *ctx);

#endif	/* _BLKMAP_H */
