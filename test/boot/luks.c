// SPDX-License-Identifier: GPL-2.0+
/*
 * Test for LUKS detection
 *
 * Copyright (C) 2025 Canonical Ltd
 */

#include <blk.h>
#include <dm.h>
#include <luks.h>
#include <mmc.h>
#include <part.h>
#include <asm/global_data.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <dm/test.h>
#include <test/test.h>
#include <test/ut.h>
#include "bootstd_common.h"

DECLARE_GLOBAL_DATA_PTR;

/**
 * setup_mmc_device() - Set up an MMC device for testing
 *
 * This function binds and probes an MMC device specified by its device tree
 * node name. It is used to prepare MMC devices containing test disk images
 * with various configurations (e.g., LUKS1, LUKS2 encryption).
 *
 * @uts: Unit test state
 * @node_name: Name of the device tree node (e.g., "mmc11", "mmc12")
 * @mmcp: Returns pointer to the MMC device
 * Return: 0 if OK, -ve on error
 */
static int setup_mmc_device(struct unit_test_state *uts, const char *node_name,
			    struct udevice **mmcp)
{
	struct udevice *mmc;
	ofnode root, node;

	/* Enable the specified mmc node */
	root = oftree_root(oftree_default());
	node = ofnode_find_subnode(root, node_name);
	ut_assert(ofnode_valid(node));
	ut_assertok(lists_bind_fdt(gd->dm_root, node, &mmc, NULL, false));

	/* Probe the device */
	ut_assertok(device_probe(mmc));
	*mmcp = mmc;

	return 0;
}

/* Setup mmc11 device */
static int setup_mmc11(struct unit_test_state *uts, struct udevice **mmcp)
{
	ut_assertok(setup_mmc_device(uts, "mmc11", mmcp));

	return 0;
}

/* Setup mmc12 device */
static int setup_mmc12(struct unit_test_state *uts, struct udevice **mmcp)
{
	ut_assertok(setup_mmc_device(uts, "mmc12", mmcp));

	return 0;
}

/* Test LUKS detection on mmc11 partitions */
static int bootstd_test_luks_detect(struct unit_test_state *uts)
{
	struct disk_partition info;
	struct blk_desc *desc;
	struct udevice *mmc;
	int ret;

	ut_assertok(setup_mmc11(uts, &mmc));
	desc = blk_get_by_device(mmc);
	ut_assertnonnull(desc);
	ut_assertnonnull(desc->bdev);

	/* Check partition 1 - should NOT be LUKS */
	ut_assertok(part_get_info(desc, 1, &info));
	ret = luks_detect(desc->bdev, &info);
	ut_assert(ret < 0);  /* Should fail - not LUKS */

	/* Check partition 2 - should BE LUKS */
	ut_assertok(part_get_info(desc, 2, &info));
	ut_assertok(luks_detect(desc->bdev, &info));

	/* Verify it's LUKS version 1 */
	ut_asserteq(1, luks_get_version(desc->bdev, &info));

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks_detect, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);

/* Test LUKS command on mmc11 partitions */
static int bootstd_test_luks_cmd(struct unit_test_state *uts)
{
	struct udevice *mmc;

	ut_assertok(setup_mmc11(uts, &mmc));

	/* Test partition 1 - should NOT be LUKS */
	ut_asserteq(1, run_command("luks detect mmc b:1", 0));
	ut_assert_nextlinen("Not a LUKS partition (error -");
	ut_assert_console_end();

	/* Test partition 2 - should BE LUKS */
	ut_assertok(run_command("luks detect mmc b:2", 0));
	ut_assert_nextline("LUKS1 encrypted partition detected");
	ut_assert_console_end();

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks_cmd, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);

/* Test LUKS info command on mmc11 partition 2 */
static int bootstd_test_luks_info(struct unit_test_state *uts)
{
	struct udevice *mmc;

	ut_assertok(setup_mmc11(uts, &mmc));

	/* Test partition 2 LUKS info */
	ut_assertok(run_command("luks info mmc b:2", 0));
	ut_assert_nextline("Version:        1");
	ut_assert_nextlinen("Cipher name:");
	ut_assert_nextlinen("Cipher mode:");
	ut_assert_nextlinen("Hash spec:");
	ut_assert_nextlinen("Payload offset:");
	ut_assert_nextlinen("Key bytes:");
	ut_assert_console_end();

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks_info, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);

/* Test LUKSv2 detection on mmc12 partitions */
static int bootstd_test_luks2_detect(struct unit_test_state *uts)
{
	struct disk_partition info;
	struct blk_desc *desc;
	struct udevice *mmc;
	int ret;

	ut_assertok(setup_mmc12(uts, &mmc));
	desc = blk_get_by_device(mmc);
	ut_assertnonnull(desc);
	ut_assertnonnull(desc->bdev);

	/* Check partition 1 - should NOT be LUKS */
	ut_assertok(part_get_info(desc, 1, &info));
	ret = luks_detect(desc->bdev, &info);
	ut_assert(ret < 0);  /* Should fail - not LUKS */

	/* Check partition 2 - should BE LUKS */
	ut_assertok(part_get_info(desc, 2, &info));
	ut_assertok(luks_detect(desc->bdev, &info));

	/* Verify it's LUKS version 2 */
	ut_asserteq(2, luks_get_version(desc->bdev, &info));

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks2_detect, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);

/* Test LUKSv2 command on mmc12 partitions */
static int bootstd_test_luks2_cmd(struct unit_test_state *uts)
{
	struct udevice *mmc;

	ut_assertok(setup_mmc12(uts, &mmc));

	/* Test partition 1 - should NOT be LUKS */
	ut_asserteq(1, run_command("luks detect mmc c:1", 0));
	ut_assert_nextlinen("Not a LUKS partition (error -");
	ut_assert_console_end();

	/* Test partition 2 - should BE LUKS */
	ut_assertok(run_command("luks detect mmc c:2", 0));
	ut_assert_nextline("LUKS2 encrypted partition detected");
	ut_assert_console_end();

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks2_cmd, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);

/* Test LUKSv2 info command on mmc12 partition 2 */
static int bootstd_test_luks2_info(struct unit_test_state *uts)
{
	struct udevice *mmc;

	ut_assertok(setup_mmc12(uts, &mmc));

	/* Test partition 2 LUKS info */
	ut_assertok(run_command("luks info mmc c:2", 0));
	ut_assert_nextline("Version:        2");
	ut_assert_nextlinen("Header size:");
	ut_assert_nextlinen("Sequence ID:");
	ut_assert_nextlinen("UUID:");
	ut_assert_nextlinen("Label:");
	ut_assert_nextlinen("Checksum alg:");

	/* Verify JSON metadata section is present (skip empty line first) */
	ut_assert_skip_to_line("");
	ut_assert_nextlinen("JSON metadata (");
	ut_assert_nextline("{");
	/* JSON output varies and there is little value in checking it here */

	return 0;
}
BOOTSTD_TEST(bootstd_test_luks2_info, UTF_DM | UTF_SCAN_FDT | UTF_CONSOLE);
