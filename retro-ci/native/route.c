/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "route.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define RP_MAX_TARGET 4096

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int copy_request_target(const char *request, char *target, size_t cap) {
    const char *begin;
    const char *end;
    size_t length;

    if (!request || !target || cap == 0 || strncmp(request, "GET ", 4) != 0) {
        return -1;
    }
    begin = request + 4;
    end = strchr(begin, ' ');
    if (!end || end == begin) {
        return -1;
    }
    length = (size_t)(end - begin);
    if (length >= cap) {
        return -1;
    }
    memcpy(target, begin, length);
    target[length] = '\0';
    return 0;
}

static int decode_component(const char *src, size_t length, char *dst, size_t cap) {
    size_t in = 0;
    size_t out = 0;

    if (!src || !dst || cap == 0) {
        return -1;
    }
    while (in < length) {
        unsigned char c = (unsigned char)src[in++];
        if (c == '%') {
            int hi;
            int lo;
            if (in + 1 >= length) {
                return -1;
            }
            hi = hex_value((unsigned char)src[in]);
            lo = hex_value((unsigned char)src[in + 1]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            c = (unsigned char)((hi << 4) | lo);
            in += 2;
        } else if (c == '+') {
            c = ' ';
        }

        if (c == 0 || c == '\r' || c == '\n' || c == 0x7f || c < 0x20) {
            return -1;
        }
        if (out + 1 >= cap) {
            return -1;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return 0;
}

static int path_is_safe(const char *path, int allow_root) {
    if (!path || path[0] != '/') {
        return 0;
    }
    if (strstr(path, "/../") || strstr(path, "/./") ||
        (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0) ||
        (strlen(path) >= 2 && strcmp(path + strlen(path) - 2, "/.") == 0)) {
        return 0;
    }
    if (allow_root && strcmp(path, "/") == 0) {
        return 1;
    }
    return strncmp(path, "/data/homebrew/", 15) == 0;
}

int rp_request_path_is(const char *request, const char *expected_path) {
    char target[RP_MAX_TARGET];
    char *query;

    if (!expected_path || copy_request_target(request, target, sizeof(target)) != 0) {
        return 0;
    }
    query = strchr(target, '?');
    if (query) {
        *query = '\0';
    }
    return strcmp(target, expected_path) == 0;
}

int rp_parse_hbldr_request(const char *request, rp_launch_request_t *out) {
    char target[RP_MAX_TARGET];
    char *query;
    char *cursor;
    int have_path = 0;
    int have_cwd = 0;
    int have_args = 0;
    int have_env = 0;

    if (!out || copy_request_target(request, target, sizeof(target)) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->cwd, sizeof(out->cwd), "/");

    query = strchr(target, '?');
    if (!query) {
        return -1;
    }
    *query++ = '\0';
    if (strcmp(target, "/hbldr") != 0 || !*query) {
        return -1;
    }

    cursor = query;
    while (*cursor) {
        char *separator = strchr(cursor, '&');
        char *equals;
        size_t token_length = separator ? (size_t)(separator - cursor) : strlen(cursor);
        size_t key_length;
        const char *value;
        size_t value_length;

        if (token_length == 0) {
            return -1;
        }
        equals = memchr(cursor, '=', token_length);
        if (!equals) {
            return -1;
        }
        key_length = (size_t)(equals - cursor);
        value = equals + 1;
        value_length = token_length - key_length - 1;

        if (key_length == 4 && memcmp(cursor, "path", 4) == 0) {
            if (have_path || decode_component(value, value_length, out->path, sizeof(out->path)) != 0) {
                return -1;
            }
            have_path = 1;
        } else if (key_length == 3 && memcmp(cursor, "cwd", 3) == 0) {
            if (have_cwd || decode_component(value, value_length, out->cwd, sizeof(out->cwd)) != 0) {
                return -1;
            }
            have_cwd = 1;
        } else if (key_length == 4 && memcmp(cursor, "args", 4) == 0) {
            if (have_args || decode_component(value, value_length, out->args, sizeof(out->args)) != 0) {
                return -1;
            }
            have_args = 1;
        } else if (key_length == 3 && memcmp(cursor, "env", 3) == 0) {
            if (have_env || decode_component(value, value_length, out->env, sizeof(out->env)) != 0) {
                return -1;
            }
            have_env = 1;
        }

        if (!separator) {
            break;
        }
        cursor = separator + 1;
    }

    if (!have_path || !out->path[0] || !path_is_safe(out->path, 0) ||
        !path_is_safe(out->cwd, 1)) {
        return -1;
    }
    return 0;
}
