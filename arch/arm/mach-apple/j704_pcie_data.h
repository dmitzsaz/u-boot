/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _J704_PCIE_DATA_H
#define _J704_PCIE_DATA_H
#include <linux/libfdt.h>

/* Shared with the Linux board's loader contract. Values stay in the live DT. */
enum j704_pcie_table_id {
	J704_TABLE_AXI, J704_TABLE_PHY, J704_TABLE_PLL, J704_TABLE_AUSPMA,
	J704_TABLE_CONFIG, J704_TABLE_ROOT, J704_TABLE_GEN3, J704_TABLE_GEN4,
	J704_TABLE_COUNT,
};
struct j704_pcie_table {
	const fdt32_t *words;
	unsigned int count;
};
static struct j704_pcie_table j704_pcie_tables[J704_TABLE_COUNT];

static int j704_pcie_load_tables(const void *fdt)
{
	static const struct {
		const char *role, *property;
		u32 span;
	} contract[J704_TABLE_COUNT] = {
		{ "shared", "apple,tunable-axi2af", 0x1000000 },
		{ "shared", "apple,tunable-phy", 0x40000 },
		{ "shared", "apple,tunable-pll", 0x20000 },
		{ "shared", "apple,tunable-auspma", 0x20000 },
		{ "port2", "apple,tunable-config", 0x8000 },
		{ "port2", "apple,tunable-root", 0x4000 },
		{ "port2", "apple,tunable-gen3", 0x4000 },
		{ "port2", "apple,tunable-gen4", 0x4000 },
	};
	struct j704_pcie_table tables[J704_TABLE_COUNT] = {};
	const fdt32_t *version;
	int parent, node, length;
	unsigned int i, j;

	memset(j704_pcie_tables, 0, sizeof(j704_pcie_tables));
	parent = fdt_path_offset(fdt, "/j704-platform-data");
	if (parent < 0 || fdt_node_check_compatible(fdt, parent, "apple,j704-platform-data") ||
	    !fdt_getprop(fdt, parent, "apple,pcie-tunables-ready", NULL))
		return -ENODATA;
	version = fdt_getprop(fdt, parent, "apple,format-version", &length);
	if (!version || length != sizeof(*version) || fdt32_to_cpu(*version) != 1)
		return -EINVAL;
	for (i = 0; i < J704_TABLE_COUNT; i++) {
		node = fdt_subnode_offset(fdt, parent, contract[i].role);
		if (node < 0)
			return -ENODATA;
		tables[i].words = fdt_getprop(fdt, node, contract[i].property, &length);
		if (!tables[i].words || length <= 0 || length % (3 * sizeof(fdt32_t)) ||
		    length > 4096 * 3 * sizeof(fdt32_t))
			return -EINVAL;
		tables[i].count = length / (3 * sizeof(fdt32_t));
		for (j = 0; j < tables[i].count; j++) {
			u32 offset = fdt32_to_cpu(tables[i].words[3 * j]);
			if ((offset & 3) || offset > contract[i].span - sizeof(u32))
				return -EINVAL;
		}
	}
	memcpy(j704_pcie_tables, tables, sizeof(tables));
	return 0;
}
#endif
