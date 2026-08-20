#include "ui_font.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

extern const unsigned char rp_font_regular[];
extern const unsigned char rp_font_regular_end[];
extern const unsigned char rp_font_bold[];
extern const unsigned char rp_font_bold_end[];

#define TEXT_CACHE_CAP 384
#define TEXT_CACHE_TEXT 384

typedef struct {
    char text[TEXT_CACHE_TEXT];
    int pixel_height;
    int bold;
    Uint32 rgba;
    SDL_Texture *texture;
    int width;
    int height;
    uint32_t age;
} TextCacheEntry;

typedef struct {
    SDL_Renderer *renderer;
    stbtt_fontinfo regular;
    stbtt_fontinfo bold;
    int ready;
    TextCacheEntry cache[TEXT_CACHE_CAP];
    int cache_count;
    uint32_t age;
} FontState;

static FontState g_font;

static uint32_t pack_color(SDL_Color c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

static int utf8_next(const char *s, size_t len, size_t *offset, int *codepoint) {
    unsigned char c;
    size_t i;
    int cp;

    if (!s || !offset || !codepoint || *offset >= len) return 0;
    i = *offset;
    c = (unsigned char)s[i++];
    if (c < 0x80) {
        *codepoint = c;
        *offset = i;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && i < len) {
        cp = (c & 0x1F) << 6;
        cp |= (unsigned char)s[i++] & 0x3F;
    } else if ((c & 0xF0) == 0xE0 && i + 1 < len) {
        cp = (c & 0x0F) << 12;
        cp |= ((unsigned char)s[i++] & 0x3F) << 6;
        cp |= (unsigned char)s[i++] & 0x3F;
    } else if ((c & 0xF8) == 0xF0 && i + 2 < len) {
        cp = (c & 0x07) << 18;
        cp |= ((unsigned char)s[i++] & 0x3F) << 12;
        cp |= ((unsigned char)s[i++] & 0x3F) << 6;
        cp |= (unsigned char)s[i++] & 0x3F;
    } else {
        cp = '?';
    }
    *codepoint = cp;
    *offset = i;
    return 1;
}

static stbtt_fontinfo *font_for(int bold) {
    return bold ? &g_font.bold : &g_font.regular;
}

static float font_scale(int pixel_height, int bold) {
    if (pixel_height < 8) pixel_height = 8;
    return stbtt_ScaleForPixelHeight(font_for(bold), (float)pixel_height);
}

static int measure_internal(const char *text, int pixel_height, int bold) {
    stbtt_fontinfo *font;
    float scale;
    size_t len, offset = 0;
    int previous = 0;
    float width = 0.0f;

    if (!g_font.ready || !text || !*text) return 0;
    font = font_for(bold);
    scale = font_scale(pixel_height, bold);
    len = strlen(text);
    while (offset < len) {
        int cp, advance, bearing;
        if (!utf8_next(text, len, &offset, &cp)) break;
        if (previous) width += (float)stbtt_GetCodepointKernAdvance(font, previous, cp) * scale;
        stbtt_GetCodepointHMetrics(font, cp, &advance, &bearing);
        (void)bearing;
        width += (float)advance * scale;
        previous = cp;
    }
    return (int)ceilf(width);
}

int rp_text_height(int pixel_height, int bold) {
    stbtt_fontinfo *font;
    float scale;
    int ascent, descent, line_gap;

    if (!g_font.ready) return pixel_height;
    font = font_for(bold);
    scale = font_scale(pixel_height, bold);
    stbtt_GetFontVMetrics(font, &ascent, &descent, &line_gap);
    (void)line_gap;
    return (int)ceilf((float)(ascent - descent) * scale);
}

int rp_text_width(const char *text, int pixel_height, int bold) {
    return measure_internal(text, pixel_height, bold);
}

static SDL_Texture *render_text_texture(
    const char *text,
    int pixel_height,
    int bold,
    SDL_Color color,
    int *out_w,
    int *out_h) {
    stbtt_fontinfo *font;
    float scale;
    int ascent, descent, line_gap;
    int width, height, baseline;
    SDL_Surface *surface;
    Uint32 color_lut[256];
    size_t len, offset = 0;
    int previous = 0;
    float pen_x = 0.0f;

    if (!g_font.ready || !text || !*text) return NULL;
    font = font_for(bold);
    scale = font_scale(pixel_height, bold);
    width = measure_internal(text, pixel_height, bold);
    stbtt_GetFontVMetrics(font, &ascent, &descent, &line_gap);
    height = (int)ceilf((float)(ascent - descent) * scale);
    baseline = (int)ceilf((float)ascent * scale);
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    for (int a = 0; a < 256; ++a) {
        int alpha = (a * color.a + 127) / 255;
        color_lut[a] = SDL_MapRGBA(surface->format, color.r, color.g, color.b, (Uint8)alpha);
    }

    len = strlen(text);
    while (offset < len) {
        int cp, advance, bearing;
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        unsigned char *bitmap;
        int gx, gy;

        if (!utf8_next(text, len, &offset, &cp)) break;
        if (previous) pen_x += (float)stbtt_GetCodepointKernAdvance(font, previous, cp) * scale;
        bitmap = stbtt_GetCodepointBitmap(font, 0.0f, scale, cp, &gw, &gh, &xoff, &yoff);
        gx = (int)floorf(pen_x) + xoff;
        gy = baseline + yoff;
        if (bitmap) {
            for (int yy = 0; yy < gh; ++yy) {
                int dy = gy + yy;
                if (dy < 0 || dy >= height) continue;
                Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + dy * surface->pitch);
                for (int xx = 0; xx < gw; ++xx) {
                    int dx = gx + xx;
                    Uint8 coverage;
                    if (dx < 0 || dx >= width) continue;
                    coverage = bitmap[yy * gw + xx];
                    if (coverage) row[dx] = color_lut[coverage];
                }
            }
            stbtt_FreeBitmap(bitmap, NULL);
        }
        stbtt_GetCodepointHMetrics(font, cp, &advance, &bearing);
        (void)bearing;
        pen_x += (float)advance * scale;
        previous = cp;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(g_font.renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return NULL;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    return texture;
}

static TextCacheEntry *cache_lookup(const char *text, int px, int bold, SDL_Color color) {
    Uint32 rgba = pack_color(color);
    for (int i = 0; i < g_font.cache_count; ++i) {
        TextCacheEntry *e = &g_font.cache[i];
        if (e->pixel_height == px && e->bold == bold && e->rgba == rgba &&
            strcmp(e->text, text) == 0) {
            e->age = ++g_font.age;
            return e;
        }
    }
    return NULL;
}

static TextCacheEntry *cache_store(const char *text, int px, int bold, SDL_Color color) {
    TextCacheEntry *e;
    int index;

    if (g_font.cache_count < TEXT_CACHE_CAP) {
        index = g_font.cache_count++;
    } else {
        uint32_t oldest = UINT32_MAX;
        index = 0;
        for (int i = 0; i < TEXT_CACHE_CAP; ++i) {
            if (g_font.cache[i].age < oldest) {
                oldest = g_font.cache[i].age;
                index = i;
            }
        }
        if (g_font.cache[index].texture) SDL_DestroyTexture(g_font.cache[index].texture);
    }
    e = &g_font.cache[index];
    memset(e, 0, sizeof(*e));
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    e->pixel_height = px;
    e->bold = bold;
    e->rgba = pack_color(color);
    e->texture = render_text_texture(text, px, bold, color, &e->width, &e->height);
    e->age = ++g_font.age;
    return e->texture ? e : NULL;
}

int rp_font_init(SDL_Renderer *renderer) {
    int regular_offset, bold_offset;
    size_t regular_size = (size_t)(rp_font_regular_end - rp_font_regular);
    size_t bold_size = (size_t)(rp_font_bold_end - rp_font_bold);

    memset(&g_font, 0, sizeof(g_font));
    if (!renderer || regular_size < 1024 || bold_size < 1024) return -1;
    regular_offset = stbtt_GetFontOffsetForIndex(rp_font_regular, 0);
    bold_offset = stbtt_GetFontOffsetForIndex(rp_font_bold, 0);
    if (regular_offset < 0 || bold_offset < 0) return -1;
    if (!stbtt_InitFont(&g_font.regular, rp_font_regular, regular_offset)) return -1;
    if (!stbtt_InitFont(&g_font.bold, rp_font_bold, bold_offset)) return -1;
    g_font.renderer = renderer;
    g_font.ready = 1;
    return 0;
}

void rp_font_shutdown(void) {
    for (int i = 0; i < g_font.cache_count; ++i) {
        if (g_font.cache[i].texture) SDL_DestroyTexture(g_font.cache[i].texture);
    }
    memset(&g_font, 0, sizeof(g_font));
}

void rp_draw_text(
    const char *text,
    int x,
    int y,
    int pixel_height,
    int bold,
    SDL_Color color) {
    TextCacheEntry *e;
    SDL_Rect dst;

    if (!text || !*text || !g_font.ready) return;
    e = cache_lookup(text, pixel_height, bold, color);
    if (!e) e = cache_store(text, pixel_height, bold, color);
    if (!e || !e->texture) return;
    dst.x = x;
    dst.y = y;
    dst.w = e->width;
    dst.h = e->height;
    SDL_RenderCopy(g_font.renderer, e->texture, NULL, &dst);
}

static size_t utf8_sequence_bytes(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && s[1]) return 2;
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) return 3;
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) return 4;
    return 1;
}

void rp_text_ellipsize(
    const char *text,
    int pixel_height,
    int bold,
    int max_width,
    char *out,
    size_t out_size) {
    size_t src = 0, dst = 0, len;
    char candidate[TEXT_CACHE_TEXT];

    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!text) return;
    if (rp_text_width(text, pixel_height, bold) <= max_width) {
        snprintf(out, out_size, "%s", text);
        return;
    }
    len = strlen(text);
    while (src < len && dst + 4 < out_size) {
        size_t step = utf8_sequence_bytes(text + src);
        if (src + step > len || dst + step + 4 >= out_size) break;
        memcpy(out + dst, text + src, step);
        dst += step;
        out[dst] = 0;
        snprintf(candidate, sizeof(candidate), "%s...", out);
        if (rp_text_width(candidate, pixel_height, bold) > max_width) {
            dst -= step;
            out[dst] = 0;
            break;
        }
        src += step;
    }
    if (out_size - dst > 3) strcat(out, "...");
}

int rp_draw_text_wrapped(
    const char *text,
    int x,
    int y,
    int max_width,
    int pixel_height,
    int bold,
    int line_gap,
    int max_lines,
    SDL_Color color) {
    char work[1400];
    char line[512] = {0};
    char candidate[512];
    char *save = NULL;
    char *word;
    int lines = 0;
    int line_height = rp_text_height(pixel_height, bold);

    if (!text || !*text || max_lines <= 0) return 0;
    snprintf(work, sizeof(work), "%s", text);
    for (char *p = work; *p; ++p) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';

    word = strtok_r(work, " ", &save);
    while (word) {
        if (line[0]) snprintf(candidate, sizeof(candidate), "%s %s", line, word);
        else snprintf(candidate, sizeof(candidate), "%s", word);

        if (line[0] && rp_text_width(candidate, pixel_height, bold) > max_width) {
            rp_draw_text(line, x, y + lines * (line_height + line_gap), pixel_height, bold, color);
            lines++;
            if (lines >= max_lines) return lines;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", candidate);
        }
        word = strtok_r(NULL, " ", &save);
    }
    if (line[0] && lines < max_lines) {
        char fitted[512];
        rp_text_ellipsize(line, pixel_height, bold, max_width, fitted, sizeof(fitted));
        rp_draw_text(fitted, x, y + lines * (line_height + line_gap), pixel_height, bold, color);
        lines++;
    }
    return lines;
}
