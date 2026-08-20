#!/usr/bin/env python3
"""Generate the resident-launcher UI translation unit from app/main.c."""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_exact(text: str, old: str, new: str, count: int, label: str) -> str:
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"{label}: expected {count} occurrence(s), found {actual}")
    return text.replace(old, new)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")
    text = replace_exact(
        text,
        " * Runtime game launching is delegated silently to the already-running\n"
        " * ps5-payload-websrv daemon on 127.0.0.1:8080.\n"
        " *\n"
        " * The native build transform rewrites that loopback handoff to Retro Papa's\n"
        " * resident launcher on 127.0.0.1:9041.",
        " * Runtime game launching is delegated directly to Retro Papa's resident,\n"
        " * loopback-only native launcher on 127.0.0.1:9041. The resident payload\n"
        " * owns the Sony BigApp transition and starts emulator ELFs without WebKit.",
        1,
        "architecture comment",
    )
    text = replace_exact(
        text,
        "static int websrv_launch(",
        "static int native_launcher_launch(",
        1,
        "launch function declaration",
    )
    text = replace_exact(
        text,
        '"Host: 127.0.0.1:8080\\r\\nConnection: close\\r\\n\\r\\n",',
        '"Host: 127.0.0.1:9041\\r\\nConnection: close\\r\\n\\r\\n",',
        1,
        "HTTP Host header",
    )
    text = replace_exact(
        text,
        "a.sin_port = htons(8080);",
        "a.sin_port = htons(9041);",
        1,
        "loopback port",
    )
    text = replace_exact(
        text,
        "/* Do not wait indefinitely: websrv may kill this big app while replacing it. */",
        "/* The resident launcher may replace this BigApp before a response arrives. */",
        1,
        "handoff comment",
    )
    text = replace_exact(
        text,
        'if (websrv_launch(e, rom)) set_status("WEBSRV NO ESTA ACTIVO");',
        'if (native_launcher_launch(e, rom)) set_status("LANZADOR NATIVO NO ESTA ACTIVO");',
        1,
        "launch call and error",
    )

    # The official websrv launch extensions for Mednafen, LakeSnes and FBNeo do
    # not send a cwd at all; sys_launch_homebrew() therefore uses '/'. Keep the
    # native resident handoff byte-for-byte equivalent instead of making the ROM
    # directory the process cwd. ROM arguments and emulator homes are absolute.
    text = replace_exact(
        text,
        "    url_encode(e->romdir, cwd_enc, sizeof(cwd_enc));",
        "    url_encode(\"/\", cwd_enc, sizeof(cwd_enc));",
        1,
        "upstream-compatible homebrew cwd",
    )

    # The runtime config is pipe-delimited and legitimately contains empty
    # columns (for example LakeSnes has no environment key/value). strtok_r()
    # collapses repeated delimiters, so preserve empty columns explicitly.
    text = replace_exact(
        text,
        '''static char *field(char **save) {
    return strtok_r(NULL, "|", save);
}
''',
        '''static char *pipe_field(char **cursor) {
    char *start;
    char *separator;
    if (!cursor || !*cursor) return NULL;
    start = *cursor;
    separator = strchr(start, '|');
    if (separator) {
        *separator = 0;
        *cursor = separator + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}
''',
        1,
        "delimiter-preserving field parser",
    )
    text = replace_exact(
        text,
        " field(&save)",
        " pipe_field(&save)",
        15,
        "pipe parser subsequent fields",
    )
    text = replace_exact(
        text,
        '        char *save = NULL;\n        char *p = strtok_r(line, "|", &save);',
        '        char *save = line;\n        char *p = pipe_field(&save);',
        2,
        "pipe parser initial field",
    )
    text = replace_exact(
        text,
        '#define CPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src) ? (src) : "")',
        '#define CPY(dst, src) do { const char *rp_value = (src); snprintf((dst), sizeof(dst), "%s", rp_value ? rp_value : ""); } while (0)',
        1,
        "single-evaluation emulator field copy",
    )

    # A curated catalogue entry is presentation metadata, not authority that a
    # game exists. Keep it only if it resolves to a real ROM on this console.
    text = replace_exact(
        text,
        "static int known_file_for_system(",
        '''static void prune_missing_catalog_games(void) {
    int write_index = 0;
    for (int i = 0; i < game_count; ++i) {
        Emulator *e = find_emu(games[i].system);
        char resolved[192];
        if (!e || resolve_rom(&games[i], e, resolved, sizeof(resolved)) != 0) continue;
        if (write_index != i) games[write_index] = games[i];
        write_index++;
    }
    game_count = write_index;
}

static int known_file_for_system(''',
        1,
        "real-ROM catalogue authority",
    )
    text = replace_exact(
        text,
        '    if (load_catalog() != 0) fprintf(stderr, "Retro Papa: no catalog\\n");\n    add_unlisted_roms();\n',
        '    if (load_catalog() != 0) fprintf(stderr, "Retro Papa: no catalog\\n");\n'
        '    prune_missing_catalog_games();\n'
        '    add_unlisted_roms();\n',
        1,
        "real-ROM catalogue startup",
    )

    # The home screen is a view of the actual library, not of every possible
    # collection. Favorites, puzzle groups and systems with zero installed ROMs
    # therefore disappear automatically.
    text = replace_exact(
        text,
        '''static int category_count(void) {
    return 2 + emu_count;
}

static const char *category_label(int cat) {
    if (cat == 0) return "Favoritos";
    if (cat == 1) return "Tetris y puzles";
    int i = cat - 2;
    return (i >= 0 && i < emu_count) ? emus[i].label : "Juegos";
}

static const char *category_system(int cat) {
    int i = cat - 2;
    return (i >= 0 && i < emu_count) ? emus[i].system : NULL;
}

static int game_in_category(const Game *g, int cat) {
    if (cat == 0) return g->favorite;
    if (cat == 1) return g->tetris;
    int i = cat - 2;
    return i >= 0 && i < emu_count && !strcmp(g->system, emus[i].system);
}

static int filtered_indices(int cat, int *out, int maxn) {
    int n = 0;
    for (int i = 0; i < game_count && n < maxn; ++i) {
        if (game_in_category(&games[i], cat)) out[n++] = i;
    }
    return n;
}
''',
        '''static int raw_category_count(void) {
    return 2 + emu_count;
}

static int raw_game_in_category(const Game *g, int raw_cat) {
    if (raw_cat == 0) return g->favorite;
    if (raw_cat == 1) return g->tetris;
    int i = raw_cat - 2;
    return i >= 0 && i < emu_count && !strcmp(g->system, emus[i].system);
}

static int raw_category_game_count(int raw_cat) {
    int count = 0;
    for (int i = 0; i < game_count; ++i) {
        if (raw_game_in_category(&games[i], raw_cat)) count++;
    }
    return count;
}

static int category_raw_from_visible(int visible_cat) {
    int visible = 0;
    for (int raw = 0; raw < raw_category_count(); ++raw) {
        if (raw_category_game_count(raw) <= 0) continue;
        if (visible == visible_cat) return raw;
        visible++;
    }
    return -1;
}

static int category_count(void) {
    int count = 0;
    for (int raw = 0; raw < raw_category_count(); ++raw) {
        if (raw_category_game_count(raw) > 0) count++;
    }
    return count;
}

static const char *category_label(int cat) {
    int raw = category_raw_from_visible(cat);
    if (raw == 0) return "Favoritos";
    if (raw == 1) return "Tetris y puzles";
    int i = raw - 2;
    return (i >= 0 && i < emu_count) ? emus[i].label : "Juegos";
}

static const char *category_system(int cat) {
    int raw = category_raw_from_visible(cat);
    int i = raw - 2;
    return (i >= 0 && i < emu_count) ? emus[i].system : NULL;
}

static int game_in_category(const Game *g, int cat) {
    int raw = category_raw_from_visible(cat);
    return raw >= 0 && raw_game_in_category(g, raw);
}

static int filtered_indices(int cat, int *out, int maxn) {
    int n = 0;
    for (int i = 0; i < game_count && n < maxn; ++i) {
        if (game_in_category(&games[i], cat)) out[n++] = i;
    }
    return n;
}
''',
        1,
        "hide empty collections",
    )
    text = replace_exact(
        text,
        '''static SDL_Color category_accent(int cat) {
    if (cat == 0) return C_ACCENT;
    if (cat == 1) return (SDL_Color){92, 210, 184, 255};
    return system_accent(category_system(cat));
}

static const char *category_mark(int cat) {
    if (cat == 0) return "FAV";
    if (cat == 1) return "PUZZLE";
    return system_mark(category_system(cat));
}
''',
        '''static SDL_Color category_accent(int cat) {
    int raw = category_raw_from_visible(cat);
    if (raw == 0) return C_ACCENT;
    if (raw == 1) return (SDL_Color){92, 210, 184, 255};
    return system_accent(category_system(cat));
}

static const char *category_mark(int cat) {
    int raw = category_raw_from_visible(cat);
    if (raw == 0) return "FAV";
    if (raw == 1) return "PUZZLE";
    return system_mark(category_system(cat));
}
''',
        1,
        "visible collection styling",
    )
    text = replace_exact(
        text,
        "Elige una colección. Solo aparecen tus juegos y los sistemas configurados.",
        "Elige una colección. Solo aparecen sistemas que tienen juegos disponibles.",
        1,
        "library home copy",
    )
    text = replace_exact(
        text,
        "    const int cols = 3, cw = 540, ch = 278, gapx = 42, gapy = 32, ox = 88, oy = 272;",
        "    const int cw = 540, ch = 278, gapx = 42, gapy = 32, oy = 272;\n"
        "    int cols = (n > 0 && n < 3) ? n : 3;\n"
        "    int grid_w = cols * cw + (cols - 1) * gapx;\n"
        "    int ox = (SCREEN_W - grid_w) / 2;",
        1,
        "center sparse collection grid",
    )

    for forbidden in ("ps5-payload-websrv", "htons(8080)", "WEBSRV NO ESTA ACTIVO"):
        if forbidden in text:
            raise RuntimeError(f"websrv launch dependency remains in generated UI source: {forbidden}")
    for required in (
        "prune_missing_catalog_games();",
        "rp_font_init(renderer)",
        ".nes",
        ".fds",
        "pipe_field(&save)",
        "category_raw_from_visible",
        "grid_w = cols * cw",
        'url_encode("/", cwd_enc',
    ):
        if required not in text:
            raise RuntimeError(f"native UI feature missing: {required}")

    args.destination.write_text(
        "/* Generated by scripts/prepare-native-ui.py. Do not edit directly. */\n" + text,
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
