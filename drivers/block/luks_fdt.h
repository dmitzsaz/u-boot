/* SPDX-License-Identifier: GPL-2.0+ */
/* Private, short-lived JSON trees must not enter DM's global tree registry. */
#ifndef _LUKS_FDT_H
#define _LUKS_FDT_H
#include <linux/libfdt.h>
#include <linux/string.h>

struct luks_node {
	const void *fdt;
	int offset;
};

static inline struct luks_node luks_fdt_root(const void *fdt)
{
	return (struct luks_node){ fdt, 0 };
}

static inline bool luks_node_valid(struct luks_node n)
{
	return n.fdt && n.offset >= 0;
}

static inline struct luks_node luks_node_find_subnode(struct luks_node n,
						     const char *name)
{
	return (struct luks_node){ n.fdt,
		fdt_subnode_offset(n.fdt, n.offset, name) };
}

static inline const char *luks_node_read_string(struct luks_node n,
					       const char *name)
{
	int len;
	const char *s = fdt_getprop(n.fdt, n.offset, name, &len);

	return s && len > 0 && s[len - 1] == '\0' ? s : NULL;
}

static inline int luks_node_read_u32(struct luks_node n, const char *name,
				   u32 *out)
{
	int len;
	const fdt32_t *v = fdt_getprop(n.fdt, n.offset, name, &len);

	if (!v || len != sizeof(*v))
		return -EINVAL;
	*out = fdt32_to_cpu(*v);
	return 0;
}

#define luks_node_for_each_subnode(n, p) \
	for ((n).fdt = (p).fdt, \
	     (n).offset = fdt_first_subnode((p).fdt, (p).offset); \
	     (n).offset >= 0; \
	     (n).offset = fdt_next_subnode((n).fdt, (n).offset))
#endif
