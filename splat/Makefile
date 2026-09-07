ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
ACE_DIR ?= $(CURDIR)/../ace-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
SPLAT_SRC := splat.c xdg-shell-protocol.c
ICON_SRC := icon_maker.c
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
# lodepng is only for icon_maker, which encodes the .png desktop icons at
# build time. The toy itself decodes nothing.
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
TARGETS := splat icon_maker

ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

include $(ACE_DIR)/install.mk
include $(TOYAUDIO_DIR)/pipewire.mk

# Spray is a pure-Wayland client: Wayland + EGL/GLES for the overlay and
# PipeWire for the spray-can hiss. No SDL, no toolkit.
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 2>/dev/null)
ifeq ($(strip $(WAYLAND_LIBS)),)
WAYLAND_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2
endif

# The paint rasterizer is hot float loops; build vectorization-friendly but
# portable (no -mcpu=native, so the binary can be packaged for other machines).
CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE \
          -O3 -ffast-math -funroll-loops -fPIC -I$(CURDIR) -I$(RINGMENU_DIR) \
          -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) $(WAYLAND_CFLAGS) $(TOY_AUDIO_PIPEWIRE_CFLAGS)
LDFLAGS := $(WAYLAND_LIBS) $(TOY_AUDIO_PIPEWIRE_LIBS) -lm -lpthread

.PHONY: all clean run icons install uninstall test

all: $(TARGETS)

# Wayland protocol glue is generated, not checked in.
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)
WAYLAND_SCANNER := $(shell command -v wayland-scanner 2>/dev/null || echo wayland-scanner)

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

splat.o: xdg-shell-client-protocol.h $(RINGMENU_DIR)/ringmenu.h \
		$(TOYAUDIO_DIR)/toy_audio.h $(TOYAUDIO_DIR)/toy_audio_stream.h \
		$(GHOSTICON_DIR)/ghost_icon.h

# The ring menu and audio core live in their own library checkouts; delegate
# so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

splat: $(SPLAT_SRC:.c=.o) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
icon_maker: icon_maker.o $(LODEPNG_LIB)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker
	./icon_maker

run: splat
	./splat

# Offscreen engine test: includes splat.c directly and writes PNGs of a
# simulated session (spray, mixing, eraser, cursor art) plus checks.
test: test_engine
	cd $(CURDIR) && ./test_engine

test_engine: test_engine.c splat.c spraycan.h \
		xdg-shell-client-protocol.h $(RINGMENU_LIB) \
		xdg-shell-protocol.o $(LODEPNG_LIB) $(TOYAUDIO_LIB) \
		$(GHOSTICON_LIB)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -I$(CURDIR) -I$(RINGMENU_DIR) \
		-I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) -I$(CURDIR)/../third_party/lodepng \
		$(TOY_AUDIO_PIPEWIRE_CFLAGS) test_engine.c \
		$(RINGMENU_LIB) xdg-shell-protocol.o $(LODEPNG_LIB) \
		$(TOYAUDIO_LIB) $(GHOSTICON_LIB) -o $@ $(WAYLAND_LIBS) \
		$(TOY_AUDIO_PIPEWIRE_LIBS) -lm -lpthread

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
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

uninstall: ace-uninstall
	$(RM) "$(DESTDIR)$(PREFIX)/bin/splat"
	$(RM) "$(DESTDIR)$(PREFIX)/share/applications/splat.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/splat.png"; \
	done
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

clean:
	$(RM) $(TARGETS) test_engine $(SPLAT_SRC:.c=.o) icon_maker.o test_engine.o $(LODEPNG_LIB) $(ICON_PNGS)
	$(RM) xdg-shell-client-protocol.h xdg-shell-protocol.c
	$(RM) test_*.png
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
