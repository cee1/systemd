/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef fooutf8utilhfoo
#define fooutf8utilhfoo

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <iconv.h>
#include "macro.h"
#include "util.h"

char *utf8_find_prev_char(const char *str, const char *p);
bool utf8_validate (const char *str, ssize_t max_len, const char **end);
int convert_with_iconv (char **converted_str,
                        const char *str, ssize_t len,
                        iconv_t converter,
                        size_t *bytes_read,
                        size_t *bytes_written);
bool get_charset(const char **charset);
int locale_to_utf8(char **converted_str,
                   const char *opsysstring, ssize_t len,
                   size_t *bytes_read, size_t *bytes_written);

char *utf8_merge_backspace_char(char *utf8_line);

#endif
