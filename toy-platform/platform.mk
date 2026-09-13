# The window-system layer a toy links: Win32 and ANGLE on Windows, Wayland
# and EGL everywhere else. Include from a toy's Makefile:
#
#   include $(CURDIR)/../toy-platform/platform.mk
#
# then compile with $(TOY_PLATFORM_CFLAGS), link $(TOYPLATFORM_LIB) followed
# by $(TOY_PLATFORM_LIBS), link the program itself with $(APP_LDFLAGS), and
# name it with $(EXE).

TOYPLATFORM_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TOYPLATFORM_LIB := $(TOYPLATFORM_DIR)/libtoyplatform.a
PKG_CONFIG ?= pkg-config

# Windows means OS=Windows_NT (cmd) or MSYSTEM set (an MSYS2 shell).
ifneq ($(OS)$(MSYSTEM),)
PLATFORM := win32
EXE := .exe
TOY_PLATFORM_BACKEND_CFLAGS :=
# ANGLE supplies EGL and GLES 2 on top of Direct3D 11.
TOY_PLATFORM_LIBS := -lEGL -lGLESv2 -lgdi32 -luser32
# A GUI program: launched from the Start menu, it opens no console window.
APP_LDFLAGS := -mwindows
else
PLATFORM := wayland
EXE :=
TOY_PLATFORM_BACKEND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
TOY_PLATFORM_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
ifeq ($(strip $(TOY_PLATFORM_LIBS)),)
TOY_PLATFORM_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lxkbcommon
endif
APP_LDFLAGS :=
endif

# Toys include platform.h and compat.h, and draw with GLES 2.
TOY_PLATFORM_CFLAGS := -I$(TOYPLATFORM_DIR) $(TOY_PLATFORM_BACKEND_CFLAGS)
