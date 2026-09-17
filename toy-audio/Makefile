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
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -fPIC

# The mixer is portable; only the device stream has a backend per host.
ifneq ($(findstring serenity,$(shell $(CC) -dumpmachine 2>/dev/null)),)
include serenity.mk
STREAM_SRC := toy_audio_stream_serenity.cpp
STREAM_OBJ := toy_audio_stream_serenity.o
else ifneq ($(OS)$(MSYSTEM),)
include wasapi.mk
CFLAGS += $(TOY_AUDIO_WASAPI_CFLAGS)
STREAM_SRC := toy_audio_stream_wasapi.c
else
include pipewire.mk
CFLAGS += $(TOY_AUDIO_PIPEWIRE_CFLAGS)
STREAM_SRC := toy_audio_stream.c
STREAM_OBJ := $(STREAM_SRC:.c=.o)
endif

all: libtoyaudio.a

libtoyaudio.a: toy_audio.o $(STREAM_OBJ)
	$(AR) rcs $@ $^

toy_audio.o: toy_audio.c toy_audio.h
	$(CC) $(CFLAGS) -c toy_audio.c -o $@

toy_audio_stream_serenity.o: toy_audio_stream_serenity.cpp toy_audio_stream.h
	$(CXX) $(TOY_AUDIO_SERENITY_CXXFLAGS) -I$(CURDIR) -c $< -o $@

%.o: %.c toy_audio_stream.h
	$(CC) $(CFLAGS) -c $< -o $@

test: test_mixer
	./test_mixer

test_mixer: test_mixer.c toy_audio.c toy_audio.h
	$(CC) $(CFLAGS) test_mixer.c toy_audio.c -o $@ -lm -pthread

clean:
	$(RM) libtoyaudio.a *.o test_mixer

.PHONY: all clean test
