/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * JSON utilities
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#ifndef __JSON_H__
#define __JSON_H__

struct abuf;

/**
 * json_print_pretty() - Print JSON with indentation
 *
 * This function takes a JSON string and prints it with proper indentation,
 * making it more human-readable. It handles nested objects and arrays.
 *
 * @json:	JSON string to print (may be nul terminated before @len)
 * @len:	Length of JSON string
 */
void json_print_pretty(const char *json, int len);

/**
 * json_to_fdt() - Convert JSON to a Flattened Device Tree (DTB) blob
 *
 * Parse a JSON string and convert it to a dtb blob. JSON objects become nodes;
 * JSON properties become device tree properties. This is useful for converting
 * LUKS2 metadata to a format that can be queried using U-Boot's ofnode APIs.
 *
 * This function temporarily modifies the JSON string in-place, writing nul
 * terminators during parsing, then restores the original characters. The JSON
 * string is only modified during the function call and is restored before
 * returning.
 *
 * The resulting DTB contains copies of all data, so the JSON string can be
 * freed or modified after this function returns.
 *
 * Conversion rules:
 * - JSON objects → DT nodes
 * - JSON strings → string properties
 * - JSON numbers → u32 or u64 cell properties
 * - JSON arrays of numbers → cell array properties (max MAX_ARRAY_SIZE)
 * - JSON arrays of strings → stringlist properties (max MAX_ARRAY_SIZE)
 * - JSON booleans → u32 properties (0 or 1). This breaks the dtb convention of
 *	simply using presence to indicate true, so that we can actually check
 *	what was present in the JSON data
 * - JSON null → empty property
 *
 * @json:	JSON string to parse (temporarily modified during call)
 * @buf:	abuf to init and populate with the DTB (called must uninit)
 * Return: 0 on success, negative error code on failure
 */
int json_to_fdt(const char *json, struct abuf *buf);

#endif /* __JSON_H__ */
