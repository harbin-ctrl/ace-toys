# win-packaging

The Windows packaging of the toys: the win-toys counterpart of
`ace-packaging` and the `ace-toys` Debian package.

Each toy has its own per-user installer, for ARM64 and x64 Windows. No
administrator rights are needed.

- `%LOCALAPPDATA%\Programs\Ace\<toy>\` — the toy's `.exe`, beside the MSYS2
  runtime DLLs it loads (ANGLE, libc++, zlib, winpthread).
- Start menu → `Ace` → the toy's shortcut, as Linux has an Ace menu.
- Settings → Apps → Installed apps lists it, with its uninstaller.

At the repository root, in an MSYS2 CLANGARM64 shell with the CLANG64
toolchain and [Inno Setup 6](https://jrsoftware.org/isinfo.php) installed:

- `make installer` — `installer/<toy>-<version>-setup.exe` per toy, version
  from `debian/changelog`. Build on ARM64 Windows; it runs the x64 toolchain
  under emulation.
- `make install` — runs them silently, first removing any copy the old zip
  package installed in `%LOCALAPPDATA%\Programs\win-toys\`.
- `make uninstall` — runs their uninstallers.

The same targets work in one toy's directory.

- `installer.mk` — make fragment building one toy's installer; see its header.
- `toy.iss` — the Inno Setup script every toy's installer is built from.
- `install.mk` — `win_uninstall` (removes a pre-installer copy) and `win_ico`.
- `remove-old-install.ps1` — that removal.
- `make-ico.ps1` — packs a toy's PNG icons into the `.ico` its resource file
  embeds.
- `test-installers.ps1` — installs, upgrades, uninstalls and installs the x64
  build of each toy on this machine.

A toy launched from the Start menu has no console, so it writes stderr to
`%LOCALAPPDATA%\<toy>\session.log`, as `poingo-logged` does on Linux.
