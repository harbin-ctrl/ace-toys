.DEFAULT_GOAL := all

ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
CFLAGS := -O3 -ffast-math -Wall -pthread $(shell pkg-config --cflags wayland-client wayland-egl egl glesv2)
LIBS   := $(shell pkg-config --libs wayland-client wayland-egl egl glesv2) -lm -pthread

TARGET  := balloons
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
ACE_DIR ?= $(CURDIR)/../ace-packaging
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
include $(TOYAUDIO_DIR)/pipewire.mk
CFLAGS  += $(TOY_AUDIO_PIPEWIRE_CFLAGS)
LIBS    += $(TOY_AUDIO_PIPEWIRE_LIBS)
CFLAGS  += -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR)
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a

# lodepng is only for icon_maker, which encodes the .png desktop icons.
# The toy itself decodes nothing: all its graphics are generated, and the
# grab cursor is compiled in as raw pixels (cursor_hand_grab.h).
LODEPNG_DIR := $(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(LODEPNG_DIR)/liblodepng.a
COMMON  := xdg-shell-protocol.o xdg-decoration-protocol.o
OBJS    := balloons.o balloon_gen.o thunder_synth.o audio.o $(COMMON)

PREFIX := /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: all clean install uninstall icons

all: $(TARGET)

$(TARGET): $(OBJS) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)
	$(CC) -o $@ $(OBJS) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) $(LIBS)

balloons.o: balloons.c balloon_gen.h audio.h cursor_hand_grab.h $(RINGMENU_DIR)/ringmenu.h xdg-shell-client-protocol.h xdg-decoration-client-protocol.h $(GHOSTICON_DIR)/ghost_icon.h
	$(CC) $(CFLAGS) -I$(GHOSTICON_DIR) -c -o $@ $<

balloon_gen.o: balloon_gen.c balloon_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<

audio.o: audio.c audio.h $(TOYAUDIO_DIR)/toy_audio.h \
	$(TOYAUDIO_DIR)/toy_audio_stream.h
	$(CC) $(CFLAGS) -c -o $@ $<

thunder_synth.o: thunder_synth.c thunder_synth.h
	$(CC) $(CFLAGS) -c -o $@ $<

# The ring menu and audio core live in their own library checkouts;
# delegate so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-decoration-protocol.o: xdg-decoration-protocol.c xdg-decoration-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-decoration-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

xdg-decoration-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

clean:
	$(RM) $(TARGET) $(OBJS)
	$(RM) xdg-*.h xdg-*.c icon_maker icon_maker.o balloon_assets.h thunder_pcm.h
	$(RM) assets/*.apng assets/icon*.png assets/icon.png
	$(RM) assets/.apngs_generated assets/.pops_generated

ICON_SIZES    := 16 32 48 64 128 256 512

DATADIR := $(PREFIX)/share

include $(ACE_DIR)/install.mk

# System install: binary plus the desktop entry, Ace menu and
# every icon size — the same set the user install ships, like splat.
icon_maker.o: CFLAGS := -O3 -Wall -I$(CURDIR)/../third_party/lodepng
icon_maker: icon_maker.o $(LODEPNG_LIB)
	$(CC) -o $@ $^ -lm

icons: icon_maker
	./icon_maker assets

install: $(TARGET) icons ace-install
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -Dm644 balloons.desktop "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		install -Dm644 assets/icon_$${size}x$${size}.png \
			"$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true

uninstall: ace-uninstall
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(RM) "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true
