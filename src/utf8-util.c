/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

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

#include <inttypes.h>
#include <stdlib.h>
#include <langinfo.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <iconv.h>
#include "macro.h"
#include "util.h"
#include "utf8-util.h"

char *utf8_find_prev_char(const char *str, const char *p) {
        for (--p; p >= str; --p) {
                if ((*p & 0xc0) != 0x80)
                        return (char *)p;
        }
        return NULL;
}

#define UNICODE_VALID(Char)                       \
        ((Char) < 0x110000 &&                     \
        (((Char) & 0xFFFFF800) != 0xD800) &&      \
        ((Char) < 0xFDD0 || (Char) > 0xFDEF) &&   \
        ((Char) & 0xFFFE) != 0xFFFE)

#define CONTINUATION_CHAR                                                \
        do {                                                             \
                if ((*(unsigned char *)p & 0xc0) != 0x80) /* 10xxxxxx */ \
                        goto error;                                      \
                val <<= 6;                                               \
                val |= (*(unsigned char *)p) & 0x3f;                     \
        } while(false)

static const char *fast_validate(const char *str) {
        uint32_t val = 0;
        uint32_t min = 0;
        const char *p;

        for (p = str; *p; p++) {
                if (*(unsigned char *) p < 128)
                        /* done */;
                else {
                        const char *last;

                        last = p;
                        if ((*(unsigned char *) p & 0xe0) == 0xc0) /* 110xxxxx */ {
                                if (_unlikely_((*(unsigned char *) p & 0x1e) == 0))
                                        goto error;
                                p++;
                        if (_unlikely_((*(unsigned char *) p & 0xc0) != 0x80)) /* 10xxxxxx */
                                        goto error;
                        } else {
                                if ((*(unsigned char *) p & 0xf0) == 0xe0) /* 1110xxxx */ {
                                        min = (1 << 11);
                                        val = *(unsigned char *) p & 0x0f;
                                        goto TWO_REMAINING;
                                } else if ((*(unsigned char *) p & 0xf8) == 0xf0) /* 11110xxx */ {
                                        min = (1 << 16);
                                        val = *(unsigned char *) p & 0x07;
                                } else
                                        goto error;

                                p++;
                                CONTINUATION_CHAR;
                        TWO_REMAINING:
                                p++;
                                CONTINUATION_CHAR;
                                p++;
                                CONTINUATION_CHAR;

                                if (_unlikely_(val < min))
                                        goto error;

                                if (_unlikely_(!UNICODE_VALID(val)))
                                        goto error;
                        }

                        continue;

                error:
                        return last;
                }
        }

        return p;
}

static const char *fast_validate_len(const char *str, ssize_t max_len) {
        uint32_t val = 0;
        uint32_t min = 0;
        const char *p;

        assert(max_len >= 0);

        for (p = str; ((p - str) < max_len) && *p; p++) {
                if (*(unsigned char *) p < 128)
                        /* done */;
                else {
                        const char *last;

                        last = p;
                        if ((*(unsigned char *) p & 0xe0) == 0xc0) /* 110xxxxx */ {
                                if (_unlikely_(max_len - (p - str) < 2))
                                        goto error;

                                if (_unlikely_((*(unsigned char *) p & 0x1e) == 0))
                                        goto error;
                                p++;
                                if (_unlikely_((*(unsigned char *) p & 0xc0) != 0x80)) /* 10xxxxxx */
                                goto error;
                        } else {
                                if ((*(unsigned char *) p & 0xf0) == 0xe0) /* 1110xxxx */ {
                                        if (_unlikely_(max_len - (p - str) < 3))
                                                goto error;

                                        min = (1 << 11);
                                        val = *(unsigned char *) p & 0x0f;
                                        goto TWO_REMAINING;
                                } else if ((*(unsigned char *) p & 0xf8) == 0xf0) /* 11110xxx */ {
                                        if (_unlikely_(max_len - (p - str) < 4))
                                                goto error;

                                        min = (1 << 16);
                                        val = *(unsigned char *) p & 0x07;
                                } else
                                        goto error;

                                p++;
                                CONTINUATION_CHAR;
                        TWO_REMAINING:
                                p++;
                                CONTINUATION_CHAR;
                                p++;
                                CONTINUATION_CHAR;

                                if (_unlikely_(val < min))
                                        goto error;
                                if (_unlikely_(!UNICODE_VALID(val)))
                                        goto error;
                        }

                        continue;

                error:
                        return last;
                }
        }

        return p;
}

bool utf8_validate (const char *str, ssize_t max_len, const char **end) {
        const char *p;

        if (max_len < 0)
                p = fast_validate (str);
        else
                p = fast_validate_len (str, max_len);

        if (end)
                *end = p;

        if ((max_len >= 0 && p != str + max_len) ||
             (max_len < 0 && *p != '\0'))
                return false;
        else
                return true;
}

#define NUL_TERMINATOR_LENGTH 4
int convert_with_iconv (char **converted_str,
                        const char *str, ssize_t len,
                        iconv_t converter,
                        size_t *bytes_read,
                        size_t *bytes_written) {
        int r = 0;
        char *dest;
        char *outp;
        const char *p;
        size_t inbytes_remaining;
        size_t outbytes_remaining;
        size_t outbuf_size;
        bool have_error = false;
        bool done = false;
        bool reset = false;

        assert_se(converter != (iconv_t) -1);
        assert_se(converted_str);

        if (len < 0)
                len = strlen(str);

        p = str;
        inbytes_remaining = len;
        outbuf_size = len + NUL_TERMINATOR_LENGTH;

        outbytes_remaining = outbuf_size - NUL_TERMINATOR_LENGTH;
        outp = dest = malloc(outbuf_size);

        while (!done && !have_error) {
                size_t err;
                if (reset)
                        err = iconv(converter, NULL, &inbytes_remaining, &outp, &outbytes_remaining);
                else
                        err = iconv(converter, (char **)&p, &inbytes_remaining, &outp, &outbytes_remaining);

                if (err == (size_t) -1) {
                        switch (errno) {
                                case EINVAL:
                                        /* Incomplete text, do not report an error */
                                        done = true;
                                        break;
                                case E2BIG: {
                                        size_t used = outp - dest;
                                        outbuf_size *= 2;
                                        dest = realloc(dest, outbuf_size);

                                        outp = dest + used;
                                        outbytes_remaining = \
                                          outbuf_size - used - NUL_TERMINATOR_LENGTH;
                                        break;
                                }
                                case EILSEQ:
                                        /* fall through */
                                default:
                                        r = -errno;
                                        have_error = true;
                                        break;
                        }
                } else {
                        if (!reset) {
                                /* call iconv with NULL inbuf to cleanup shift state */
                                reset = true;
                                inbytes_remaining = 0;
                        } else
                                done = true;
                }
        }

        memset(outp, 0, NUL_TERMINATOR_LENGTH);

        if (bytes_read)
                *bytes_read = p - str;
        else {
                if ((p - str) != len) {
                        if (!have_error) {
                                r = -EINVAL;
                                have_error = true;
                        }
                }
        }

        if (bytes_written)
                *bytes_written = outp - dest; /* Doesn't include '\0' */

        if (have_error) {
                free(dest);
                *converted_str = NULL;
        } else
                *converted_str = dest;

        return r;
}

bool get_charset(const char **charset) {
        static __thread char *_charset = NULL;
        static __thread bool is_utf8;

        if (!_charset) {
                _charset = nl_langinfo(CODESET);
                is_utf8 = streq(_charset, "UTF-8");
        }

        *charset = _charset;
        return is_utf8;
}

int locale_to_utf8(char **converted_str,
                   const char *opsysstring, ssize_t len,
                   size_t *bytes_read, size_t *bytes_written) {
        int r = 0;
        const char *charset;

        assert_se(converted_str);
        if (get_charset(&charset))
                if (!utf8_validate(opsysstring, len, NULL)) {
                        r = -EILSEQ;
                        *converted_str = NULL;
                } else {
                        if (len < 0)
                                len = strlen(opsysstring);
                        *converted_str = strndup(opsysstring, len);
                        if (bytes_read)
                                *bytes_read = len;
                        if (bytes_written)
                                *bytes_written = len;
                }
        else {
                iconv_t cd = iconv_open("UTF-8", charset);
                if (cd == (iconv_t) -1) {
                        r = convert_with_iconv(converted_str, opsysstring, len,
                                               cd, bytes_read, bytes_written);
                        iconv_close(cd);
                } else
                        r = -EINVAL;
        }

        return r;
}

char *utf8_merge_backspace_char(char *utf8_line) {
        char *not_before, *ptr, *new_start, *start, *next;

        start = new_start = NULL;
        not_before = utf8_line;
        ptr = strstr(utf8_line, "\b");
        while (ptr) {
                next = ptr + strlen("\b");

                if (new_start) {
                        char *end = utf8_find_prev_char(not_before, ptr);

                        if (!end) /* bad encoding utf8 */
                                break;

                        if (end > start) {
                                memmove(new_start, start, end - start);
                                new_start = end;
                                start = next;
                        } else if (end == start) {
                                start = next;
                        } else {
                                /* re-locate new_start, move back  */
                                if ((new_start = utf8_find_prev_char(not_before, new_start)))
                                        start = next;
                                else
                                        not_before = ptr;
                        }
                } else {
                        /* decide initial value for new_start */
                        if ((new_start = utf8_find_prev_char(not_before, ptr)))
                                start = next;
                        else
                                not_before = ptr;
                }

                ptr = strstr(next, "\b");
        }

        if (new_start)
                memmove(new_start, start, strlen(start) + 1);

        return utf8_line;
}
