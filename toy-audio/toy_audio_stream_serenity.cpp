/*
 * SerenityOS implementation of toy_audio_stream.h.
 *
 * AudioServer reads each client from a shared ring of fixed 50-frame stereo
 * buffers. A thread of ours keeps that ring filled to a small target and
 * nothing more, so what is queued stays close to the speaker:
 *
 *     render -> resample -> ring --> AudioServer mixer --> device
 *     toy rate   device rate  ~target    512 frames
 *
 * The ring is filled at the device's own rate. AudioServer would otherwise
 * convert each 50-frame buffer on its own by dropping or repeating samples,
 * a step in the waveform about every buffer: audible crunch. Here the toy's
 * rate is converted by linear interpolation that carries its position from
 * buffer to buffer. A device rate change (asctl) is followed as it happens.
 *
 * Latency is what sits in the ring plus one mixer buffer. The device's own
 * buffering is not reported to clients, so it is not included.
 *
 * Without a sound device AudioServer still accepts clients, and its mixer then
 * spins a whole CPU with nowhere to write. So no device means no stream.
 *
 * With TOY_AUDIO_LOG_DEBUG the thread counts underruns (ring found empty at a
 * top-up) and its longest gap between top-ups, prints a summary every 10 s,
 * and logs each event with a CLOCK_MONOTONIC timestamp to a trace file.
 */
extern "C" {
#include "toy_audio_stream.h"
}

#include <AK/Atomic.h>
#include <AK/Time.h>
#include <LibAudio/ConnectionToManagerServer.h>
#include <LibAudio/ConnectionToServer.h>
#include <LibAudio/Queue.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Where the kernel lists sound devices, one entry per output. */
static constexpr char const *SER_AUDIO_DEVICES = "/dev/audio";
/* Services/AudioServer/Mixer.h: HARDWARE_BUFFER_SIZE. */
static constexpr double SER_MIXER_FRAMES = 512.0;
/* What the ring is kept filled to. The thread asks to sleep 5 ms, but
   Serenity wakes it after ~17 ms, and under load up to ~37 ms (measured in
   QEMU). A 30 ms target ran dry and clicked; this covers the worst gap with
   margin. The toy schedules sounds by the reported latency, so they still
   land on their impacts. */
static constexpr double SER_TARGET_SECONDS = 0.080;
/* How long the thread asks to sleep between top-ups. */
static constexpr long SER_TOP_UP_NS = 5'000'000;
static constexpr u64 SER_NS_PER_SEC = 1'000'000'000;
/* Frames asked of the toy per render call. */
static constexpr size_t SER_RENDER_FRAMES = 64;
static constexpr size_t SER_STEREO = 2;
/* Debug: summary interval, and top-up gaps worth a trace line. */
static constexpr u64 SER_REPORT_NS = 10 * SER_NS_PER_SEC;
static constexpr u64 SER_GAP_TRACE_NS = 15'000'000;

/* Converts the toy's stereo stream to the device rate. Position carries
   across calls, so buffer edges leave no step in the waveform. */
struct SerResampler {
    /* Interleaved stereo at the toy's rate; frames before `position` are spent. */
    float source[(SER_RENDER_FRAMES + SER_STEREO) * SER_STEREO] {};
    size_t source_frames { 0 };
    double position { 0.0 };
};

struct ToyAudioStream {
    RefPtr<Audio::ConnectionToServer> connection;
    RefPtr<Audio::ConnectionToManagerServer> manager;
    ToyAudioRenderCallback render { nullptr };
    void *userdata { nullptr };
    u32 source_rate { 0 };
    u32 channels { 0 };
    Atomic<u32> device_rate { 0 };
    SerResampler resampler;

    pthread_t thread {};
    bool thread_started { false };
    Atomic<bool> running { false };
    Atomic<bool> streaming { false };
    Atomic<u64> latency_ns { 0 };

    ToyAudioLog log { TOY_AUDIO_LOG_QUIET };
    FILE *trace { nullptr };
    u64 underruns { 0 };
    u64 longest_gap_ns { 0 };
    u64 report_start_ns { 0 };
};

static u64 ser_now_ns()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * SER_NS_PER_SEC + (u64)now.tv_nsec;
}

static bool ser_has_audio_device()
{
    DIR *devices = opendir(SER_AUDIO_DEVICES);
    if(!devices) {
        return false;
    }
    bool found = false;
    while(dirent *entry = readdir(devices)) {
        if(entry->d_name[0] != '.') {
            found = true;
            break;
        }
    }
    closedir(devices);
    return found;
}

static double ser_queued_seconds(ToyAudioStream const &audio)
{
    size_t frames = audio.connection->remaining_buffers() * Audio::AUDIO_BUFFER_SIZE;
    return (double)frames / (double)audio.device_rate.load();
}

/* Drop spent frames, then append one render call's worth, as stereo. */
static void ser_resampler_refill(ToyAudioStream &audio)
{
    auto &r = audio.resampler;
    size_t spent = (size_t)r.position;
    memmove(r.source, r.source + spent * SER_STEREO, (r.source_frames - spent) * SER_STEREO * sizeof(float));
    r.source_frames -= spent;
    r.position -= (double)spent;

    float rendered[SER_RENDER_FRAMES * SER_STEREO] {};
    audio.render(audio.userdata, rendered, SER_RENDER_FRAMES, audio.channels);

    float *dst = r.source + r.source_frames * SER_STEREO;
    for(size_t i = 0; i < SER_RENDER_FRAMES; i++) {
        bool mono = audio.channels == 1;
        dst[i * 2] = rendered[mono ? i : i * 2];
        dst[i * 2 + 1] = rendered[mono ? i : i * 2 + 1];
    }
    r.source_frames += SER_RENDER_FRAMES;
}

/* One ring buffer at the device rate: interpolate, hand it over. */
static bool ser_push_buffer(ToyAudioStream &audio)
{
    auto &r = audio.resampler;
    double step = (double)audio.source_rate / (double)audio.device_rate.load();

    Array<Audio::Sample, Audio::AUDIO_BUFFER_SIZE> buffer;
    for(size_t i = 0; i < Audio::AUDIO_BUFFER_SIZE; i++) {
        /* Interpolation reads the frame at position and the one after it. */
        while((size_t)r.position + 1 >= r.source_frames) {
            ser_resampler_refill(audio);
        }

        size_t index = (size_t)r.position;
        float t = (float)(r.position - (double)index);
        float const *a = r.source + index * SER_STEREO;
        float const *b = a + SER_STEREO;
        buffer[i] = Audio::Sample(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
        r.position += step;
    }
    return !audio.connection->realtime_enqueue(buffer).is_error();
}

static void ser_debug_top_up(ToyAudioStream &audio, u64 now, u64 last_top_up)
{
    if(audio.log != TOY_AUDIO_LOG_DEBUG || last_top_up == 0) {
        return;
    }

    u64 gap = now - last_top_up;
    audio.longest_gap_ns = max(audio.longest_gap_ns, gap);
    bool empty = audio.connection->remaining_buffers() == 0;
    if(empty) {
        audio.underruns++;
    }
    if(audio.trace && (empty || gap >= SER_GAP_TRACE_NS)) {
        fprintf(audio.trace, "%c %llu %llu\n", empty ? 'U' : 'G', (unsigned long long)now, (unsigned long long)gap);
    }

    if(now - audio.report_start_ns < SER_REPORT_NS) {
        return;
    }
    fprintf(stderr, "[audio] 10s: underruns=%llu longest top-up gap=%.1f ms device=%u Hz\n",
            (unsigned long long)audio.underruns, (double)audio.longest_gap_ns / 1e6, audio.device_rate.load());
    if(audio.trace) {
        fflush(audio.trace);
    }
    audio.underruns = 0;
    audio.longest_gap_ns = 0;
    audio.report_start_ns = now;
}

static void *ser_render_thread(void *data)
{
    auto &audio = *static_cast<ToyAudioStream *>(data);

    /* A late top-up is a click; a late frame is merely late. LibAudio runs its
       own writer thread at this priority for the same reason. */
    sched_param scheduling {};
    scheduling.sched_priority = THREAD_PRIORITY_MAX;
    if(pthread_setschedparam(pthread_self(), 0, &scheduling) != 0) {
        warnln("audio: could not raise the thread priority");
    }

    timespec pause { 0, SER_TOP_UP_NS };
    u64 last_top_up = 0;
    audio.report_start_ns = ser_now_ns();

    while(audio.running.load()) {
        u64 now = ser_now_ns();
        /* Only once playing: before the first fill an empty ring is expected. */
        if(audio.streaming.load()) {
            ser_debug_top_up(audio, now, last_top_up);
        }
        last_top_up = now;

        while(ser_queued_seconds(audio) < SER_TARGET_SECONDS && audio.connection->can_enqueue()) {
            if(!ser_push_buffer(audio)) {
                break;
            }
        }

        u32 device_rate = audio.device_rate.load();
        double latency = ser_queued_seconds(audio) + SER_MIXER_FRAMES / (double)device_rate;
        audio.latency_ns.store((u64)(latency * (double)SER_NS_PER_SEC));
        audio.streaming.store(true);
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}

/* The device rate, or the toy's own if the manager cannot say. Also follows
   later changes: the ring switches rate with the device. */
static void ser_follow_device_rate(ToyAudioStream &audio, char const *name)
{
    audio.device_rate.store(audio.source_rate);
    auto manager_or_error = Audio::ConnectionToManagerServer::try_create();
    if(manager_or_error.is_error()) {
        warnln("{}: cannot read the device rate ({}); AudioServer will resample", name, manager_or_error.error());
        return;
    }

    audio.manager = manager_or_error.release_value();
    audio.device_rate.store(audio.manager->get_device_sample_rate());
    audio.manager->on_device_sample_rate_change = [&audio](u32 sample_rate) {
        audio.device_rate.store(sample_rate);
        audio.connection->set_self_sample_rate(sample_rate);
    };
}

ToyAudioStream *toy_audio_stream_start(ToyAudioStreamConfig const *config)
{
    if(!config || !config->render || config->sample_rate == 0 || config->channels < 1 || config->channels > 2) {
        return nullptr;
    }

    if(!ser_has_audio_device()) {
        warnln("{}: no sound device in {}", config->name, SER_AUDIO_DEVICES);
        return nullptr;
    }

    auto connection_or_error = Audio::ConnectionToServer::try_create();
    if(connection_or_error.is_error()) {
        warnln("{}: no AudioServer: {}", config->name, connection_or_error.error());
        return nullptr;
    }

    auto *audio = new(nothrow) ToyAudioStream;
    if(!audio) {
        return nullptr;
    }
    audio->connection = connection_or_error.release_value();
    audio->render = config->render;
    audio->userdata = config->userdata;
    audio->source_rate = config->sample_rate;
    audio->channels = config->channels;
    audio->log = config->log;

    ser_follow_device_rate(*audio, config->name);
    audio->connection->set_self_sample_rate(audio->device_rate.load());
    audio->connection->async_start_playback();

    if(audio->log == TOY_AUDIO_LOG_DEBUG) {
        auto path = ByteString::formatted("/tmp/{}_audio_{}.log", config->name, getpid());
        audio->trace = fopen(path.characters(), "w");
        if(audio->trace) {
            warnln("[debug] audio trace:  {}", path);
        }
    }

    audio->running.store(true);
    if(pthread_create(&audio->thread, nullptr, ser_render_thread, audio) != 0) {
        warnln("{}: failed to start the audio thread", config->name);
        toy_audio_stream_stop(audio);
        return nullptr;
    }
    audio->thread_started = true;

    outln(stderr, "{}: AudioServer stream toy={} Hz device={} Hz channels={} target={:.0} ms", config->name,
          audio->source_rate, audio->device_rate.load(), config->channels, SER_TARGET_SECONDS * 1000.0);
    return audio;
}

void toy_audio_stream_stop(ToyAudioStream *audio)
{
    if(!audio) {
        return;
    }
    audio->running.store(false);
    if(audio->thread_started) {
        pthread_join(audio->thread, nullptr);
    }
    if(audio->manager) {
        audio->manager->on_device_sample_rate_change = nullptr;
    }
    if(audio->connection) {
        audio->connection->async_clear_buffer();
        audio->connection->async_pause_playback();
    }
    if(audio->trace) {
        fclose(audio->trace);
    }
    delete audio;
}

bool toy_audio_stream_get_latency(ToyAudioStream const *audio, double *seconds)
{
    if(!audio || !seconds || !toy_audio_stream_is_ready(audio)) {
        return false;
    }
    u64 latency_ns = audio->latency_ns.load();
    if(latency_ns == 0) {
        return false;
    }
    *seconds = (double)latency_ns / (double)SER_NS_PER_SEC;
    return true;
}

bool toy_audio_stream_is_ready(ToyAudioStream const *audio)
{
    return audio && audio->streaming.load();
}
