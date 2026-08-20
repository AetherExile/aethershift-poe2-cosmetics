/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

typedef struct rp_launch_request {
    char path[512];
    char cwd[512];
    char args[1536];
    char env[768];
} rp_launch_request_t;

int rp_request_path_is(const char *request, const char *expected_path);
int rp_parse_hbldr_request(const char *request, rp_launch_request_t *out);
