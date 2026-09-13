.DEFAULT_GOAL := all

ifeq ($(origin CC),default)
ifneq ($(OS)$(MSYSTEM),)
CC := gcc
else
CC := ccache gcc
endif
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX

# The window system and audio backend follow the host: Win32 and WASAPI on
# Windows, Wayland and PipeWire everywhere else. See ../toy-platform.
include $(CURDIR)/../toy-platform/platform.mk

CFLAGS := -O3 -ffast-math -Wall -pthread $(TOY_PLATFORM_CFLAGS)
LIBS   := $(TOY_PLATFORM_LIBS) -lm -pthread

TARGET  := balloons$(EXE)
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
ACE_DIR ?= $(CURDIR)/../ace-packaging
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
ifeq ($(PLATFORM),win32)
include $(TOYAUDIO_DIR)/wasapi.mk
CFLAGS  += $(TOY_AUDIO_WASAPI_CFLAGS)
LIBS    += $(TOY_AUDIO_WASAPI_LIBS)
else
include $(TOYAUDIO_DIR)/pipewire.mk
CFLAGS  += $(TOY_AUDIO_PIPEWIRE_CFLAGS)
LIBS    += $(TOY_AUDIO_PIPEWIRE_LIBS)
endif
CFLAGS  += -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR)
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a

# lodepng is only for icon_maker, which encodes the .png desktop icons.
# The toy itself decodes nothing: all its graphics are generated, and the
# grab cursor is compiled in as raw pixels (cursor_hand_grab.h).
LODEPNG_DIR := $(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(LODEPNG_DIR)/liblodepng.a
OBJS    := balloons.o balloon_gen.o thunder_synth.o audio.o
TOY_LIBS := $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) $(TOYPLATFORM_LIB)
ifeq ($(PLATFORM),win32)
# The icon, embedded so the taskbar, Explorer and the shortcut all show it.
RES_OBJ := balloons_res.o
else
RES_OBJ :=
endif

PREFIX := /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: all clean install uninstall stage icons

all: $(TARGET)

$(TARGET): $(OBJS) $(RES_OBJ) $(TOY_LIBS)
	$(CC) -o $@ $(OBJS) $(RES_OBJ) $(TOY_LIBS) $(LIBS) $(APP_LDFLAGS)

balloons.o: balloons.c balloon_gen.h audio.h cursor_hand_grab.h $(RINGMENU_DIR)/ringmenu.h \
	$(GHOSTICON_DIR)/ghost_icon.h $(TOYPLATFORM_DIR)/platform.h $(TOYPLATFORM_DIR)/compat.h
	$(CC) $(CFLAGS) -I$(GHOSTICON_DIR) -c -o $@ $<

balloon_gen.o: balloon_gen.c balloon_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<

audio.o: audio.c audio.h $(TOYAUDIO_DIR)/toy_audio.h \
	$(TOYAUDIO_DIR)/toy_audio_stream.h
	$(CC) $(CFLAGS) -c -o $@ $<

thunder_synth.o: thunder_synth.c thunder_synth.h
	$(CC) $(CFLAGS) -c -o $@ $<

# The ring menu, audio core and platform layer live in their own library
# checkouts; delegate so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
$(TOYPLATFORM_LIB): FORCE
	$(MAKE) -C $(TOYPLATFORM_DIR)
FORCE:

clean:
	$(RM) $(TARGET) $(OBJS) $(RES_OBJ) balloons.ico
	$(RM) xdg-*.h xdg-*.c icon_maker$(EXE) icon_maker.o balloon_assets.h thunder_pcm.h
	$(RM) assets/*.apng assets/icon*.png assets/icon.png
	$(RM) assets/.apngs_generated assets/.pops_generated

ICON_SIZES    := 16 32 48 64 128 256 512

DATADIR := $(PREFIX)/share

include $(ACE_DIR)/install.mk

icon_maker.o: CFLAGS := -O3 -Wall -I$(CURDIR)/../third_party/lodepng
icon_maker$(EXE): icon_maker.o $(LODEPNG_LIB)
	$(CC) -o $@ $^ -lm

icons: icon_maker$(EXE)
	./icon_maker$(EXE) assets

ifeq ($(PLATFORM),win32)
WIN_DIR ?= $(CURDIR)/../win-packaging
WINDRES ?= windres
include $(WIN_DIR)/install.mk

# One per-user win-toys folder and a shortcut in the Start menu's Ace folder,
# as the ace-toys package installs on Linux. See win-packaging/install.mk.
install: $(TARGET)
	$(call win_install,balloons,Balloons,$(TARGET))

uninstall:
	$(call win_uninstall,balloons,Balloons)

# Into the package the root Makefile's `package` builds.
stage: $(TARGET)
	$(call win_stage,balloons,Balloons,$(TARGET),$(DESTDIR))

# An .ico entry holds at most 256 px.
ICO_SIZES := 16 32 48 64 128 256

balloons.ico: icon_maker$(EXE)
	./icon_maker$(EXE) assets
	$(call win_ico,$@,$(foreach size,$(ICO_SIZES),assets/icon_$(size)x$(size).png))

balloons_res.o: balloons.rc balloons.ico
	$(WINDRES) $< -O coff -o $@
else
# System install: binary plus the desktop entry, Ace menu and
# every icon size — the same set the user install ships, like splat.
# The caches are refreshed only for a live install. A staged one (DESTDIR,
# as in a package build) would otherwise ship system-wide caches; the
# package's triggers refresh the real ones.
install: $(TARGET) icons ace-install
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -Dm644 balloons.desktop "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		install -Dm644 assets/icon_$${size}x$${size}.png \
			"$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true; \
	fi

uninstall: ace-uninstall
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(RM) "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true; \
	fi
endif
