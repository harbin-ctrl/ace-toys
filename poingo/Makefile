ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
TARGETS := poingo icon_maker
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
ACE_DIR ?= $(CURDIR)/../ace-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
POINGO_SRC := poingo.c xdg-shell-protocol.c
ICON_SRC := icon_maker.c
ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_BMPS := $(addprefix icon_,$(addsuffix .bmp,$(ICON_SIZES)))
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

include $(ACE_DIR)/install.mk
include $(TOYAUDIO_DIR)/pipewire.mk

# Poingo uses Wayland, EGL/GLES, and xkbcommon.
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
ifeq ($(strip $(WAYLAND_LIBS)),)
WAYLAND_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lxkbcommon
endif

CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE -O2 -fPIC \
          -I$(CURDIR) -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) $(WAYLAND_CFLAGS) $(TOY_AUDIO_PIPEWIRE_CFLAGS)
LDFLAGS := $(WAYLAND_LIBS) $(TOY_AUDIO_PIPEWIRE_LIBS) -lm -lpthread

.PHONY: all clean run debug icons install uninstall test

all: $(TARGETS)

poingo: $(POINGO_SRC:.c=.o) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# The ring menu and audio core live in their own library checkouts;
# delegate so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

# lodepng survives only for icon_maker, which encodes the .png desktop
# icons at build time. The toy itself decodes nothing.
icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
icon_maker: icon_maker.o $(LODEPNG_LIB)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker
	./icon_maker

debug: CFLAGS += -O0 -g
debug: all

# Headless tests: test_poingo.c includes poingo.c directly, so nothing here
# opens a display. The plain build covers physics and the command line; the
# ThreadSanitizer build is what catches the regeneration races.
TEST_DEPS := test_poingo.c poingo.c xdg-shell-client-protocol.h \
	xdg-shell-protocol.c \
	$(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)

test: test_poingo test_poingo_tsan
	./test_poingo
	./test_poingo_tsan

test_poingo: $(TEST_DEPS)
	$(CC) $(CFLAGS) -g test_poingo.c xdg-shell-protocol.c \
		$(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) -o $@ $(LDFLAGS)

test_poingo_tsan: $(TEST_DEPS)
	$(CC) $(CFLAGS) -g -fsanitize=thread test_poingo.c xdg-shell-protocol.c \
		$(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) -o $@ $(LDFLAGS)

run: poingo
	./poingo

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

WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

poingo.o: xdg-shell-client-protocol.h \
	$(TOYAUDIO_DIR)/toy_audio.h $(TOYAUDIO_DIR)/toy_audio_stream.h

clean:
	$(RM) $(TARGETS) test_poingo test_poingo_tsan $(POINGO_SRC:.c=.o) $(ICON_SRC:.c=.o) $(LODEPNG_LIB) $(ICON_BMPS) $(ICON_PNGS)
	$(RM) poingo_*.png xdg-*.h xdg-*.c
	$(RM) -rf third_party/sysroot

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
