// SPDX-License-Identifier: GPL-2.0+
/*
 * LUKS (Linux Unified Key Setup) command
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#include <blk.h>
#include <blkmap.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <luks.h>
#include <part.h>
#include <mbedtls/platform_util.h>

/* Longest passphrase accepted at the prompt */
#define LUKS_PASSPHRASE_MAX	512

/**
 * luks_read_passphrase() - Read a passphrase from the console without echo
 *
 * @buf: Buffer for the passphrase, nul-terminated on success
 * @size: Size of @buf
 * Return: length of the passphrase, -EINTR if Ctrl-C was pressed, -E2BIG if
 * it is too long
 */
static int luks_read_passphrase(char *buf, int size)
{
	int len = 0;

	printf("Passphrase: ");
	for (;;) {
		int c = getchar();

		switch (c) {
		case '\r':
		case '\n':
			buf[len] = '\0';
			puts("\n");
			return len;
		case 0x03:		/* Ctrl-C */
			puts("\n");
			return -EINTR;
		case '\b':
		case 0x7f:
			if (len)
				len--;
			break;
		default:
			if (len == size - 1) {
				puts("\n");
				return -E2BIG;
			}
			buf[len++] = c;
			break;
		}
	}
}

static void luks_label(char *label, int size, char *const argv[])
{
	snprintf(label, size, "luks-%s-%s", argv[1], argv[2]);
}

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
	char passphrase[LUKS_PASSPHRASE_MAX];
	struct udevice *blkmap_dev;
	int part, ret, version, len;
	u8 master_key[128];
	char label[64];
	u32 key_size;

	if (argc != 3 && argc != 4)
		return CMD_RET_USAGE;

	part = blk_get_device_part_str(argv[1], argv[2], &dev_desc, &info, 1);
	if (part < 0)
		return CMD_RET_FAILURE;

	/* Verify it's a LUKS partition */
	version = luks_get_version(dev_desc->bdev, &info);
	if (version < 0) {
		printf("Not a LUKS partition\n");
		return CMD_RET_FAILURE;
	}

	/* Create blkmap device with label based on source device */
	luks_label(label, sizeof(label), argv);
	if (blkmap_from_label(label)) {
		printf("Already unlocked as blkmap device '%s'\n", label);
		return CMD_RET_FAILURE;
	}

	/* Without a passphrase argument, ask for it without echoing it */
	if (argc == 4) {
		len = strlcpy(passphrase, argv[3], sizeof(passphrase));
		if (len >= sizeof(passphrase))
			return CMD_RET_FAILURE;
	} else {
		len = luks_read_passphrase(passphrase, sizeof(passphrase));
		if (len < 0) {
			ret = CMD_RET_FAILURE;
			goto cleanup;
		}
	}

	printf("Unlocking LUKS%d partition...\n", version);

	/* Unlock the partition to get the master key */
	ret = luks_unlock(dev_desc->bdev, &info, (const u8 *)passphrase, len,
			  false, master_key, &key_size);
	if (ret) {
		if (ret == -EACCES)
			printf("Wrong passphrase\n");
		else
			printf("Failed to unlock LUKS partition (err %dE)\n",
			       ret);
		ret = CMD_RET_FAILURE;
		goto cleanup;
	}

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
	/* Wipe master key and passphrase from stack */
	mbedtls_platform_zeroize(master_key, sizeof(master_key));
	mbedtls_platform_zeroize(passphrase, sizeof(passphrase));

	return ret;
}

static int do_luks_close(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	struct udevice *dev;
	char label[64];

	if (argc != 3)
		return CMD_RET_USAGE;

	luks_label(label, sizeof(label), argv);
	dev = blkmap_from_label(label);
	if (!dev) {
		printf("No unlocked partition '%s'\n", label);
		return CMD_RET_FAILURE;
	}

	/* Destroying the blkmap wipes the master key */
	if (blkmap_destroy(dev)) {
		printf("Failed to close '%s'\n", label);
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

static char luks_help_text[] =
	"detect <interface> <dev[:part]> - detect if partition is LUKS encrypted\n"
	"luks info <interface> <dev[:part]> - show LUKS header information\n"
	"luks unlock <interface> <dev[:part]> [<passphrase>] - unlock LUKS partition,\n"
	"    asking for the passphrase if it is not given\n"
	"luks close <interface> <dev[:part]> - remove the unlocked partition\n";

U_BOOT_CMD_WITH_SUBCMDS(luks, "LUKS (Linux Unified Key Setup) operations",
			luks_help_text,
	U_BOOT_SUBCMD_MKENT(detect, 3, 1, do_luks_detect),
	U_BOOT_SUBCMD_MKENT(info, 3, 1, do_luks_info),
	U_BOOT_SUBCMD_MKENT(unlock, 4, 1, do_luks_unlock),
	U_BOOT_SUBCMD_MKENT(close, 3, 1, do_luks_close));
