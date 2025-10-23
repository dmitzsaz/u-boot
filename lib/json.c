// SPDX-License-Identifier: GPL-2.0+
/*
 * JSON pretty-printer
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#include <ctype.h>
#include <log.h>

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
