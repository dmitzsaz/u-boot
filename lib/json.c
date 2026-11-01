// SPDX-License-Identifier: GPL-2.0+
/*
 * JSON utilities including parser and devicetree converter
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#include <abuf.h>
#include <ctype.h>
#include <errno.h>
#include <log.h>
#include <linux/libfdt.h>
#include <malloc.h>

/* Maximum number of elements in a JSON array */
#define MAX_ARRAY_SIZE 256

/* JSON token types */
enum json_token_type {
	JSONT_EOF = 0,
	JSONT_LBRACE,		/* { */
	JSONT_RBRACE,		/* } */
	JSONT_LBRACKET,		/* [ */
	JSONT_RBRACKET,		/* ] */
	JSONT_COLON,		/* : */
	JSONT_COMMA,		/* , */
	JSONT_STRING,		/* "string" */
	JSONT_NUMBER,		/* 123 or 123.45 */
	JSONT_TRUE,		/* true */
	JSONT_FALSE,		/* false */
	JSONT_NULL,		/* null */
	JSONT_ERROR
};

/**
 * struct json_parser - JSON parser context
 * @json: Input JSON string
 * @pos: Current position in the input string
 * @tok: Current token type
 * @tok_start: Pointer to the start of the current token
 * @tok_end: Pointer to the end of the current token
 * @fdt: Flattened Device Tree buffer being constructed
 * @fdt_size: Size of the FDT buffer in bytes
 */
struct json_parser {
	const char *json;
	const char *pos;
	enum json_token_type tok;
	const char *tok_start;
	const char *tok_end;
	void *fdt;
	int fdt_size;
};

/* Increment position in JSON string */
#define INC() ctx->pos++

/* Set token type */
#define SET(t) ctx->tok = (t)

/* Forward declarations for recursive parser functions */
static int parse_object(struct json_parser *ctx, const char *node_name);
static int parse_value(struct json_parser *ctx, const char *prop_name);

/**
 * print_indent() - Print indentation spaces
 *
 * @indent:	Indentation level (each level is 2 spaces)
 */
static void print_indent(int indent)
{
	for (int i = 0; i < indent * 2; i++)
		putc(' ');
}

void json_print_pretty(const char *json, int len)
{
	int indent = 0;
	bool in_string = false;
	bool escaped = false;
	bool after_open = false;
	int i;

	for (i = 0; i < len && json[i]; i++) {
		char c = json[i];

		/* Handle escape sequences */
		if (escaped) {
			putc(c);
			escaped = false;
			continue;
		}

		if (c == '\\') {
			putc(c);
			escaped = true;
			continue;
		}

		/* Track whether we're inside a string */
		if (c == '"') {
			in_string = !in_string;
			if (after_open) {
				print_indent(indent);
				after_open = false;
			}
			putc(c);
			continue;
		}

		/* Don't format inside strings */
		if (in_string) {
			putc(c);
			continue;
		}

		/* Format structural characters */
		switch (c) {
		case '{':
		case '[':
			if (after_open) {
				print_indent(indent);
				after_open = false;
			}
			putc(c);
			putc('\n');
			indent++;
			after_open = true;
			break;

		case '}':
		case ']':
			if (!after_open) {
				putc('\n');
				indent--;
				print_indent(indent);
			} else {
				indent--;
			}
			putc(c);
			after_open = false;
			break;

		case ',':
			putc(c);
			putc('\n');
			print_indent(indent);
			after_open = false;
			break;

		case ':':
			putc(c);
			putc(' ');
			after_open = false;
			break;

		case ' ':
		case '\t':
		case '\n':
		case '\r':
			/* Skip whitespace outside strings */
			break;

		default:
			if (after_open) {
				print_indent(indent);
				after_open = false;
			}
			putc(c);
			break;
		}
	}

	putc('\n');
}

/**
 * skip_whitespace() - Skip whitespace characters
 *
 * @ctx: Parser context
 */
static void skip_whitespace(struct json_parser *ctx)
{
	while (*ctx->pos && isspace(*ctx->pos))
		INC();
}

/**
 * next_token() - Get the next JSON token
 *
 * @ctx: Parser context
 * Return: token type
 */
static enum json_token_type next_token(struct json_parser *ctx)
{
	skip_whitespace(ctx);

	if (!*ctx->pos) {
		SET(JSONT_EOF);
		return JSONT_EOF;
	}

	ctx->tok_start = ctx->pos;

	switch (*ctx->pos) {
	case '{':
		INC();
		SET(JSONT_LBRACE);
		break;
	case '}':
		INC();
		SET(JSONT_RBRACE);
		break;
	case '[':
		INC();
		SET(JSONT_LBRACKET);
		break;
	case ']':
		INC();
		SET(JSONT_RBRACKET);
		break;
	case ':':
		INC();
		SET(JSONT_COLON);
		break;
	case ',':
		INC();
		SET(JSONT_COMMA);
		break;
	case '"': {
		/* Parse string */
		INC();
		ctx->tok_start = ctx->pos;
		while (*ctx->pos && *ctx->pos != '"') {
			if (*ctx->pos == '\\' && ctx->pos[1])
				INC();
			INC();
		}
		ctx->tok_end = ctx->pos;
		if (*ctx->pos == '"')
			INC();
		SET(JSONT_STRING);
		break;
	}
	case '-':
	case '0' ... '9': {
		/* Parse number */
		if (*ctx->pos == '-')
			INC();
		while (*ctx->pos && isdigit(*ctx->pos))
			INC();
		if (*ctx->pos == '.') {
			INC();
			while (*ctx->pos && isdigit(*ctx->pos))
				INC();
		}
		ctx->tok_end = ctx->pos;
		SET(JSONT_NUMBER);
		break;
	}
	case 't':
		if (!strncmp(ctx->pos, "true", 4)) {
			ctx->pos += 4;
			ctx->tok_end = ctx->pos;
			SET(JSONT_TRUE);
		} else {
			SET(JSONT_ERROR);
		}
		break;
	case 'f':
		if (!strncmp(ctx->pos, "false", 5)) {
			ctx->pos += 5;
			ctx->tok_end = ctx->pos;
			SET(JSONT_FALSE);
		} else {
			SET(JSONT_ERROR);
		}
		break;
	case 'n':
		if (!strncmp(ctx->pos, "null", 4)) {
			ctx->pos += 4;
			ctx->tok_end = ctx->pos;
			SET(JSONT_NULL);
		} else {
			SET(JSONT_ERROR);
		}
		break;
	default:
		SET(JSONT_ERROR);
		break;
	}

	return ctx->tok;
}

/**
 * parse_array() - Parse a JSON array
 *
 * @ctx: Parser context
 * @prop: Property name for this array
 * Return: 0 on success, negative error code on failure
 */
static int parse_array(struct json_parser *ctx, const char *prop)
{
	u32 values[MAX_ARRAY_SIZE];
	int index = 0;
	int count = 0;
	int ret;

	/* Expect [ */
	if (ctx->tok != JSONT_LBRACKET)
		return -EINVAL;

	next_token(ctx);

	/* Handle empty array */
	if (ctx->tok == JSONT_RBRACKET) {
		next_token(ctx);
		return fdt_property(ctx->fdt, prop, NULL, 0);
	}

	/* Check if this is an array of objects */
	if (ctx->tok == JSONT_LBRACE) {
		/* Array of objects - create a subnode for each */
		while (ctx->tok != JSONT_RBRACKET) {
			char name[64];

			snprintf(name, sizeof(name), "%s-%d", prop, index++);

			ret = parse_object(ctx, name);
			if (ret)
				return ret;

			if (ctx->tok == JSONT_COMMA)
				next_token(ctx);
			else if (ctx->tok != JSONT_RBRACKET)
				return -EINVAL;
		}

		next_token(ctx);  /* Skip ] */
		return 0;
	}

	/* Array of primitives - collect into cell array */
	while (ctx->tok != JSONT_RBRACKET) {
		if (ctx->tok == JSONT_NUMBER) {
			char num_str[32];
			int len = min((int)(ctx->tok_end - ctx->tok_start),
				      (int)sizeof(num_str) - 1);

			if (count >= MAX_ARRAY_SIZE)
				return -E2BIG;

			memcpy(num_str, ctx->tok_start, len);
			num_str[len] = '\0';
			values[count++] = simple_strtoul(num_str, NULL, 0);
			next_token(ctx);
		} else if (ctx->tok == JSONT_STRING) {
			/* String array - collect strings into a stringlist */
			char *strings[MAX_ARRAY_SIZE];
			char saved_chars[MAX_ARRAY_SIZE];
			int str_lens[MAX_ARRAY_SIZE];
			int count = 0;
			int total_len = 0;
			char *buf, *p;
			int i;

			/* Collect all strings, temporarily nul-terminating */
			while (ctx->tok == JSONT_STRING) {
				if (count >= MAX_ARRAY_SIZE)
					return -E2BIG;

				strings[count] = (char *)ctx->tok_start;
				saved_chars[count] = *ctx->tok_end;
				*(char *)ctx->tok_end = '\0';
				str_lens[count] = strlen(strings[count]) + 1;
				total_len += str_lens[count];
				count++;

				next_token(ctx);

				if (ctx->tok == JSONT_COMMA)
					next_token(ctx);
				else if (ctx->tok != JSONT_RBRACKET)
					return -EINVAL;
			}

			/* Build stringlist: concatenate all strings with nulls */
			buf = malloc(total_len);
			if (!buf)
				return -ENOMEM;

			p = buf;
			for (i = 0; i < count; i++) {
				memcpy(p, strings[i], str_lens[i]);
				p += str_lens[i];
			}

			/* Create FDT stringlist property */
			ret = fdt_property(ctx->fdt, prop, buf, total_len);
			free(buf);

			/* Restore all the saved characters */
			for (i = 0; i < count; i++)
				strings[i][str_lens[i] - 1] = saved_chars[i];

			if (ret)
				return ret;

			next_token(ctx);
			return 0;
		} else {
			return -EINVAL;
		}

		if (ctx->tok == JSONT_COMMA)
			next_token(ctx);
		else if (ctx->tok != JSONT_RBRACKET)
			return -EINVAL;
	}

	next_token(ctx);  /* Skip ] */

	/* Write array as FDT property */
	if (count == 1) {
		/* Single element - use fdt_property_u32() */
		ret = fdt_property_u32(ctx->fdt, prop, values[0]);
		if (ret)
			return ret;
	} else if (count > 1) {
		/* Multiple elements - convert to big-endian and write */
		u32 cells[MAX_ARRAY_SIZE];
		int i;

		for (i = 0; i < count; i++)
			cells[i] = cpu_to_fdt32(values[i]);

		ret = fdt_property(ctx->fdt, prop, cells, count * sizeof(u32));
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * parse_value() - Parse a JSON value
 *
 * @ctx: Parser context
 * @prop_name: Property name for this value
 * Return: 0 on success, negative error code on failure
 */
static int parse_value(struct json_parser *ctx, const char *prop_name)
{
	int ret;

	switch (ctx->tok) {
	case JSONT_STRING: {
		char str[256];
		int len = min((int)(ctx->tok_end - ctx->tok_start),
			      (int)sizeof(str) - 1);

		memcpy(str, ctx->tok_start, len);
		str[len] = '\0';

		ret = fdt_property_string(ctx->fdt, prop_name, str);
		if (ret)
			return ret;

		next_token(ctx);
		break;
	}
	case JSONT_NUMBER: {
		char num_str[32];
		u64 val;
		int len = min((int)(ctx->tok_end - ctx->tok_start),
			      (int)sizeof(num_str) - 1);

		memcpy(num_str, ctx->tok_start, len);
		num_str[len] = '\0';

		val = simple_strtoull(num_str, NULL, 0);

		/* Use u32 if it fits, otherwise u64 */
		if (val <= 0xffffffff)
			ret = fdt_property_u32(ctx->fdt, prop_name, (u32)val);
		else
			ret = fdt_property_u64(ctx->fdt, prop_name, val);

		if (ret)
			return ret;

		next_token(ctx);
		break;
	}
	case JSONT_TRUE:
		ret = fdt_property_u32(ctx->fdt, prop_name, 1);
		if (ret)
			return ret;
		next_token(ctx);
		break;
	case JSONT_FALSE:
		ret = fdt_property_u32(ctx->fdt, prop_name, 0);
		if (ret)
			return ret;
		next_token(ctx);
		break;
	case JSONT_NULL:
		ret = fdt_property(ctx->fdt, prop_name, NULL, 0);
		if (ret)
			return ret;
		next_token(ctx);
		break;
	case JSONT_LBRACE:
		/* Nested object */
		ret = parse_object(ctx, prop_name);
		if (ret)
			return ret;
		break;
	case JSONT_LBRACKET:
		/* Array */
		ret = parse_array(ctx, prop_name);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * parse_object() - Parse a JSON object
 *
 * @ctx: Parser context
 * @node_name: Name for the device tree node (NULL for root)
 * Return: 0 on success, negative error code on failure
 */
static int parse_object(struct json_parser *ctx, const char *node_name)
{
	int ret;

	/* Expect { */
	if (ctx->tok != JSONT_LBRACE)
		return -EINVAL;

	/* Begin device tree node */
	if (node_name) {
		ret = fdt_begin_node(ctx->fdt, node_name);
		if (ret)
			return ret;
	}

	next_token(ctx);

	/* Handle empty object */
	if (ctx->tok == JSONT_RBRACE) {
		next_token(ctx);
		if (node_name) {
			ret = fdt_end_node(ctx->fdt);
			if (ret)
				return ret;
		}
		return 0;
	}

	/* Parse key-value pairs */
	while (ctx->tok != JSONT_RBRACE) {
		char key[128];
		int len;

		/* Expect string key */
		if (ctx->tok != JSONT_STRING)
			return -EINVAL;

		len = min((int)(ctx->tok_end - ctx->tok_start),
			  (int)sizeof(key) - 1);
		memcpy(key, ctx->tok_start, len);
		key[len] = '\0';

		next_token(ctx);

		/* Expect : */
		if (ctx->tok != JSONT_COLON)
			return -EINVAL;

		next_token(ctx);

		/* Parse value */
		ret = parse_value(ctx, key);
		if (ret)
			return ret;

		/* Expect , or } */
		if (ctx->tok == JSONT_COMMA)
			next_token(ctx);
		else if (ctx->tok != JSONT_RBRACE)
			return -EINVAL;
	}

	next_token(ctx);  /* Skip } */

	/* End device tree node */
	if (node_name) {
		ret = fdt_end_node(ctx->fdt);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * parse_json_root() - Parse JSON and create FDT
 *
 * @ctx: Parser context (must be initialized with FDT buffer)
 * Return: 0 on success, negative error code on failure
 */
static int parse_json_root(struct json_parser *ctx)
{
	int ret;

	/* Initialize FDT */
	ret = fdt_create(ctx->fdt, ctx->fdt_size);
	if (ret)
		return ret;

	ret = fdt_finish_reservemap(ctx->fdt);
	if (ret)
		return ret;

	/* Begin root node */
	ret = fdt_begin_node(ctx->fdt, "");
	if (ret)
		return ret;

	/* Get first token */
	next_token(ctx);

	/* Parse the JSON (expecting an object at the root) */
	if (ctx->tok == JSONT_LBRACE) {
		/* Parse the root's contents directly into the root node */
		next_token(ctx);

		while (ctx->tok != JSONT_RBRACE) {
			char key[128];
			int len;

			if (ctx->tok != JSONT_STRING)
				return -EINVAL;

			len = min((int)(ctx->tok_end - ctx->tok_start),
				  (int)sizeof(key) - 1);
			memcpy(key, ctx->tok_start, len);
			key[len] = '\0';

			next_token(ctx);

			if (ctx->tok != JSONT_COLON)
				return -EINVAL;

			next_token(ctx);

			ret = parse_value(ctx, key);
			if (ret)
				return ret;

			if (ctx->tok == JSONT_COMMA)
				next_token(ctx);
			else if (ctx->tok != JSONT_RBRACE)
				return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	/* End root node */
	ret = fdt_end_node(ctx->fdt);
	if (ret)
		return ret;

	/* Finalize FDT */
	ret = fdt_finish(ctx->fdt);
	if (ret)
		return ret;

	return 0;
}

int json_to_fdt(const char *json, struct abuf *buf)
{
	struct json_parser ctx;
	void *fdt_buf;
	int fdt_size;
	int ret;

	if (!json || !buf)
		return -EINVAL;

	/* Estimate FDT size: JSON length * 2 should be plenty */
	fdt_size = max((int)strlen(json) * 2, 4096);

	abuf_init(buf);
	if (!abuf_realloc(buf, fdt_size))
		return -ENOMEM;

	fdt_buf = abuf_data(buf);

	/* Init parser */
	memset(&ctx, 0, sizeof(ctx));
	ctx.json = json;
	ctx.pos = json;
	ctx.fdt = fdt_buf;
	ctx.fdt_size = fdt_size;

	/* Parse JSON and create FDT */
	ret = parse_json_root(&ctx);
	if (ret)
		goto err;

	/* Adjust abuf size to actual FDT size */
	abuf_realloc(buf, fdt_totalsize(fdt_buf));
	log_debug("json %ld buf %ld\n", strlen(json), buf->size);

	return 0;

err:
	abuf_uninit(buf);
	return ret;
}
