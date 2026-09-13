ifeq ($(origin CC),default)
ifneq ($(OS)$(MSYSTEM),)
CC := gcc
else
CC := ccache gcc
endif
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
AR ?= ar

include platform.mk

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE -fPIC \
          -I$(CURDIR) $(TOY_PLATFORM_BACKEND_CFLAGS)

ifeq ($(PLATFORM),win32)
OBJS := platform_win32.o
else
# Wayland protocol glue is generated, not checked in.
PROTOCOL_SRC := xdg-shell-protocol.c xdg-decoration-protocol.c
PROTOCOL_HDR := xdg-shell-client-protocol.h xdg-decoration-client-protocol.h
OBJS := platform_wayland.o $(PROTOCOL_SRC:.c=.o)
endif

all: libtoyplatform.a

libtoyplatform.a: $(OBJS)
	$(AR) rcs $@ $^

platform_win32.o: platform_win32.c platform.h
	$(CC) $(CFLAGS) -c platform_win32.c -o $@

platform_wayland.o: platform_wayland.c platform.h $(PROTOCOL_HDR)
	$(CC) $(CFLAGS) -c platform_wayland.c -o $@

xdg-shell-protocol.o: xdg-shell-protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

xdg-decoration-protocol.o: xdg-decoration-protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_XML) $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL_XML) $@

xdg-decoration-client-protocol.h:
	wayland-scanner client-header $(XDG_DECORATION_XML) $@

xdg-decoration-protocol.c:
	wayland-scanner private-code $(XDG_DECORATION_XML) $@

ifeq ($(PLATFORM),win32)
test:
	@echo "toy-platform: the Win32 layer has no headless tests"
else
# Headless: test_platform_wayland.c includes platform_wayland.c and calls its
# listeners directly, so nothing here connects to a compositor.
test: test_platform_wayland
	./test_platform_wayland

test_platform_wayland: test_platform_wayland.c platform_wayland.c platform.h \
		$(PROTOCOL_HDR) $(PROTOCOL_SRC)
	$(CC) $(CFLAGS) -g test_platform_wayland.c $(PROTOCOL_SRC) -o $@ $(TOY_PLATFORM_LIBS)
endif

clean:
	$(RM) libtoyplatform.a *.o test_platform_wayland xdg-*.h xdg-*.c

.PHONY: all clean test
