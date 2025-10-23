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

static char luks_help_text[] =
	"detect <interface> <dev[:part]> - detect if partition is LUKS encrypted\n"
	"luks info <interface> <dev[:part]> - show LUKS header information";

U_BOOT_CMD_WITH_SUBCMDS(luks, "LUKS (Linux Unified Key Setup) operations",
			luks_help_text,
	U_BOOT_SUBCMD_MKENT(detect, 3, 1, do_luks_detect),
	U_BOOT_SUBCMD_MKENT(info, 3, 1, do_luks_info));
