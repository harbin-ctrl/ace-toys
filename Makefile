# Ace — one package: the shared libraries and the toys that link them.
# Libraries build first; each toy also delegates to its libraries on its
# own, so a toy still builds standalone, but building libs up front keeps
# a parallel (make -jN) build correct and free of duplicate work.
#

# ccache and distcc are the Linux build setup; Windows calls the compiler directly.
ifeq ($(origin CC),default)
ifneq ($(OS)$(MSYSTEM),)
CC := gcc
else
CC := ccache gcc
endif
endif
ifeq ($(origin CXX),default)
ifneq ($(OS)$(MSYSTEM),)
CXX := g++
else
CXX := ccache g++
endif
endif
CCACHE_PREFIX ?= distcc
export CC CXX CCACHE_PREFIX

# Shared libraries (built here; toys reference them as ../<name> siblings).
# ace-packaging ships only assets + install.mk, so it is not built.
LIBS := toy-audio toy-platform ring-menu third_party/lodepng shared

# The interactive toys.
TOYS := splat poingo balloons

LINT_SOURCES := \
	toy-audio/toy_audio.c \
	ring-menu/ringmenu.c \
	shared/ghost_icon.c \
	splat/splat.c \
	poingo/poingo.c \
	balloons/balloon_gen.c balloons/thunder_synth.c balloons/audio.c balloons/balloons.c
LINT_INCLUDES := -Itoy-audio -Iring-menu -Ishared -Isplat -Ipoingo -Iballoons -Ithird_party/lodepng
TIDY_SOURCES := $(addprefix $(CURDIR)/,$(LINT_SOURCES))

.PHONY: all libs clean install stage-install uninstall deb debs inno \
	$(LIBS) $(TOYS) lint cppcheck analyzer tidy compile_commands.json

all: $(TOYS)

libs: $(LIBS)

$(LIBS):
	$(MAKE) -C $@

# Toys wait on every library before any of them build.
$(TOYS): libs
	$(MAKE) -C $@

clean:
	@for d in $(LIBS) $(TOYS); do $(MAKE) -C $$d clean; done
	$(RM) -r build installer

stage-install:
	@for d in $(TOYS); do $(MAKE) -C $$d install || exit $$?; done

deb:
	@set -euo pipefail; \
	dpkg-checkbuilddeps debian/control; \
	dpkg-buildpackage -b -uc -us

debs: deb

ifneq ($(OS)$(MSYSTEM),)
# Windows: one installer per toy, for ARM64 and x64, in installer/.
# See win-packaging/README.md.
inno: $(TOYS)
	@for d in $(TOYS); do $(MAKE) -C $$d inno || exit $$?; done

install: $(TOYS)
	@for d in $(TOYS); do $(MAKE) -C $$d install || exit $$?; done

uninstall:
	@for d in $(TOYS); do $(MAKE) -C $$d uninstall || exit $$?; done
else
# --reinstall: a rebuild keeps its version, and apt skips an equal version.
install: deb
	@set -euo pipefail; \
	package=../ace-toys_$$(dpkg-parsechangelog -SVersion)_$$(dpkg --print-architecture).deb; \
	if [ "$$(id -u)" -eq 0 ]; then apt install -y --reinstall "$$package"; else sudo apt install -y --reinstall "$$package"; fi
endif

lint:
	$(MAKE) cppcheck
	$(MAKE) analyzer
	$(MAKE) tidy

cppcheck:
	cppcheck -j2 --quiet --enable=warning,performance,portability \
		--error-exitcode=1 --suppress=missingIncludeSystem \
		--suppress=normalCheckLevelMaxBranches \
		--std=c11 --language=c $(LINT_INCLUDES) $(LINT_SOURCES)

analyzer:
	$(MAKE) clean
	# Static analyzer findings are printed for review; the compiler/build still gates this target.
	scan-build --exclude third_party --use-cc=clang --keep-going $(MAKE) -j1 all

compile_commands.json:
	$(MAKE) clean
	bear --output $@ -- $(MAKE) -j1 all

tidy: compile_commands.json
	@status=0; for src in $(TIDY_SOURCES); do \
		clang-tidy -p . "$$src" \
			--checks='-*,clang-analyzer-core.NullDereference,clang-analyzer-core.DivideZero,clang-analyzer-core.UndefinedBinaryOperatorResult,clang-analyzer-unix.Malloc' \
			--extra-arg=-I$(CURDIR)/toy-audio --extra-arg=-I$(CURDIR)/ring-menu \
			--extra-arg=-I$(CURDIR)/shared --extra-arg=-I$(CURDIR)/balloons --quiet || status=1; \
	done; exit $$status
