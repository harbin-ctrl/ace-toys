# win-packaging

The Windows packaging of the toys: the win-toys counterpart of
`ace-packaging` and the `ace-toys` Debian package.

`make install` at the repository root, in an MSYS2 CLANGARM64 shell, builds
every ported toy and installs it for the current user. No administrator
rights are needed.

- `%LOCALAPPDATA%\Programs\win-toys\` — each toy's `.exe`, beside the MSYS2
  runtime DLLs they load (ANGLE, libc++, zlib, winpthread).
- Start menu → `Ace` → one shortcut per toy, as Linux has an Ace menu.

`make uninstall` removes them. The shared folder and the Ace folder go with
the last toy.

`make package` builds the distributable, as `make deb` does on Linux:
`../win-toys_<version>_<arch>.zip`, version from `debian/changelog`. It holds
every toy, their DLLs, `install.cmd`, `uninstall.cmd` and `README.txt`.
`make install` builds that package and installs from it.

- `install.mk` — make fragment with `win_install`, `win_uninstall`,
  `win_stage` and `win_ico`; see its header comment for usage.
- `install.ps1` — the per-user install and removal, of one toy or of a
  whole package.
- `install.cmd`, `uninstall.cmd`, `README.txt` — shipped in the package.
- `make-ico.ps1` — packs a toy's PNG icons into the `.ico` its resource file
  embeds.

A toy launched from the Start menu has no console, so it writes stderr to
`%LOCALAPPDATA%\<toy>\session.log`, as `poingo-logged` does on Linux.
