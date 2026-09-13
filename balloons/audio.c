#define _GNU_SOURCE
#include "audio.h"
#include "balloon_pop_pcm.h"
#include "thunder_synth.h"
#include "toy_audio.h"
#include "toy_audio_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define PI 3.14159265358979323846f

#define SAMPLE_RATE TOY_AUDIO_SAMPLE_RATE

#define AUDIO_NORMALIZE_TARGET TOY_AUDIO_NORMALIZE_TARGET
#define AUDIO_THUMP_NORMALIZE_TARGET 0.22f
#define POP_SOUND_SECONDS 0.35f
#define THUMP_SOUND_SECONDS 0.8f
#define THUNDER_BASE_VOLUME    0.90f
#define THUNDER_DISTANCE_ATTEN 0.35f
#define THUNDER_CLIP_COUNT     4
#define THUNDER_CLIP_SECONDS   12
#define WHOOSH_MAX_GUST_SECONDS 9.0f
#define WHOOSH_TAIL_SECONDS 3.0f
#define WHOOSH_CAPACITY_SAMPLES \
    ((int)((WHOOSH_MAX_GUST_SECONDS + WHOOSH_TAIL_SECONDS) * SAMPLE_RATE))
#define BREEZE_EASE_RATE 1.6f
#define LAYER_DETUNE TOY_AUDIO_LAYER_DETUNE
#define AUDIO_SHUTDOWN_CHECK_MS 100u
#define AUDIO_SHUTDOWN_MAX_MS 2000u
#define AUDIO_SHUTDOWN_FADE_SECONDS 0.2f

typedef ToySamplePair SamplePair;

#define POP_VARIATIONS 8
static SamplePair g_pop_variations[POP_VARIATIONS] = {
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true },
    { NULL, NULL, 0, true }
};

static SamplePair g_thunder[THUNDER_CLIP_COUNT] = {0};

typedef struct {
    SamplePair bounce;   
    SamplePair thump;    
    SamplePair whoosh;   
    float pending_size_scale;
    float current_size_scale;
} AudioState;

static AudioState audio_state = {
    .bounce = { NULL, NULL, 0, true },
    .thump = { NULL, NULL, 0, true },
    .whoosh = { NULL, NULL, 0, false },
    .pending_size_scale = 1.0f,
    .current_size_scale = 1.0f,
};

static ToyMixer g_mixer;

static bool audio_device_open = false;
static BounceSoundStyle g_bounce_sound_style = BOUNCE_SOUND_POINGO;

static ToyBounceStyle toy_style(void) {
    return g_bounce_sound_style == BOUNCE_SOUND_NOSTALGIA
               ? TOY_BOUNCE_NOSTALGIA : TOY_BOUNCE_POINGO;
}

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_lock(void)   { pthread_mutex_lock(&g_audio_lock); }
static void audio_unlock(void) { pthread_mutex_unlock(&g_audio_lock); }

static pthread_mutex_t g_synth_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_synth_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_synth_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_synth_thread;
static bool g_synth_thread_running = false;
static bool g_synth_quit = false;
static bool g_synth_pregen_pending = false;
static int g_synth_trace = 0;

typedef struct {
    bool valid;
    float strength01;
    float duration;
    float gain;
    bool is_gust;     
} WhooshJob;
static WhooshJob g_whoosh_job;      
static bool g_gust_ready = false;   
static float g_gust_ready_latency = 0.0f;

static ToyAudioStream *g_audio_stream = NULL;

static void generate_pop_sound(float * restrict buffer, int sample_count,
                               float detune, float size_scale) {
    float playback_speed = powf(1.0f / size_scale, 0.4f) * detune;

    for (int i = 0; i < sample_count; i++) {
        float read_pos = i * playback_speed;
        int idx = (int)read_pos;
        float frac = read_pos - idx;

        float val = 0.0f;
        if (idx < BALLOON_POP_PCM_LENGTH - 1) {
            val = balloon_pop_pcm[idx] * (1.0f - frac) + balloon_pop_pcm[idx + 1] * frac;
        } else if (idx == BALLOON_POP_PCM_LENGTH - 1) {
            val = balloon_pop_pcm[idx];
        }

        float env = 1.0f;
        int fade_samples = (int)(0.005f * SAMPLE_RATE); 
        if (i > sample_count - fade_samples) {
            env = (float)(sample_count - i) / fade_samples;
        }

        buffer[i] = val * env;
    }


    toy_audio_micro_reverb(buffer, sample_count, 1.0f, 0.055f, 0.45f);
}

static void generate_thump_sound(float * restrict buffer, int sample_count,
                                 float detune, float size_scale) {
    float thud_freq = 68.0f * powf(1.0f / size_scale, 0.3f) * detune;
    if (thud_freq > 220.0f) thud_freq = 220.0f;

    float attack_time = 0.004f;                                  
    float decay_time = 0.10f + 0.09f * fminf(size_scale, 1.5f);  

    const float SILENCE_THRESHOLD = 0.001f;
    float filter1 = 0.0f;
    float filter2 = 0.0f;
    float filter3 = 0.0f;

    for (int i = 0; i < sample_count; i++) {
        float t = i / (float)SAMPLE_RATE;

        float env;
        if (t < attack_time) {
            env = t / attack_time;
        } else {
            env = expf(-(t - attack_time) / decay_time);
        }

        if (t > attack_time && env < SILENCE_THRESHOLD) {
            memset(buffer + i, 0, (sample_count - i) * sizeof(float));
            break;
        }

        float noise = (float)(((double)rand() / (double)RAND_MAX) * 2.0 - 1.0);

        float cutoff = 0.055f;
        filter1 = filter1 + cutoff * (noise - filter1) + 1e-18f;
        filter2 = filter2 + cutoff * (filter1 - filter2) + 1e-18f;
        filter3 = filter3 + cutoff * (filter2 - filter3) + 1e-18f;

        float thud = sinf(2.0f * PI * thud_freq * t) * 0.5f;

        float mixed = filter3 * 1.1f + thud;
        buffer[i] = mixed * env;
    }

    toy_audio_micro_reverb(buffer, sample_count, 1.8f, 0.11f, 0.35f);
}

static void generate_whoosh_sound(float * restrict buffer, int sample_count,
                                  float detune, float strength01,
                                  float gust_seconds) {
    float gust_env = 1.0f - expf(-BREEZE_EASE_RATE * gust_seconds);

    float filter1 = 0.0f;
    float filter2 = 0.0f;
    float filter3 = 0.0f;
    float filter4 = 0.0f;
    float p0 = detune * 17.0f;
    float p1 = detune * 5.0f;

    uint32_t rand_state = (uint32_t)rand();
    if (rand_state == 0) rand_state = 0x12345678U;

    const float exp_multiplier = expf(-BREEZE_EASE_RATE / (float)SAMPLE_RATE);
    float exp_acc = 1.0f;
    int transition_index = (int)(gust_seconds * (float)SAMPLE_RATE);

    float f0 = 0.23f * detune;
    float f1 = 0.117f;
    float dtheta0 = 2.0f * PI * f0 / (float)SAMPLE_RATE;
    float dtheta1 = 2.0f * PI * f1 / (float)SAMPLE_RATE;
    float c0 = cosf(dtheta0), s0 = sinf(dtheta0);
    float c1 = cosf(dtheta1), s1 = sinf(dtheta1);
    float z0_re = cosf(p0), z0_im = sinf(p0);
    float z1_re = cosf(p1), z1_im = sinf(p1);

    for (int i = 0; i < sample_count; i++) {
        float env;
        if (i < transition_index) {
            env = 1.0f - exp_acc;
            exp_acc *= exp_multiplier;
        } else {
            if (i == transition_index) {
                exp_acc = 1.0f;
            }
            env = gust_env * exp_acc;
            exp_acc *= exp_multiplier;
        }

        float aero = env * sqrtf(fmaxf(0.0f, env));

        rand_state ^= rand_state << 13;
        rand_state ^= rand_state >> 17;
        rand_state ^= rand_state << 5;
        float noise = (float)(int32_t)rand_state * (1.0f / 2147483648.0f);

        float tmp0_re = z0_re * c0 - z0_im * s0;
        z0_im = z0_re * s0 + z0_im * c0;
        z0_re = tmp0_re;

        float tmp1_re = z1_re * c1 - z1_im * s1;
        z1_im = z1_re * s1 + z1_im * c1;
        z1_re = tmp1_re;

        if ((i & 1023) == 0) {
            float len0_sq = z0_re * z0_re + z0_im * z0_im;
            float scale0 = 1.5f - 0.5f * len0_sq;
            z0_re *= scale0;
            z0_im *= scale0;

            float len1_sq = z1_re * z1_re + z1_im * z1_im;
            float scale1 = 1.5f - 0.5f * len1_sq;
            z1_re *= scale1;
            z1_im *= scale1;
        }

        float wobble = 0.6f * z0_im + 0.4f * z1_im;

        float cutoff = 0.015f +
                       (0.030f + 0.045f * strength01) * env *
                       (0.85f + 0.15f * wobble);
        filter1 = filter1 + cutoff * (noise - filter1) + 1e-18f;
        filter2 = filter2 + cutoff * (filter1 - filter2) + 1e-18f;
        filter3 = filter3 + cutoff * (filter2 - filter3) + 1e-18f;
        filter4 = filter4 + cutoff * (filter3 - filter4) + 1e-18f;

        float body = filter4;
        float mids = (filter2 - filter4) * (0.35f + 0.35f * env);
        buffer[i] = (body * 0.9f + mids) * aero;
    }

    toy_audio_micro_reverb(buffer, sample_count, 2.2f, 0.13f, 0.30f);

    int fade = (int)(0.1f * SAMPLE_RATE);
    if (fade > sample_count) fade = sample_count;
    for (int i = sample_count - fade; i < sample_count; i++) {
        buffer[i] *= (float)(sample_count - i) / (float)fade;
    }
}

static bool audio_is_perceptually_quiet(void) {
    if (!audio_device_open) {
        return true;
    }

    bool quiet = false;
    audio_lock();
    quiet = toy_mixer_is_quiet(&g_mixer);
    audio_unlock();
    return quiet;
}

void mark_sounds_dirty(float size_scale) {
    audio_state.bounce.dirty = true;
    for (int p = 0; p < POP_VARIATIONS; ++p) {
        g_pop_variations[p].dirty = true;
    }
    audio_state.thump.dirty = true;
    audio_state.pending_size_scale = fmaxf(size_scale, 0.01f);
}

void set_bounce_sound_style(BounceSoundStyle style) {
    if (g_bounce_sound_style == style) {
        return;
    }
    g_bounce_sound_style = style;
    float size_scale = audio_state.bounce.dirty ? audio_state.pending_size_scale : audio_state.current_size_scale;
    mark_sounds_dirty(fmaxf(size_scale, 0.01f));
}

static void start_voice(const SamplePair *pair, float base_volume) {
    ToyVoiceClaim claim;
    audio_lock();
    bool claimed = toy_mixer_claim_voice(&g_mixer, pair->length, &claim);
    audio_unlock();
    if (!claimed) return;

    /*
     * Sample synthesis and snapshot copying share this lock so a regenerated
     * source cannot change underneath the unlocked copy.
     */
    pthread_mutex_lock(&g_synth_lock);
    bool copied = toy_mixer_copy_claimed_voice(&g_mixer, claim, pair);
    pthread_mutex_unlock(&g_synth_lock);

    audio_lock();
    if (!copied ||
        !toy_mixer_commit_voice(&g_mixer, claim, pair->length,
                                base_volume, 1.0f, 1.0f)) {
        toy_mixer_cancel_voice(&g_mixer, claim);
    }
    audio_unlock();
}

static float base_volume_for(int volume, float size_scale) {
    float size_factor = 0.7f + 0.3f * fminf(size_scale, 1.0f);
    float velocity_factor = fminf(volume / 32.0f, 1.0f);
    velocity_factor = fmaxf(velocity_factor, 0.6f);  
    return size_factor * velocity_factor;
}

static void ensure_bounce_sound(void) {
    if (!audio_state.bounce.dirty) return;
    pthread_mutex_lock(&g_synth_lock);
    if (audio_state.bounce.dirty) {
        float size_scale = fmaxf(audio_state.pending_size_scale, 0.05f);

        toy_audio_generate_bounce_pair(&audio_state.bounce, size_scale,
                                       toy_style());

        audio_lock();
        audio_state.current_size_scale = size_scale;
        audio_state.bounce.dirty = false;
        audio_unlock();
    }
    pthread_mutex_unlock(&g_synth_lock);
}

static void ensure_pop_sounds(void) {
    if (!g_pop_variations[0].dirty) return;
    pthread_mutex_lock(&g_synth_lock);
    if (g_pop_variations[0].dirty) {
        float size_scale = fmaxf(audio_state.pending_size_scale, 0.05f);
        for (int p = 0; p < POP_VARIATIONS; ++p) {
            float rand_detune = 0.85f + 0.30f * ((float)rand() / (float)RAND_MAX);
            generate_pop_sound(g_pop_variations[p].data1, g_pop_variations[p].length, rand_detune, size_scale);
            toy_audio_normalize(g_pop_variations[p].data1, g_pop_variations[p].length, AUDIO_NORMALIZE_TARGET);
            generate_pop_sound(g_pop_variations[p].data2, g_pop_variations[p].length, rand_detune * LAYER_DETUNE, size_scale);
            toy_audio_normalize(g_pop_variations[p].data2, g_pop_variations[p].length, AUDIO_NORMALIZE_TARGET);
        }
        audio_lock();
        audio_state.current_size_scale = size_scale;
        for (int p = 0; p < POP_VARIATIONS; ++p) {
            g_pop_variations[p].dirty = false;
        }
        audio_unlock();
    }
    pthread_mutex_unlock(&g_synth_lock);
}

static void ensure_thump_sound(void) {
    if (!audio_state.thump.dirty) return;
    pthread_mutex_lock(&g_synth_lock);
    if (audio_state.thump.dirty) {
        float size_scale = fmaxf(audio_state.pending_size_scale, 0.05f);
        generate_thump_sound(audio_state.thump.data1, audio_state.thump.length, 1.0f, size_scale);
        toy_audio_normalize(audio_state.thump.data1, audio_state.thump.length, AUDIO_THUMP_NORMALIZE_TARGET);
        generate_thump_sound(audio_state.thump.data2, audio_state.thump.length, LAYER_DETUNE, size_scale);
        toy_audio_normalize(audio_state.thump.data2, audio_state.thump.length, AUDIO_THUMP_NORMALIZE_TARGET);
        audio_lock();
        audio_state.current_size_scale = size_scale;
        audio_state.thump.dirty = false;
        audio_unlock();
    }
    pthread_mutex_unlock(&g_synth_lock);
}

void play_bounce_sound(int volume) {
    if (!audio_device_open) {
        return;
    }
    ensure_bounce_sound();
    audio_lock();
    float size_scale = fmaxf(audio_state.current_size_scale, 0.05f);
    audio_unlock();
    start_voice(&audio_state.bounce, base_volume_for(volume, size_scale));
}

void play_pop_sound(int volume) {
    if (!audio_device_open) {
        return;
    }
    ensure_pop_sounds();
    int idx = rand() % POP_VARIATIONS;
    audio_lock();
    float size_scale = fmaxf(audio_state.pending_size_scale, 0.05f);
    audio_unlock();
    start_voice(&g_pop_variations[idx], base_volume_for(volume, size_scale));
}

void play_thump_sound(int volume) {
    if (!audio_device_open) {
        return;
    }
    ensure_thump_sound();
    audio_lock();
    float size_scale = fmaxf(audio_state.current_size_scale, 0.05f);
    audio_unlock();
    start_voice(&audio_state.thump, base_volume_for(volume, size_scale));
}

void play_thunder_sound(float distance01) {
    if (!audio_device_open) {
        return;
    }
    float d = fminf(fmaxf(distance01, 0.0f), 1.0f);
    int idx = rand() % THUNDER_CLIP_COUNT;
    if (g_thunder[idx].length <= 0) {
        return;
    }
    float volume = THUNDER_BASE_VOLUME * (1.0f - THUNDER_DISTANCE_ATTEN * d);
    start_voice(&g_thunder[idx], volume);
}


static float output_latency_seconds(void) {
    double seconds = 0.0;
    if (toy_audio_stream_get_latency(g_audio_stream, &seconds)) {
        return fminf(fmaxf((float)seconds, 0.0f), 1.0f);
    }
    return 0.0f;
}

bool play_whoosh_async(float strength01, float duration_seconds, float gain,
                       bool is_gust) {
    if (!audio_device_open || !g_synth_thread_running) {
        return false;
    }
    strength01 = fmaxf(0.0f, fminf(strength01, 1.0f));
    duration_seconds = fmaxf(0.5f, fminf(duration_seconds, WHOOSH_MAX_GUST_SECONDS));

    pthread_mutex_lock(&g_synth_queue_lock);
    g_whoosh_job.valid = true;
    g_whoosh_job.strength01 = strength01;
    g_whoosh_job.duration = duration_seconds;
    g_whoosh_job.gain = gain;
    g_whoosh_job.is_gust = is_gust;
    g_gust_ready = false;   
    pthread_cond_signal(&g_synth_cond);
    pthread_mutex_unlock(&g_synth_queue_lock);
    return true;
}

bool whoosh_gust_poll(float *delay_seconds) {
    bool ready = false;
    pthread_mutex_lock(&g_synth_queue_lock);
    if (g_gust_ready) {
        ready = true;
        *delay_seconds = g_gust_ready_latency;
        g_gust_ready = false;
    }
    pthread_mutex_unlock(&g_synth_queue_lock);
    return ready;
}

void whoosh_gust_cancel(void) {
    pthread_mutex_lock(&g_synth_queue_lock);
    g_gust_ready = false;
    if (g_whoosh_job.valid && g_whoosh_job.is_gust) {
        g_whoosh_job.valid = false;
    }
    pthread_mutex_unlock(&g_synth_queue_lock);
}

void whoosh_stop_playing(void) {
    audio_lock();
    for (int i = 0; i < TOY_AUDIO_MAX_VOICES; ++i) {
        if (g_mixer.voices[i].playing &&
            audio_state.whoosh.length > 0 &&
            g_mixer.voices[i].snapshot_length == audio_state.whoosh.length) {
            g_mixer.voices[i].playing = false;
        }
    }
    audio_unlock();
}

void audio_pregen_async(void) {
    if (!audio_device_open || !g_synth_thread_running) {
        return;
    }
    pthread_mutex_lock(&g_synth_queue_lock);
    g_synth_pregen_pending = true;
    pthread_cond_signal(&g_synth_cond);
    pthread_mutex_unlock(&g_synth_queue_lock);
}

void adjust_master_volume(float delta) {
    if (delta == 0.0f) {
        return;
    }
    audio_lock();
    toy_mixer_adjust_volume(&g_mixer, delta);
    audio_unlock();
}

void set_master_mute(bool mute) {
    audio_lock();
    toy_mixer_set_mute(&g_mixer, mute);
    audio_unlock();
}

void toggle_master_mute(void) {
    set_master_mute(!g_mixer.muted);
}

bool audio_is_muted(void) {
    return g_mixer.muted;
}


static void balloons_render(void *userdata, float *output,
                            uint32_t nframes, uint32_t channels) {
    (void)userdata;
    (void)channels;
    audio_lock();
    toy_mixer_render_stereo(&g_mixer, output, (int)nframes, false);
    audio_unlock();
}

static void *synth_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_synth_queue_lock);
        while (!g_synth_quit && !g_synth_pregen_pending && !g_whoosh_job.valid) {
            pthread_cond_wait(&g_synth_cond, &g_synth_queue_lock);
        }
        if (g_synth_quit) {
            pthread_mutex_unlock(&g_synth_queue_lock);
            break;
        }
        if (g_synth_pregen_pending) {
            g_synth_pregen_pending = false;
            pthread_mutex_unlock(&g_synth_queue_lock);
            ensure_pop_sounds();
            ensure_thump_sound();
            ensure_bounce_sound();
            continue;
        }
        WhooshJob job = g_whoosh_job;
        g_whoosh_job.valid = false;
        pthread_mutex_unlock(&g_synth_queue_lock);

        struct timespec t0 = {0}, t1 = {0};
        if (g_synth_trace) clock_gettime(CLOCK_MONOTONIC, &t0);

        int length = (int)((job.duration + WHOOSH_TAIL_SECONDS) * SAMPLE_RATE);
        if (length > WHOOSH_CAPACITY_SAMPLES) {
            length = WHOOSH_CAPACITY_SAMPLES;
        }
        pthread_mutex_lock(&g_synth_lock);
        generate_whoosh_sound(audio_state.whoosh.data1, length, 1.0f,
                              job.strength01, job.duration);
        generate_whoosh_sound(audio_state.whoosh.data2, length, LAYER_DETUNE,
                              job.strength01, job.duration);
        float target = (0.05f + 0.06f * job.strength01) *
                       fmaxf(0.1f, fminf(job.gain, 4.0f));
        toy_audio_normalize(audio_state.whoosh.data1, length, target);
        toy_audio_normalize(audio_state.whoosh.data2, length, target);
        pthread_mutex_unlock(&g_synth_lock);

        pthread_mutex_lock(&g_synth_queue_lock);
        bool stale = g_whoosh_job.valid || g_synth_quit;
        pthread_mutex_unlock(&g_synth_queue_lock);
        if (stale) continue;

        audio_lock();
        audio_state.whoosh.length = length;
        audio_unlock();
        start_voice(&audio_state.whoosh, 1.0f);

        if (g_synth_trace) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fprintf(stderr, "[audio] whoosh synth %.1f ms (gust=%d, %.1fs)\n",
                    (t1.tv_sec - t0.tv_sec) * 1e3 +
                    (t1.tv_nsec - t0.tv_nsec) * 1e-6,
                    job.is_gust, job.duration);
        }

        if (job.is_gust) {
            float latency = output_latency_seconds();
            pthread_mutex_lock(&g_synth_queue_lock);
            g_gust_ready = true;
            g_gust_ready_latency = latency;
            pthread_mutex_unlock(&g_synth_queue_lock);
        }
    }
    return NULL;
}

__attribute__((cold))
void audio_shutdown(void) {
    if (!audio_device_open) {
        return;
    }

    if (g_synth_thread_running) {
        pthread_mutex_lock(&g_synth_queue_lock);
        g_synth_quit = true;
        pthread_cond_signal(&g_synth_cond);
        pthread_mutex_unlock(&g_synth_queue_lock);
        pthread_join(g_synth_thread, NULL);   
        g_synth_thread_running = false;
        g_synth_quit = false;
        g_synth_pregen_pending = false;
        g_whoosh_job.valid = false;
        g_gust_ready = false;
    }

    toy_audio_stream_stop(g_audio_stream);
    g_audio_stream = NULL;
    audio_device_open = false;

    SamplePair *pairs[3] = { &audio_state.bounce,
                             &audio_state.thump, &audio_state.whoosh };
    for (int p = 0; p < 3; ++p) {
        toy_sample_pair_free(pairs[p]);
    }
    audio_state.whoosh.dirty = false;  
    for (int p = 0; p < POP_VARIATIONS; ++p) {
        toy_sample_pair_free(&g_pop_variations[p]);
    }
    for (int c = 0; c < THUNDER_CLIP_COUNT; ++c) {
        toy_sample_pair_free(&g_thunder[c]);
    }
    toy_mixer_reset(&g_mixer);
    toy_audio_release_scratch();
    audio_state.pending_size_scale = 1.0f;
    audio_state.current_size_scale = 1.0f;
}

static bool build_thunder_clip(SamplePair *pair, int clip_index) {
    const int out_len = THUNDER_CLIP_SECONDS * SAMPLE_RATE;
    if (!toy_sample_pair_alloc(pair, out_len)) {
        return false;
    }
    uint32_t seed = 0x7f4a7c15u + (uint32_t)clip_index * 0x9e3779b9u;
    thunder_synthesize(pair->data1, out_len, SAMPLE_RATE, seed, clip_index, 0);
    thunder_synthesize(pair->data2, out_len, SAMPLE_RATE, seed, clip_index, 1);
    pair->dirty = false;
    return true;
}

typedef struct {
    int index;
    bool ok;
} ThunderBuildJob;

static void *build_thunder_clip_worker(void *userdata) {
    ThunderBuildJob *job = userdata;
    job->ok = build_thunder_clip(&g_thunder[job->index], job->index);
    return NULL;
}

bool audio_init(void) {
    if (audio_device_open) {
        return true;
    }

    toy_mixer_init(&g_mixer, TOY_AUDIO_DEFAULT_VOLUME);
    audio_device_open = true;

    if (!toy_sample_pair_alloc(&audio_state.bounce,
                               toy_audio_bounce_pair_length()) ||
        !toy_sample_pair_alloc(&audio_state.thump,
                               (int)(THUMP_SOUND_SECONDS * SAMPLE_RATE)) ||
        !toy_sample_pair_alloc(&audio_state.whoosh, WHOOSH_CAPACITY_SAMPLES)) {
        fprintf(stderr, "balloons: failed to allocate audio sample buffers\n");
        audio_shutdown();
        return false;
    }
    audio_state.whoosh.dirty = false;  

    for (int p = 0; p < POP_VARIATIONS; ++p) {
        if (!toy_sample_pair_alloc(&g_pop_variations[p],
                                   (int)(POP_SOUND_SECONDS * SAMPLE_RATE))) {
            fprintf(stderr, "balloons: failed to allocate audio sample buffers\n");
            audio_shutdown();
            return false;
        }
    }

    pthread_t thunder_threads[THUNDER_CLIP_COUNT];
    ThunderBuildJob thunder_jobs[THUNDER_CLIP_COUNT];
    bool thunder_started[THUNDER_CLIP_COUNT] = { false };
    for (int c = 0; c < THUNDER_CLIP_COUNT; ++c) {
        thunder_jobs[c] = (ThunderBuildJob){ .index = c, .ok = false };
        if (pthread_create(&thunder_threads[c], NULL,
                           build_thunder_clip_worker, &thunder_jobs[c]) != 0) {
            break;
        }
        thunder_started[c] = true;
    }
    bool thunder_ok = true;
    for (int c = 0; c < THUNDER_CLIP_COUNT; ++c) {
        if (thunder_started[c]) {
            pthread_join(thunder_threads[c], NULL);
        } else {
            thunder_jobs[c].ok = build_thunder_clip(&g_thunder[c], c);
        }
        if (!thunder_jobs[c].ok) {
            fprintf(stderr, "balloons: failed to build thunder clip %d\n", c);
            thunder_ok = false;
        }
    }
    if (!thunder_ok) {
        audio_shutdown();
        return false;
    }
    if (!toy_mixer_reserve(&g_mixer, THUNDER_CLIP_SECONDS * SAMPLE_RATE) ||
        !toy_audio_reserve_scratch(THUNDER_CLIP_SECONDS * SAMPLE_RATE)) {
        fprintf(stderr, "balloons: failed to allocate audio mixer workspace\n");
        audio_shutdown();
        return false;
    }

    g_synth_trace = getenv("APNGO_TRACE") != NULL;
    if (pthread_create(&g_synth_thread, NULL, synth_thread_main, NULL) == 0) {
        g_synth_thread_running = true;
    } else {
        fprintf(stderr, "balloons: no synthesis thread; breezes will be silent\n");
    }

    ToyAudioStreamConfig stream_config = {
        .name = "balloons",
        .description = "Balloons",
        .sample_rate = SAMPLE_RATE,
        .channels = 2,
        .render = balloons_render,
        .userdata = NULL,
    };
    g_audio_stream = toy_audio_stream_start(&stream_config);
    if (!g_audio_stream) {
        fprintf(stderr,
                "balloons: failed to start audio; quitting\n");
        audio_shutdown();
        return false;
    }
    return true;
}

void audio_shutdown_graceful(void) {
    if (!audio_device_open) {
        return;
    }
    audio_lock();
    toy_mixer_begin_fade_out(&g_mixer, AUDIO_SHUTDOWN_FADE_SECONDS);
    audio_unlock();

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!audio_is_perceptually_quiet()) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned long elapsed_ms =
            (unsigned long)((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_nsec - start.tv_nsec) / 1000000);
        if (elapsed_ms >= AUDIO_SHUTDOWN_MAX_MS) {
            break;
        }
        usleep(AUDIO_SHUTDOWN_CHECK_MS * 1000);
    }
    audio_shutdown();
}
