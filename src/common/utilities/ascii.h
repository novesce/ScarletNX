/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ASCII_H
#define SNX_ASCII_H

/* ASCII character classes and case for the formats the runtimes parse (keys, logs, ini files, language tags, paths),
   independent of the C library's locale, which <ctype.h> follows. Each takes any int, EOF and negative chars
   included, and answers for the ASCII range only. */

static inline int snx_is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int snx_is_upper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int snx_is_lower(int c) { return c >= 'a' && c <= 'z'; }
static inline int snx_is_alpha(int c) { return snx_is_upper(c) || snx_is_lower(c); }
static inline int snx_is_alnum(int c) { return snx_is_alpha(c) || snx_is_digit(c); }
static inline int snx_is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
/* a space or a tab, the blanks inside a line */
static inline int snx_is_blank(int c) { return c == ' ' || c == '\t'; }
static inline int snx_to_lower(int c) { return snx_is_upper(c) ? c - 'A' + 'a' : c; }
static inline int snx_to_upper(int c) { return snx_is_lower(c) ? c - 'a' + 'A' : c; }
/* a hexadecimal digit's value, -1 for any other character */
static inline int snx_hex_value(int c) {
    if (snx_is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static inline int snx_is_hex(int c) { return snx_hex_value(c) >= 0; }
/* a path separator of the host: '/', and '\\' on Windows */
static inline int snx_is_separator(int c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}
/* '/' or '\\' on any host: a guest's or an archive's path, or one shown whichever system wrote it */
static inline int snx_is_any_separator(int c) { return c == '/' || c == '\\'; }

#endif
