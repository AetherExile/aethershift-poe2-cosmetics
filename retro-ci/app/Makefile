SDL_ROOT ?= /tmp/SDL
UI_CACHE ?= $(HOME)/.cache/retro-papa-wsl/ui-deps
STB_COMMIT := 2c980bb59875b0d32144a71867fbdebb2f77cd20
STB_BLOB := 90a5c2e2b3fe563c98585294ca7a949876aec94c
FONT_COMMIT := 3b1480ea4b6e15fed70a42f4cb29216476a044ed
FONT_REGULAR_BLOB := caaa68c02e6f84cd444d0fa1d59dabb067ca9d75
FONT_BOLD_BLOB := f60bdd1e970191f8e53e394eab79769dd62535e8
STB_ROOT := $(UI_CACHE)/stb-$(STB_COMMIT)
STB_HEADER := $(STB_ROOT)/stb_truetype.h
FONT_REGULAR := $(UI_CACHE)/Play-Regular-$(FONT_COMMIT).ttf
FONT_BOLD := $(UI_CACHE)/Play-Bold-$(FONT_COMMIT).ttf

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined)
endif

SDL_LIB := $(SDL_ROOT)/build-ps5/libSDL2.a
SDL_MAIN := $(SDL_ROOT)/build-ps5/libSDL2main.a
FONT_DATA := font-data.S

# stb_truetype is compiled as a private single-header implementation; unused
# API entry points are expected and should not drown the actual build output.
CFLAGS := -O2 -Wall -Wextra -Wno-unused-function -D__PS5__ -I$(SDL_ROOT)/include -I. -I$(STB_ROOT)
ICON_SOURCE := $(if $(wildcard icon-source.png),icon-source.png,icon.svg)

SDL_LDADD := $(SDL_MAIN) $(SDL_LIB)
SDL_LDADD += -lSceSystemService -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad
# The official ps5-payload SDK links libc + libSceLibcInternal automatically.
# It does not ship a standalone libm archive, so never pass -lm here.
SDL_LDADD += -lSceKeyboard -lSceImeDialog -lkernel_sys

all: retro-papa-ui.elf icon0.png

check-sdl:
	@test -f "$(SDL_LIB)" || (echo "Missing $(SDL_LIB). Build the PS5 SDL port first." && exit 1)

$(STB_HEADER):
	@mkdir -p "$(STB_ROOT)"
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	curl -fsSL --retry 3 "https://raw.githubusercontent.com/nothings/stb/$(STB_COMMIT)/stb_truetype.h" -o "$$tmp"; \
	actual="$$(git hash-object "$$tmp")"; \
	[ "$$actual" = "$(STB_BLOB)" ] || { echo "Unexpected stb_truetype blob $$actual"; rm -f "$$tmp"; exit 1; }; \
	mv "$$tmp" "$@"

$(FONT_REGULAR):
	@mkdir -p "$(UI_CACHE)"
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	curl -fsSL --retry 3 "https://raw.githubusercontent.com/google/fonts/$(FONT_COMMIT)/ofl/play/Play-Regular.ttf" -o "$$tmp"; \
	actual="$$(git hash-object "$$tmp")"; \
	[ "$$actual" = "$(FONT_REGULAR_BLOB)" ] || { echo "Unexpected Play Regular blob $$actual"; rm -f "$$tmp"; exit 1; }; \
	mv "$$tmp" "$@"

$(FONT_BOLD):
	@mkdir -p "$(UI_CACHE)"
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	curl -fsSL --retry 3 "https://raw.githubusercontent.com/google/fonts/$(FONT_COMMIT)/ofl/play/Play-Bold.ttf" -o "$$tmp"; \
	actual="$$(git hash-object "$$tmp")"; \
	[ "$$actual" = "$(FONT_BOLD_BLOB)" ] || { echo "Unexpected Play Bold blob $$actual"; rm -f "$$tmp"; exit 1; }; \
	mv "$$tmp" "$@"

check-ui-deps: $(STB_HEADER) $(FONT_REGULAR) $(FONT_BOLD)
	@test -s "$(STB_HEADER)"
	@test -s "$(FONT_REGULAR)"
	@test -s "$(FONT_BOLD)"

main-native.c: main.c ../scripts/prepare-native-ui.py
	python3 ../scripts/prepare-native-ui.py main.c main-native.c

$(FONT_DATA): ../scripts/prepare-font-embed.py check-ui-deps
	python3 ../scripts/prepare-font-embed.py "$(FONT_REGULAR)" "$(FONT_BOLD)" "$@"

# entry.c intentionally includes main-native.c after renaming its main()
# symbol to retro_papa_main. Keep main-native.c as a dependency only; compiling
# it separately would create a second main symbol at link time.
retro-papa-ui.elf: entry.c main-native.c ui_font.c ui_font.h $(FONT_DATA) check-sdl check-ui-deps
	$(CC) $(CFLAGS) -o $@ entry.c ui_font.c $(FONT_DATA) $(SDL_LDADD)

icon0.png: $(ICON_SOURCE) ../scripts/resize-png.mjs ../scripts/convert-covers.mjs
	node ../scripts/resize-png.mjs "$<" "$@" 512 512

clean:
	rm -f retro-papa-ui.elf icon0.png main-native.c $(FONT_DATA)

.PHONY: all clean check-sdl check-ui-deps
