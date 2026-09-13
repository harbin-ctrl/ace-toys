/*
 * WASAPI implementation of toy_audio_stream.h: the Windows counterpart of the
 * PipeWire stream in toy_audio_stream.c.
 *
 * Shared mode, event driven. The mixer always renders 32-bit float at the
 * configured rate and channel count; AUTOCONVERTPCM has Windows convert that
 * to the device mix format, so the caller never sees what the device runs at.
 *
 *     render thread:  event -> padding -> GetBuffer -> render -> ReleaseBuffer
 *                                                                 |
 *                        latency = frames still queued + endpoint latency
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "toy_audio_stream.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <initguid.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#define TOY_AUDIO_STREAM_NAME_MAX 64
#define TOY_AUDIO_NSEC_PER_SEC 1000000000ULL
#define TOY_AUDIO_NSEC_PER_HNS 100ULL                   /* REFERENCE_TIME tick */
#define TOY_AUDIO_BUFFER_HNS ((REFERENCE_TIME)200000)   /* 20 ms */
#define TOY_AUDIO_START_TIMEOUT_MS 5000
#define TOY_AUDIO_WAKE_MS 200
#define TOY_AUDIO_SAMPLE_BITS 32

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid ksmedia.h. */
static const GUID TOY_AUDIO_SUBTYPE_FLOAT = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

struct ToyAudioStream {
    ToyAudioRenderCallback render;
    void *userdata;
    uint32_t sample_rate;
    uint32_t channels;
    char name[TOY_AUDIO_STREAM_NAME_MAX];
    bool com_initialized;
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render_client;
    bool client_started;
    HANDLE buffer_event;
    HANDLE ready_event;
    HANDLE thread;
    UINT32 buffer_frames;
    uint64_t endpoint_latency_ns;
    atomic_bool stop;
    atomic_bool streaming;
    atomic_uint_fast64_t latency_ns;
};

static void report_hresult(const ToyAudioStream *audio, const char *what, HRESULT hr)
{
    fprintf(stderr, "%s: WASAPI %s failed: 0x%08lx\n", audio->name, what, (unsigned long)hr);
}

/* Top the device buffer up. Runs on the render thread only. */
static bool render_period(ToyAudioStream *audio)
{
    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(audio->client, &padding);
    if (FAILED(hr)) {
        report_hresult(audio, "GetCurrentPadding", hr);
        return false;
    }

    UINT32 frames = audio->buffer_frames - padding;
    if (frames > 0) {
        BYTE *data = NULL;
        hr = IAudioRenderClient_GetBuffer(audio->render_client, frames, &data);
        if (FAILED(hr)) {
            report_hresult(audio, "GetBuffer", hr);
            return false;
        }
        audio->render(audio->userdata, (float *)data, frames, audio->channels);
        hr = IAudioRenderClient_ReleaseBuffer(audio->render_client, frames, 0);
        if (FAILED(hr)) {
            report_hresult(audio, "ReleaseBuffer", hr);
            return false;
        }
    }

    /* The next buffer's first frame plays once everything queued ahead of it
       has, plus the endpoint's own delay. */
    uint64_t queued_ns = (uint64_t)(padding + frames) * TOY_AUDIO_NSEC_PER_SEC / audio->sample_rate;
    atomic_store_explicit(&audio->latency_ns, queued_ns + audio->endpoint_latency_ns,
                          memory_order_release);

    if (!atomic_load_explicit(&audio->streaming, memory_order_acquire)) {
        atomic_store_explicit(&audio->streaming, true, memory_order_release);
        SetEvent(audio->ready_event);
    }
    return true;
}

static DWORD WINAPI render_thread(LPVOID arg)
{
    ToyAudioStream *audio = arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* MMCSS keeps the render thread ahead of ordinary work under load. */
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    while (!atomic_load_explicit(&audio->stop, memory_order_acquire)) {
        if (WaitForSingleObject(audio->buffer_event, TOY_AUDIO_WAKE_MS) != WAIT_OBJECT_0) {
            continue;
        }
        if (!render_period(audio)) {
            /* Usually the device went away. Without a latency the game stops
               predicting, and nothing more is heard. */
            atomic_store_explicit(&audio->streaming, false, memory_order_release);
            break;
        }
    }

    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    /* A start still waiting must not sit out the whole timeout. */
    SetEvent(audio->ready_event);
    return 0;
}

static void destroy_stream(ToyAudioStream *audio)
{
    atomic_store_explicit(&audio->stop, true, memory_order_release);
    if (audio->thread) {
        WaitForSingleObject(audio->thread, INFINITE);
        CloseHandle(audio->thread);
        audio->thread = NULL;
    }
    if (audio->client_started) {
        IAudioClient_Stop(audio->client);
        audio->client_started = false;
    }
    if (audio->render_client) {
        IAudioRenderClient_Release(audio->render_client);
        audio->render_client = NULL;
    }
    if (audio->client) {
        IAudioClient_Release(audio->client);
        audio->client = NULL;
    }
    if (audio->device) {
        IMMDevice_Release(audio->device);
        audio->device = NULL;
    }
    if (audio->enumerator) {
        IMMDeviceEnumerator_Release(audio->enumerator);
        audio->enumerator = NULL;
    }
    if (audio->buffer_event) {
        CloseHandle(audio->buffer_event);
        audio->buffer_event = NULL;
    }
    if (audio->ready_event) {
        CloseHandle(audio->ready_event);
        audio->ready_event = NULL;
    }
    if (audio->com_initialized) {
        CoUninitialize();
        audio->com_initialized = false;
    }
    atomic_store_explicit(&audio->streaming, false, memory_order_release);
    atomic_store_explicit(&audio->latency_ns, 0, memory_order_release);
}

static bool open_client(ToyAudioStream *audio)
{
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&audio->enumerator);
    if (FAILED(hr)) {
        report_hresult(audio, "device enumerator", hr);
        return false;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(audio->enumerator, eRender, eConsole,
            &audio->device);
    if (FAILED(hr)) {
        report_hresult(audio, "default output device", hr);
        return false;
    }

    hr = IMMDevice_Activate(audio->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&audio->client);
    if (FAILED(hr)) {
        report_hresult(audio, "Activate", hr);
        return false;
    }

    WAVEFORMATEXTENSIBLE format = {0};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = (WORD)audio->channels;
    format.Format.nSamplesPerSec = audio->sample_rate;
    format.Format.wBitsPerSample = TOY_AUDIO_SAMPLE_BITS;
    format.Format.nBlockAlign = (WORD)(audio->channels * sizeof(float));
    format.Format.nAvgBytesPerSec = audio->sample_rate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = TOY_AUDIO_SAMPLE_BITS;
    /* One channel is MONO, two are FL,FR, as the PipeWire stream negotiates. */
    format.dwChannelMask = audio->channels == 1
                           ? SPEAKER_FRONT_CENTER
                           : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    format.SubFormat = TOY_AUDIO_SUBTYPE_FLOAT;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = IAudioClient_Initialize(audio->client, AUDCLNT_SHAREMODE_SHARED, flags,
                                 TOY_AUDIO_BUFFER_HNS, 0, (const WAVEFORMATEX *)&format, NULL);
    if (FAILED(hr)) {
        report_hresult(audio, "Initialize", hr);
        return false;
    }

    hr = IAudioClient_GetBufferSize(audio->client, &audio->buffer_frames);
    if (FAILED(hr)) {
        report_hresult(audio, "GetBufferSize", hr);
        return false;
    }

    REFERENCE_TIME endpoint = 0;
    if (SUCCEEDED(IAudioClient_GetStreamLatency(audio->client, &endpoint)) && endpoint > 0) {
        audio->endpoint_latency_ns = (uint64_t)endpoint * TOY_AUDIO_NSEC_PER_HNS;
    }

    hr = IAudioClient_GetService(audio->client, &IID_IAudioRenderClient,
                                 (void **)&audio->render_client);
    if (FAILED(hr)) {
        report_hresult(audio, "GetService", hr);
        return false;
    }
    return true;
}

ToyAudioStream *toy_audio_stream_start(const ToyAudioStreamConfig *config)
{
    if (!config || !config->name || !config->render ||
            config->sample_rate == 0 ||
            (config->channels != 1 && config->channels != 2)) {
        return NULL;
    }

    ToyAudioStream *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return NULL;
    }
    audio->render = config->render;
    audio->userdata = config->userdata;
    audio->sample_rate = config->sample_rate;
    audio->channels = config->channels;
    snprintf(audio->name, sizeof(audio->name), "%s", config->name);
    atomic_init(&audio->stop, false);
    atomic_init(&audio->streaming, false);
    atomic_init(&audio->latency_ns, 0);

    /* S_FALSE (already initialized) also needs its CoUninitialize. A caller
       already in a single-threaded apartment keeps it; the audio interfaces
       work from either. */
    audio->com_initialized = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    if (!open_client(audio)) {
        toy_audio_stream_stop(audio);
        return NULL;
    }

    audio->buffer_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    audio->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!audio->buffer_event || !audio->ready_event) {
        fprintf(stderr, "%s: failed to create WASAPI events\n", audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    HRESULT hr = IAudioClient_SetEventHandle(audio->client, audio->buffer_event);
    if (FAILED(hr)) {
        report_hresult(audio, "SetEventHandle", hr);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    audio->thread = CreateThread(NULL, 0, render_thread, audio, 0, NULL);
    if (!audio->thread) {
        fprintf(stderr, "%s: failed to start the WASAPI render thread\n", audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    hr = IAudioClient_Start(audio->client);
    if (FAILED(hr)) {
        report_hresult(audio, "Start", hr);
        toy_audio_stream_stop(audio);
        return NULL;
    }
    audio->client_started = true;

    if (WaitForSingleObject(audio->ready_event, TOY_AUDIO_START_TIMEOUT_MS) != WAIT_OBJECT_0 ||
            !toy_audio_stream_is_ready(audio)) {
        fprintf(stderr, "%s: WASAPI audio never started streaming\n", audio->name);
        toy_audio_stream_stop(audio);
        return NULL;
    }

    fprintf(stderr, "%s: WASAPI shared stream rate=%u channels=%u buffer=%u frames "
            "endpoint latency=%.1f ms\n",
            audio->name, audio->sample_rate, audio->channels, audio->buffer_frames,
            (double)audio->endpoint_latency_ns / 1e6);
    return audio;
}

void toy_audio_stream_stop(ToyAudioStream *audio)
{
    if (!audio) {
        return;
    }
    destroy_stream(audio);
    free(audio);
}

bool toy_audio_stream_get_latency(const ToyAudioStream *audio, double *seconds)
{
    if (!audio || !seconds || !toy_audio_stream_is_ready(audio)) {
        return false;
    }
    uint64_t latency_ns = atomic_load_explicit(&audio->latency_ns, memory_order_acquire);
    if (latency_ns == 0) {
        return false;
    }
    *seconds = (double)latency_ns / (double)TOY_AUDIO_NSEC_PER_SEC;
    return true;
}

bool toy_audio_stream_is_ready(const ToyAudioStream *audio)
{
    return audio && atomic_load_explicit(&audio->streaming, memory_order_acquire);
}
