# ace-packaging

The shared desktop packaging of the toys (spray, splat, Poingo, balloons):
the "Ace" application-menu category. Every toy used to carry its own
byte-identical copies of these files; now they live once, here.

- `ace.menu` — merged-menu file placing apps with the `Ace` category into an
  Ace submenu (installed to both `applications-merged` and Raspberry Pi's
  `rpd-applications-merged`).
- `Ace.directory` — the submenu's name and icon.
- `ace-icon-*.png` (+ `ace-icon.svg` source) — the category icon at the
  hicolor sizes.
- `install.mk` — include-able make fragment with
  `ace-install` / `ace-uninstall` targets;
  see its header comment for usage.

Each toy still owns its own `.desktop` entry and app icons; only the shared
category lives here.
