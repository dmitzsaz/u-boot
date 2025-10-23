/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * JSON utilities
 *
 * Copyright (C) 2025 Canonical Ltd
 * Written by Simon Glass <simon.glass@canonical.com>
 */

#ifndef __JSON_H__
#define __JSON_H__

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

#endif /* __JSON_H__ */
