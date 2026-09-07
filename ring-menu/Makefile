ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -fPIC

all: libringmenu.a

test: test_ringmenu

test_ringmenu: test_ringmenu.c ringmenu.c ringmenu.h

	$(CC) $(CFLAGS) test_ringmenu.c ringmenu.c -o $@ -lm

libringmenu.a: ringmenu.o
	$(AR) rcs $@ $^

ringmenu.o: ringmenu.c ringmenu.h
	$(CC) $(CFLAGS) -c ringmenu.c -o $@

clean:
	$(RM) libringmenu.a ringmenu.o test_ringmenu

.PHONY: all clean test
