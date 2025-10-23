// SPDX-License-Identifier: GPL-2.0+
/*
 * Tests for JSON pretty-printer
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#include <json.h>
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
