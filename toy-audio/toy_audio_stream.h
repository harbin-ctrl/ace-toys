#ifndef TOY_AUDIO_STREAM_H
#define TOY_AUDIO_STREAM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ToyAudioStream ToyAudioStream;

typedef void (*ToyAudioRenderCallback)(void *userdata, float *output,
                                       uint32_t nframes,
                                       uint32_t channels);

typedef enum {
    TOY_AUDIO_LOG_QUIET,
    TOY_AUDIO_LOG_DEBUG,    /* backends may report underruns and timing */
} ToyAudioLog;

typedef struct {
    const char *name;
    const char *description;
    uint32_t sample_rate;
    uint32_t channels;
    ToyAudioRenderCallback render;
    void *userdata;
    ToyAudioLog log;
} ToyAudioStreamConfig;

/*
 * Start a native playback stream: PipeWire (toy_audio_stream.c) or WASAPI
 * (toy_audio_stream_wasapi.c). All allocation and format setup happens
 * before this function returns; the render callback only fills buffers the
 * audio system owns.
 *
 * One channel negotiates MONO. Two channels negotiate explicit FL,FR stereo.
 */
ToyAudioStream *toy_audio_stream_start(const ToyAudioStreamConfig *config);

/* Stop, disconnect, and free a stream. Passing NULL is harmless. */
void toy_audio_stream_stop(ToyAudioStream *stream);

/*
 * Return the current delay from the first frame of the next submitted buffer
 * to presentation by the output device. The value includes queued frames,
 * converter/resampler buffering, and the remaining PipeWire graph/device
 * delay.
 */
bool toy_audio_stream_get_latency(const ToyAudioStream *stream,
                                  double *seconds);

bool toy_audio_stream_is_ready(const ToyAudioStream *stream);

#endif
