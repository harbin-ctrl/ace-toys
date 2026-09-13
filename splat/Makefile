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

RINGMENU_DIR ?= $(CURDIR)/../ring-menu
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
ACE_DIR ?= $(CURDIR)/../ace-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
SPLAT_SRC := splat.c
ICON_SRC := icon_maker.c
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
# lodepng is only for icon_maker, which encodes the .png desktop icons at
# build time. The toy itself decodes nothing.
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
TARGETS := splat$(EXE) icon_maker$(EXE)
ifeq ($(PLATFORM),win32)
# The icon, embedded so the taskbar, Explorer and the shortcut all show it.
SPLAT_RES := splat_res.o
else
SPLAT_RES :=
endif

ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local

include $(ACE_DIR)/install.mk

ifeq ($(PLATFORM),win32)
WIN_DIR ?= $(CURDIR)/../win-packaging
WINDRES ?= windres
include $(WIN_DIR)/install.mk
include $(TOYAUDIO_DIR)/wasapi.mk
TOY_AUDIO_CFLAGS := $(TOY_AUDIO_WASAPI_CFLAGS)
TOY_AUDIO_LIBS := $(TOY_AUDIO_WASAPI_LIBS)
else
include $(TOYAUDIO_DIR)/pipewire.mk
TOY_AUDIO_CFLAGS := $(TOY_AUDIO_PIPEWIRE_CFLAGS)
TOY_AUDIO_LIBS := $(TOY_AUDIO_PIPEWIRE_LIBS)
endif

# The paint rasterizer is hot float loops; build vectorization-friendly but
# portable (no -mcpu=native, so the binary can be packaged for other machines).
CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE \
          -O3 -ffast-math -funroll-loops -fPIC -I$(CURDIR) -I$(RINGMENU_DIR) \
          -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) $(TOY_PLATFORM_CFLAGS) $(TOY_AUDIO_CFLAGS)
LDFLAGS := $(TOY_PLATFORM_LIBS) $(TOY_AUDIO_LIBS) -lm -lpthread
TOY_LIBS := $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) $(TOYPLATFORM_LIB)

.PHONY: all clean run icons install uninstall stage test

all: $(TARGETS)

splat.o: $(RINGMENU_DIR)/ringmenu.h \
		$(TOYAUDIO_DIR)/toy_audio.h $(TOYAUDIO_DIR)/toy_audio_stream.h \
		$(GHOSTICON_DIR)/ghost_icon.h $(TOYPLATFORM_DIR)/platform.h spraycan.h

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

splat$(EXE): $(SPLAT_SRC:.c=.o) $(SPLAT_RES) $(TOY_LIBS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(APP_LDFLAGS)

icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
icon_maker$(EXE): icon_maker.o $(LODEPNG_LIB)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker$(EXE)
	./icon_maker$(EXE)

run: splat$(EXE)
	./splat$(EXE)

# Offscreen engine test: includes splat.c directly and writes PNGs of a
# simulated session (spray, mixing, eraser, cursor art) plus checks.
test: test_engine$(EXE)
	cd $(CURDIR) && ./test_engine$(EXE)

test_engine$(EXE): test_engine.c splat.c spraycan.h $(LODEPNG_LIB) $(TOY_LIBS)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -I$(CURDIR) -I$(RINGMENU_DIR) \
		-I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) -I$(CURDIR)/../third_party/lodepng \
		$(TOY_PLATFORM_CFLAGS) $(TOY_AUDIO_CFLAGS) test_engine.c \
		$(LODEPNG_LIB) $(TOY_LIBS) -o $@ $(LDFLAGS)

ifeq ($(PLATFORM),win32)
# A per-user installer, for ARM64 and x64, with a shortcut in the Start menu's
# Ace folder. See win-packaging/installer.mk.
WIN_TOY := splat
WIN_TOY_NAME := Splat
include $(WIN_DIR)/installer.mk

# An .ico entry holds at most 256 px.
ICO_SIZES := 32 48 64 128 256

splat.ico: icon_maker$(EXE)
	./icon_maker$(EXE)
	$(call win_ico,$@,$(addprefix icon_,$(addsuffix .png,$(ICO_SIZES))))

splat_res.o: splat.rc splat.ico
	$(WINDRES) $< -O coff -o $@
else
# The caches are refreshed only for a live install. A staged one (DESTDIR,
# as in a package build) would otherwise ship system-wide caches; the
# package's triggers refresh the real ones.
install: splat icons ace-install
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 splat "$(DESTDIR)$(PREFIX)/bin/splat"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 splat.desktop "$(DESTDIR)$(PREFIX)/share/applications/splat.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/splat.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true; \
	fi

uninstall: ace-uninstall
	$(RM) "$(DESTDIR)$(PREFIX)/bin/splat"
	$(RM) "$(DESTDIR)$(PREFIX)/share/applications/splat.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/splat.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true; \
	fi
endif

clean:
	$(RM) $(TARGETS) test_engine$(EXE) $(SPLAT_SRC:.c=.o) icon_maker.o test_engine.o $(LODEPNG_LIB) $(ICON_PNGS)
	$(RM) xdg-shell-client-protocol.h xdg-shell-protocol.c splat.ico splat_res.o
	$(RM) test_*.png
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
