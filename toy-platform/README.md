# toy-platform

The toys' window-system layer, `platform.h`: window, GL ES 2 context, input,
frame pacing and cursors behind one interface, so a toy's own code never
touches Wayland, EGL or Win32.

- `platform_wayland.c` — Wayland: xdg-shell, EGL, wl_seat, frame callbacks,
  an input region, damage and buffer age.
- `platform_win32.c` — Windows: Win32 and ANGLE on Direct3D 11, presented
  through DirectComposition for per-pixel alpha. Click-through comes from
  toggling `WS_EX_TRANSPARENT` against the toy's input region.
- `compat.h` — the few C library calls the Windows CRT lacks.
- `platform.mk` — include from a toy's Makefile. It picks the backend and
  sets `PLATFORM`, `EXE`, `TOY_PLATFORM_CFLAGS`, `TOY_PLATFORM_LIBS` and
  `APP_LDFLAGS`.

`make` builds `libtoyplatform.a`. `make test` runs the Wayland layer's
headless tests on Linux.
