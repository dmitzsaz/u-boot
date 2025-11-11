// SPDX-License-Identifier: GPL-2.0+
/*
 * Tests for JSON utilities including parser and FDT converter
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#include <dm/ofnode.h>
#include <fdt_support.h>
#include <image.h>
#include <json.h>
#include <linux/libfdt.h>
#include <test/lib.h>
#include <test/test.h>
#include <test/ut.h>

static int lib_test_json_simple_object(struct unit_test_state *uts)
{
	const char *json = "{\"name\":\"value\"}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"name\": \"value\"");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_simple_object, UTF_CONSOLE);

static int lib_test_json_simple_array(struct unit_test_state *uts)
{
	const char *json = "[1,2,3]";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("[");
	ut_assert_nextline("  1,");
	ut_assert_nextline("  2,");
	ut_assert_nextline("  3");
	ut_assert_nextline("]");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_simple_array, UTF_CONSOLE);

static int lib_test_json_nested_object(struct unit_test_state *uts)
{
	const char *json = "{\"outer\":{\"inner\":\"value\"}}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"outer\": {");
	ut_assert_nextline("    \"inner\": \"value\"");
	ut_assert_nextline("  }");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_nested_object, UTF_CONSOLE);

static int lib_test_json_nested_array(struct unit_test_state *uts)
{
	const char *json = "[[1,2],[3,4]]";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("[");
	ut_assert_nextline("  [");
	ut_assert_nextline("    1,");
	ut_assert_nextline("    2");
	ut_assert_nextline("  ],");
	ut_assert_nextline("  [");
	ut_assert_nextline("    3,");
	ut_assert_nextline("    4");
	ut_assert_nextline("  ]");
	ut_assert_nextline("]");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_nested_array, UTF_CONSOLE);

static int lib_test_json_mixed_nested(struct unit_test_state *uts)
{
	const char *json = "{\"array\":[1,{\"nested\":\"obj\"}]}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"array\": [");
	ut_assert_nextline("    1,");
	ut_assert_nextline("    {");
	ut_assert_nextline("      \"nested\": \"obj\"");
	ut_assert_nextline("    }");
	ut_assert_nextline("  ]");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_mixed_nested, UTF_CONSOLE);

static int lib_test_json_string_with_colon(struct unit_test_state *uts)
{
	const char *json = "{\"url\":\"http://example.com\"}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"url\": \"http://example.com\"");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_string_with_colon, UTF_CONSOLE);

static int lib_test_json_string_with_comma(struct unit_test_state *uts)
{
	const char *json = "{\"name\":\"last, first\"}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"name\": \"last, first\"");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_string_with_comma, UTF_CONSOLE);

static int lib_test_json_string_with_braces(struct unit_test_state *uts)
{
	const char *json = "{\"text\":\"some {braces} here\"}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"text\": \"some {braces} here\"");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_string_with_braces, UTF_CONSOLE);

static int lib_test_json_escaped_quote(struct unit_test_state *uts)
{
	const char *json = "{\"quote\":\"He said \\\"hello\\\"\"}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"quote\": \"He said \\\"hello\\\"\"");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_escaped_quote, UTF_CONSOLE);

static int lib_test_json_multiple_fields(struct unit_test_state *uts)
{
	const char *json = "{\"name\":\"test\",\"age\":25,\"active\":true}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"name\": \"test\",");
	ut_assert_nextline("  \"age\": 25,");
	ut_assert_nextline("  \"active\": true");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_multiple_fields, UTF_CONSOLE);

static int lib_test_json_empty_object(struct unit_test_state *uts)
{
	const char *json = "{}";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_empty_object, UTF_CONSOLE);

static int lib_test_json_empty_array(struct unit_test_state *uts)
{
	const char *json = "[]";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("[");
	ut_assert_nextline("]");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_empty_array, UTF_CONSOLE);

static int lib_test_json_whitespace(struct unit_test_state *uts)
{
	const char *json = "{ \"name\" : \"value\" , \"num\" : 42 }";

	json_print_pretty(json, strlen(json));
	ut_assert_nextline("{");
	ut_assert_nextline("  \"name\": \"value\",");
	ut_assert_nextline("  \"num\": 42");
	ut_assert_nextline("}");
	ut_assert_console_end();

	return 0;
}
LIB_TEST(lib_test_json_whitespace, UTF_CONSOLE);

/* JSON to FDT conversion tests */

static int lib_test_json_to_fdt_simple(struct unit_test_state *uts)
{
	const char *json = "{\"name\":\"test\",\"value\":42}";
	struct abuf buf;
	void *fdt_buf;

	ut_assertok(json_to_fdt(json, &buf));

	fdt_buf = abuf_data(&buf);

	/* Verify FDT is valid */
	ut_assertok(fdt_check_header(fdt_buf));

	/* Check string property */
	ut_asserteq_str("test", fdt_getprop(fdt_buf, 0, "name", NULL));

	/* Check integer property */
	ut_asserteq(42, fdtdec_get_int(fdt_buf, 0, "value", 0));

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_simple, 0);

static int lib_test_json_to_fdt_nested(struct unit_test_state *uts)
{
	const char *json = "{\"outer\":{\"inner\":\"value\"}}";
	struct abuf buf;
	void *fdt_buf;
	int node;

	ut_assertok(json_to_fdt(json, &buf));

	fdt_buf = abuf_data(&buf);

	/* Verify FDT is valid */
	ut_assertok(fdt_check_header(fdt_buf));

	/* Find nested node */
	node = fdt_path_offset(fdt_buf, "/outer");
	ut_assert(node >= 0);

	/* Check property in nested node */
	ut_asserteq_str("value", fdt_getprop(fdt_buf, node, "inner", NULL));

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_nested, 0);

static int lib_test_json_to_fdt_array(struct unit_test_state *uts)
{
	const char *json = "{\"numbers\":[1,2,3]}";
	struct abuf buf;
	void *fdt_buf;
	u32 arr[8];
	int size;
	oftree tree;
	ofnode root;

	ut_assertok(json_to_fdt(json, &buf));

	fdt_buf = abuf_data(&buf);

	/* Verify FDT is valid */
	ut_assertok(fdt_check_header(fdt_buf));

	/* Create oftree from FDT */
	tree = oftree_from_fdt(fdt_buf);
	ut_assert(oftree_valid(tree));

	root = oftree_root(tree);
	ut_assert(ofnode_valid(root));

	/* Check array property */
	ut_assertnonnull(ofnode_get_property(root, "numbers", &size));
	size /= sizeof(u32);
	ut_asserteq(3, size);
	ut_assertok(ofnode_read_u32_array(root, "numbers", arr, size));
	ut_asserteq(1, arr[0]);
	ut_asserteq(2, arr[1]);
	ut_asserteq(3, arr[2]);

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_array, 0);

static int lib_test_json_to_fdt_string_array(struct unit_test_state *uts)
{
	char json[] = "{\"tags\":[\"first\",\"second\",\"third\"]}";
	struct abuf buf;
	void *fdt_buf;
	const char *str;
	ofnode root;
	oftree tree;

	ut_assertok(json_to_fdt(json, &buf));

	fdt_buf = abuf_data(&buf);

	/* Verify FDT is valid */
	ut_assertok(fdt_check_header(fdt_buf));

	/* Create oftree from FDT */
	tree = oftree_from_fdt(fdt_buf);
	ut_assert(oftree_valid(tree));

	root = oftree_root(tree);
	ut_assert(ofnode_valid(root));

	/* Check string array property */
	ut_assertok(ofnode_read_string_index(root, "tags", 0, &str));
	ut_asserteq_str("first", str);
	ut_assertok(ofnode_read_string_index(root, "tags", 1, &str));
	ut_asserteq_str("second", str);
	ut_assertok(ofnode_read_string_index(root, "tags", 2, &str));
	ut_asserteq_str("third", str);

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_string_array, 0);

static int lib_test_json_to_fdt_bool(struct unit_test_state *uts)
{
	const char *json = "{\"enabled\":true,\"disabled\":false}";
	struct abuf buf;
	void *fdt_buf;

	ut_assertok(json_to_fdt(json, &buf));

	fdt_buf = abuf_data(&buf);

	/* Verify FDT is valid */
	ut_assertok(fdt_check_header(fdt_buf));

	/* Check boolean properties */
	ut_asserteq(1, fdtdec_get_int(fdt_buf, 0, "enabled", 0));
	ut_asserteq(0, fdtdec_get_int(fdt_buf, 0, "disabled", 0));

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_bool, 0);

/* Test with realistic LUKS2 JSON metadata using ofnode API */
static int lib_test_json_to_fdt_luks2(struct unit_test_state *uts)
{
	/* Simplified LUKS2 JSON metadata structure */
	const char *luks2_json =
		"{"
		"  \"keyslots\": {"
		"    \"0\": {"
		"      \"type\": \"luks2\","
		"      \"key_size\": 32,"
		"      \"area\": {"
		"        \"type\": \"raw\","
		"        \"offset\": \"32768\","
		"        \"size\": \"258048\""
		"      },"
		"      \"kdf\": {"
		"        \"type\": \"pbkdf2\","
		"        \"hash\": \"sha256\","
		"        \"iterations\": 1000,"
		"        \"salt\": \"aGVsbG93b3JsZA==\""
		"      }"
		"    },"
		"    \"1\": {"
		"      \"type\": \"luks2\","
		"      \"key_size\": 32,"
		"      \"area\": {"
		"        \"type\": \"raw\","
		"        \"offset\": \"290816\","
		"        \"size\": \"258048\""
		"      },"
		"      \"kdf\": {"
		"        \"type\": \"pbkdf2\","
		"        \"hash\": \"sha256\","
		"        \"iterations\": 2000,"
		"        \"salt\": \"YW5vdGhlcnNhbHQ=\""
		"      }"
		"    }"
		"  },"
		"  \"segments\": {"
		"    \"0\": {"
		"      \"type\": \"crypt\","
		"      \"offset\": \"16777216\","
		"      \"size\": \"dynamic\","
		"      \"iv_tweak\": \"0\","
		"      \"encryption\": \"aes-cbc-essiv:sha256\","
		"      \"sector_size\": 512"
		"    }"
		"  },"
		"  \"digests\": {"
		"    \"0\": {"
		"      \"type\": \"pbkdf2\","
		"      \"keyslots\": [0, 1],"
		"      \"segments\": [0],"
		"      \"hash\": \"sha256\","
		"      \"iterations\": 1000,"
		"      \"salt\": \"c2FsdHlzYWx0\""
		"    }"
		"  },"
		"  \"config\": {"
		"    \"json_size\": \"12288\","
		"    \"keyslots_size\": \"3145728\""
		"  }"
		"}";

	ofnode segments, segment0, digests, digest0, config;
	ofnode root, keyslots, keyslot0, kdf;
	struct abuf buf;
	u32 arr[8];
	int size;
	void *fdt_buf;
	oftree tree;

	ut_assertok(json_to_fdt(luks2_json, &buf));

	/* Verify FDT is valid */
	fdt_buf = abuf_data(&buf);
	ut_assertok(fdt_check_header(fdt_buf));

	/* Create oftree from FDT */
	tree = oftree_from_fdt(fdt_buf);
	ut_assert(oftree_valid(tree));

	/* Get root node */
	root = oftree_root(tree);
	ut_assert(ofnode_valid(root));

	/* Navigate to keyslots node */
	keyslots = ofnode_find_subnode(root, "keyslots");
	ut_assert(ofnode_valid(keyslots));

	/* Navigate to keyslot 0 */
	keyslot0 = ofnode_find_subnode(keyslots, "0");
	ut_assert(ofnode_valid(keyslot0));

	/* Check keyslot type */
	ut_asserteq_str("luks2", ofnode_read_string(keyslot0, "type"));

	/* Check key_size */
	ut_asserteq(32, ofnode_read_u32_default(keyslot0, "key_size", 0));

	/* Navigate to KDF node */
	kdf = ofnode_find_subnode(keyslot0, "kdf");
	ut_assert(ofnode_valid(kdf));

	/* Check KDF type */
	ut_asserteq_str("pbkdf2", ofnode_read_string(kdf, "type"));

	/* Check KDF hash */
	ut_asserteq_str("sha256", ofnode_read_string(kdf, "hash"));

	/* Check iterations */
	ut_asserteq(1000, ofnode_read_u32_default(kdf, "iterations", 0));

	/* Check salt (base64 string) */
	ut_asserteq_str("aGVsbG93b3JsZA==", ofnode_read_string(kdf, "salt"));

	/* Navigate to segments node */
	segments = ofnode_find_subnode(root, "segments");
	ut_assert(ofnode_valid(segments));

	/* Navigate to segment 0 */
	segment0 = ofnode_find_subnode(segments, "0");
	ut_assert(ofnode_valid(segment0));

	/* Check segment type */
	ut_asserteq_str("crypt", ofnode_read_string(segment0, "type"));

	/* Check encryption */
	ut_asserteq_str("aes-cbc-essiv:sha256", ofnode_read_string(segment0, "encryption"));

	/* Check offset (stored as string in JSON) */
	ut_asserteq_str("16777216", ofnode_read_string(segment0, "offset"));

	/* Check sector_size */
	ut_asserteq(512, ofnode_read_u32_default(segment0, "sector_size", 0));

	/* Navigate to digests node */
	digests = ofnode_find_subnode(root, "digests");
	ut_assert(ofnode_valid(digests));

	/* Navigate to digest 0 */
	digest0 = ofnode_find_subnode(digests, "0");
	ut_assert(ofnode_valid(digest0));

	/* Check digest type */
	ut_asserteq_str("pbkdf2", ofnode_read_string(digest0, "type"));

	/* Check keyslots array */
	ut_assertnonnull(ofnode_get_property(digest0, "keyslots", &size));
	size /= sizeof(u32);
	ut_asserteq(2, size);
	ut_assertok(ofnode_read_u32_array(digest0, "keyslots", arr, size));
	ut_asserteq(0, arr[0]);
	ut_asserteq(1, arr[1]);

	/* Check segments array */
	ut_assertnonnull(ofnode_get_property(digest0, "segments", &size));
	ut_asserteq(4, size);
	size /= sizeof(u32);
	ut_assertok(ofnode_read_u32_array(digest0, "segments", arr, size));
	ut_asserteq(0, arr[0]);

	/* Navigate to config node */
	config = ofnode_find_subnode(root, "config");
	ut_assert(ofnode_valid(config));

	/* Check json_size (stored as string in JSON) */
	ut_asserteq_str("12288", ofnode_read_string(config, "json_size"));

	/* Check keyslots_size (stored as string in JSON) */
	ut_asserteq_str("3145728", ofnode_read_string(config, "keyslots_size"));

	abuf_uninit(&buf);

	return 0;
}
LIB_TEST(lib_test_json_to_fdt_luks2, 0);
