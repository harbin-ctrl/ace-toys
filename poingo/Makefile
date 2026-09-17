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

TARGETS := poingo$(EXE) icon_maker$(EXE)
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
ACE_DIR ?= $(CURDIR)/../ace-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
POINGO_SRC := poingo.c
ifeq ($(PLATFORM),win32)
# The icon, embedded so the taskbar, Explorer and the shortcut all show it.
POINGO_RES := poingo_res.o
else
POINGO_RES :=
endif
ICON_SRC := icon_maker.c
ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_BMPS := $(addprefix icon_,$(addsuffix .bmp,$(ICON_SIZES)))
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
else ifeq ($(PLATFORM),serenity)
include $(TOYAUDIO_DIR)/serenity.mk
TOY_AUDIO_LIBS := $(TOY_AUDIO_SERENITY_LIBS)
else
include $(TOYAUDIO_DIR)/pipewire.mk
TOY_AUDIO_CFLAGS := $(TOY_AUDIO_PIPEWIRE_CFLAGS)
TOY_AUDIO_LIBS := $(TOY_AUDIO_PIPEWIRE_LIBS)
endif

CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE -O2 -fPIC \
          -I$(CURDIR) -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) $(TOY_PLATFORM_CFLAGS) $(TOY_AUDIO_CFLAGS)
LDFLAGS := $(TOY_PLATFORM_LIBS) $(TOY_AUDIO_LIBS) -lm -lpthread
TOY_LIBS := $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) $(TOYPLATFORM_LIB)

.PHONY: all clean run debug icons install uninstall stage test

all: $(TARGETS)

poingo$(EXE): $(POINGO_SRC:.c=.o) $(POINGO_RES) $(TOY_LIBS)
	$(TOY_LINK) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(APP_LDFLAGS)

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

# lodepng survives only for icon_maker, which encodes the .png desktop
# icons at build time. The toy itself decodes nothing.
icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
icon_maker$(EXE): icon_maker.o $(LODEPNG_LIB)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker$(EXE)
	./icon_maker$(EXE)

debug: CFLAGS += -O0 -g
debug: all

# Headless tests: test_poingo.c includes poingo.c directly, so nothing here
# opens a display. The plain build covers physics and the command line; the
# ThreadSanitizer build is what catches the regeneration races.
TEST_DEPS := test_poingo.c poingo.c $(TOY_LIBS)

ifeq ($(PLATFORM),win32)
test: test_poingo$(EXE)
	./test_poingo$(EXE)
else
test: test_poingo test_poingo_tsan
	./test_poingo
	./test_poingo_tsan
endif

test_poingo$(EXE): $(TEST_DEPS)
	$(CC) $(CFLAGS) -g test_poingo.c $(TOY_LIBS) -o $@ $(LDFLAGS)

test_poingo_tsan: $(TEST_DEPS)
	$(CC) $(CFLAGS) -g -fsanitize=thread test_poingo.c $(TOY_LIBS) -o $@ $(LDFLAGS)

run: poingo$(EXE)
	./poingo$(EXE)

ifeq ($(PLATFORM),win32)
# A per-user installer, for ARM64 and x64, with a shortcut in the Start menu's
# Ace folder. See win-packaging/installer.mk.
WIN_TOY := poingo
WIN_TOY_NAME := Poingo
include $(WIN_DIR)/installer.mk

# An .ico entry holds at most 256 px.
ICO_SIZES := 32 48 64 128 256

poingo.ico: icon_maker$(EXE)
	./icon_maker$(EXE)
	$(call win_ico,$@,$(addprefix icon_,$(addsuffix .png,$(ICO_SIZES))))

poingo_res.o: poingo.rc poingo.ico
	$(WINDRES) $< -O coff -o $@
else
# The caches are refreshed only for a live install. A staged one (DESTDIR,
# as in a package build) would otherwise ship system-wide caches; the
# package's triggers refresh the real ones.
install: poingo icon_maker icons ace-install
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 poingo "$(DESTDIR)$(PREFIX)/bin/poingo"
	install -m 755 poingo-logged "$(DESTDIR)$(PREFIX)/bin/poingo-logged"
	install -m 755 poingo-audio-mark "$(DESTDIR)$(PREFIX)/bin/poingo-audio-mark"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 poingo.desktop "$(DESTDIR)$(PREFIX)/share/applications/poingo.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true; \
	fi

uninstall: ace-uninstall
	$(RM) "$(DESTDIR)$(PREFIX)/bin/poingo"
	$(RM) "$(DESTDIR)$(PREFIX)/bin/poingo-logged"
	$(RM) "$(DESTDIR)$(PREFIX)/bin/poingo-audio-mark"
	$(RM) "$(DESTDIR)$(PREFIX)/share/applications/poingo.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	if [ -z "$(DESTDIR)" ]; then \
		update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true; \
		gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true; \
	fi
endif

poingo.o: $(TOYPLATFORM_DIR)/platform.h $(TOYPLATFORM_DIR)/compat.h \
	$(TOYAUDIO_DIR)/toy_audio.h $(TOYAUDIO_DIR)/toy_audio_stream.h

clean:
	$(RM) $(TARGETS) test_poingo$(EXE) test_poingo_tsan $(POINGO_SRC:.c=.o) $(ICON_SRC:.c=.o) $(LODEPNG_LIB) $(ICON_BMPS) $(ICON_PNGS)
	$(RM) poingo_*.png xdg-*.h xdg-*.c poingo.ico poingo_res.o
	$(RM) -rf third_party/sysroot

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
