/*
 * SerenityOS implementation of toy_audio_stream.h.
 *
 * AudioServer reads each client from a shared ring of fixed 50-frame stereo
 * buffers. A thread of ours keeps that ring filled to a small target and
 * nothing more, so what is queued stays close to the speaker:
 *
 *     render thread  --realtime_enqueue-->  ring  -->  AudioServer mixer  -->  device
 *                                        ~target      512 frames
 *
 * Latency is what sits in the ring plus one mixer buffer. The device's own
 * buffering is not reported to clients, so it is not included.
 *
 * Without a sound device AudioServer still accepts clients, and its mixer then
 * spins a whole CPU with nowhere to write. So no device means no stream.
 */
extern "C" {
#include "toy_audio_stream.h"
}

#include <AK/Atomic.h>
#include <AK/Time.h>
#include <LibAudio/ConnectionToServer.h>
#include <LibAudio/Queue.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Where the kernel lists sound devices, one entry per output. */
static constexpr char const *SER_AUDIO_DEVICES = "/dev/audio";
/* Services/AudioServer/Mixer.h: HARDWARE_BUFFER_SIZE. */
static constexpr double SER_MIXER_FRAMES = 512.0;
/* What the ring is kept filled to. */
static constexpr double SER_TARGET_SECONDS = 0.030;
/* How long the thread sleeps between top-ups: a fraction of the target. */
static constexpr long SER_TOP_UP_NS = 5'000'000;
static constexpr u64 SER_NS_PER_SEC = 1'000'000'000;

struct ToyAudioStream {
    RefPtr<Audio::ConnectionToServer> connection;
    ToyAudioRenderCallback render { nullptr };
    void *userdata { nullptr };
    u32 sample_rate { 0 };
    u32 channels { 0 };

    pthread_t thread {};
    bool thread_started { false };
    Atomic<bool> running { false };
    Atomic<bool> streaming { false };
    Atomic<u64> latency_ns { 0 };
};

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
    return (double)frames / (double)audio.sample_rate;
}

/* One ring buffer: render it, fold to stereo, hand it over. */
static bool ser_push_buffer(ToyAudioStream &audio)
{
    float interleaved[Audio::AUDIO_BUFFER_SIZE * 2] {};
    audio.render(audio.userdata, interleaved, Audio::AUDIO_BUFFER_SIZE, audio.channels);

    Array<Audio::Sample, Audio::AUDIO_BUFFER_SIZE> buffer;
    for(size_t i = 0; i < Audio::AUDIO_BUFFER_SIZE; i++) {
        if(audio.channels == 1) {
            buffer[i] = Audio::Sample(interleaved[i]);
            continue;
        }
        buffer[i] = Audio::Sample(interleaved[i * 2], interleaved[i * 2 + 1]);
    }
    return !audio.connection->realtime_enqueue(buffer).is_error();
}

static void *ser_render_thread(void *data)
{
    auto &audio = *static_cast<ToyAudioStream *>(data);
    timespec pause { 0, SER_TOP_UP_NS };
    double mixer_seconds = SER_MIXER_FRAMES / (double)audio.sample_rate;

    while(audio.running.load()) {
        while(ser_queued_seconds(audio) < SER_TARGET_SECONDS && audio.connection->can_enqueue()) {
            if(!ser_push_buffer(audio)) {
                break;
            }
        }

        double latency = ser_queued_seconds(audio) + mixer_seconds;
        audio.latency_ns.store((u64)(latency * (double)SER_NS_PER_SEC));
        audio.streaming.store(true);
        nanosleep(&pause, nullptr);
    }
    return nullptr;
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
    audio->sample_rate = config->sample_rate;
    audio->channels = config->channels;

    audio->connection->set_self_sample_rate(config->sample_rate);
    audio->connection->async_start_playback();

    audio->running.store(true);
    if(pthread_create(&audio->thread, nullptr, ser_render_thread, audio) != 0) {
        warnln("{}: failed to start the audio thread", config->name);
        toy_audio_stream_stop(audio);
        return nullptr;
    }
    audio->thread_started = true;

    outln(stderr, "{}: AudioServer stream rate={} channels={} target={:.0} ms", config->name, config->sample_rate,
          config->channels, SER_TARGET_SECONDS * 1000.0);
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
    if(audio->connection) {
        audio->connection->async_clear_buffer();
        audio->connection->async_pause_playback();
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
