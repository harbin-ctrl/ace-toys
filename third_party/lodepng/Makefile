ifeq ($(origin CC),default)
ifneq ($(OS)$(MSYSTEM),)
CC := gcc
else
CC := ccache gcc
endif
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wno-unused-function -fPIC

all: liblodepng.a

liblodepng.a: lodepng.o
	$(AR) rcs $@ $^

lodepng.o: lodepng.c lodepng.h
	$(CC) $(CFLAGS) -c lodepng.c -o $@

clean:
	$(RM) liblodepng.a lodepng.o

.PHONY: all clean
