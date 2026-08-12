# Makefile -- GTA CTW R36S port
#
# Target: ARMv7 HF (32-bit ARM, matches the Android armeabi-v7a .so)
#         Cross-compile from x86-64 with arm-linux-gnueabihf-gcc.
#
# Quick start:
#   make setup    # install cross-dev packages (needs sudo)
#   make          # build gtactw_r36
#   make data     # prepare game data directory

CROSS   := arm-linux-gnueabihf-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# Local armhf sysroot built from extracted .deb packages
SYSROOT := armhf-sysroot/root

CFLAGS  := -march=armv7-a -mfpu=neon -mfloat-abi=hard \
           -O1 -g -fno-omit-frame-pointer \
           -DDEBUG \
           -Wall -Wno-unused-function \
           -I src \
           -I $(SYSROOT)/usr/include \
           -I $(SYSROOT)/usr/include/arm-linux-gnueabihf

LDFLAGS := -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
           -lSDL2 -lEGL -lGLESv2 -lopenal -lmpg123 -lz -ldl -lpthread -lm \
           -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
           -Wl,--allow-shlib-undefined

SRCS    := src/main.c \
           src/clock_fix.c \
           src/so_util.c \
           src/jni_patch.c \
           src/openal_patch.c \
           src/opengl_patch.c \
           src/mpg123_patch.c

OBJS    := $(SRCS:.c=.o)
TARGET  := gtactw_r36
PRELOAD := libclock_fix.so

# ── PortMaster (old-glibc) build ───────────────────────────────────────────
# Links against a Debian 11 "bullseye" glibc-2.31 sysroot so the binary runs
# on CFWs as old as glibc 2.31 (ArkOS-for-Clone, Debian 11 — GitHub issue #6).
# Build the sysroot first with:  scripts/build-bullseye-sysroot.sh
#
# Only glibc is swapped.  The graphics/audio dev libs remain link-only stubs
# from the newer armhf-sysroot (the device supplies the real ones at runtime;
# their symbols carry no GLIBC_2.34+ version tags, so they don't raise the
# glibc floor).  Ordering is deliberate:
#   * --sysroot + bullseye lib dirs FIRST  -> -lc/-lm/-lpthread bind to 2.31
#   * -idirafter for the noble includes    -> bullseye's pre-C23 <stdlib.h>
#     wins (no __isoc23_strtol@GLIBC_2.38), noble only fills SDL2/GLES/AL/…
BULLSEYE := bullseye-sysroot/root

# GCC's own intrinsic header dir (stddef.h, stdarg.h, …).  Needed because we
# use -nostdinc to purge the cross-toolchain's BUILT-IN glibc headers, which
# otherwise sit ahead of --sysroot and reintroduce the new-glibc redirects
# (__isoc23_strtol@2.38, __*_time64@2.34).
GCC_INC := $(shell $(CC) -print-file-name=include)

PM_CFLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard \
             -O1 -g -fno-omit-frame-pointer \
             -DDEBUG \
             -Wall -Wno-unused-function \
             --sysroot=$(BULLSEYE) \
             -nostdinc \
             -I src \
             -isystem $(GCC_INC) \
             -isystem $(BULLSEYE)/usr/include/arm-linux-gnueabihf \
             -isystem $(BULLSEYE)/usr/include \
             -idirafter $(SYSROOT)/usr/include \
             -idirafter $(SYSROOT)/usr/include/arm-linux-gnueabihf

PM_LDFLAGS := --sysroot=$(BULLSEYE) \
             -L$(BULLSEYE)/usr/lib/arm-linux-gnueabihf \
             -L$(BULLSEYE)/lib/arm-linux-gnueabihf \
             -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -lSDL2 -lEGL -lGLESv2 -lopenal -lmpg123 -lz -ldl -lpthread -lm \
             -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -Wl,--allow-shlib-undefined

PM_OBJS   := $(SRCS:.c=.pm.o)
PM_TARGET := $(TARGET).pm

# ── First-run installer ────────────────────────────────────────────────────
# Standalone SDL2 splash + extractor, run by the launcher only when the game
# data is not yet unpacked.  Deliberately links SDL2 ONLY (no GL, no OpenAL,
# no mpg123): it must be able to draw even on a device where the game's
# EGL/GLES path is broken.  Built with the same bullseye flags so it carries
# the same low glibc floor as the game binary.
INSTALLER_TARGET := installer.armhf
INSTALLER_LDFLAGS := --sysroot=$(BULLSEYE) \
             -L$(BULLSEYE)/usr/lib/arm-linux-gnueabihf \
             -L$(BULLSEYE)/lib/arm-linux-gnueabihf \
             -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -lSDL2 -lz -lm \
             -Wl,-rpath-link,$(SYSROOT)/usr/lib/arm-linux-gnueabihf \
             -Wl,--allow-shlib-undefined

.PHONY: all clean setup data install check-syms portmaster installer font

all: $(TARGET) $(PRELOAD)

# Build the old-glibc binary + preload for PortMaster distribution.
portmaster: $(PM_TARGET) $(PRELOAD) $(INSTALLER_TARGET)
	@echo "=== $(PM_TARGET): glibc version requirements ==="
	@$(CROSS)objdump -T $(PM_TARGET) | grep -oE 'GLIBC_[0-9.]+' | sort -u | tr '\n' ' '; echo
	@echo "=== $(INSTALLER_TARGET): glibc version requirements ==="
	@$(CROSS)objdump -T $(INSTALLER_TARGET) | grep -oE 'GLIBC_[0-9.]+' | sort -u | tr '\n' ' '; echo

installer: $(INSTALLER_TARGET)

$(INSTALLER_TARGET): src/installer.c src/png_min.c src/png_min.h src/installer_font.h
	$(CC) $(PM_CFLAGS) -o $@ src/installer.c src/png_min.c $(INSTALLER_LDFLAGS)

# Regenerate the embedded bitmap font (authoring-time only; the header is
# committed so the normal build needs no console-font packages).
font:
	python3 scripts/gen-installer-font.py

$(PM_TARGET): $(PM_OBJS)
	$(CC) $(PM_CFLAGS) -o $@ $^ $(PM_LDFLAGS)

src/%.pm.o: src/%.c
	$(CC) $(PM_CFLAGS) -c -o $@ $<

$(PRELOAD): src/clock_preload.c src/clock_preload.map
	$(CC) -O1 -fPIC -shared -nostdlib \
	  -march=armv7-a -mfpu=neon -mfloat-abi=hard \
	  -Wl,-soname,$(PRELOAD) \
	  -Wl,--version-script=src/clock_preload.map \
	  -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(PRELOAD)

# Install cross-dev packages for armhf (requires sudo)
setup:
	sudo dpkg --add-architecture armhf
	sudo apt-get update
	sudo apt-get install -y \
	    libsdl2-dev:armhf \
	    libopenal-dev:armhf \
	    libgles2-mesa-dev:armhf \
	    libegl1-mesa-dev:armhf \
	    libz-dev:armhf \
	    gcc-arm-linux-gnueabihf

# Prepare the game data directory
DATA_PATH ?= data

data: $(DATA_PATH)
$(DATA_PATH):
	mkdir -p $@
	cp extracted/lib/armeabi-v7a/libCTW.so $@/
	cp main.4.com.rockstargames.gtactw.obb $@/
	unzip -o gtacw.apk 'assets/*' -d $@/

# Copy binary + data to device (set DEVICE_IP or use SSH)
install: all data
	scp $(TARGET) $(DATA_PATH)/libCTW.so $(DATA_PATH)/main.4.com.rockstargames.gtactw.obb \
	    $(if $(DEVICE_IP),root@$(DEVICE_IP):/opt/gtactw/,$(DATA_PATH)/)

# Print all unresolved symbols from the .so to verify our symbol table
check-syms:
	@echo "=== Undefined symbols in libCTW.so ==="
	@arm-linux-gnueabihf-nm -D extracted/lib/armeabi-v7a/libCTW.so \
	    | awk '/ U /{ print $$3 }' | sed 's/@@.*//' | sort -u
