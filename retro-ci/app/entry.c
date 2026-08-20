#include <SDL.h>
#include <stdlib.h>

int sceSystemServiceHideSplashScreen(void);

static SDL_Window *rp_create_window(const char *title, int x, int y, int w, int h, Uint32 flags) {
    SDL_Window *win = SDL_CreateWindow(title, x, y, w, h, flags);
    if (win) sceSystemServiceHideSplashScreen();
    return win;
}

#define SDL_CreateWindow rp_create_window
#define main retro_papa_main
#include "main-native.c"
#undef main
#undef SDL_CreateWindow

int main(int argc, char **argv) {
    putenv("SDL_VIDEODRIVER=ps5");
    SDL_SetHint("SDL_VIDEODRIVER", "ps5");
    return retro_papa_main(argc, argv);
}
