# Windows packaging shared by the toys: the win-toys counterpart of
# ace-packaging/install.mk.
#
# Include from a toy's Makefile when building for Windows:
#
#   WIN_DIR ?= $(CURDIR)/../win-packaging
#   include $(WIN_DIR)/install.mk
#
# then, in the toy's recipes:
#
#   $(call win_uninstall,<toy>,<Start menu name>)   removes a pre-installer copy
#   $(call win_ico,<toy>.ico,<PNG files, 256 px and under>)
#
# The installer itself is installer.mk. Runs in an MSYS2 shell: cygpath hands
# Windows paths to PowerShell.

WIN_POWERSHELL := powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
WIN_EMPTY :=
WIN_SPACE := $(WIN_EMPTY) $(WIN_EMPTY)

define win_uninstall
	$(WIN_POWERSHELL) "$$(cygpath -w "$(WIN_DIR)/remove-old-install.ps1")" -Toy "$(1)" -Name "$(2)"
endef

define win_ico
	$(WIN_POWERSHELL) "$$(cygpath -w "$(WIN_DIR)/make-ico.ps1")" -Out "$(1)" -Png "$(subst $(WIN_SPACE),;,$(strip $(2)))"
endef
