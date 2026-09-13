# The toy's Inno Setup installer, for ARM64 and x64 Windows. In a toy's
# Makefile, when building for Windows:
#
#   WIN_TOY := poingo
#   WIN_TOY_NAME := Poingo
#   include $(WIN_DIR)/installer.mk
#
# after install.mk, with a rule for $(WIN_TOY).ico and a FORCE target. Then:
#
#   make installer   ../installer/<toy>-<version>-setup.exe (toy.iss)
#   make install     runs it silently, replacing a pre-installer copy
#   make uninstall   runs its uninstaller
#
# Each architecture builds with its own MSYS2 toolchain in its own copy of the
# sources, ../build/windows/<arch>/<toy>, so neither overwrites the other's
# objects. Build on ARM64 Windows: it runs the x64 toolchain under emulation,
# and x64 Windows cannot run the ARM64 one.

WIN_REPO := $(abspath $(WIN_DIR)/..)
WIN_VERSION := $(shell sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$(WIN_REPO)/debian/changelog")
WIN_ISCC ?= $$(cygpath -u "$$LOCALAPPDATA")/Programs/Inno Setup 6/ISCC.exe
WIN_ARCHES := arm64 x64
WIN_BUILD := $(WIN_REPO)/build/windows
WIN_OUT := $(WIN_REPO)/installer
WIN_STAGE := $(WIN_OUT)/stage/$(WIN_TOY)
WIN_SETUP := $(WIN_TOY)-$(WIN_VERSION)-setup
WIN_UNINSTALLER := $$(cygpath -u "$$LOCALAPPDATA")/Programs/Ace/$(WIN_TOY)/unins000.exe
# The sources a toy builds from; build outputs belong to one architecture.
WIN_SOURCE_DIRS := debian toy-audio toy-platform ring-menu shared third_party \
	ace-packaging win-packaging $(WIN_TOY)
WIN_COPY_EXCLUDES := --exclude=.git --exclude='*.o' --exclude='*.a' --exclude='*.exe' \
	--exclude='*.ico' --exclude=win-packaging/stage
# Keeps MSYS2 from rewriting /FLAG arguments into paths.
WIN_NO_ARGCONV := MSYS2_ARG_CONV_EXCL='*'

win_prefix = $(if $(filter x64,$(1)),/clang64,/clangarm64)
win_env = MSYSTEM=$(if $(filter x64,$(1)),CLANG64,CLANGARM64) \
	MSYSTEM_PREFIX=$(call win_prefix,$(1)) \
	MSYSTEM_CARCH=$(if $(filter x64,$(1)),x86_64,aarch64) \
	PATH="$(call win_prefix,$(1))/bin:$$PATH"

.PHONY: installer

installer: $(addprefix installer-stage-,$(WIN_ARCHES)) $(WIN_TOY).ico
	$(WIN_NO_ARGCONV) "$(WIN_ISCC)" /Q "/DToy=$(WIN_TOY)" "/DName=$(WIN_TOY_NAME)" \
		"/DAppVersion=$(WIN_VERSION)" "/DStage=$$(cygpath -w "$(WIN_STAGE)")" \
		"/DIcon=$$(cygpath -w "$(CURDIR)/$(WIN_TOY).ico")" "/O$$(cygpath -w "$(WIN_OUT)")" \
		"/F$(WIN_SETUP)" "$$(cygpath -w "$(WIN_DIR)/toy.iss")"
	@echo "installer: $(WIN_OUT)/$(WIN_SETUP).exe"

# $(WIN_STAGE)/<arch>: the program and the runtime DLLs it loads.
installer-stage-%: FORCE
	rm -rf "$(WIN_BUILD)/$*/$(WIN_TOY)" "$(WIN_STAGE)/$*"
	mkdir -p "$(WIN_BUILD)/$*/$(WIN_TOY)" "$(WIN_STAGE)/$*"
	tar -c -C "$(WIN_REPO)" $(WIN_COPY_EXCLUDES) $(WIN_SOURCE_DIRS) | tar -x -C "$(WIN_BUILD)/$*/$(WIN_TOY)"
	# lodepng, for icon_maker: the root Makefile builds it before any toy.
	env $(call win_env,$*) $(MAKE) -C "$(WIN_BUILD)/$*/$(WIN_TOY)/third_party/lodepng"
	env $(call win_env,$*) $(MAKE) -C "$(WIN_BUILD)/$*/$(WIN_TOY)/$(WIN_TOY)" $(WIN_TOY).exe
	cp "$(WIN_BUILD)/$*/$(WIN_TOY)/$(WIN_TOY)/$(WIN_TOY).exe" "$(WIN_STAGE)/$*/"
	env $(call win_env,$*) ldd "$(WIN_STAGE)/$*/$(WIN_TOY).exe" | \
		awk -v prefix="$(call win_prefix,$*)/" 'index($$3, prefix) == 1 { print $$3 }' | \
		sort -u | xargs -r -I{} cp {} "$(WIN_STAGE)/$*/"

install: installer
	$(call win_uninstall,$(WIN_TOY),$(WIN_TOY_NAME))
	$(WIN_NO_ARGCONV) "$(WIN_OUT)/$(WIN_SETUP).exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS

uninstall:
	$(call win_uninstall,$(WIN_TOY),$(WIN_TOY_NAME))
	uninstaller="$(WIN_UNINSTALLER)"; \
	if [ -f "$$uninstaller" ]; then $(WIN_NO_ARGCONV) "$$uninstaller" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART; fi
