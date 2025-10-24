// SPDX-License-Identifier: GPL-2.0+
/*
 * LUKS (Linux Unified Key Setup) command
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#include <blk.h>
#include <command.h>
#include <dm.h>
#include <luks.h>
#include <part.h>

static int do_luks_detect(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	struct blk_desc *dev_desc;
	struct disk_partition info;
	int part, ret, version;

	if (argc != 3)
		return CMD_RET_USAGE;

	part = blk_get_device_part_str(argv[1], argv[2], &dev_desc, &info, 1);
	if (part < 0)
		return CMD_RET_FAILURE;

	ret = luks_detect(dev_desc->bdev, &info);
	if (ret < 0) {
		printf("Not a LUKS partition (error %dE)\n", ret);
		return CMD_RET_FAILURE;
	}
	version = luks_get_version(dev_desc->bdev, &info);
	printf("LUKS%d encrypted partition detected\n", version);

	return CMD_RET_SUCCESS;
}

static int do_luks_info(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct blk_desc *dev_desc;
	struct disk_partition info;
	int part, ret;

	if (argc != 3)
		return CMD_RET_USAGE;

	part = blk_get_device_part_str(argv[1], argv[2], &dev_desc, &info, 1);
	if (part < 0)
		return CMD_RET_FAILURE;

	ret = luks_show_info(dev_desc->bdev, &info);
	if (ret < 0)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static int do_luks_unlock(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	struct blk_desc *dev_desc;
	struct disk_partition info;
	struct udevice *blkmap_dev;
	const char *passphrase;
	int part, ret, version;
	u8 master_key[128];
	char label[64];
	u32 key_size;

	if (argc != 4)
		return CMD_RET_USAGE;

	part = blk_get_device_part_str(argv[1], argv[2], &dev_desc, &info, 1);
	if (part < 0)
		return CMD_RET_FAILURE;

	passphrase = argv[3];

	/* Verify it's a LUKS partition */
	version = luks_get_version(dev_desc->bdev, &info);
	if (version < 0) {
		printf("Not a LUKS partition\n");
		return CMD_RET_FAILURE;
	}

	if (version != LUKS_VERSION_1) {
		printf("Only LUKS1 is currently supported\n");
		return CMD_RET_FAILURE;
	}

	/* Unlock the partition to get the master key */
	ret = luks_unlock(dev_desc->bdev, &info, passphrase, master_key,
			  &key_size);
	if (ret) {
		printf("Failed to unlock LUKS partition (err %dE)\n", ret);
		return CMD_RET_FAILURE;
	}

	/* Create blkmap device with label based on source device */
	snprintf(label, sizeof(label), "luks-%s-%s", argv[1], argv[2]);

	/* Create and map the blkmap device */
	ret = luks_create_blkmap(dev_desc->bdev, &info, master_key, key_size,
				 label, &blkmap_dev);
	if (ret) {
		printf("Failed to create blkmap device (err %dE)\n", ret);
		ret = CMD_RET_FAILURE;
		goto cleanup;
	}

	printf("Unlocked LUKS partition as blkmap device '%s'\n", label);

	ret = CMD_RET_SUCCESS;

cleanup:
	/* Wipe master key from stack */
	memset(master_key, '\0', sizeof(master_key));

	return ret;
}

static char luks_help_text[] =
	"detect <interface> <dev[:part]> - detect if partition is LUKS encrypted\n"
	"luks info <interface> <dev[:part]> - show LUKS header information\n"
	"luks unlock <interface> <dev[:part]> <passphrase> - unlock LUKS partition\n";

U_BOOT_CMD_WITH_SUBCMDS(luks, "LUKS (Linux Unified Key Setup) operations",
			luks_help_text,
	U_BOOT_SUBCMD_MKENT(detect, 3, 1, do_luks_detect),
	U_BOOT_SUBCMD_MKENT(info, 3, 1, do_luks_info),
	U_BOOT_SUBCMD_MKENT(unlock, 4, 1, do_luks_unlock));
