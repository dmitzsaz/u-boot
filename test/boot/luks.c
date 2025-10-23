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

/* Common function to setup mmc11 device */
static int setup_mmc11(struct unit_test_state *uts, struct udevice **mmcp)
{
	ofnode root, node;

	/* Enable the mmc11 node */
	root = oftree_root(oftree_default());
	node = ofnode_find_subnode(root, "mmc11");
	ut_assert(ofnode_valid(node));
	ut_assertok(lists_bind_fdt(gd->dm_root, node, mmcp, NULL, false));

	/* Probe the device */
	ut_assertok(device_probe(*mmcp));

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
