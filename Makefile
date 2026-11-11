ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

DEVKITA64 ?= $(DEVKITPRO)/devkitA64
PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX := $(DEVKITPRO)/libnx
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc

PKG_CONFIG_LIBDIR := $(PORTLIBS)/lib/pkgconfig
export PKG_CONFIG_LIBDIR

TARGET := UAPlay
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE
COMMON := -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections $(ARCH)
CFLAGS := $(COMMON) -D__SWITCH__ -I$(LIBNX)/include -I$(PORTLIBS)/include $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image mpv libcurl)
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--gc-sections
LIBS := $(shell pkg-config --libs mpv sdl2 SDL2_ttf SDL2_image libcurl) -lGLESv2 -lharfbuzz -lfreetype -ljpeg -lwebp -llzma -lzstd -L$(LIBNX)/lib -lnx -lm

.PHONY: all clean
all: $(TARGET).nro

main.o: source/main.c source/catalog.h source/posters.h source/library.h source/playback.h source/downloads.h
	$(CC) $(CFLAGS) -c $< -o $@

catalog.o: source/catalog.c source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

posters.o: source/posters.c source/posters.h source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

library.o: source/library.c source/library.h source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

playback.o: source/playback.c source/playback.h
	$(CC) $(CFLAGS) -c $< -o $@

downloads.o: source/downloads.c source/downloads.h source/catalog.h source/platform.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: main.o catalog.o posters.o library.o playback.o downloads.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).nacp:
	nacptool --create "UAPlay" "Small182" "0.5.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp --icon=icon.jpg --romfsdir=romfs

clean:
	rm -f main.o catalog.o posters.o library.o playback.o $(TARGET).elf $(TARGET).nacp $(TARGET).nro
