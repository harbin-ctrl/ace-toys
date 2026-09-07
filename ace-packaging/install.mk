# Ace shared packaging — the "Ace" menu category, its directory entry,
# and its icons, installed identically by every toy.
#
# Include from a toy's Makefile:
#
#   ACE_DIR ?= $(CURDIR)/../ace-packaging
#   include $(ACE_DIR)/install.mk
#
# then make the toy's `install` target depend on `ace-install`
# (honors DESTDIR/PREFIX).
# Run your update-desktop-database / gtk-update-icon-cache refresh in the
# toy's target as before.
#
# The uninstall targets remove the shared files; note that other installed
# toys use them too, so only uninstall the category when removing the last
# toy.

# This fragment is usually included before the toy's own `all` target;
# save and restore .DEFAULT_GOAL so our targets never become the default.
ACE_SAVED_GOAL := $(.DEFAULT_GOAL)

ACE_ICON_SIZES := 16 22 24 32 48 64 128 256 512
ACE_TOYS := splat poingo balloons
ACE_OWNER := $(notdir $(CURDIR))

.PHONY: ace-install ace-uninstall

ace-install:
	install -d "$(DESTDIR)$(PREFIX)/share/desktop-directories"
	install -m 644 "$(ACE_DIR)/Ace.directory" "$(DESTDIR)$(PREFIX)/share/desktop-directories/Ace.directory"
	install -d "$(DESTDIR)/etc/xdg/menus/applications-merged"
	install -m 644 "$(ACE_DIR)/ace.menu" "$(DESTDIR)/etc/xdg/menus/applications-merged/ace.menu"
	install -d "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged"
	install -m 644 "$(ACE_DIR)/ace.menu" "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged/ace.menu"
	for size in $(ACE_ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 "$(ACE_DIR)/ace-icon-$${size}.png" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/ace.png"; \
	done
	install -d "$(DESTDIR)$(PREFIX)/share/ace/.owners"
	touch "$(DESTDIR)$(PREFIX)/share/ace/.owners/$(ACE_OWNER)"

ace-uninstall:
	owners="$(DESTDIR)$(PREFIX)/share/ace/.owners"; \
	owner="$$owners/$(ACE_OWNER)"; \
	$(RM) "$$owner"; \
	keep=0; \
	for toy in $(ACE_TOYS); do \
		if [ "$$toy" != "$(ACE_OWNER)" ] && [ -f "$(DESTDIR)$(PREFIX)/share/applications/$$toy.desktop" ]; then keep=1; fi; \
	done; \
	if [ "$$keep" -eq 0 ] && [ -d "$$owners" ] && [ -z "$$(find "$$owners" -type f -print -quit)" ]; then \
		$(RM) "$(DESTDIR)$(PREFIX)/share/desktop-directories/Ace.directory"; \
		$(RM) "$(DESTDIR)/etc/xdg/menus/applications-merged/ace.menu"; \
		$(RM) "$(DESTDIR)/etc/xdg/menus/rpd-applications-merged/ace.menu"; \
		for size in $(ACE_ICON_SIZES); do \
			$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/ace.png"; \
		done; \
		rmdir "$$owners" "$(DESTDIR)$(PREFIX)/share/ace" 2>/dev/null || true; \
	fi

.DEFAULT_GOAL := $(ACE_SAVED_GOAL)
