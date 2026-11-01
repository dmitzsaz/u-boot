// SPDX-License-Identifier: GPL-2.0+
/* LUKS command, based on Simon Glass / Canonical's 2025 implementation. */
#include <blk.h>
#include <blkmap.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <env.h>
#include <luks.h>
#include <part.h>
#include <stdio.h>
#include <mbedtls/platform_util.h>

static struct udevice *unlocked;

/* Never pass a password through the shell, environment or command history. */
static int read_passphrase(char *buf, size_t size)
{
	size_t len = 0;
	int ch;

	puts("LUKS passphrase (input hidden): ");
	for (;;) {
		ch = getchar();
		if (ch == '\r' || ch == '\n') {
			putc('\n');
			buf[len] = '\0';
			return len;
		}
		if (ch == 3 || ch == 27) {
			putc('\n');
			return -EINTR;
		}
		if (ch == '\b' || ch == 127) {
			if (len)
				len--;
		} else if (ch >= 32 && ch <= 255) {
			if (len + 1 >= size) {
				puts("\nPassphrase too long\n");
				return -E2BIG;
			}
			buf[len++] = ch;
		}
	}
}

static int do_luks(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	struct disk_partition info;
	struct blk_desc *desc;
	u8 master_key[128] = {};
	char pass[512] = {};
	u32 key_size = 0;
	int ret, len;

	if (argc == 2 && !strcmp(argv[1], "close")) {
		ret = unlocked ? blkmap_destroy(unlocked) : 0;
		if (!ret) {
			unlocked = NULL;
			env_set("luksdev", NULL);
		}
		return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	if (argc != 4 || (strcmp(argv[1], "detect") &&
			  strcmp(argv[1], "unlock")))
		return CMD_RET_USAGE;
	if (blk_get_device_part_str(argv[2], argv[3], &desc, &info, 1) < 0)
		return CMD_RET_FAILURE;
	ret = luks_get_version(desc->bdev, &info);
	if (ret < 0)
		return CMD_RET_FAILURE;
	printf("LUKS%d partition detected\n", ret);
	if (!strcmp(argv[1], "detect"))
		return CMD_RET_SUCCESS;
	if (unlocked) {
		puts("Close the existing LUKS mapping first\n");
		return CMD_RET_FAILURE;
	}
	len = read_passphrase(pass, sizeof(pass));
	ret = len < 0 ? len : luks_unlock(desc->bdev, &info, (u8 *)pass,
					 len, false, master_key, &key_size);
	mbedtls_platform_zeroize(pass, sizeof(pass));
	if (!ret)
		ret = luks_create_blkmap(desc->bdev, &info, master_key, key_size,
					 "luks-boot", &unlocked);
	mbedtls_platform_zeroize(master_key, sizeof(master_key));
	if (!ret) {
		struct blkmap *bm = dev_get_plat(unlocked);
		struct blk_desc *out = dev_get_uclass_plat(bm->blk);
		char devpart[32];

		snprintf(devpart, sizeof(devpart), "%x:0", out->devnum);
		ret = env_set("luksdev", devpart);
		if (ret) {
			blkmap_destroy(unlocked);
			unlocked = NULL;
		} else {
			printf("Unlocked read-only as blkmap %s\n", devpart);
		}
	}
	if (ret)
		printf("LUKS unlock failed (%d)\n", ret);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(luks, 4, 0, do_luks, "Read-only LUKS unlock",
	  "detect <interface> <dev:part>\n"
	  "luks unlock <interface> <dev:part> - prompt for passphrase\n"
	  "luks close - destroy mapping and wipe key");
