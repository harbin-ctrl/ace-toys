# WASAPI build flags for toy_audio_stream_wasapi.c: the Windows counterpart
# of pipewire.mk. A toy includes this instead of pipewire.mk on Windows.

TOY_AUDIO_WASAPI_CFLAGS :=
TOY_AUDIO_WASAPI_LIBS := -lole32 -lavrt -luuid
