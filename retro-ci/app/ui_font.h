#ifndef RETRO_PAPA_UI_FONT_H
#define RETRO_PAPA_UI_FONT_H

#include <SDL.h>
#include <stddef.h>

int rp_font_init(SDL_Renderer *renderer);
void rp_font_shutdown(void);
int rp_text_width(const char *text, int pixel_height, int bold);
int rp_text_height(int pixel_height, int bold);
void rp_text_ellipsize(
    const char *text,
    int pixel_height,
    int bold,
    int max_width,
    char *out,
    size_t out_size);
void rp_draw_text(
    const char *text,
    int x,
    int y,
    int pixel_height,
    int bold,
    SDL_Color color);
int rp_draw_text_wrapped(
    const char *text,
    int x,
    int y,
    int max_width,
    int pixel_height,
    int bold,
    int line_gap,
    int max_lines,
    SDL_Color color);

#endif
