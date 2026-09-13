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
#   $(call win_install,<toy>,<Start menu name>,<toy>.exe)
#   $(call win_uninstall,<toy>,<Start menu name>)
#   $(call win_stage,<toy>,<Start menu name>,<toy>.exe,<package folder>)
#   $(call win_ico,<toy>.ico,<PNG files, 256 px and under>)
#
# Runs in an MSYS2 shell: ldd finds the runtime DLLs to ship, and cygpath
# hands Windows paths to PowerShell.

WIN_POWERSHELL := powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
WIN_EMPTY :=
WIN_SPACE := $(WIN_EMPTY) $(WIN_EMPTY)
# Where MSYS2 keeps the toolchain and its DLLs, e.g. /clangarm64.
WIN_RUNTIME_PREFIX := $(or $(MSYSTEM_PREFIX),/clangarm64)

# The toolchain DLLs a program loads, transitively, one MSYS2 path per line.
# System DLLs live elsewhere and are left out.
win_runtime_dll_paths = ldd "$(1)" | awk -v prefix="$(WIN_RUNTIME_PREFIX)/" 'index($$3, prefix) == 1 { print $$3 }' | sort -u

# The same, as one ;-separated list of Windows paths.
win_runtime_dlls = $$($(call win_runtime_dll_paths,$(1)) | xargs -r cygpath -w | paste -sd ';')

define win_install
	$(WIN_POWERSHELL) "$$(cygpath -w "$(WIN_DIR)/install.ps1")" -Action Install -Toy "$(1)" -Name "$(2)" -Exe "$$(cygpath -w "$(3)")" -Dlls "$(call win_runtime_dlls,$(3))"
endef

define win_uninstall
	$(WIN_POWERSHELL) "$$(cygpath -w "$(WIN_DIR)/install.ps1")" -Action Uninstall -Toy "$(1)" -Name "$(2)"
endef

# Copy a toy and its DLLs into a package folder, and list it in the folder's
# toys.txt for install.ps1 -Action InstallAll.
define win_stage
	test -n "$(4)"
	mkdir -p "$(4)"
	cp "$(3)" "$(4)/$(1).exe"
	$(call win_runtime_dll_paths,$(3)) | xargs -r -I{} cp {} "$(4)/"
	printf '%s|%s\n' "$(1)" "$(2)" >> "$(4)/toys.txt"
endef

define win_ico
	$(WIN_POWERSHELL) "$$(cygpath -w "$(WIN_DIR)/make-ico.ps1")" -Out "$(1)" -Png "$(subst $(WIN_SPACE),;,$(strip $(2)))"
endef
