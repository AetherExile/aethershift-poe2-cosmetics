ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined)
endif

CORE_DIR ?= build/core
BUILD_ID ?= local
ELF := retro-papa-native.elf
GENERATED_LAUNCHER := launcher-native.c
CORE_SOURCES := \
	$(CORE_DIR)/pt.c \
	$(CORE_DIR)/elfldr.c \
	$(CORE_DIR)/hbldr.c
CORE_HEADERS := \
	$(CORE_DIR)/pt.h \
	$(CORE_DIR)/elfldr.h \
	$(CORE_DIR)/hbldr.h \
	$(CORE_DIR)/standalone_fs.h
SOURCES := $(GENERATED_LAUNCHER) dashboard_watch.c route.c $(CORE_SOURCES)
CFLAGS := -O2 -std=c17 -Wall -Wextra -Wpedantic -Wno-unused-parameter
# The adapted loader headers live under build/core. Include both build/core
# for its local includes and build/ so source files can use core/hbldr.h.
CFLAGS += -I. -I$(CORE_DIR) -I$(dir $(CORE_DIR)) -DRP_BUILD_ID=\"$(BUILD_ID)\"
LDADD := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil

all: $(ELF)

check-inputs:
	@test -s ../app/retro-papa-ui.elf
	@test -s ../app/icon0.png
	@test -s assets/param.json
	@for file in $(CORE_SOURCES) $(CORE_HEADERS); do test -s "$$file" || exit 1; done

$(GENERATED_LAUNCHER): launcher.c dashboard_watch.h ../scripts/prepare-resident-launcher.py
	python3 ../scripts/prepare-resident-launcher.py launcher.c $@

$(ELF): check-inputs $(GENERATED_LAUNCHER) dashboard_watch.c dashboard_watch.h route.c route.h $(CORE_SOURCES) $(CORE_HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDADD)

host-test:
	@mkdir -p build/host
	cc -O2 -std=c17 -Wall -Wextra -Werror -I. -o build/host/route-tests tests/route_tests.c route.c
	./build/host/route-tests

clean:
	rm -f $(ELF) $(GENERATED_LAUNCHER)
	rm -rf build/host

.PHONY: all check-inputs host-test clean
