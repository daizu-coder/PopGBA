# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
#
# Windows CE (SHARP Brain PW-G5200, ARM, no HW integer divide) port of gpSP.
# Built with the cegcc cross toolchain.  Run from inside this directory:
#   make clean && make && make strip
#
# The emulation core is libretro/gpsp itself, in the parent directory
# (this repository is upstream gpSP plus this CE/ frontend).  Notes:
#  - cpu.cc / video.cc are C++ (-std=c++11 -fno-rtti -fno-exceptions),
#    built with arm-mingw32ce-g++; everything else is C.  We still LINK
#    with gcc (not g++): the core uses no libstdc++/STL, and this device
#    has no libstdc++-6.dll.  If a C++ runtime symbol turns up undefined
#    at link time, add -lsupc++ to LDFLAGS (NOT -lstdc++).
#  - retro_run() runs exactly one frame synchronously and returns, which
#    is the model ce_main.c's Sleep-based pacer assumes.
#  - The dynarec uses static .bss translation caches: arm/arm_stub.S lays
#    out the core memory region (iwram/vram/ewram/reg/spsr/reg_mode/
#    oam_ram/palette_ram/memory_map_read/io_registers/
#    palette_ram_converted) as one hand-offset .bss block with
#    reg_base = r11.  HAVE_MMAP / MMAP_JIT_CACHE stay OFF.
#  - The core reads BIOS/ROM through libretro-common's file_stream API.
#    It references only 6 filestream_* entry points and strlcpy/strlcat,
#    so instead of porting vfs_implementation.c we provide
#    ce_filestream.c (fopen-based) and compile just compat_strl.c.
#
# armv5te / mtune=arm926ej-s / mno-unaligned-access match the PW-G5200's
# actual core and its strict alignment trap.

COREROOT := ..

TOOLPREFIX := /opt/cegcc/bin/arm-mingw32ce-
CC      := $(TOOLPREFIX)gcc
CXX     := $(TOOLPREFIX)g++
STRIP   := $(TOOLPREFIX)strip
OBJDUMP := $(TOOLPREFIX)objdump
WINDRES := $(TOOLPREFIX)windres

ARCH_FLAGS := -march=armv5te -mtune=arm926ej-s -mno-unaligned-access

# === Core sources (upstream gpSP in the parent directory) ===

SOURCES_CC := \
	$(COREROOT)/cpu.cc \
	$(COREROOT)/video.cc

SOURCES_CORE := \
	$(COREROOT)/main.c $(COREROOT)/gba_memory.c $(COREROOT)/savestate.c \
	$(COREROOT)/input.c $(COREROOT)/sound.c $(COREROOT)/cheats.c \
	$(COREROOT)/memmap.c $(COREROOT)/serial.c $(COREROOT)/gbp.c \
	$(COREROOT)/rfu.c $(COREROOT)/serial_proto.c \
	$(COREROOT)/gba_cc_lut.c $(COREROOT)/cpu_threaded.c \
	$(COREROOT)/libretro/libretro.c \
	$(COREROOT)/libretro/libretro-common/compat/compat_strl.c \
	$(COREROOT)/bios_data.S $(COREROOT)/arm/arm_stub.S

SOURCES_CE := ce_main.c ce_log.c ce_gapi.c ce_input.c ce_audio.c ce_video.c \
	ce_config.c ce_lang.c ce_bmpfont.c ce_fileopen.c ce_filestream.c ce_fopen_utf8.c

# MMAP_JIT_CACHE / HAVE_MMAP are left off (arm_stub.S owns the .bss
# layout).  INLINE and __LIBRETRO__ match upstream's own Makefile DEFINES.
DEFINES  += -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DHAVE_STRINGS_H
DEFINES  += -DINLINE=inline -D__LIBRETRO__
DEFINES  += -DFRONTEND_SUPPORTS_RGB565
DEFINES  += -DHAVE_DYNAREC -DARM_ARCH

INCLUDES += -I. -Icompat
INCLUDES += -I$(COREROOT)
INCLUDES += -I$(COREROOT)/libretro
INCLUDES += -I$(COREROOT)/libretro/libretro-common/include

CFLAGS  := $(ARCH_FLAGS) -O3 -fomit-frame-pointer -fno-strict-aliasing \
	$(DEFINES) $(INCLUDES)

# Escape hatch for one-off diagnostic builds without editing this file, e.g.
#   make -C CE clean && make -C CE CE_EXTRA_CFLAGS=-DCE_JIT_TRACE
CFLAGS  += $(CE_EXTRA_CFLAGS)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=c++11

# bios_data.S uses .incbin "bios/open_gba_bios.bin" (path relative to
# the core directory), so the assembler needs -I$(COREROOT).  arm_stub.S is run
# through cpp for #define/## and #include "../gpsp_config.h".
ASFLAGS := $(ARCH_FLAGS) $(DEFINES) -I$(COREROOT)

# Link with gcc: no libstdc++ dependency (see header comment).
LDFLAGS := -static-libgcc -lm

RESOURCES := ce_res.o

OBJECTS := \
	$(patsubst %.cc,%.o,$(SOURCES_CC)) \
	$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(SOURCES_CORE))) \
	$(SOURCES_CE:.c=.o) $(RESOURCES)

TARGET := AppMain.exe

# UI bitmap font (ce_bmpfont.c) - same switch as PopPCE (a sister port,
# 2026-09-27). Galmuri14 is the default; the others stay buildable:
#   make                     Galmuri14 (default)  -> AppMain.exe
#   make CE_FONT=shinonome   Shinonome 16dot      -> AppMain_shinonome.exe
#   make CE_FONT=galmuri11   GalmuriMono11        -> AppMain_galmuri11.exe
# ce_galmuri14.h / ce_galmuri11.h are copies of PopPCE's
# tools/galmuri2c.py output. Only ce_bmpfont.o and ce_fileopen.o see the
# define; the .font-<name> stamp makes them rebuild when CE_FONT changes.
CE_FONT ?= galmuri14
ifeq ($(CE_FONT),galmuri14)
FONT_DEFS := -DCE_FONT_GALMURI14
else ifeq ($(CE_FONT),shinonome)
FONT_DEFS :=
TARGET := AppMain_shinonome.exe
else ifeq ($(CE_FONT),galmuri11)
FONT_DEFS := -DCE_FONT_GALMURI11
TARGET := AppMain_galmuri11.exe
else
$(error CE_FONT must be galmuri14, shinonome or galmuri11)
endif
FONT_STAMP := .font-$(CE_FONT)

.PHONY: all clean strip

all: $(TARGET)

$(FONT_STAMP):
	rm -f .font-*
	touch $@

ce_bmpfont.o ce_fileopen.o: $(FONT_STAMP) ce_bmpfont.h ce_galmuri14.h ce_galmuri11.h ce_shinonome16.h
ce_bmpfont.o ce_fileopen.o: CFLAGS += $(FONT_DEFS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

# upstream builds cpu_threaded.o with these two warnings suppressed.
$(COREROOT)/cpu_threaded.o: CFLAGS += -Wno-unused-variable -Wno-unused-label

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# ce_res.o also has to be rebuilt when the resource IDs, the embedded app
# icon or the main-menu mascot bitmap (both in CE/icon/) change.
ce_res.o: ce_resource.h ce_app_config.h icon/popgba.ico icon/popgba_mascot.bmp

%.o: %.rc
	$(WINDRES) -F pe-arm-wince-little -I. -I$(COREROOT) -o $@ $<

strip: all
	$(STRIP) $(TARGET)

clean:
	rm -f .font-* AppMain_shinonome.exe AppMain_galmuri11.exe
	rm -f $(OBJECTS) $(TARGET)
