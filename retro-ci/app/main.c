/*
 * Retro Papa native PS5 frontend.
 *
 * Native SDL2 library UI; no browser is shown to the user.
 * Runtime game launching is delegated silently to the already-running
 * ps5-payload-websrv daemon on 127.0.0.1:8080.
 *
 * The native build transform rewrites that loopback handoff to Retro Papa's
 * resident launcher on 127.0.0.1:9041.
 */

#include <SDL.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui_font.h"

#define RP_ROOT "/data/homebrew/RetroPapa"
#define CATALOG_PATH RP_ROOT "/catalog-native.tsv"
#define EMULATORS_PATH RP_ROOT "/emulators.cfg"

#define SCREEN_W 1920
#define SCREEN_H 1080
#define MAX_GAMES 256
#define MAX_EMUS 12
#define MAX_ALIASES 18
#define PAGE_SIZE 6
#define GAME_COLS 3

typedef struct {
    char system[32];
    char label[64];
    char elf[256];
    char romdir[256];
    char mode[24];
    char envkey[64];
    char envvalue[256];
    char exts[128];
} Emulator;

typedef struct {
    char title[128];
    char system[32];
    char rom[192];
    char cover[256];
    char description[640];
    char aliases[MAX_ALIASES][192];
    int alias_count;
    int year;
    int favorite;
    int tetris;
    int dynamic;
    SDL_Texture *texture;
} Game;

typedef enum {
    VIEW_CATEGORIES = 0,
    VIEW_GAMES = 1
} ViewMode;

static Game games[MAX_GAMES];
static int game_count = 0;
static Emulator emus[MAX_EMUS];
static int emu_count = 0;

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_GameController *controller = NULL;

static ViewMode view_mode = VIEW_CATEGORIES;
static int selected = 0;
static int active_category = 0;
static char status_text[192] = "Listo";

static const SDL_Color C_BG_TOP = {11, 16, 27, 255};
static const SDL_Color C_BG_BOTTOM = {5, 8, 15, 255};
static const SDL_Color C_PANEL = {20, 28, 42, 245};
static const SDL_Color C_PANEL_SOFT = {27, 37, 54, 245};
static const SDL_Color C_BORDER = {55, 70, 94, 255};
static const SDL_Color C_TEXT = {247, 249, 252, 255};
static const SDL_Color C_TEXT_SOFT = {190, 201, 217, 255};
static const SDL_Color C_MUTED = {137, 151, 173, 255};
static const SDL_Color C_ACCENT = {246, 187, 65, 255};
static const SDL_Color C_ACCENT_BLUE = {92, 184, 255, 255};
static const SDL_Color C_SHADOW = {0, 0, 0, 95};

static void set_status(const char *s) {
    snprintf(status_text, sizeof(status_text), "%s", s ? s : "");
}

static SDL_Color mix_color(SDL_Color a, SDL_Color b, float t) {
    SDL_Color c;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    c.r = (Uint8)((float)a.r + ((float)b.r - a.r) * t);
    c.g = (Uint8)((float)a.g + ((float)b.g - a.g) * t);
    c.b = (Uint8)((float)a.b + ((float)b.b - a.b) * t);
    c.a = (Uint8)((float)a.a + ((float)b.a - a.a) * t);
    return c;
}

static void fill_rect(int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x, y, w, h};
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(renderer, &r);
}

static void fill_round_rect(int x, int y, int w, int h, int radius, SDL_Color c) {
    if (radius <= 0 || w <= radius * 2 || h <= radius * 2) {
        fill_rect(x, y, w, h, c);
        return;
    }
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_Rect middle = {x, y + radius, w, h - radius * 2};
    SDL_Rect center = {x + radius, y, w - radius * 2, h};
    SDL_RenderFillRect(renderer, &middle);
    SDL_RenderFillRect(renderer, &center);
    for (int yy = 0; yy < radius; ++yy) {
        float dy = (float)(radius - yy) - 0.5f;
        int inset = radius - (int)sqrtf((float)(radius * radius) - dy * dy);
        SDL_Rect top = {x + inset, y + yy, w - inset * 2, 1};
        SDL_Rect bottom = {x + inset, y + h - 1 - yy, w - inset * 2, 1};
        SDL_RenderFillRect(renderer, &top);
        SDL_RenderFillRect(renderer, &bottom);
    }
}

static void draw_panel(int x, int y, int w, int h, int radius, SDL_Color fill, SDL_Color border, int border_px) {
    fill_round_rect(x, y, w, h, radius, border);
    fill_round_rect(
        x + border_px,
        y + border_px,
        w - border_px * 2,
        h - border_px * 2,
        radius > border_px ? radius - border_px : 0,
        fill);
}

static void draw_background(void) {
    const int bands = 36;
    int band_h = (SCREEN_H + bands - 1) / bands;
    for (int i = 0; i < bands; ++i) {
        float t = (float)i / (float)(bands - 1);
        SDL_Color c = mix_color(C_BG_TOP, C_BG_BOTTOM, t);
        fill_rect(0, i * band_h, SCREEN_W, band_h + 1, c);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    fill_round_rect(-180, 90, 760, 520, 220, (SDL_Color){31, 91, 155, 24});
    fill_round_rect(1420, 220, 720, 620, 240, (SDL_Color){173, 103, 38, 18});
}

static int file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void trim_eol(char *s) {
    if (!s) return;
    s[strcspn(s, "\r\n")] = 0;
}

static char *field(char **save) {
    return strtok_r(NULL, "|", save);
}

static int load_emulators(void) {
    FILE *fp = fopen(EMULATORS_PATH, "rb");
    char line[1400];
    emu_count = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && emu_count < MAX_EMUS) {
        trim_eol(line);
        if (!line[0] || line[0] == '#') continue;
        char *save = NULL;
        char *p = strtok_r(line, "|", &save);
        if (!p) continue;
        Emulator *e = &emus[emu_count];
        memset(e, 0, sizeof(*e));
#define CPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src) ? (src) : "")
        CPY(e->system, p);
        CPY(e->label, field(&save));
        CPY(e->elf, field(&save));
        CPY(e->romdir, field(&save));
        CPY(e->mode, field(&save));
        CPY(e->envkey, field(&save));
        CPY(e->envvalue, field(&save));
        CPY(e->exts, field(&save));
#undef CPY
        if (e->system[0] && e->elf[0] && e->romdir[0]) emu_count++;
    }
    fclose(fp);
    return emu_count ? 0 : -1;
}

static int load_catalog(void) {
    FILE *fp = fopen(CATALOG_PATH, "rb");
    char line[3200];
    game_count = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && game_count < MAX_GAMES) {
        trim_eol(line);
        if (!line[0] || line[0] == '#') continue;
        char *save = NULL;
        char *p = strtok_r(line, "|", &save);
        if (!p) continue;
        Game *g = &games[game_count];
        memset(g, 0, sizeof(*g));
        snprintf(g->system, sizeof(g->system), "%s", p);
        p = field(&save); g->favorite = p ? atoi(p) : 0;
        p = field(&save); g->tetris = p ? atoi(p) : 0;
        p = field(&save); snprintf(g->title, sizeof(g->title), "%s", p ? p : "Juego");
        p = field(&save); snprintf(g->cover, sizeof(g->cover), "%s", p ? p : "");
        p = field(&save); snprintf(g->rom, sizeof(g->rom), "%s", p ? p : "");
        p = field(&save);
        if (p && *p) {
            char *asave = NULL;
            for (char *a = strtok_r(p, ";", &asave);
                 a && g->alias_count < MAX_ALIASES;
                 a = strtok_r(NULL, ";", &asave)) {
                snprintf(g->aliases[g->alias_count++], sizeof(g->aliases[0]), "%s", a);
            }
        }
        p = field(&save); g->year = p ? atoi(p) : 0;
        p = field(&save); snprintf(g->description, sizeof(g->description), "%s", p ? p : "");
        game_count++;
    }
    fclose(fp);
    return game_count ? 0 : -1;
}

static Emulator *find_emu(const char *system) {
    for (int i = 0; i < emu_count; ++i) {
        if (!strcmp(emus[i].system, system)) return &emus[i];
    }
    return NULL;
}

static const char *system_label(const char *system) {
    Emulator *e = find_emu(system);
    return e && e->label[0] ? e->label : (system ? system : "Sistema");
}

static SDL_Color system_accent(const char *system) {
    if (!system) return C_ACCENT_BLUE;
    if (!strcmp(system, "nes")) return (SDL_Color){232, 82, 82, 255};
    if (!strcmp(system, "snes")) return (SDL_Color){154, 123, 238, 255};
    if (!strcmp(system, "megadrive")) return (SDL_Color){74, 145, 232, 255};
    if (!strcmp(system, "arcade")) return (SDL_Color){238, 142, 72, 255};
    return C_ACCENT_BLUE;
}

static const char *system_mark(const char *system) {
    if (!system) return "RP";
    if (!strcmp(system, "nes")) return "NES";
    if (!strcmp(system, "snes")) return "SNES";
    if (!strcmp(system, "megadrive")) return "MD";
    if (!strcmp(system, "arcade")) return "ARCADE";
    return "RP";
}

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

static int ext_allowed(const Emulator *e, const char *name) {
    if (!e || !e->exts[0]) return 1;
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", e->exts);
    char *save = NULL;
    for (char *p = strtok_r(copy, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        if (!strcasecmp(p, ext_of(name))) return 1;
    }
    return 0;
}

static void normalize_base(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    int meta = 0;
    for (size_t i = 0; src && src[i] && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '(' || c == '[') { meta++; continue; }
        if (c == ')' || c == ']') { if (meta) meta--; continue; }
        if (meta) continue;
        if (c == '.') {
            const char *tail = src + i;
            if (!strcasecmp(tail, ".zip") || !strcasecmp(tail, ".sfc") ||
                !strcasecmp(tail, ".smc") || !strcasecmp(tail, ".md") ||
                !strcasecmp(tail, ".gen") || !strcasecmp(tail, ".bin") ||
                !strcasecmp(tail, ".rom") || !strcasecmp(tail, ".nes") ||
                !strcasecmp(tail, ".fds")) break;
        }
        if (isalnum(c)) dst[o++] = (char)tolower(c);
        else if ((c == ' ' || c == '_' || c == '-') && o && dst[o - 1] != ' ') dst[o++] = ' ';
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static int region_score(const char *s) {
    char b[512];
    size_t n = 0;
    for (; s && *s && n + 1 < sizeof(b); ++s) b[n++] = (char)tolower((unsigned char)*s);
    b[n] = 0;
    int sc = 0;
    if (strstr(b, "spain") || strstr(b, "spanish") || strstr(b, "castellano")) sc += 1000;
    if (strstr(b, "europe") || strstr(b, " pal")) sc += 500;
    if (strstr(b, "world")) sc += 300;
    if (strstr(b, "usa")) sc += 150;
    if (strstr(b, "japan")) sc += 50;
    if (strstr(b, "[!]")) sc += 40;
    if (strstr(b, "beta") || strstr(b, "prototype") || strstr(b, "demo") ||
        strstr(b, "hack") || strstr(b, "bootleg") || strstr(b, "translation")) sc -= 250;
    return sc;
}

static int alias_matches(const Game *g, const char *filename) {
    char fb[256], ab[256];
    normalize_base(filename, fb, sizeof(fb));
    if (g->rom[0]) {
        if (!strcasecmp(g->rom, filename)) return 1;
        normalize_base(g->rom, ab, sizeof(ab));
        if (ab[0] && !strcmp(ab, fb)) return 1;
    }
    for (int i = 0; i < g->alias_count; ++i) {
        if (!strcasecmp(g->aliases[i], filename)) return 1;
        normalize_base(g->aliases[i], ab, sizeof(ab));
        if (ab[0] && !strcmp(ab, fb)) return 1;
    }
    return 0;
}

static int resolve_rom(const Game *g, const Emulator *e, char *out, size_t cap) {
    char path[512];
    if (g->rom[0]) {
        snprintf(path, sizeof(path), "%s/%s", e->romdir, g->rom);
        if (file_exists(path)) { snprintf(out, cap, "%s", g->rom); return 0; }
    }
    DIR *d = opendir(e->romdir);
    if (!d) return -1;
    int best = -100000;
    char bestname[192] = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !ext_allowed(e, de->d_name)) continue;
        if (alias_matches(g, de->d_name)) {
            int s = region_score(de->d_name);
            if (s > best) { best = s; snprintf(bestname, sizeof(bestname), "%s", de->d_name); }
        }
    }
    closedir(d);
    if (!bestname[0]) return -1;
    snprintf(out, cap, "%s", bestname);
    return 0;
}

static int known_file_for_system(const char *system, const char *filename) {
    for (int i = 0; i < game_count; ++i) {
        if (!strcmp(games[i].system, system) && alias_matches(&games[i], filename)) return 1;
    }
    return 0;
}

static void human_title(const char *filename, char *out, size_t cap) {
    char tmp[256];
    normalize_base(filename, tmp, sizeof(tmp));
    size_t o = 0;
    int upper = 1;
    for (size_t i = 0; tmp[i] && o + 1 < cap; ++i) {
        char c = tmp[i];
        if (c == ' ') { out[o++] = ' '; upper = 1; }
        else { out[o++] = upper ? (char)toupper((unsigned char)c) : c; upper = 0; }
    }
    out[o] = 0;
}

static void add_unlisted_roms(void) {
    for (int ei = 0; ei < emu_count && game_count < MAX_GAMES; ++ei) {
        Emulator *e = &emus[ei];
        DIR *d = opendir(e->romdir);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && game_count < MAX_GAMES) {
            if (de->d_name[0] == '.' || !ext_allowed(e, de->d_name)) continue;
            if (known_file_for_system(e->system, de->d_name)) continue;
            Game *g = &games[game_count++];
            memset(g, 0, sizeof(*g));
            snprintf(g->system, sizeof(g->system), "%s", e->system);
            snprintf(g->rom, sizeof(g->rom), "%s", de->d_name);
            human_title(de->d_name, g->title, sizeof(g->title));
            snprintf(
                g->description,
                sizeof(g->description),
                "Añadido automáticamente desde tu colección de %s.",
                system_label(e->system));
            g->dynamic = 1;
        }
        closedir(d);
    }
}

static void cover_bmp_path(const Game *g, char *out, size_t cap) {
    if (!g->cover[0]) { out[0] = 0; return; }
    char rel[256];
    snprintf(rel, sizeof(rel), "%s", g->cover);
    char *dot = strrchr(rel, '.');
    if (dot) snprintf(dot, (size_t)(&rel[sizeof(rel)] - dot), ".bmp");
    if (!strncmp(rel, "covers/", 7)) snprintf(out, cap, "%s/covers-native/%s", RP_ROOT, rel + 7);
    else snprintf(out, cap, "%s/covers-native/%s", RP_ROOT, rel);
}

static SDL_Texture *game_texture(Game *g) {
    if (g->texture) return g->texture;
    char p[512];
    cover_bmp_path(g, p, sizeof(p));
    if (!p[0] || !file_exists(p)) return NULL;
    SDL_Surface *s = SDL_LoadBMP(p);
    if (!s) return NULL;
    g->texture = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    return g->texture;
}

static int category_count(void) {
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

static SDL_Color category_accent(int cat) {
    if (cat == 0) return C_ACCENT;
    if (cat == 1) return (SDL_Color){92, 210, 184, 255};
    return system_accent(category_system(cat));
}

static const char *category_mark(int cat) {
    if (cat == 0) return "FAV";
    if (cat == 1) return "PUZZLE";
    return system_mark(category_system(cat));
}

static void draw_texture_contain(SDL_Texture *texture, int x, int y, int w, int h) {
    int tw = 0, th = 0;
    if (!texture || SDL_QueryTexture(texture, NULL, NULL, &tw, &th) != 0 || tw <= 0 || th <= 0) return;
    float sx = (float)w / (float)tw;
    float sy = (float)h / (float)th;
    float scale = sx < sy ? sx : sy;
    int dw = (int)((float)tw * scale);
    int dh = (int)((float)th * scale);
    SDL_Rect dst = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(renderer, texture, NULL, &dst);
}

static void game_initials(const Game *g, char out[4]) {
    int n = 0;
    int take = 1;
    memset(out, 0, 4);
    for (const char *p = g->title; p && *p && n < 3; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '-' || c == '_') { take = 1; continue; }
        if (c < 0x80 && take && isalnum(c)) {
            out[n++] = (char)toupper(c);
            take = 0;
        }
    }
    if (!out[0]) snprintf(out, 4, "RP");
}

static void draw_placeholder_cover(const Game *g, int x, int y, int w, int h) {
    SDL_Color accent = system_accent(g->system);
    SDL_Color dark = mix_color(accent, C_BG_BOTTOM, 0.72f);
    fill_round_rect(x, y, w, h, 16, dark);
    for (int i = 0; i < 7; ++i) {
        SDL_Color stripe = accent;
        stripe.a = (Uint8)(12 + i * 3);
        fill_rect(x + 18 + i * 42, y + 18, 18, h - 36, stripe);
    }
    char initials[4];
    game_initials(g, initials);
    int iw = rp_text_width(initials, 70, 1);
    rp_draw_text(initials, x + (w - iw) / 2, y + h / 2 - 52, 70, 1, C_TEXT);
    char mark[32];
    snprintf(mark, sizeof(mark), "%s", system_mark(g->system));
    int mw = rp_text_width(mark, 21, 1);
    rp_draw_text(mark, x + (w - mw) / 2, y + h - 48, 21, 1, C_TEXT_SOFT);
}

static void draw_cover(const Game *g, int x, int y, int w, int h) {
    fill_round_rect(x, y, w, h, 16, (SDL_Color){12, 17, 27, 255});
    SDL_Texture *t = game_texture((Game *)g);
    if (t) draw_texture_contain(t, x + 8, y + 8, w - 16, h - 16);
    else draw_placeholder_cover(g, x + 4, y + 4, w - 8, h - 8);
}

static void draw_button_hint(int x, int y, const char *button, const char *label, SDL_Color accent) {
    fill_round_rect(x, y, 34, 34, 17, accent);
    int bw = rp_text_width(button, 20, 1);
    rp_draw_text(button, x + (34 - bw) / 2, y + 6, 20, 1, (SDL_Color){13, 17, 25, 255});
    rp_draw_text(label, x + 46, y + 4, 22, 0, C_TEXT_SOFT);
}

static void draw_header(const char *section) {
    rp_draw_text("RETRO PAPA", 72, 42, 46, 1, C_TEXT);
    rp_draw_text(section, 74, 101, 23, 0, C_MUTED);
    char count[96];
    snprintf(count, sizeof(count), "%d juegos en la biblioteca", game_count);
    int cw = rp_text_width(count, 20, 0);
    rp_draw_text(count, 1840 - cw, 49, 20, 0, C_TEXT_SOFT);
    char fitted[160];
    rp_text_ellipsize(status_text, 18, 0, 470, fitted, sizeof(fitted));
    int sw = rp_text_width(fitted, 18, 0);
    rp_draw_text(fitted, 1840 - sw, 82, 18, 0, C_MUTED);
}

static void draw_footer(int games_view, int page, int pages) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    fill_rect(0, 1002, SCREEN_W, 78, (SDL_Color){6, 10, 17, 225});
    draw_button_hint(72, 1024, "X", games_view ? "Jugar" : "Entrar", C_ACCENT_BLUE);
    if (games_view) draw_button_hint(236, 1024, "O", "Volver", (SDL_Color){238, 112, 135, 255});
    rp_draw_text("Cruceta  Mover", games_view ? 415 : 236, 1028, 20, 0, C_MUTED);
    if (games_view && pages > 0) {
        char pg[64];
        snprintf(pg, sizeof(pg), "Página %d de %d", page + 1, pages);
        int pw = rp_text_width(pg, 19, 0);
        rp_draw_text(pg, 1845 - pw, 1029, 19, 0, C_MUTED);
    }
}

static void draw_categories(void) {
    draw_background();
    draw_header("Biblioteca retro");
    rp_draw_text("¿A qué jugamos?", 88, 156, 44, 1, C_TEXT);
    rp_draw_text("Elige una colección. Solo aparecen tus juegos y los sistemas configurados.", 90, 213, 21, 0, C_TEXT_SOFT);

    int n = category_count();
    const int cols = 3, cw = 540, ch = 278, gapx = 42, gapy = 32, ox = 88, oy = 272;
    for (int i = 0; i < n; ++i) {
        int row = i / cols, col = i % cols;
        int x = ox + col * (cw + gapx), y = oy + row * (ch + gapy);
        int idx[MAX_GAMES];
        int count = filtered_indices(i, idx, MAX_GAMES);
        SDL_Color accent = category_accent(i);
        SDL_Color fill = i == selected ? C_PANEL_SOFT : C_PANEL;
        SDL_Color border = i == selected ? accent : C_BORDER;

        if (i == selected) fill_round_rect(x + 10, y + 14, cw, ch, 24, C_SHADOW);
        draw_panel(x, y, cw, ch, 24, fill, border, i == selected ? 5 : 2);
        fill_round_rect(x + 26, y + 28, 8, ch - 56, 4, accent);

        rp_draw_text(category_mark(i), x + 58, y + 37, 20, 1, accent);
        char title[96];
        rp_text_ellipsize(category_label(i), 34, 1, 340, title, sizeof(title));
        rp_draw_text(title, x + 58, y + 92, 34, 1, C_TEXT);
        char count_text[64];
        snprintf(count_text, sizeof(count_text), "%d %s", count, count == 1 ? "juego" : "juegos");
        rp_draw_text(count_text, x + 60, y + 150, 22, 0, C_TEXT_SOFT);

        SDL_Color ghost = accent;
        ghost.a = i == selected ? 85 : 45;
        int mark_w = rp_text_width(category_mark(i), 58, 1);
        rp_draw_text(category_mark(i), x + cw - 36 - mark_w, y + ch - 94, 58, 1, ghost);
        if (i == selected) draw_button_hint(x + 60, y + ch - 62, "X", "Entrar", accent);
    }
    draw_footer(0, 0, 0);
}

static void draw_game_card(const Game *g, int x, int y, int w, int h, int is_selected) {
    SDL_Color accent = system_accent(g->system);
    SDL_Color fill = is_selected ? C_PANEL_SOFT : C_PANEL;
    SDL_Color border = is_selected ? accent : C_BORDER;
    if (is_selected) fill_round_rect(x + 8, y + 10, w, h, 22, C_SHADOW);
    draw_panel(x, y, w, h, 22, fill, border, is_selected ? 5 : 2);
    draw_cover(g, x + 16, y + 16, w - 32, 257);

    char fitted[160];
    rp_text_ellipsize(g->title, 25, 1, w - 38, fitted, sizeof(fitted));
    rp_draw_text(fitted, x + 19, y + 292, 25, 1, C_TEXT);
    char meta[96];
    if (g->year > 0) snprintf(meta, sizeof(meta), "%s  ·  %d", system_label(g->system), g->year);
    else snprintf(meta, sizeof(meta), "%s", system_label(g->system));
    rp_text_ellipsize(meta, 18, 0, w - 70, fitted, sizeof(fitted));
    rp_draw_text(fitted, x + 20, y + 333, 18, 0, C_MUTED);
    if (g->favorite) {
        fill_round_rect(x + w - 59, y + 326, 42, 25, 12, C_ACCENT);
        int fw = rp_text_width("FAV", 12, 1);
        rp_draw_text("FAV", x + w - 38 - fw / 2, y + 331, 12, 1, (SDL_Color){20, 24, 31, 255});
    }
}

static void draw_detail_panel(const Game *g, int x, int y, int w, int h) {
    SDL_Color accent = system_accent(g->system);
    draw_panel(x, y, w, h, 26, (SDL_Color){18, 25, 38, 247}, C_BORDER, 2);
    draw_cover(g, x + 22, y + 22, w - 44, 272);

    fill_round_rect(x + 24, y + 316, 140, 34, 17, mix_color(accent, C_BG_BOTTOM, 0.55f));
    char sys[48];
    rp_text_ellipsize(system_label(g->system), 17, 1, 112, sys, sizeof(sys));
    rp_draw_text(sys, x + 38, y + 323, 17, 1, C_TEXT);
    if (g->year > 0) {
        char year[16];
        snprintf(year, sizeof(year), "%d", g->year);
        rp_draw_text(year, x + 180, y + 323, 18, 0, C_TEXT_SOFT);
    }

    rp_draw_text_wrapped(g->title, x + 24, y + 374, w - 48, 34, 1, 5, 2, C_TEXT);
    const char *description = g->description[0]
        ? g->description
        : "Juego detectado en tu colección. Retro Papa lo ha añadido automáticamente a esta biblioteca.";
    rp_draw_text_wrapped(description, x + 24, y + 476, w - 48, 20, 0, 7, 5, C_TEXT_SOFT);

    rp_draw_text("Archivo", x + 24, y + 676, 16, 1, C_MUTED);
    char rom_fit[200];
    rp_text_ellipsize(g->rom[0] ? g->rom : "Se resolverá por nombre", 17, 0, w - 48, rom_fit, sizeof(rom_fit));
    rp_draw_text(rom_fit, x + 24, y + 701, 17, 0, C_TEXT_SOFT);

    fill_round_rect(x + 24, y + h - 76, w - 48, 52, 18, accent);
    int play_w = rp_text_width("X   JUGAR", 22, 1);
    rp_draw_text("X   JUGAR", x + (w - play_w) / 2, y + h - 66, 22, 1, (SDL_Color){13, 17, 25, 255});
}

static void draw_games(void) {
    draw_background();
    draw_header(category_label(active_category));
    int idx[MAX_GAMES], n = filtered_indices(active_category, idx, MAX_GAMES);
    if (n <= 0) {
        rp_draw_text("Esta colección está vacía", 90, 290, 46, 1, C_TEXT);
        rp_draw_text("Añade tus ROMs y Retro Papa las detectará automáticamente.", 92, 357, 23, 0, C_TEXT_SOFT);
        draw_footer(1, 0, 0);
        return;
    }
    if (selected < 0) selected = 0;
    if (selected >= n) selected = n - 1;
    int page = selected / PAGE_SIZE;
    int pages = (n + PAGE_SIZE - 1) / PAGE_SIZE;
    int start = page * PAGE_SIZE;

    const int cardw = 390, cardh = 385, gapx = 25, gapy = 24, ox = 70, oy = 170;
    for (int slot = 0; slot < PAGE_SIZE; ++slot) {
        int fi = start + slot;
        if (fi >= n) break;
        int row = slot / GAME_COLS, col = slot % GAME_COLS;
        int x = ox + col * (cardw + gapx), y = oy + row * (cardh + gapy);
        draw_game_card(&games[idx[fi]], x, y, cardw, cardh, fi == selected);
    }

    draw_detail_panel(&games[idx[selected]], 1335, 170, 515, 794);
    draw_footer(1, page, pages);
}

static void url_encode(const char *src, char *dst, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 4 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == ':') dst[o++] = (char)c;
        else {
            dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15];
        }
    }
    dst[o] = 0;
}

static int websrv_launch(const Emulator *e, const char *romfile) {
    char path_enc[768], args_raw[768], args_enc[1600], cwd_enc[768], env_raw[512], env_enc[1200];
    if (!strcmp(e->mode, "fbneo")) {
        snprintf(args_raw, sizeof(args_raw), "%s", romfile);
        char *dot = strrchr(args_raw, '.'); if (dot) *dot = 0;
    } else {
        snprintf(args_raw, sizeof(args_raw), "%s/%s", e->romdir, romfile);
    }
    env_raw[0] = 0;
    if (e->envkey[0]) snprintf(env_raw, sizeof(env_raw), "%s=%s", e->envkey, e->envvalue);
    url_encode(e->elf, path_enc, sizeof(path_enc));
    url_encode(args_raw, args_enc, sizeof(args_enc));
    url_encode(e->romdir, cwd_enc, sizeof(cwd_enc));
    url_encode(env_raw, env_enc, sizeof(env_enc));

    char request[4096];
    snprintf(request, sizeof(request),
        "GET /hbldr?pipe=0&daemon=0&path=%s&args=%s&cwd=%s&env=%s HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\nConnection: close\r\n\r\n",
        path_enc, args_enc, cwd_enc, env_enc);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    ssize_t sent = send(fd, request, strlen(request), 0);
    if (sent < 0) { close(fd); return -1; }
    shutdown(fd, SHUT_WR);
    /* Do not wait indefinitely: websrv may kill this big app while replacing it. */
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[128];
    (void)recv(fd, buf, sizeof(buf), 0);
    close(fd);
    return 0;
}

static void launch_selected(void) {
    int idx[MAX_GAMES], n = filtered_indices(active_category, idx, MAX_GAMES);
    if (selected < 0 || selected >= n) return;
    Game *g = &games[idx[selected]];
    Emulator *e = find_emu(g->system);
    if (!e) { set_status("Emulador no configurado"); return; }
    if (!file_exists(e->elf)) { set_status("Falta el emulador"); return; }
    char rom[192];
    if (resolve_rom(g, e, rom, sizeof(rom))) { set_status("No encuentro esa ROM"); return; }
    set_status("Abriendo juego...");
    draw_games();
    SDL_RenderPresent(renderer);
    SDL_Delay(120);
    if (websrv_launch(e, rom)) set_status("WEBSRV NO ESTA ACTIVO");
}

static void category_move(int dx, int dy) {
    int n = category_count();
    int cols = 3;
    int row = selected / cols, col = selected % cols;
    col += dx;
    row += dy;
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;
    int next = row * cols + col;
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    selected = next;
}

static void game_move(int dx, int dy) {
    int idx[MAX_GAMES], n = filtered_indices(active_category, idx, MAX_GAMES);
    if (!n) return;
    int next = selected + dx + dy * GAME_COLS;
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    selected = next;
}

static void handle_button(SDL_GameControllerButton b) {
    if (view_mode == VIEW_CATEGORIES) {
        if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) category_move(-1, 0);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) category_move(1, 0);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) category_move(0, -1);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) category_move(0, 1);
        else if (b == SDL_CONTROLLER_BUTTON_A) { active_category = selected; selected = 0; view_mode = VIEW_GAMES; }
    } else {
        if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) game_move(-1, 0);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) game_move(1, 0);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) game_move(0, -1);
        else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) game_move(0, 1);
        else if (b == SDL_CONTROLLER_BUTTON_A) launch_selected();
        else if (b == SDL_CONTROLLER_BUTTON_B) { view_mode = VIEW_CATEGORIES; selected = active_category; }
    }
}

static void cleanup(void) {
    for (int i = 0; i < game_count; ++i) if (games[i].texture) SDL_DestroyTexture(games[i].texture);
    rp_font_shutdown();
    if (controller) SDL_GameControllerClose(controller);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (load_emulators() != 0) fprintf(stderr, "Retro Papa: no emulator config\n");
    if (load_catalog() != 0) fprintf(stderr, "Retro Papa: no catalog\n");
    add_unlisted_roms();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return 2;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    window = SDL_CreateWindow(
        "Retro Papa",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W,
        SCREEN_H,
        SDL_WINDOW_FULLSCREEN);
    if (!window) { cleanup(); return 3; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { cleanup(); return 4; }
    SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (rp_font_init(renderer) != 0) { cleanup(); return 5; }
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) controller = SDL_GameControllerOpen(0);

    char ready[96];
    snprintf(ready, sizeof(ready), "%d juegos detectados", game_count);
    set_status(ready);

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_CONTROLLERBUTTONDOWN) handle_button((SDL_GameControllerButton)ev.cbutton.button);
            else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (view_mode == VIEW_CATEGORIES) {
                    if (k == SDLK_LEFT) category_move(-1, 0);
                    else if (k == SDLK_RIGHT) category_move(1, 0);
                    else if (k == SDLK_UP) category_move(0, -1);
                    else if (k == SDLK_DOWN) category_move(0, 1);
                    else if (k == SDLK_RETURN) { active_category = selected; selected = 0; view_mode = VIEW_GAMES; }
                } else {
                    if (k == SDLK_LEFT) game_move(-1, 0);
                    else if (k == SDLK_RIGHT) game_move(1, 0);
                    else if (k == SDLK_UP) game_move(0, -1);
                    else if (k == SDLK_DOWN) game_move(0, 1);
                    else if (k == SDLK_RETURN) launch_selected();
                    else if (k == SDLK_ESCAPE) { view_mode = VIEW_CATEGORIES; selected = active_category; }
                }
            }
        }
        if (view_mode == VIEW_CATEGORIES) draw_categories();
        else draw_games();
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    cleanup();
    return 0;
}
