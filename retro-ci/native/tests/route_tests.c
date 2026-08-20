/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../route.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_path_matching(void) {
    assert(rp_request_path_is("GET /launch HTTP/1.1\r\n\r\n", "/launch"));
    assert(rp_request_path_is("GET /launch?x=1 HTTP/1.1\r\n\r\n", "/launch"));
    assert(!rp_request_path_is("GET /launching HTTP/1.1\r\n\r\n", "/launch"));
    assert(!rp_request_path_is("POST /launch HTTP/1.1\r\n\r\n", "/launch"));
}

static void test_valid_hbldr_request(void) {
    rp_launch_request_t request;
    const char *wire =
        "GET /hbldr?pipe=0&daemon=0&path=%2Fdata%2Fhomebrew%2FLakeSnes%2Flakesnes.elf"
        "&args=%2Fdata%2Fhomebrew%2FRetroPapa%2Froms%2Fsnes%2FSuper%20Mario%20World.sfc"
        "&cwd=%2Fdata%2Fhomebrew%2FRetroPapa%2Froms%2Fsnes&env=LANG%3Des_ES HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n\r\n";

    assert(rp_parse_hbldr_request(wire, &request) == 0);
    assert(strcmp(request.path, "/data/homebrew/LakeSnes/lakesnes.elf") == 0);
    assert(strcmp(request.cwd, "/data/homebrew/RetroPapa/roms/snes") == 0);
    assert(strcmp(request.args, "/data/homebrew/RetroPapa/roms/snes/Super Mario World.sfc") == 0);
    assert(strcmp(request.env, "LANG=es_ES") == 0);
}

static void test_rejections(void) {
    rp_launch_request_t request;

    assert(rp_parse_hbldr_request(
        "GET /hbldr?path=%2Fsystem%2Fcommon%2Flib%2Fbad.elf HTTP/1.1\r\n\r\n",
        &request) != 0);
    assert(rp_parse_hbldr_request(
        "GET /hbldr?path=%2Fdata%2Fhomebrew%2F..%2Fbad.elf HTTP/1.1\r\n\r\n",
        &request) != 0);
    assert(rp_parse_hbldr_request(
        "GET /hbldr?path=%2Fdata%2Fhomebrew%2Fx.elf&path=%2Fdata%2Fhomebrew%2Fy.elf HTTP/1.1\r\n\r\n",
        &request) != 0);
    assert(rp_parse_hbldr_request(
        "GET /hbldr?path=%2Fdata%2Fhomebrew%2Fx.elf&args=bad%00value HTTP/1.1\r\n\r\n",
        &request) != 0);
    assert(rp_parse_hbldr_request("GET /hbldr HTTP/1.1\r\n\r\n", &request) != 0);
}

int main(void) {
    test_path_matching();
    test_valid_hbldr_request();
    test_rejections();
    puts("native route tests passed");
    return 0;
}
