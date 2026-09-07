
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <stdint.h>

#include "ringmenu.h"
#include "toy_audio.h"
#include "toy_audio_stream.h"
#include <stdatomic.h>

#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

static inline uint64_t poingo_perf_freq(void) { return 1000000000ULL; }
static inline uint64_t poingo_perf_counter(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline uint32_t poingo_ticks_ms(void) {
    return (uint32_t)(poingo_perf_counter() / 1000000ULL);
}

static inline int poingo_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

typedef struct { atomic_int value; } PoingoAtomic;
static inline int poingo_atomic_set(PoingoAtomic *a, int v) {
    return atomic_exchange(&a->value, v);
}
static inline int poingo_atomic_get(PoingoAtomic *a) {
    return atomic_load(&a->value);
}
static inline int poingo_atomic_add(PoingoAtomic *a, int v) {
    return atomic_fetch_add(&a->value, v);
}

typedef int (*PoingoThreadFn)(void *);
typedef struct { PoingoThreadFn fn; void *data; } PoingoThreadTramp;
static void *poingo_thread_trampoline(void *arg) {
    PoingoThreadTramp *tr = arg;
    tr->fn(tr->data);
    free(tr);
    return NULL;
}
static pthread_t *poingo_thread_create(PoingoThreadFn fn, const char *name,
                                       void *data) {
    (void)name;
    PoingoThreadTramp *tr = malloc(sizeof(*tr));
    if (unlikely(!tr)) return NULL;
    tr->fn = fn;
    tr->data = data;
    pthread_t *t = malloc(sizeof(pthread_t));
    if (unlikely(!t)) { free(tr); return NULL; }
    if (unlikely(pthread_create(t, NULL, poingo_thread_trampoline, tr) != 0)) {
        free(t);
        free(tr);
        return NULL;
    }
    return t;
}
static void poingo_thread_wait(pthread_t *t, int *status) {
    (void)status;
    if (!t) return;
    pthread_join(*t, NULL);
    free(t);
}

#define POINGO_MENU_ITEMS 10
#define POINGO_MENU_DISC 36
enum {
    POINGO_MENU_LIGHT = 1,
    POINGO_MENU_DARK = 2,
    POINGO_MENU_NOSTALGIA = 3,
    POINGO_MENU_POINGO = 4,
    POINGO_MENU_NEWCOLOR = 5,
    POINGO_MENU_ADD_BALL = 6,
    POINGO_MENU_REMOVE_BALL = 7,
    POINGO_MENU_MUTE = 8,
    POINGO_MENU_GHOST = 9,
    POINGO_MENU_QUIT = 10,
};
static RingMenu *g_menu = NULL;
static uint32_t *g_menu_scratch = NULL;
static size_t g_menu_scratch_cap = 0;
static int g_menu_rect[4] = {0, 0, -1, -1};
static unsigned int g_menu_tex = 0;
static int g_menu_tex_w = 0, g_menu_tex_h = 0;
/* The colour field costs a per-pixel atan2 + HSL over its whole disc, but it
   depends only on which slot it belongs to and on the rect's placement
   relative to the menu centre - never on the pointer, and never on where the
   menu sits on screen, since the rect is always centred on it. So one cache
   per pen slot outlives both moving the menu and switching pens. */
#define FIELD_CACHE_SLOTS 2
static struct {
    uint32_t *px;
    size_t cap;
    int slot;           /* -1 when nothing is cached here */
    int w, h;           /* rect the field was drawn for */
    int off_x, off_y;   /* that rect's origin, relative to the menu centre */
} g_field_cache[FIELD_CACHE_SLOTS] = {
    { NULL, 0, -1, 0, 0, 0, 0 },
    { NULL, 0, -1, 0, 0, 0, 0 },
};
static bool g_menu_full_upload = true;   /* next redraw must refresh it all */
static int g_menu_tex_slot = -1;         /* whose field the texture holds */
static uint32_t *g_upload_staging = NULL;
static size_t g_upload_cap = 0;
static bool g_menu_was_open = false;
static int g_picker_slot = -1;
static bool g_picker_locked = false;
static uint8_t g_picker_original[3];
static uint8_t g_picker_color[3];
/*
 * A right-button release while the ring menu is open ordinarily means one
 * thing: commit whatever slot the pointer is over. But labwc can also send
 * a right RELEASED that was never a real user decision at all -- one it
 * synthesises to correct a press it delivered live and then had to take
 * back, the moment a right-click-drag gets promoted to the compositor's own
 * screen-bar menu. Wayland gives no way to tell the two apart from the
 * button event alone; both look identical.
 *
 * What does differ is what follows it. A synthetic correction is always
 * paired with a real wl_pointer.leave for this surface -- labwc clears
 * pointer focus right after sending it, because the pointer now belongs to
 * its own menu -- and the two arrive back to back, in the same read of the
 * Wayland socket, well before freerange_pump_events() returns. A genuine
 * user release that lands on a slot is not followed by a leave at all; the
 * pointer is still sitting right there.
 *
 * So the release is deferred rather than acted on immediately: this flag
 * records that one is pending, and freerange_resolve_pending_ring_release()
 * -- called once per frame, after the whole batch a pump can see has been
 * drained -- decides commit or cancel based on whether a leave arrived
 * alongside it. Left unset, and the deferred release resolves as an
 * ordinary commit; a real drag that ends by leaving the surface entirely
 * (no release at all yet) is handled separately, directly in
 * freerange_pointer_leave().
 */
static bool g_ring_right_release_pending = false;
static bool g_ring_pointer_left_pending = false;

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include "xdg-shell-client-protocol.h"
#include "ghost_icon.h"


#define GRAVITY 0.00014f
/* A flick can reach window_w * 0.4 per frame and the speed multiplier goes to
   SPEED_MAX, so one step can carry the ball roughly five window widths -- past
   several walls, not just one. Each pass of the collision loop resolves one
   reflection; the cap stops a window narrower than the ball from spinning. */
#define MAX_WALL_PASSES_PER_STEP 8
#define CANVAS_WIDTH 480.0f
#define CANVAS_HEIGHT 270.0f
#define BALL_BASE_DIAMETER 124.0f
#define FLOOR_Y_NORMALIZED 0.9f
#define TARGET_PEAK_Y 0.1f


#define FPS 60
#define ROT_PERIOD 0.5f  // Half second per half rotation

static int g_ball_w = 512;
static int g_ball_h = 512;
#define BALL_W g_ball_w
#define BALL_H g_ball_h

#define FREERANGE_REGEN_BLOCK_SIZE 8


/* Regen runs as fast as the machine allows rather than to a fixed duration.
 * Spreading it over a set number of ticks was an attempt to make it look the
 * same everywhere, and machines are too different for that to work: it was
 * either sluggish on a fast box or still overrunning on a slow one.
 *
 * What is held constant instead is the frame: each tick spends whatever is
 * left of the frame after the rest of the work, minus this margin. A fast
 * machine finishes in a few frames, a Pi takes longer, and neither drops a
 * frame doing it. */
#define FREERANGE_REGEN_HEADROOM_MS 2.0

#ifndef PI
#define PI 3.14159265358979323846f
#endif

#define LON_TILES 16
#define LAT_TILES 8

#define COLOR_LIGHT_R 131
#define COLOR_LIGHT_G 233
#define COLOR_LIGHT_B 255
#define COLOR_DARK_R 41
#define COLOR_DARK_G 173
#define COLOR_DARK_B 255

#define GRID_COLOR_R 99
#define GRID_COLOR_G 95
#define GRID_COLOR_B 97
#define GRID_A_COLOR_R 101
#define GRID_A_COLOR_G 91
#define GRID_A_COLOR_B 134

#define SAMPLE_RATE 48000
#define MASTER_VOLUME_MIN 0.0f
#define MASTER_VOLUME_MAX 1.5f
#define MASTER_VOLUME_STEP 0.05f
#define VOLUME_HUD_HOLD_TIME 1.5f

#define MIN_BOUNCE_SOUND_SPEED_RATIO 0.4f
#define TING_SOUND_SCALE 0.05f
#define TING_VOLUME_SCALE 0.42f
#define POINGO_EXIT_FADE_SECONDS 0.5f
#define AUDIO_PREDICT_SECONDS 2.0f
#define AUDIO_PREDICT_DELAY_SECONDS 0.015f
#define AUDIO_PREDICT_HORIZON_MIN_SECONDS 0.25f
#define AUDIO_PREDICT_HORIZON_MARGIN_SECONDS 0.10f
#define AUDIO_PREDICT_HORIZON_DELAY_MULTIPLIER 2.0f
#define AUDIO_PREDICT_HORIZON_MAX_SECONDS 0.75f
#define AUDIO_PREDICT_ONSET_COMPENSATION_SECONDS 0.0125f
#define AUDIO_PREDICT_MAX_EVENTS 128
#define AUDIO_PREDICT_FIXED_STEP_SECONDS (1.0f / 240.0f)
#define AUDIO_PREDICT_EVENT_STALE_SECONDS 0.150f

static float AUDIO_NORMALIZED_MIN = 0.0f;
static const float AUDIO_NORMALIZED_MAX = 10.0f;
static const float AUDIO_RESCALE_MIN = 0.1f;
static const float AUDIO_RESCALE_MAX = 1.0f;
#define VOLUME_HUD_FADE_TIME 0.5f

static volatile sig_atomic_t g_freerange_quit_requested = 0;
static float g_floor_y_normalized = FLOOR_Y_NORMALIZED;
static float g_target_peak_y = TARGET_PEAK_Y;

#define SPEED_MIN 0.1f
#ifdef SPEED_MAX
#undef SPEED_MAX
#endif
#define SPEED_MAX 9.9f
#define SPEED_STEP 0.1f
#define SPEED_HUD_HOLD_TIME 1.5f
#define SPEED_HUD_FADE_TIME 0.5f
#define FREEDOM_BALL_SCALE_MIN 0.25f
#define FREEDOM_BALL_SCALE_MAX 1.5f
#define FREEDOM_BALL_SCALE_STEP 0.05f
#define POINGO_MAX_BALLS 6
#define POINGO_SPAWN_ATTEMPTS 256
#define POINGO_SPAWN_GAP 4.0f
#define POINGO_COLLISION_RESTITUTION 0.94f
#define POINGO_COLLISION_TRAVEL_FRACTION 0.25f
#define POINGO_COLLISION_MAX_STEPS 1024

typedef enum {
    BALL_MODE_POINGO = 0,
    BALL_MODE_NOSTALGIA = 1,
    BALL_MODE_COUNT = 2,
} BallMode;

typedef enum {
    BALL_SOUND_SILENT = 0,
    BALL_SOUND_PLAY = 1,
} BallSound;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    int vx_direction;
    float scale;
    float diameter;
    float diameter_norm;
    uint8_t light_rgb[3];
    uint8_t dark_rgb[3];
    BallMode mode;
} PoingoBall;

static uint8_t g_color_light_rgb[3] = { COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B };
static uint8_t g_color_dark_rgb[3] = { COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B };
static uint8_t g_grid_color_rgb[3] = { GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B };
static uint32_t g_color_drop_light[POINGO_MENU_DISC * POINGO_MENU_DISC];
static uint32_t g_color_drop_dark[POINGO_MENU_DISC * POINGO_MENU_DISC];

static void poingo_menu_set_drop(int slot, const uint8_t rgb[3]) {
    if (!g_menu) return;
    uint32_t *img = slot == 0 ? g_color_drop_light : g_color_drop_dark;
    if (ringmenu_color_drop_into(img, POINGO_MENU_DISC * POINGO_MENU_DISC,
                                 rgb[0], rgb[1], rgb[2], slot,
                                 POINGO_MENU_ITEMS, POINGO_MENU_DISC))
        ringmenu_update_image(g_menu, slot, img);
}

static void poingo_menu_sync_drops(void) {
    poingo_menu_set_drop(0, g_color_light_rgb);
    poingo_menu_set_drop(1, g_color_dark_rgb);
}
static bool g_a_mode = false;
static float g_ball_tilt_deg = 15.0f;

static uint32_t gcd_u32(uint32_t a, uint32_t b);
static void regen_make_shuffled_order(uint32_t *order, uint32_t total);
static inline float clamp_freerange_ball_scale(float value);
static float remap_normalized_scale_to_sound(float normalized_scale);
static void mark_sounds_dirty(float size_scale);



#define KEY_REPEAT_INITIAL_DELAY 0.50f   // Half second before first repeat
#define KEY_REPEAT_INTERVAL      0.12f   // ~8 repeats per second when holding

#define MIN_BRIGHTNESS 0.35f
#define AMBIENT_LIGHT 0.3f
#define DIFFUSE_LIGHT 0.7f

static int g_frame_texture_w = 0;
static int g_frame_texture_h = 0;
static int g_ball_texture_offset_x = 0;
static int g_ball_texture_offset_y = 0;

static float axis_ax, axis_ay, axis_az;
static float axis_ux, axis_uy, axis_uz;
static float axis_vx, axis_vy, axis_vz;

static float get_audio_predict_effective_delay_seconds(void);
static float get_audio_predict_horizon_seconds(void);
static void update_bounce_sound_style_for_mode(void);

typedef struct {
    float lon_norm;
    uint8_t light_rgb[3];
    uint8_t dark_rgb[3];
    uint8_t lat_index;
    uint8_t valid;
    uint8_t padding[2];
} SpherePixelCache;

static SpherePixelCache *sphere_cache = NULL;
static int sphere_cache_size = 0;
static int sphere_cache_capacity_size = 0;

static void invalidate_sphere_pixel_cache(void) {
    /* Keep the storage; color/mode changes only require recomputation. */
    sphere_cache_size = 0;
}

static void release_sphere_pixel_cache(void) {
    free(sphere_cache);
    sphere_cache = NULL;
    sphere_cache_size = 0;
    sphere_cache_capacity_size = 0;
}

static inline void compute_axis_vectors(void) {
    float tilt_rad = g_ball_tilt_deg * PI / 180.0f;
    float s = sinf(tilt_rad);
    float c = cosf(tilt_rad);

    axis_ax = s;
    axis_ay = c;
    axis_az = 0.0f;

    float hx = 0.0f, hy = 0.0f, hz = 1.0f;
    if (fabsf(axis_ax * hx + axis_ay * hy + axis_az * hz) > 0.99f) {
        hx = 1.0f;
        hy = 0.0f;
        hz = 0.0f;
    }

    axis_ux = hy * axis_az - hz * axis_ay;
    axis_uy = hz * axis_ax - hx * axis_az;
    axis_uz = hx * axis_ay - hy * axis_ax;
    float ulen = sqrtf(axis_ux * axis_ux + axis_uy * axis_uy + axis_uz * axis_uz);
    axis_ux /= ulen;
    axis_uy /= ulen;
    axis_uz /= ulen;

    axis_vx = axis_ay * axis_uz - axis_az * axis_uy;
    axis_vy = axis_az * axis_ux - axis_ax * axis_uz;
    axis_vz = axis_ax * axis_uy - axis_ay * axis_ux;
}

static bool set_a_mode_enabled(bool enabled) {
    if (g_a_mode == enabled) {
        return false;
    }
    g_a_mode = enabled;
    if (g_a_mode) {
        g_grid_color_rgb[0] = GRID_A_COLOR_R;
        g_grid_color_rgb[1] = GRID_A_COLOR_G;
        g_grid_color_rgb[2] = GRID_A_COLOR_B;
        g_ball_tilt_deg = -12.5f;
    } else {
        g_grid_color_rgb[0] = GRID_COLOR_R;
        g_grid_color_rgb[1] = GRID_COLOR_G;
        g_grid_color_rgb[2] = GRID_COLOR_B;
        g_ball_tilt_deg = 15.0f;
    }
    compute_axis_vectors();
    update_bounce_sound_style_for_mode();
    invalidate_sphere_pixel_cache();
    return true;
}

static inline float clampf(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

static void pick_palette_colors(float light_value,
                                uint8_t out_light[3], uint8_t out_dark[3],
                                bool allow_white);
static bool ensure_sphere_pixel_cache(int size);
static double get_perf_seconds(uint64_t start_counter, uint64_t end_counter);
static bool g_debug_mode;
static void audio_predict_reset(float now_time);


static void pick_palette_colors(float light_value,
                                uint8_t out_light[3], uint8_t out_dark[3],
                                bool allow_white) {
    (void)allow_white;

    typedef struct {
        uint8_t light[3];
        uint8_t dark[3];
    } BallPalettePair;

    static const BallPalettePair palette_pairs[] = {
        { {255, 196,  54}, {164,  51,  87} },  
        { {255, 142,  52}, { 84,  48, 156} },  
        { {255, 224,  74}, {181,  72, 142} },  
        { {255, 120, 176}, { 64,  92, 198} },  
        { {169, 245,  92}, {  0, 150, 124} }   
    };

    int palette_count = (int)(sizeof(palette_pairs) / sizeof(palette_pairs[0]));
    if (light_value < 0.0f) light_value = 0.0f;
    if (light_value > 1.0f) light_value = 1.0f;

    int current_pair = -1;
    for (int i = 0; i < palette_count; ++i) {
        if (memcmp(palette_pairs[i].light, g_color_light_rgb, 3) == 0 &&
            memcmp(palette_pairs[i].dark, g_color_dark_rgb, 3) == 0) {
            current_pair = i;
            break;
        }
    }

    int pair_index = random() % palette_count;
    if (palette_count > 1) {
        while (pair_index == current_pair) {
            pair_index = random() % palette_count;
        }
    }

    float light_target = light_value * 255.0f;
    float dark_target = light_target * 0.78f;
    const BallPalettePair *pair = &palette_pairs[pair_index];

    float base_light_max = fmaxf(pair->light[0], fmaxf(pair->light[1], pair->light[2]));
    float base_dark_max = fmaxf(pair->dark[0], fmaxf(pair->dark[1], pair->dark[2]));
    float light_scale = (base_light_max > 0.0f) ? (light_target / base_light_max) : 0.0f;
    float dark_scale = (base_dark_max > 0.0f) ? (dark_target / base_dark_max) : 0.0f;

    for (int c = 0; c < 3; ++c) {
        out_light[c] = (uint8_t)clampf(pair->light[c] * light_scale, 0.0f, 255.0f);
        out_dark[c] = (uint8_t)clampf(pair->dark[c] * dark_scale, 0.0f, 255.0f);
    }
}




static bool ensure_sphere_pixel_cache(int size) {
    if (sphere_cache && sphere_cache_size == size) {
        return true;
    }

    if (sphere_cache && sphere_cache_capacity_size != size) {
        /* A different cache size would require a runtime allocation. All
         * callers use the startup-selected ball texture size. */
        return false;
    }

    size_t total_pixels = (size_t)size * (size_t)size;
    SpherePixelCache *cache = sphere_cache;
    if (!cache) {
        cache = (SpherePixelCache *)calloc(total_pixels, sizeof(SpherePixelCache));
        if (!cache) return false;
    }

    float light_x = -0.3f;
    float light_y = -0.5f;
    float light_z = 1.0f;
    float light_len = sqrtf(light_x * light_x + light_y * light_y + light_z * light_z);
    float inv_light_len = (light_len != 0.0f) ? (1.0f / light_len) : 1.0f;
    light_x *= inv_light_len;
    light_y *= inv_light_len;
    light_z *= inv_light_len;

    float radius = size / 2.0f;
    float cx, cy;
    cx = cy = radius - 0.5f;
    float inv_radius = (radius != 0.0f) ? (1.0f / radius) : 0.0f;

    /* A pixel covers about (radius - dist) + 0.5 of the disc: one whose centre
     * sits exactly on the circle is half covered, not empty. The ramp straddles
     * the silhouette, so the scanned span has to reach half a pixel past it --
     * stopping at the circle throws away the outer half of every edge pixel and
     * shrinks the ball. */
    float radius_ext = radius + 0.5f;
    float radius_ext_sq = radius_ext * radius_ext;

    float half_pi = PI * 0.5f;
    float lat_scale = (float)LAT_TILES / PI;
    float inv_two_pi = 1.0f / (2.0f * PI);

    for (int y = 0; y < size; y++) {
        float dy = (float)y - cy;
        float rem = radius_ext_sq - dy * dy;
        if (rem < 0.0f) {
            continue;
        }

        float dx = sqrtf(rem);
        int xl = (int)floorf(cx - dx);
        int xr = (int)ceilf(cx + dx);
        if (xl < 0) xl = 0;
        if (xr >= size) xr = size - 1;

        float vy = dy * inv_radius;
        float vy_sq = vy * vy;

        for (int x = xl; x <= xr; x++) {
            float vx = ((float)x - cx) * inv_radius;
            float r2 = vx * vx + vy_sq;
            float dist = sqrtf(r2) * radius;
            float coverage = (radius - dist) + 0.5f;
            if (coverage <= 0.0f) {
                continue;
            }
            uint8_t edge_alpha = (coverage >= 1.0f)
                                     ? 255
                                     : (uint8_t)(coverage * 255.0f + 0.5f);

            /* Past the silhouette there is no sphere to take a normal from, so
             * the outer half of the ramp borrows the rim's: project onto the
             * equator rather than leaving a non-unit vector to skew the shading. */
            float nx = vx;
            float ny = vy;
            float nz;
            if (r2 < 1.0f) {
                nz = sqrtf(1.0f - r2);
            } else {
                float inv_len = 1.0f / sqrtf(r2);
                nx *= inv_len;
                ny *= inv_len;
                nz = 0.0f;
            }

            float dot_product = nx * light_x + ny * light_y + nz * light_z;
            float brightness = AMBIENT_LIGHT + DIFFUSE_LIGHT * fmaxf(0.0f, dot_product);
            brightness = clampf(brightness, MIN_BRIGHTNESS, 1.0f);

            float na = nx * axis_ax + ny * axis_ay + nz * axis_az;
            float nu = nx * axis_ux + ny * axis_uy + nz * axis_uz;
            float nv = nx * axis_vx + ny * axis_vy + nz * axis_vz;

            float lat = asinf(clampf(na, -1.0f, 1.0f));
            float lon = atan2f(nv, nu);

            float lat_scaled = (lat + half_pi) * lat_scale;
            int v = (int)floorf(lat_scaled);
            if (v < 0) v = 0;
            if (v >= LAT_TILES) v = LAT_TILES - 1;

            float lon_norm = lon * inv_two_pi;
            lon_norm -= floorf(lon_norm);  

            int idx = y * size + x;
            SpherePixelCache *c = &cache[idx];
            c->valid = edge_alpha;
            c->lat_index = (uint8_t)v;
            c->lon_norm = lon_norm;
            c->light_rgb[0] = (uint8_t)clampf(g_color_light_rgb[0] * brightness, 0.0f, 255.0f);
            c->light_rgb[1] = (uint8_t)clampf(g_color_light_rgb[1] * brightness, 0.0f, 255.0f);
            c->light_rgb[2] = (uint8_t)clampf(g_color_light_rgb[2] * brightness, 0.0f, 255.0f);
            c->dark_rgb[0] = (uint8_t)clampf(g_color_dark_rgb[0] * brightness, 0.0f, 255.0f);
            c->dark_rgb[1] = (uint8_t)clampf(g_color_dark_rgb[1] * brightness, 0.0f, 255.0f);
            c->dark_rgb[2] = (uint8_t)clampf(g_color_dark_rgb[2] * brightness, 0.0f, 255.0f);
        }
    }

    sphere_cache = cache;
    sphere_cache_size = size;
    sphere_cache_capacity_size = size;
    return true;
}

static double get_perf_seconds(uint64_t start_counter, uint64_t end_counter) {
    if (end_counter < start_counter) {
        return 0.0;
    }
    return (double)(end_counter - start_counter) / 1000000000.0;
}



__attribute__((hot))
static void generate_sphere_pixels_band(uint8_t * restrict pixels,
                                        const int size,
                                        const float phase_offset,
                                        const int y0,
                                        const int y1) {
    if (!pixels || size <= 0) return;
    if (!sphere_cache || sphere_cache_size != size) return;

    int start_y = y0;
    int end_y = y1;
    if (start_y < 0) start_y = 0;
    if (end_y > size) end_y = size;
    if (start_y >= end_y) return;

    float phase_norm = phase_offset * (1.0f / (2.0f * PI));

    for (int y = start_y; y < end_y; y++) {
        uint8_t *row = pixels + (size_t)y * (size_t)size * 4;
        memset(row, 0, (size_t)size * 4);

        size_t row_base = (size_t)y * (size_t)size;
        for (int x = 0; x < size; x++) {
            const SpherePixelCache *c = &sphere_cache[row_base + (size_t)x];
            if (!c->valid) {
                continue;
            }

            float lon_norm = c->lon_norm + phase_norm;
            lon_norm -= floorf(lon_norm);

            int u = (int)(lon_norm * (float)LON_TILES);
            if (u >= LON_TILES) u = LON_TILES - 1;

            bool is_light = ((u + c->lat_index) & 1) == 0;
            const uint8_t *src = is_light ? c->light_rgb : c->dark_rgb;

            size_t idx = (size_t)x * 4;
            unsigned int a = c->valid;
            if (a == 255) {
                row[idx + 0] = src[0];
                row[idx + 1] = src[1];
                row[idx + 2] = src[2];
            } else {
                row[idx + 0] = (uint8_t)((src[0] * a + 127u) / 255u);
                row[idx + 1] = (uint8_t)((src[1] * a + 127u) / 255u);
                row[idx + 2] = (uint8_t)((src[2] * a + 127u) / 255u);
            }
            row[idx + 3] = (uint8_t)a;
        }
    }
}

__attribute__((hot))
static void generate_sphere_pixels(uint8_t * restrict pixels, const int size,
                                   const float phase_offset) {
    generate_sphere_pixels_band(pixels, size, phase_offset, 0, size);
}

__attribute__((hot))
static uint8_t* generate_shadow_pixels(int shadow_size, int * restrict out_width, int * restrict out_height) {
    int size = shadow_size;
    uint8_t *pixels = NULL;
    if (posix_memalign((void**)&pixels, 32, (size_t)size * size * 4) != 0) {
        return NULL;
    }

    float cx, cy, shadow_radius;
    cx = cy = shadow_radius = size / 2.0f;
    const float max_alpha = 0.4f;

    const float inner_threshold = shadow_radius * 0.7f;
    const float inner_threshold_sq = inner_threshold * inner_threshold;
    const float outer_threshold_sq = shadow_radius * shadow_radius;
    const float falloff_range = shadow_radius * 0.3f;

    int total_pixels = size * size;
    #pragma GCC ivdep
    for (int i = 0; i < total_pixels; i++) {
        int y = i / size;
        int x = i % size;

        float dx = x - cx;
        float dy = y - cy;
        float dist_sq = dx * dx + dy * dy;

        float alpha = 0.0f;

        if (dist_sq < inner_threshold_sq) {
            alpha = max_alpha;
        } else if (dist_sq < outer_threshold_sq) {
            float dist = sqrtf(dist_sq);
            float t = (dist - inner_threshold) / falloff_range;

            t = t * t * (3.0f - 2.0f * t);
            alpha = (1.0f - t) * max_alpha;
        }

        int idx = i * 4;
        pixels[idx + 0] = 0;
        pixels[idx + 1] = 0;
        pixels[idx + 2] = 0;
        pixels[idx + 3] = (uint8_t)(alpha * 255.0f);
    }

    *out_width = size;
    *out_height = size;
    return pixels;
}

/* Source-over for premultiplied operands: out = src + dst * (1 - src_a).
 *
 * Everything that lands in a frame texture -- the sphere and the shadow both --
 * is premultiplied, and the frame is drawn with GL_ONE/GL_ONE_MINUS_SRC_ALPHA to
 * match. That is what keeps the edge clean: GL_LINEAR interpolates premultiplied
 * colour correctly, whereas straight alpha drags the transparent surround's
 * black into every edge texel and rings the ball in a dark fringe.
 *
 * It is also the cheaper form -- no divide by the output alpha, and no
 * un-premultiply back to straight colour afterwards. */
static inline void blend_over_premul(uint8_t * restrict dp,
                                     const uint8_t * restrict sp) {
    unsigned int src_a = sp[3];
    if (src_a == 0) {
        return;
    }
    if (src_a == 255) {
        memcpy(dp, sp, 4);
        return;
    }
    unsigned int inv = 255u - src_a;
    dp[0] = (uint8_t)(sp[0] + (dp[0] * inv + 127u) / 255u);
    dp[1] = (uint8_t)(sp[1] + (dp[1] * inv + 127u) / 255u);
    dp[2] = (uint8_t)(sp[2] + (dp[2] * inv + 127u) / 255u);
    dp[3] = (uint8_t)(src_a + (dp[3] * inv + 127u) / 255u);
}

__attribute__((hot))
static void composite_ball_and_shadow(uint8_t * restrict result,
                                      const uint8_t * restrict ball_pixels, const int ball_w, const int ball_h,
                                      const uint8_t * restrict shadow_pixels, const int shadow_w, const int shadow_h,
                                      const int composite_w, const int composite_h,
                                      const int ball_x, const int ball_y,
                                      const int shadow_x, const int shadow_y) {
    size_t alloc_size = (size_t)composite_w * (size_t)composite_h * 4;
    memset(result, 0, alloc_size);

    for (int y = 0; y < shadow_h; y++) {
        int dst_y = shadow_y + y;
        if (dst_y < 0 || dst_y >= composite_h) continue;

        for (int x = 0; x < shadow_w; x++) {
            int dst_x = shadow_x + x;
            if (dst_x < 0 || dst_x >= composite_w) continue;

            int src_idx = (y * shadow_w + x) * 4;
            int dst_idx = (dst_y * composite_w + dst_x) * 4;

            result[dst_idx + 0] = shadow_pixels[src_idx + 0];
            result[dst_idx + 1] = shadow_pixels[src_idx + 1];
            result[dst_idx + 2] = shadow_pixels[src_idx + 2];
            result[dst_idx + 3] = shadow_pixels[src_idx + 3];
        }
    }

    for (int y = 0; y < ball_h; y++) {
        int dst_y = ball_y + y;
        if (dst_y < 0 || dst_y >= composite_h) continue;

        for (int x = 0; x < ball_w; x++) {
            int dst_x = ball_x + x;
            if (dst_x < 0 || dst_x >= composite_w) continue;

            int src_idx = (y * ball_w + x) * 4;
            int dst_idx = (dst_y * composite_w + dst_x) * 4;

            blend_over_premul(result + dst_idx, ball_pixels + src_idx);
        }
    }
}

typedef struct {
    int frame_count;
    float angle_period;
    int sphere_texture_size;
    const uint8_t *shadow_pixels;
    int shadow_w;
    int shadow_h;
    int composite_w;
    int composite_h;
    int ball_offset_x;
    int ball_offset_y;
    int shadow_offset_x;
    int shadow_offset_y;
    uint8_t *frame_buffer;
    size_t frame_size;
    PoingoAtomic next_index;
    PoingoAtomic failure;
    PoingoAtomic completed;
} BallFrameJobContext;


static int generate_ball_frame_worker(void *userdata) {
    BallFrameJobContext *ctx = (BallFrameJobContext *)userdata;

    size_t frame_size = ctx->frame_size;
    size_t ball_size = (size_t)ctx->sphere_texture_size * (size_t)ctx->sphere_texture_size * 4;
    uint8_t *ball_pixels = NULL;
    if (posix_memalign((void**)&ball_pixels, 32, ball_size) != 0) {
        poingo_atomic_set(&ctx->failure, 1);
        return 0;
    }

    for (;;) {
        int index = poingo_atomic_add(&ctx->next_index, 1);
        if (index >= ctx->frame_count) {
            break;
        }

        if (poingo_atomic_get(&ctx->failure)) {
            break;
        }

        float phase_offset = ((float)index / (float)ctx->frame_count) * ctx->angle_period;
        generate_sphere_pixels(ball_pixels, ctx->sphere_texture_size, phase_offset);

        uint8_t * restrict frame_dst =
            (uint8_t * restrict)__builtin_assume_aligned(ctx->frame_buffer + (size_t)index * frame_size, 32);

        composite_ball_and_shadow(
            frame_dst,
            ball_pixels, ctx->sphere_texture_size, ctx->sphere_texture_size,
            ctx->shadow_pixels, ctx->shadow_w, ctx->shadow_h,
            ctx->composite_w, ctx->composite_h,
            ctx->ball_offset_x, ctx->ball_offset_y,
            ctx->shadow_offset_x, ctx->shadow_offset_y);

        poingo_atomic_add(&ctx->completed, 1);
    }

    free(ball_pixels);
    return 0;
}



enum {
    BALL_REGEN_FILL = 0,
    BALL_REGEN_CLEAR = 1
};










typedef enum {
    BOUNCE_SOUND_POINGO = 0,
    BOUNCE_SOUND_NOSTALGIA = 1
} BounceSoundStyle;

static ToyBounceStyle toy_style(BounceSoundStyle style) {
    return style == BOUNCE_SOUND_NOSTALGIA
               ? TOY_BOUNCE_NOSTALGIA : TOY_BOUNCE_POINGO;
}

typedef struct {
    ToySamplePair pair;
    float pending_size_scale;
    float current_size_scale;
    BounceSoundStyle style;
} BallAudio;

typedef struct {
    BallAudio balls[POINGO_MAX_BALLS];
    ToySamplePair ting;
} AudioState;

static ToyMixer g_mixer;
static bool g_ghost_mute = false;

typedef struct {
    float time;
    int volume;
    int surface;
    float pan;
    uint64_t serial;
} BounceEvent;

typedef struct {
    BounceEvent events[AUDIO_PREDICT_MAX_EVENTS];
    int count;
} AudioPredictBuffer;


AudioState audio_state = {
    .ting = { NULL, NULL, 0, true },
};
static int g_bounce_slot = 0;

static BallAudio *current_ball_audio(void) {
    return &audio_state.balls[g_bounce_slot];
}

static bool audio_device_open = false;

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_lock(void)   { pthread_mutex_lock(&g_audio_lock); }
static void audio_unlock(void) { pthread_mutex_unlock(&g_audio_lock); }

static void update_bounce_sound_style_for_mode(void) {
    BounceSoundStyle target_style = g_a_mode ? BOUNCE_SOUND_NOSTALGIA : BOUNCE_SOUND_POINGO;
    BallAudio *audio = current_ball_audio();
    if (audio->style == target_style) {
        return;
    }
    audio->style = target_style;
    float size_scale = audio->pair.dirty
                           ? audio->pending_size_scale
                           : audio->current_size_scale;
    mark_sounds_dirty(fmaxf(size_scale, 0.01f));
}

static AudioPredictBuffer g_audio_predict = {0};
static float g_audio_sim_time = 0.0f;
static float g_audio_predict_delay_seconds = AUDIO_PREDICT_DELAY_SECONDS;
static float g_audio_predict_step_seconds = 1.0f / (float)FPS;
static float g_audio_predict_poll_seconds = 1.0f / (float)FPS;
enum {
    BOUNCE_SURFACE_FLOOR = 0,
    BOUNCE_SURFACE_CEILING = 1,
    BOUNCE_SURFACE_LEFT = 2,
    BOUNCE_SURFACE_RIGHT = 3,
    BOUNCE_SURFACE_COUNT = 4
};
static uint64_t g_actual_bounce_serial[BOUNCE_SURFACE_COUNT] = {0};
static uint64_t g_played_bounce_serial[BOUNCE_SURFACE_COUNT] = {0};
static bool g_predict_audit = false;
static uint64_t g_audit_expected[BOUNCE_SURFACE_COUNT] = {0};
static uint64_t g_audit_selected[BOUNCE_SURFACE_COUNT] = {0};
static uint64_t g_audit_committed[BOUNCE_SURFACE_COUNT] = {0};
/* Ride the mixer down alongside the ball's own fade, so the sound leaves with
 * the picture. Without this a bounce landing just before the quit keeps ringing
 * for well over a second after the ball is gone -- its tail is far longer than
 * the fade -- and the shutdown sits there waiting for it to decay. */
static void audio_begin_shutdown_fade(float seconds) {
    if (!audio_device_open) {
        return;
    }

    audio_lock();
    toy_mixer_begin_fade_out(&g_mixer, seconds);
    audio_unlock();
}


static void notify_volume_changed(void);
static void adjust_master_volume(float delta);
static void set_master_mute(bool mute);
static void toggle_master_mute(void);
static void update_volume_hud(float delta_seconds);



static float g_volume_hud_time = 0.0f;
static float g_volume_hud_alpha = 0.0f;
static float g_volume_hud_value = 1.2f;
static bool g_volume_muted = false;
static float g_speed_multiplier = 1.0f;
static float g_speed_hud_time = 0.0f;
static float g_speed_hud_alpha = 0.0f;
static bool g_suppress_hud = false;
static float g_freerange_ball_scale = 1.0f;
static bool g_debug_mode = false;  

typedef struct {
    bool is_pressed;
    float hold_time;
    float repeat_timer;
} KeyRepeatState;

static KeyRepeatState g_vol_up_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_vol_down_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_speed_up_key = {false, 0.0f, 0.0f};
static KeyRepeatState g_speed_down_key = {false, 0.0f, 0.0f};

static ToyAudioStream *g_audio_stream = NULL;
static bool g_audio_output_latency_valid = false;

__attribute__((hot))
static void poingo_audio_render(void *userdata, float *output,
                                uint32_t nframes, uint32_t channels) {
    (void)userdata;
    (void)channels;
    audio_lock();
    toy_mixer_render_stereo(&g_mixer, output, (int)nframes, false);
    audio_unlock();
}

__attribute__((cold))
static void shutdown_audio(void) {
    if (!audio_device_open) {
        return;
    }

    toy_audio_stream_stop(g_audio_stream);
    g_audio_stream = NULL;
    audio_device_open = false;

    for (int i = 0; i < POINGO_MAX_BALLS; i++) {
        toy_sample_pair_free(&audio_state.balls[i].pair);
        audio_state.balls[i].pending_size_scale = 1.0f;
        audio_state.balls[i].current_size_scale = 1.0f;
        audio_state.balls[i].style = BOUNCE_SOUND_POINGO;
    }
    toy_sample_pair_free(&audio_state.ting);
    toy_mixer_reset(&g_mixer);
    toy_audio_release_scratch();
    g_bounce_slot = 0;
    g_volume_muted = false;
    g_volume_hud_alpha = 0.0f;
    g_volume_hud_time = 0.0f;
    g_audio_output_latency_valid = false;
}

static bool init_audio(bool start_muted) {
    toy_mixer_init(&g_mixer, TOY_AUDIO_DEFAULT_VOLUME);
    audio_device_open = true;

    int effect_length = toy_audio_bounce_pair_length();
    bool samples_ready = toy_sample_pair_alloc(&audio_state.ting,
                                                effect_length);
    for (int i = 0; samples_ready && i < POINGO_MAX_BALLS; i++) {
        BallAudio *audio = &audio_state.balls[i];
        samples_ready = toy_sample_pair_alloc(&audio->pair, effect_length);
        audio->pending_size_scale = 1.0f;
        audio->current_size_scale = 1.0f;
        audio->style = BOUNCE_SOUND_POINGO;
        audio->pair.dirty = true;
    }
    if (!samples_ready) {
        fprintf(stderr, "Failed to allocate audio sample buffers\n");
        shutdown_audio();
        return false;
    }
    int bounce_length = audio_state.balls[0].pair.length;
    if (!toy_mixer_reserve(&g_mixer, bounce_length) ||
        !toy_audio_reserve_scratch(bounce_length)) {
        fprintf(stderr, "Failed to allocate audio mixer workspace\n");
        shutdown_audio();
        return false;
    }

    toy_audio_generate_bounce_pair(&audio_state.ting, TING_SOUND_SCALE,
                                   TOY_BOUNCE_POINGO);

    if (start_muted) {
        g_mixer.muted = true;
        g_mixer.master_volume = 0.0f;
        notify_volume_changed();
    }

    ToyAudioStreamConfig stream_config = {
        .name = "poingo",
        .description = "Poingo",
        .sample_rate = SAMPLE_RATE,
        .channels = 2,
        .render = poingo_audio_render,
        .userdata = NULL,
    };
    g_audio_stream = toy_audio_stream_start(&stream_config);
    if (unlikely(!g_audio_stream)) {
        fprintf(stderr,
                "poingo: failed to start PipeWire audio; quitting\n");
        shutdown_audio();
        return false;
    }
    return true;
}

static bool play_pair(const ToySamplePair *pair, float gain, float pan) {
    if (!pair) {
        return false;
    }

    if (g_ghost_mute) {
        gain *= 0.05f;
    }
    float pan_shaped = 0.5f + (clampf(pan, 0.0f, 1.0f) - 0.5f) * 1.15f;
    pan_shaped = clampf(pan_shaped, 0.0f, 1.0f);
    float l_gain, r_gain;
    toy_audio_equal_power_pan(pan_shaped, &l_gain, &r_gain);

    ToyVoiceClaim claim;
    audio_lock();
    bool claimed = toy_mixer_claim_voice(&g_mixer, pair->length, &claim);
    audio_unlock();
    if (!claimed) {
        return false;
    }

    bool copied = toy_mixer_copy_claimed_voice(&g_mixer, claim, pair);
    audio_lock();
    bool committed = copied && toy_mixer_commit_voice(
        &g_mixer, claim, pair->length, gain, l_gain, r_gain);
    if (!committed) {
        toy_mixer_cancel_voice(&g_mixer, claim);
    }
    audio_unlock();
    return committed;
}

static bool play_bounce_sound(int volume, float pan) {
    BallAudio *audio = current_ball_audio();
    float size_scale = fmaxf(audio->current_size_scale, 0.05f);

    if (audio->pair.dirty) {
        size_scale = fmaxf(audio->pending_size_scale, 0.05f);
        toy_audio_generate_bounce_pair(&audio->pair, size_scale,
                                       toy_style(audio->style));
        audio->current_size_scale = size_scale;
    }

    float size_factor = 0.7f + 0.3f * fminf(size_scale, 1.0f);

    float velocity_factor = fminf(volume / 32.0f, 1.0f);
    velocity_factor = fmaxf(velocity_factor, 0.6f);
    return play_pair(&audio->pair, size_factor * velocity_factor, pan);
}

static void play_ting_sound(float impact_ratio, float pan) {
    float gain = TING_VOLUME_SCALE * clampf(impact_ratio, 0.25f, 1.5f);
    (void)play_pair(&audio_state.ting, gain, pan);
}

static void adjust_master_volume(float delta) {
    if (delta == 0.0f) {
        return;
    }

    audio_lock();
    toy_mixer_adjust_volume(&g_mixer, delta);
    audio_unlock();

    notify_volume_changed();
}

static void notify_volume_changed(void) {
    g_volume_muted = g_mixer.muted;
    g_volume_hud_value = g_mixer.muted && g_mixer.master_volume <= MASTER_VOLUME_MIN
        ? g_mixer.master_volume_before_mute
        : g_mixer.master_volume;
    g_volume_hud_value = fmaxf(MASTER_VOLUME_MIN, fminf(MASTER_VOLUME_MAX, g_volume_hud_value));
    if (g_suppress_hud) {
        return;
    }
    g_volume_hud_time = VOLUME_HUD_HOLD_TIME + VOLUME_HUD_FADE_TIME;
    g_volume_hud_alpha = 1.0f;
}

static inline float clamp_speed(float value) {
    if (value < SPEED_MIN) return SPEED_MIN;
    if (value > SPEED_MAX) return SPEED_MAX;
    return value;
}

static inline float get_natural_vx(int window_w) {
    return (float)window_w / 273.0f;
}

static inline float clamp_freerange_ball_scale(float value) {
    if (value < FREEDOM_BALL_SCALE_MIN) return FREEDOM_BALL_SCALE_MIN;
    if (value > FREEDOM_BALL_SCALE_MAX) return FREEDOM_BALL_SCALE_MAX;
    return value;
}

static bool parse_color_arg(const char *value, uint8_t out_rgb[3]) {
    if (!value || !out_rgb) {
        return false;
    }

    const char *hex = value;
    if (hex[0] == '#') {
        hex++;
    } else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    if (strlen(hex) == 6) {
        for (int i = 0; i < 6; i++) {
            if (!isxdigit((unsigned char)hex[i])) {
                return false;
            }
        }
        char buf[3] = {0};
        buf[0] = hex[0];
        buf[1] = hex[1];
        out_rgb[0] = (uint8_t)strtoul(buf, NULL, 16);
        buf[0] = hex[2];
        buf[1] = hex[3];
        out_rgb[1] = (uint8_t)strtoul(buf, NULL, 16);
        buf[0] = hex[4];
        buf[1] = hex[5];
        out_rgb[2] = (uint8_t)strtoul(buf, NULL, 16);
        return true;
    }

    int r = -1, g = -1, b = -1;
    if (sscanf(value, "%d,%d,%d", &r, &g, &b) == 3) {
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            return false;
        }
        out_rgb[0] = (uint8_t)r;
        out_rgb[1] = (uint8_t)g;
        out_rgb[2] = (uint8_t)b;
        return true;
    }

    return false;
}

static void notify_speed_changed(void) {
    if (g_suppress_hud) {
        return;
    }
    g_speed_hud_time = SPEED_HUD_HOLD_TIME + SPEED_HUD_FADE_TIME;
    g_speed_hud_alpha = 1.0f;
}

static void set_speed_multiplier(float value) {
    float clamped = clamp_speed(value);
    g_speed_multiplier = clamped;
    notify_speed_changed();
}

static void adjust_speed(float delta) {
    if (delta == 0.0f) {
        return;
    }
    set_speed_multiplier(g_speed_multiplier + delta);
}

static void set_master_mute(bool mute) {
    audio_lock();
    toy_mixer_set_mute(&g_mixer, mute);
    audio_unlock();
    notify_volume_changed();
}

static void toggle_master_mute(void) {
    set_master_mute(!g_mixer.muted);
}

static int process_key_repeat(KeyRepeatState *state, bool currently_pressed, float delta_seconds) {
    if (currently_pressed) {
        if (!state->is_pressed) {
            state->is_pressed = true;
            state->hold_time = 0.0f;
            state->repeat_timer = 0.0f;
            return 1;
        } else {
            state->hold_time += delta_seconds;
            if (state->hold_time >= KEY_REPEAT_INITIAL_DELAY) {
                state->repeat_timer += delta_seconds;
                int count = 0;
                while (state->repeat_timer >= KEY_REPEAT_INTERVAL) {
                    count++;
                    state->repeat_timer -= KEY_REPEAT_INTERVAL;
                }
                return count;
            }
        }
    } else {
        state->is_pressed = false;
        state->hold_time = 0.0f;
        state->repeat_timer = 0.0f;
    }
    return 0;
}

static inline void update_volume_hud(float delta_seconds) {
    if (g_volume_hud_time > 0.0f) {
        if (delta_seconds > 0.0f) {
            g_volume_hud_time = fmaxf(0.0f, g_volume_hud_time - delta_seconds);
        }
        if (g_volume_hud_time > VOLUME_HUD_FADE_TIME) {
            g_volume_hud_alpha = 1.0f;
        } else if (VOLUME_HUD_FADE_TIME > 0.0f) {
            g_volume_hud_alpha = g_volume_hud_time / VOLUME_HUD_FADE_TIME;
        } else {
            g_volume_hud_alpha = 0.0f;
        }
    } else {
        g_volume_hud_alpha = 0.0f;
    }
}

static inline void update_speed_hud(float delta_seconds) {
    if (g_speed_hud_time > 0.0f) {
        if (delta_seconds > 0.0f) {
            g_speed_hud_time = fmaxf(0.0f, g_speed_hud_time - delta_seconds);
        }
        if (g_speed_hud_time > SPEED_HUD_FADE_TIME) {
            g_speed_hud_alpha = 1.0f;
        } else if (SPEED_HUD_FADE_TIME > 0.0f) {
            g_speed_hud_alpha = g_speed_hud_time / SPEED_HUD_FADE_TIME;
        } else {
            g_speed_hud_alpha = 0.0f;
        }
    } else {
        g_speed_hud_alpha = 0.0f;
    }
}

#define GLYPH_ROWS 5
#define GLYPH_COLS 3

typedef struct {
    char ch;
    const char *rows[GLYPH_ROWS];
} GlyphDef;

static const GlyphDef g_volume_glyphs[] = {
    {' ', {"...", "...", "...", "...", "..."}},
    {'0', {"XXX", "X.X", "X.X", "X.X", "XXX"}},
    {'1', {".X.", "XX.", ".X.", ".X.", "XXX"}},
    {'2', {"XXX", "..X", "XXX", "X..", "XXX"}},
    {'3', {"XXX", "..X", "XXX", "..X", "XXX"}},
    {'4', {"X.X", "X.X", "XXX", "..X", "..X"}},
    {'5', {"XXX", "X..", "XXX", "..X", "XXX"}},
    {'6', {"XXX", "X..", "XXX", "X.X", "XXX"}},
    {'7', {"XXX", "..X", "..X", "..X", "..X"}},
    {'8', {"XXX", "X.X", "XXX", "X.X", "XXX"}},
    {'9', {"XXX", "X.X", "XXX", "..X", "XXX"}},
    {'V', {"X.X", "X.X", "X.X", "X.X", ".X."}},
    {'O', {"XXX", "X.X", "X.X", "X.X", "XXX"}},
    {'L', {"X..", "X..", "X..", "X..", "XXX"}},
    {'U', {"X.X", "X.X", "X.X", "X.X", "XXX"}},
    {'M', {"XXX", "XXX", "X.X", "X.X", "X.X"}},
    {'E', {"XXX", "X..", "XXX", "X..", "XXX"}},
    {'T', {"XXX", ".X.", ".X.", ".X.", ".X."}},
    {'D', {"XX.", "X.X", "X.X", "X.X", "XX."}},
    {'P', {"XXX", "X.X", "XXX", "X..", "X.."}},
    {'S', {".XX", "X..", "XXX", "..X", "XX."}},
    {'.', {"...", "...", ".X.", "...", "..."}},
    {'%', {"X..", "..X", ".X.", "X..", "..X"}},
    {'^', {".X.", "XXX", ".X.", ".X.", ".X."}},
    {'v', {".X.", ".X.", ".X.", "XXX", ".X."}},
    {'<', {".X.", "X..", "XXX", "X..", ".X."}},
    {'>', {".X.", "..X", "XXX", "..X", ".X."}},
    {'R', {"XX.", "X.X", "XX.", "X.X", "X.X"}},
    {'A', {".X.", "X.X", "XXX", "X.X", "X.X"}},
    {'B', {"XX.", "X.X", "XX.", "X.X", "XX."}},
    {'C', {".XX", "X..", "X..", "X..", ".XX"}},
    {'F', {"XXX", "X..", "XX.", "X..", "X.."}},
    {'G', {".XX", "X..", "X.X", "X.X", ".XX"}},
    {'H', {"X.X", "X.X", "XXX", "X.X", "X.X"}},
    {'I', {"XXX", ".X.", ".X.", ".X.", "XXX"}},
    {'N', {"X.X", "XXX", "XXX", "XXX", "X.X"}},
    {'Q', {"XXX", "X.X", "X.X", "XXX", "..X"}},
    {'W', {"X.X", "X.X", "X.X", "XXX", "XXX"}},
    {'X', {"X.X", ".X.", "X.X", ".X.", "X.X"}},
    {'Y', {"X.X", "X.X", ".X.", ".X.", ".X."}},
    {'a', {"...", ".XX", "X.X", "X.X", ".XX"}},
    {'b', {"X..", "X..", "XX.", "X.X", "XX."}},
    {'c', {"...", ".XX", "X..", "X..", ".XX"}},
    {'d', {"..X", "..X", ".XX", "X.X", ".XX"}},
    {'e', {"...", ".XX", "XXX", "X..", ".XX"}},
    {'f', {".XX", ".X.", "XXX", ".X.", ".X."}},
    {'g', {"...", ".XX", "X.X", ".XX", "XX."}},
    {'h', {"X..", "X..", "XX.", "X.X", "X.X"}},
    {'i', {".X.", "...", ".X.", ".X.", ".X."}},
    {'k', {"X..", "X.X", "XX.", "X.X", "X.X"}},
    {'l', {"XX.", ".X.", ".X.", ".X.", "XXX"}},
    {'n', {"...", "XX.", "X.X", "X.X", "X.X"}},
    {'o', {"...", ".X.", "X.X", "X.X", ".X."}},
    {'q', {"...", ".XX", "X.X", ".XX", "..X"}},
    {'r', {"...", ".XX", "X..", "X..", "X.."}},
    {'s', {"...", ".XX", "XX.", ".XX", "XX."}},
    {'t', {".X.", "XXX", ".X.", ".X.", ".XX"}},
    {'u', {"...", "X.X", "X.X", "X.X", ".XX"}},
    {'w', {"...", "X.X", "X.X", "XXX", "XXX"}},
    {'y', {"...", "X.X", "X.X", ".XX", "XX."}},
    {'z', {"...", "XXX", ".X.", "X..", "XXX"}},
    {'!', {".X.", ".X.", ".X.", "...", ".X."}},
    {'-', {"...", "...", "XXX", "...", "..."}},
    {'=', {"...", "XXX", "...", "XXX", "..."}},
    {'/', {"..X", "..X", ".X.", "X..", "X.."}},
    {'[', {"XX.", "X..", "X..", "X..", "XX."}},
    {']', {".XX", "..X", "..X", "..X", ".XX"}},
    {0, {NULL, NULL, NULL, NULL, NULL}}
};

static const GlyphDef *lookup_volume_glyph(char c) {
    for (size_t i = 0; g_volume_glyphs[i].ch != 0; i++) {
        if (g_volume_glyphs[i].ch == c) {
            return &g_volume_glyphs[i];
        }
    }
    if (c >= 'a' && c <= 'z') {
        char upper = (char)(c - 'a' + 'A');
        for (size_t i = 0; g_volume_glyphs[i].ch != 0; i++) {
            if (g_volume_glyphs[i].ch == upper) {
                return &g_volume_glyphs[i];
            }
        }
    }
    return NULL;
}

static float measure_text(const char *text, float scale) {
    if (!text || *text == '\0') {
        return 0.0f;
    }
    float width = 0.0f;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        width += GLYPH_COLS * scale;
        if (i + 1 < len) {
            width += scale;
        }
    }
    return width;
}

























static float remap_normalized_scale_to_sound(float normalized_scale) {
    float clamped_norm = fmaxf(AUDIO_NORMALIZED_MIN, fminf(AUDIO_NORMALIZED_MAX, normalized_scale));
    float t = (clamped_norm - AUDIO_NORMALIZED_MIN) / (AUDIO_NORMALIZED_MAX - AUDIO_NORMALIZED_MIN);
    float rescaled = AUDIO_RESCALE_MIN + t * (AUDIO_RESCALE_MAX - AUDIO_RESCALE_MIN);
    if (rescaled < AUDIO_RESCALE_MIN) rescaled = AUDIO_RESCALE_MIN;
    if (rescaled > AUDIO_RESCALE_MAX) rescaled = AUDIO_RESCALE_MAX;
    return rescaled;
}

static void mark_sounds_dirty(float size_scale) {
    BallAudio *audio = current_ball_audio();
    audio->pair.dirty = true;
    audio->pending_size_scale = fmaxf(size_scale, 0.01f);
}

static void calculate_equilibrium_state(int window_w, int window_h,
                                 float gravity, float floor_y_norm,
                                 float target_peak_y,
                                 float *out_ball_y, float *out_vy,
                                 float *out_bounce_speed) {
    float prop_x = window_w / CANVAS_WIDTH;
    float prop_y = window_h / CANVAS_HEIGHT;
    float total_prop = fminf(prop_x, prop_y);
    float ball_diameter = BALL_BASE_DIAMETER * total_prop;
    ball_diameter *= g_freerange_ball_scale;
    float ball_diameter_normalized = ball_diameter / (float)window_h;

    float collision_y = floor_y_norm - ball_diameter_normalized;

    float height_to_climb = collision_y - target_peak_y;

    float time_scale = 1.0f;
    float g_eff = gravity * time_scale;

    float bounce_speed = sqrtf(2.0f * g_eff * height_to_climb);

    *out_ball_y = collision_y;
    *out_vy = -bounce_speed;  
    *out_bounce_speed = bounce_speed;
}

static float get_ideal_bounce_vy(float ball_diameter_normalized, float gravity) {
    float bounce_height = g_floor_y_normalized - g_target_peak_y - ball_diameter_normalized;
    if (bounce_height < 0.0001f) {
        bounce_height = 0.0001f;
    }
    float gravity_eff = gravity;
    return sqrtf(2.0f * gravity_eff * bounce_height);
}

static float get_min_bounce_speed_ratio(void) {
    return MIN_BOUNCE_SOUND_SPEED_RATIO;
}

static bool impact_speed_is_audible(float impact_speed, float baseline_speed) {
    if (baseline_speed <= 0.0f) {
        return false;
    }
    return (impact_speed / baseline_speed) >= get_min_bounce_speed_ratio();
}

static void update_ball_physics(float *ball_x, float *ball_y,
                                float *ball_vx, float *ball_vy,
                                int *ball_vx_direction,
                                int window_w,
                                float border_inset, float border_inset_norm,
                                float ball_diameter, float ball_diameter_normalized,
                                double sim_delta,
                                bool ball_grabbed, bool make_noise,
                                uint64_t surface_serials[BOUNCE_SURFACE_COUNT],
                                BounceEvent *events, int *event_count, int max_events,
                                float event_time);

static int volume_from_impact_ratio(float ratio) {
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    float clamped = fminf(ratio, 3.0f);
    return (int)lroundf(8.0f + clamped * 8.0f);
}

static float compute_bounce_pan(float ball_x, float ball_diameter, int window_w) {
    if (window_w <= 0) {
        return 0.5f;
    }
    float pan = (ball_x + ball_diameter * 0.5f) / (float)window_w;
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    return pan;
}

static void maybe_play_actual_bounce_sound(bool make_noise, float impact_speed, float baseline_speed, float pan) {
    if (!make_noise) {
        return;
    }
    if (!impact_speed_is_audible(impact_speed, baseline_speed)) {
        return;
    }
    float ratio = impact_speed / baseline_speed;
    (void)play_bounce_sound(volume_from_impact_ratio(ratio), pan);
}

static void audit_actual_collision(bool predictive_pass, int surface,
                                   float impact_speed, float baseline_speed) {
    if (g_predict_audit && !predictive_pass &&
        surface >= 0 && surface < BOUNCE_SURFACE_COUNT &&
        impact_speed_is_audible(impact_speed, baseline_speed)) {
        g_audit_expected[surface]++;
    }
}

static void audio_predict_reset(float now_time);

static void update_audio_output_latency(void) {
    double latency_seconds = 0.0;
    if (!toy_audio_stream_get_latency(g_audio_stream,
                                      &latency_seconds)) {
        g_audio_output_latency_valid = false;
        return;
    }

    g_audio_predict_delay_seconds = (float)latency_seconds;
    g_audio_output_latency_valid = true;
    if (g_audio_predict_delay_seconds < 0.0f) {
        g_audio_predict_delay_seconds = 0.0f;
    } else if (g_audio_predict_delay_seconds > AUDIO_PREDICT_SECONDS) {
        g_audio_predict_delay_seconds = AUDIO_PREDICT_SECONDS;
    }
}

static inline void append_bounce_event(BounceEvent *events, int *event_count, int max_events,
                                       float event_time, float impact_speed, float baseline_speed,
                                       int surface, float pan, uint64_t serial) {
    if (!events || !event_count || *event_count >= max_events) {
        return;
    }
    if (!impact_speed_is_audible(impact_speed, baseline_speed)) {
        return;
    }
    float ratio = impact_speed / baseline_speed;
    int volume = volume_from_impact_ratio(ratio);
    events[*event_count].time = event_time;
    events[*event_count].volume = volume;
    events[*event_count].surface = surface;
    events[*event_count].pan = pan;
    events[*event_count].serial = serial;
    (*event_count)++;
}

static void audio_predict_reset(float now_time) {
    (void)now_time;
    for (int surface = 0; surface < BOUNCE_SURFACE_COUNT; ++surface) {
        if (g_actual_bounce_serial[surface] <
            g_played_bounce_serial[surface]) {
            g_actual_bounce_serial[surface] =
                g_played_bounce_serial[surface];
        }
        g_played_bounce_serial[surface] =
            g_actual_bounce_serial[surface];
    }
}

static float get_audio_predict_effective_delay_seconds(void) {
    /*
     * The generated impact reaches its audible attack after playback starts,
     * so onset time adds to—not subtracts from—the device latency that must
     * be anticipated.
     */
    float effective_delay =
        g_audio_predict_delay_seconds +
        AUDIO_PREDICT_ONSET_COMPENSATION_SECONDS;
    float minimum_lead =
        g_audio_predict_poll_seconds + AUDIO_PREDICT_FIXED_STEP_SECONDS;
    if (effective_delay < minimum_lead) {
        effective_delay = minimum_lead;
    }
    if (effective_delay < 0.0f) {
        effective_delay = 0.0f;
    } else if (effective_delay > AUDIO_PREDICT_SECONDS) {
        effective_delay = AUDIO_PREDICT_SECONDS;
    }
    return effective_delay;
}

static float get_audio_predict_horizon_seconds(void) {
    float effective_delay = get_audio_predict_effective_delay_seconds();
    float horizon = effective_delay * AUDIO_PREDICT_HORIZON_DELAY_MULTIPLIER +
                    AUDIO_PREDICT_HORIZON_MARGIN_SECONDS;
    if (horizon < AUDIO_PREDICT_HORIZON_MIN_SECONDS) {
        horizon = AUDIO_PREDICT_HORIZON_MIN_SECONDS;
    }
    if (horizon > AUDIO_PREDICT_HORIZON_MAX_SECONDS) {
        horizon = AUDIO_PREDICT_HORIZON_MAX_SECONDS;
    }
    if (horizon > AUDIO_PREDICT_SECONDS) {
        horizon = AUDIO_PREDICT_SECONDS;
    }
    return horizon;
}


static void build_audio_predict_buffer(AudioPredictBuffer *buffer,
                                       float base_time,
                                       float ball_x, float ball_y,
                                       float ball_vx, float ball_vy,
                                       int ball_vx_direction,
                                       int window_w,
                                       float border_inset, float border_inset_norm,
                                       float ball_diameter, float ball_diameter_normalized,
                                       float step_seconds) {
    if (!buffer) {
        return;
    }
    buffer->count = 0;
    float sim_x = ball_x;
    float sim_y = ball_y;
    float sim_vx = ball_vx;
    float sim_vy = ball_vy;
    int sim_dir = ball_vx_direction;
    uint64_t sim_surface_serials[BOUNCE_SURFACE_COUNT];
    memcpy(sim_surface_serials, g_actual_bounce_serial,
           sizeof(sim_surface_serials));
    float step = AUDIO_PREDICT_FIXED_STEP_SECONDS;
    if (step_seconds > 0.0f && step_seconds < step) {
        step = step_seconds;
    }
    if (step <= 0.0001f) {
        step = AUDIO_PREDICT_FIXED_STEP_SECONDS;
    }
    if (step_seconds > 0.0001f) {
        g_audio_predict_poll_seconds = step_seconds;
    }
    g_audio_predict_step_seconds = step;
    float horizon = get_audio_predict_horizon_seconds();
    float t = 0.0f;
    while (t < horizon && buffer->count < AUDIO_PREDICT_MAX_EVENTS) {
        update_ball_physics(&sim_x, &sim_y,
                            &sim_vx, &sim_vy,
                            &sim_dir,
                            window_w,
                            border_inset, border_inset_norm,
                            ball_diameter, ball_diameter_normalized,
                            step,
                            false, true,
                            sim_surface_serials,
                            buffer->events, &buffer->count, AUDIO_PREDICT_MAX_EVENTS,
                            base_time + t + step);
        t += step;
    }
}

static void trigger_predictive_audio_with_timing(const AudioPredictBuffer *buffer,
                                                 float now_time,
                                                 bool make_noise,
                                                 float *timing_error_out,
                                                 bool *timing_valid_out) {
    if (timing_error_out) {
        *timing_error_out = 0.0f;
    }
    if (timing_valid_out) {
        *timing_valid_out = false;
    }

    if (!buffer || !make_noise) {
        return;
    }

    float effective_delay = get_audio_predict_effective_delay_seconds();
    if (effective_delay < g_audio_predict_step_seconds) {
        effective_delay = g_audio_predict_step_seconds;
    }
    float target_time = now_time + effective_delay;
    int timing_samples = 0;
    float timing_accum = 0.0f;

    /*
     * Use only this frame's fresh simulation.  The former persistent queue
     * reconciled changing predictions in place and could associate playback
     * with stale collision state.  A collision is emitted when its freshly
     * predicted time enters the output-latency horizon, then the recent-event
     * ring suppresses subsequent predictions of that same collision.
     */
    for (int i = 0; i < buffer->count; ++i) {
        const BounceEvent *ev = &buffer->events[i];
        float event_time = ev->time;
        if (event_time > target_time) {
            continue;
        }
        if (event_time < now_time - AUDIO_PREDICT_EVENT_STALE_SECONDS) {
            continue;
        }

        if (ev->surface < 0 || ev->surface >= BOUNCE_SURFACE_COUNT ||
            ev->serial == 0 ||
            ev->serial <= g_played_bounce_serial[ev->surface]) {
            continue;
        }

        if (g_predict_audit) {
            g_audit_selected[ev->surface]++;
        }
        bool committed = play_bounce_sound(ev->volume, ev->pan);
        if (g_predict_audit && committed) {
            g_audit_committed[ev->surface]++;
        }
        g_played_bounce_serial[ev->surface] = ev->serial;

        float expected_play_time = event_time - effective_delay;
        float timing_error = now_time - expected_play_time;
        timing_accum += timing_error;
        timing_samples++;
    }

    if (timing_samples > 0) {
        if (timing_error_out) {
            *timing_error_out = timing_accum / (float)timing_samples;
        }
        if (timing_valid_out) {
            *timing_valid_out = true;
        }
    }
}

static void trigger_predictive_audio(const AudioPredictBuffer *buffer, float now_time, bool make_noise) {
    trigger_predictive_audio_with_timing(buffer, now_time, make_noise, NULL, NULL);
}

static void update_ball_physics(float *ball_x, float *ball_y,
                                float *ball_vx, float *ball_vy,
                                int *ball_vx_direction,
                                int window_w,
                                float border_inset, float border_inset_norm,
                                float ball_diameter, float ball_diameter_normalized,
                                double sim_delta,
                                bool ball_grabbed, bool make_noise,
                                uint64_t surface_serials[BOUNCE_SURFACE_COUNT],
                                BounceEvent *events, int *event_count, int max_events,
                                float event_time) {
    if (ball_grabbed) {
        return;
    }

    bool predictive_pass = make_noise && events && event_count && max_events > 0;
    bool immediate_audio_pass = make_noise && !predictive_pass;

    float time_scale = (float)(sim_delta * 60.0f * g_speed_multiplier);
    float time_scale_x = time_scale;
    float time_scale_y = time_scale;
    float gravity = GRAVITY;

    *ball_vy += gravity * time_scale_y;
    *ball_x += (*ball_vx * (float)(*ball_vx_direction)) * time_scale_x;
    *ball_y += *ball_vy * time_scale_y;

    float natural_vx = get_natural_vx(window_w);
    float vx_difference = *ball_vx - natural_vx;
    if (fabsf(vx_difference) > 0.01f) {
        /* A plain lerp towards natural_vx. On a slow frame at high speed the
           coefficient passes 1.0 and the step lands on the far side, which
           for a magnitude paired with a separate direction means the ball
           turns round. Clamped, the worst case is arriving exactly. */
        float damping_factor = clampf(0.049f * time_scale_x, 0.0f, 1.0f);
        *ball_vx -= vx_difference * damping_factor;
    } else {
        *ball_vx = natural_vx;
    }

    for (int pass = 0; pass < MAX_WALL_PASSES_PER_STEP; pass++) {
        if (*ball_x < border_inset) {
            *ball_x = border_inset + (border_inset - *ball_x);
            *ball_vx_direction = -*ball_vx_direction;
            float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
            uint64_t serial = surface_serials
                                  ? ++surface_serials[BOUNCE_SURFACE_LEFT]
                                  : 0;
            audit_actual_collision(predictive_pass, BOUNCE_SURFACE_LEFT,
                                   fabsf(*ball_vx), natural_vx);
            if (predictive_pass) {
                append_bounce_event(events, event_count, max_events, event_time,
                                    fabsf(*ball_vx), natural_vx,
                                    BOUNCE_SURFACE_LEFT, pan, serial);
            } else {
                maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(*ball_vx), natural_vx, pan);
            }
            continue;
        }
        if (*ball_x + ball_diameter > window_w - border_inset) {
            float right_edge = window_w - border_inset;
            *ball_x = (right_edge - ball_diameter) - ((*ball_x + ball_diameter) - right_edge);
            *ball_vx_direction = -*ball_vx_direction;
            float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
            uint64_t serial = surface_serials
                                  ? ++surface_serials[BOUNCE_SURFACE_RIGHT]
                                  : 0;
            audit_actual_collision(predictive_pass, BOUNCE_SURFACE_RIGHT,
                                   fabsf(*ball_vx), natural_vx);
            if (predictive_pass) {
                append_bounce_event(events, event_count, max_events, event_time,
                                    fabsf(*ball_vx), natural_vx,
                                    BOUNCE_SURFACE_RIGHT, pan, serial);
            } else {
                maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(*ball_vx), natural_vx, pan);
            }
            continue;
        }
        break;
    }
    /* Reflection cannot settle inside a span narrower than the ball, so the
       loop above may run out of passes. Keep it on screen either way. */
    *ball_x = clampf(*ball_x, border_inset,
                     (float)window_w - border_inset - ball_diameter);

    /* Same story vertically: ball_vy reaches 0.4 of the window per frame, so
       a fast step can clear both the ceiling and the floor. */
    for (int pass = 0; pass < MAX_WALL_PASSES_PER_STEP; pass++) {
        if (*ball_y < border_inset_norm && *ball_vy < 0.0f) {
            *ball_y = border_inset_norm + (border_inset_norm - *ball_y);
            *ball_vy = -*ball_vy;
            float impact_vy = fabsf(*ball_vy);
            float ideal_vy = get_ideal_bounce_vy(ball_diameter_normalized, gravity);
            float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
            uint64_t serial = surface_serials
                                  ? ++surface_serials[BOUNCE_SURFACE_CEILING]
                                  : 0;
            audit_actual_collision(predictive_pass, BOUNCE_SURFACE_CEILING,
                                   impact_vy, ideal_vy);
            if (predictive_pass) {
                append_bounce_event(events, event_count, max_events, event_time,
                                    impact_vy, ideal_vy,
                                    BOUNCE_SURFACE_CEILING, pan, serial);
            } else {
                maybe_play_actual_bounce_sound(immediate_audio_pass, impact_vy, ideal_vy, pan);
            }
            continue;
        }

        if ((*ball_y + ball_diameter_normalized >= g_floor_y_normalized) && (*ball_vy >= 0.0f)) {
            float impact_vy = *ball_vy;
            float penetration = (*ball_y + ball_diameter_normalized) - g_floor_y_normalized;
            *ball_y = (g_floor_y_normalized - ball_diameter_normalized) - penetration;

            *ball_vy = -*ball_vy;

            float ideal_vy = get_ideal_bounce_vy(ball_diameter_normalized, gravity);

            float vy_difference = fabsf(*ball_vy) - ideal_vy;
            if (fabsf(vy_difference) > 0.0001f) {
                float adjustment = vy_difference * 0.507f;
                *ball_vy = -(fabsf(*ball_vy) - adjustment);
            } else {
                *ball_vy = -ideal_vy;
            }

            float pan = compute_bounce_pan(*ball_x, ball_diameter, window_w);
            uint64_t serial = surface_serials
                                  ? ++surface_serials[BOUNCE_SURFACE_FLOOR]
                                  : 0;
            audit_actual_collision(predictive_pass, BOUNCE_SURFACE_FLOOR,
                                   fabsf(impact_vy), ideal_vy);
            if (predictive_pass) {
                append_bounce_event(events, event_count, max_events, event_time,
                                    fabsf(impact_vy), ideal_vy,
                                    BOUNCE_SURFACE_FLOOR, pan, serial);
            } else {
                maybe_play_actual_bounce_sound(immediate_audio_pass, fabsf(impact_vy), ideal_vy, pan);
            }
            continue;
        }
        break;
    }
    *ball_y = clampf(*ball_y, border_inset_norm,
                     g_floor_y_normalized - ball_diameter_normalized);
}
















static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void regen_make_shuffled_order(uint32_t *order, uint32_t total) {
    if (!order || total == 0) return;
    for (uint32_t i = 0; i < total; i++) {
        order[i] = i;
    }
    for (uint32_t i = total - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(random() % (long)(i + 1));
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

static const char *freerange_vert_shader_text =
    "attribute vec2 pos;\n"
    "attribute vec2 uv_in;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    uv = uv_in;\n"
    "}\n";

static const char *freerange_frag_shader_text =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "uniform float recolor;\n"
    "uniform vec3 light_color;\n"
    "uniform vec3 dark_color;\n"
    "void main() {\n"
    "    vec4 source = texture2D(tex, uv);\n"
    "    vec3 mapped = source.r * light_color + source.g * dark_color;\n"
    "    vec3 color = mix(source.rgb, mapped, recolor);\n"
    "    gl_FragColor = vec4(color, source.a) * alpha;\n"
    "}\n";

/* The HUD, the ring menu and the Ghost badge background all carry straight
 * alpha, so that stays the default. Only the ball frames are premultiplied --
 * they flip the mode for their own draw and hand it straight back. */
static void freerange_gl_blend_straight(void) {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void freerange_gl_blend_premul(void) {
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

typedef struct FreedomFrameSet FreedomFrameSet;

typedef struct {
    FreedomFrameSet *frames;
    int block_size;
    int num_blocks;
    uint32_t regen_start;
    uint32_t regen_stride;
    uint32_t regen_total;
    uint32_t *regen_order;
    int regen_mode;
    float angle_period;
    const uint8_t *shadow_pixels;
    int shadow_w;
    int shadow_h;
    int shadow_offset_x;
    int shadow_offset_y;
    int ball_offset_x;
    int ball_offset_y;
    PoingoAtomic next_seq;
    PoingoAtomic stop;
    /* A worker that cannot allocate its scratch image sets this. Without it
       the upload loop waits forever on a unit nobody will ever finish. */
    PoingoAtomic failed;
    PoingoAtomic *unit_done;
} RegenWorkerCtx;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct wl_egl_window *egl_window;
    GLuint gl_program;
    GLint gl_pos_loc;
    GLint gl_uv_loc;
    GLint gl_tex_loc;
    GLint gl_alpha_loc;
    GLint gl_recolor_loc;
    GLint gl_light_loc;
    GLint gl_dark_loc;
    GLuint *gl_textures;
    int gl_texture_count;
    bool gl_ready;
    GLuint gl_hud_texture;
    int gl_hud_w;
    int gl_hud_h;
    
    GLuint gl_ghost_bg_texture;
    float hud_x;
    float hud_y;
    uint8_t *hud_pixels;
    size_t hud_pixels_size;

    int width;
    int height;
    int pending_width;
    int pending_height;
    int refresh_mhz;
    bool configured;
    bool resize_pending;
    bool running;
    bool ghost_mode;

    bool pointer_down;
    int pointer_x;
    int pointer_y;
    bool key_vol_up_pressed;
    bool key_vol_down_pressed;
    bool key_speed_up_pressed;
    bool key_speed_down_pressed;

    int grabbed_ball;
    int menu_ball;
    float grab_u;
    float grab_v;
    float slingshot_pull_x;
    float slingshot_pull_y;

    FreedomFrameSet *frames_ref;
    bool color_regen_active;
    uint32_t color_regen_units_total;
    float color_regen_angle_period;
    uint8_t *color_regen_shadow_pixels;
    int color_regen_shadow_w;
    int color_regen_shadow_h;
    int color_regen_shadow_offset_x;
    int color_regen_shadow_offset_y;
    int color_regen_ball_offset_x;
    int color_regen_ball_offset_y;
    int color_regen_mode;
    int color_regen_block_size;
    /* Alpha fades in with the very first beam only. A colour change regen
     * replaces a ball that is already on screen, and fading that one out and
     * back in reads as a flicker rather than an arrival. */
    bool color_regen_fade_in;
    RegenWorkerCtx *regen_worker_ctx;
    RegenWorkerCtx regen_ctx_storage;
    PoingoAtomic *regen_unit_done_storage;
    uint32_t *regen_order_storage;
    pthread_t **regen_thread_storage;
    uint32_t regen_workspace_units;
    int regen_workspace_threads;
    pthread_t    **regen_threads;
    int             regen_thread_count;
    int             regen_units_uploaded;
    bool shutdown_pending;
    /* Exit fade: seconds remaining of the 0.5s alpha ramp that replaced
     * the regen/dissolve on quit, matching balloons and splat. */
    float exit_fade;
    bool ball_cleared;
    uint32_t shutdown_start_ticks;

    #define FREEDOM_MOUSE_HISTORY_SIZE 10
    struct {
        int x;
        int y;
        uint32_t time;
    } mouse_history[FREEDOM_MOUSE_HISTORY_SIZE];
    int mouse_history_index;

    PoingoBall balls[POINGO_MAX_BALLS];
    int ball_count;

    float phase_f;
    int phase_i;

    uint64_t last_counter;
    uint64_t performance_frequency;
    bool make_noise;

} FreedomState;

typedef struct FreedomFrameSet {
    uint8_t *frames;
    size_t frame_size;
    int frame_count;
    int frame_w;
    int frame_h;
} FreedomFrameSet;

static bool freerange_gl_init(FreedomState *st, const FreedomFrameSet *frames);
static void freerange_gl_shutdown(FreedomState *st);
static void freerange_gl_draw_frame(FreedomState *st, const FreedomFrameSet *frames, float extrap_dt);
static bool freerange_gl_update_hud(FreedomState *st);
static void freerange_gl_draw_hud(FreedomState *st);
static void freerange_regen_workers_shutdown(FreedomState *st);
static void freerange_regen_update_scanline(uint8_t *restrict dst_frame, int composite_w, int composite_h, int y, const uint8_t *restrict ball_pixels, int ball_w, int ball_h, const uint8_t *restrict shadow_pixels, int shadow_w, int shadow_h, int ball_x, int ball_y, int shadow_x, int shadow_y);

/* The workers write into the frame set and read the regeneration assets, so
   joining them is the first thing this does. Every teardown path -- normal
   exit and each startup failure alike -- goes through here, which is what
   makes "no worker outlives the memory it touches" an invariant rather than
   something each caller has to remember. */
static void freerange_color_regen_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    freerange_regen_workers_shutdown(st);
    free(st->color_regen_shadow_pixels);
    st->color_regen_shadow_pixels = NULL;
    st->color_regen_shadow_w = 0;
    st->color_regen_shadow_h = 0;
}

static int freerange_regen_worker(void *userdata) {
    RegenWorkerCtx *ctx = (RegenWorkerCtx *)userdata;

    size_t ball_size = (size_t)BALL_W * (size_t)BALL_H * 4;
    uint8_t *ball_pixels = NULL;
    if (posix_memalign((void **)&ball_pixels, 32, ball_size) != 0) {
        poingo_atomic_set(&ctx->failed, 1);
        return 0;
    }
    memset(ball_pixels, 0, ball_size);

    for (;;) {
        int seq = poingo_atomic_add(&ctx->next_seq, 1);
        if (seq >= (int)ctx->regen_total || poingo_atomic_get(&ctx->stop)) {
            break;
        }

        uint32_t unit;
        if (ctx->regen_order) {
            unit = ctx->regen_order[seq];
        } else {
            unit =
                (ctx->regen_start +
                 (uint32_t)((uint64_t)seq * (uint64_t)ctx->regen_stride % ctx->regen_total)) %
                ctx->regen_total;
        }

        int frame_index = (int)(unit / (uint32_t)ctx->num_blocks);
        int block_index = (int)(unit % (uint32_t)ctx->num_blocks);

        if (frame_index >= 0 && frame_index < ctx->frames->frame_count) {
            int y_start = block_index * ctx->block_size;
            int y_end = y_start + ctx->block_size;
            if (y_end > ctx->frames->frame_h) y_end = ctx->frames->frame_h;

            uint8_t *frame_dst = ctx->frames->frames +
                                 (size_t)frame_index * ctx->frames->frame_size;
            float phase_offset = ((float)frame_index / (float)ctx->frames->frame_count)
                                 * ctx->angle_period;

            for (int y = y_start; y < y_end; y++) {
                if (ctx->regen_mode == BALL_REGEN_CLEAR) {
                    uint8_t *row = frame_dst + (size_t)y * (size_t)ctx->frames->frame_w * 4;
                    memset(row, 0, (size_t)ctx->frames->frame_w * 4);
                } else {
                    int src_y = y - ctx->ball_offset_y;
                    if (src_y >= 0 && src_y < BALL_H) {
                        generate_sphere_pixels_band(ball_pixels, BALL_W, phase_offset,
                                                    src_y, src_y + 1);
                    }
                    freerange_regen_update_scanline(frame_dst,
                                                   ctx->frames->frame_w, ctx->frames->frame_h,
                                                   y,
                                                   ball_pixels, BALL_W, BALL_H,
                                                   ctx->shadow_pixels,
                                                   ctx->shadow_w, ctx->shadow_h,
                                                   ctx->ball_offset_x, ctx->ball_offset_y,
                                                   ctx->shadow_offset_x, ctx->shadow_offset_y);
                }
            }
        }

        poingo_atomic_set(&ctx->unit_done[seq], 1);
    }

    free(ball_pixels);
    return 0;
}

static void freerange_regen_workers_shutdown(FreedomState *st) {
    if (!st) return;
    if (st->regen_worker_ctx) {
        poingo_atomic_set(&st->regen_worker_ctx->stop, 1);
    }
    if (st->regen_threads) {
        for (int t = 0; t < st->regen_thread_count; t++) {
            if (st->regen_threads[t]) {
                poingo_thread_wait(st->regen_threads[t], NULL);
            }
        }
        st->regen_threads = NULL;
        st->regen_thread_count = 0;
    }
    if (st->regen_worker_ctx) {
        st->regen_worker_ctx = NULL;
    }
    st->regen_units_uploaded = 0;
    st->color_regen_active = false;
}

static bool freerange_color_regen_prepare_assets(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames) {
        return false;
    }
    if (!frames->frames || frames->frame_w <= 0 || frames->frame_h <= 0) {
        return false;
    }

    st->color_regen_ball_offset_x = g_ball_texture_offset_x;
    st->color_regen_ball_offset_y = g_ball_texture_offset_y;
    st->color_regen_mode = BALL_REGEN_FILL;

    if (!st->color_regen_shadow_pixels) {
        const int sphere_texture_size = BALL_W;
        const float canonical_total_prop = 1.0f;
        const float canonical_ball_diameter = BALL_BASE_DIAMETER * canonical_total_prop;
        const float canonical_ball_radius = canonical_ball_diameter * 0.5f;
        const float shadow_offset_x_screen = canonical_total_prop * 8.0f * 0.6f;
        const float shadow_offset_y_screen = canonical_total_prop * 8.0f * 1.0f;
        const float shadow_size_screen = canonical_ball_diameter * 1.1f + 20.0f * canonical_total_prop;

        const float scale_to_texture = (float)sphere_texture_size / canonical_ball_diameter;
        float desired_shadow_width_pixels = shadow_size_screen * scale_to_texture;
        int desired_shadow_size = (int)ceilf(desired_shadow_width_pixels);

        int shadow_w = 0, shadow_h = 0;
        uint8_t *shadow_pixels = generate_shadow_pixels(desired_shadow_size, &shadow_w, &shadow_h);
        if (!shadow_pixels) {
            return false;
        }

        const float shadow_x_screen = (canonical_ball_radius + shadow_offset_x_screen) - shadow_size_screen * 0.5f;
        const float shadow_y_screen = (canonical_ball_radius + shadow_offset_y_screen) - shadow_size_screen * 0.5f;
        const float shadow_x_tex = shadow_x_screen * scale_to_texture;
        const float shadow_y_tex = shadow_y_screen * scale_to_texture;

        st->color_regen_shadow_offset_x = (int)lroundf(shadow_x_tex + (float)st->color_regen_ball_offset_x);
        st->color_regen_shadow_offset_y = (int)lroundf(shadow_y_tex + (float)st->color_regen_ball_offset_y);
        st->color_regen_shadow_pixels = shadow_pixels;
        st->color_regen_shadow_w = shadow_w;
        st->color_regen_shadow_h = shadow_h;
    }

    return true;
}

static void freerange_regen_update_scanline(uint8_t * restrict dst_frame,
                                          int composite_w,
                                          int composite_h,
                                          int y,
                                          const uint8_t * restrict ball_pixels,
                                          int ball_w,
                                          int ball_h,
                                          const uint8_t * restrict shadow_pixels,
                                          int shadow_w,
                                          int shadow_h,
                                          int ball_x,
                                          int ball_y,
                                          int shadow_x,
                                          int shadow_y) {
    if (!dst_frame || composite_w <= 0 || composite_h <= 0 || y < 0 || y >= composite_h) {
        return;
    }

    uint8_t *row = dst_frame + (size_t)y * (size_t)composite_w * 4;
    memset(row, 0, (size_t)composite_w * 4);

    int shadow_src_y = y - shadow_y;
    if (shadow_pixels && shadow_src_y >= 0 && shadow_src_y < shadow_h) {
        int dst_x0 = shadow_x;
        int src_x0 = 0;
        int copy_w = shadow_w;
        if (dst_x0 < 0) { src_x0 = -dst_x0; copy_w -= src_x0; dst_x0 = 0; }
        if (dst_x0 + copy_w > composite_w) { copy_w = composite_w - dst_x0; }
        if (copy_w > 0) {
            const uint8_t *src = shadow_pixels + ((size_t)shadow_src_y * (size_t)shadow_w + (size_t)src_x0) * 4;
            uint8_t *dst = row + (size_t)dst_x0 * 4;
            memcpy(dst, src, (size_t)copy_w * 4);
        }
    }

    int ball_src_y = y - ball_y;
    if (ball_pixels && ball_src_y >= 0 && ball_src_y < ball_h) {
        int dst_x0 = ball_x;
        int src_x0 = 0;
        int copy_w = ball_w;
        if (dst_x0 < 0) { src_x0 = -dst_x0; copy_w -= src_x0; dst_x0 = 0; }
        if (dst_x0 + copy_w > composite_w) { copy_w = composite_w - dst_x0; }
        if (copy_w > 0) {
            const uint8_t *src = ball_pixels + ((size_t)ball_src_y * (size_t)ball_w + (size_t)src_x0) * 4;
            uint8_t *dst = row + (size_t)dst_x0 * 4;
            for (int x = 0; x < copy_w; x++) {
                blend_over_premul(dst + (size_t)x * 4, src + (size_t)x * 4);
            }
        }
    }
}


static bool __attribute__((unused)) freerange_regen_workspace_prepare(FreedomState *st,
                                               const FreedomFrameSet *frames) {
    if (!st || !frames || frames->frame_count <= 0 || frames->frame_h <= 0) return false;
    if (st->regen_workspace_units > 0) return true;
    uint32_t units = (uint32_t)frames->frame_count *
                     (uint32_t)((frames->frame_h + FREERANGE_REGEN_BLOCK_SIZE - 1) /
                                FREERANGE_REGEN_BLOCK_SIZE);
    int threads = poingo_cpu_count() - 1;
    if (threads < 1) threads = 1;
    if (units == 0) return false;
    st->regen_unit_done_storage = calloc(units, sizeof(*st->regen_unit_done_storage));
    st->regen_order_storage = malloc((size_t)units * sizeof(*st->regen_order_storage));
    st->regen_thread_storage = calloc((size_t)threads, sizeof(*st->regen_thread_storage));
    if (!st->regen_unit_done_storage || !st->regen_order_storage ||
        !st->regen_thread_storage) {
        free(st->regen_unit_done_storage);
        free(st->regen_order_storage);
        free(st->regen_thread_storage);
        st->regen_unit_done_storage = NULL;
        st->regen_order_storage = NULL;
        st->regen_thread_storage = NULL;
        return false;
    }
    st->regen_workspace_units = units;
    st->regen_workspace_threads = threads;
    return true;
}

static void freerange_color_regen_start(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames || !frames->frames || frames->frame_count <= 0 || frames->frame_h <= 0) {
        return;
    }

    /* Nothing below may run alongside the previous batch: they share the
       frame set, the completion array and the worker context. */
    freerange_regen_workers_shutdown(st);

    /* Blocks stay coarse because the Pi was pathologically slow when the work
       was handed out finely -- the cross-thread chatter cost more than the
       pixels. Size is about per-unit overhead, not pacing. */
    int block_size = FREERANGE_REGEN_BLOCK_SIZE;
    st->color_regen_block_size = block_size;

    int regen_num_blocks = (frames->frame_h + block_size - 1) / block_size;
    uint32_t total_units = (uint32_t)frames->frame_count * (uint32_t)regen_num_blocks;
    if (total_units == 0) {
        return;
    }

    uint32_t start = (uint32_t)(random() % (long)total_units);
    uint32_t stride = 1;
    if (total_units > 1) {
        for (int attempt = 0; attempt < 1000; attempt++) {
            stride = (uint32_t)(random() % (long)(total_units - 1)) + 1;
            if (gcd_u32(stride, total_units) == 1) {
                break;
            }
        }
    }

    st->color_regen_units_total = total_units;
    st->color_regen_fade_in = false;   /* opt-in; see the startup beam */

    if (!st->regen_unit_done_storage || !st->regen_order_storage ||
        !st->regen_thread_storage || total_units > st->regen_workspace_units) return;
    RegenWorkerCtx *ctx = &st->regen_ctx_storage;
    memset(ctx, 0, sizeof(*ctx));
    memset(st->regen_unit_done_storage, 0,
           (size_t)total_units * sizeof(*st->regen_unit_done_storage));
    ctx->unit_done = st->regen_unit_done_storage;

    ctx->frames          = (FreedomFrameSet *)frames;
    ctx->block_size      = block_size;
    ctx->num_blocks      = regen_num_blocks;
    ctx->regen_start     = start;
    ctx->regen_stride    = stride;
    ctx->regen_total     = total_units;
    regen_make_shuffled_order(st->regen_order_storage, total_units);
    ctx->regen_order     = st->regen_order_storage;
    ctx->regen_mode      = st->color_regen_mode;
    ctx->angle_period    = st->color_regen_angle_period;
    ctx->shadow_pixels   = st->color_regen_shadow_pixels;
    ctx->shadow_w        = st->color_regen_shadow_w;
    ctx->shadow_h        = st->color_regen_shadow_h;
    ctx->shadow_offset_x = st->color_regen_shadow_offset_x;
    ctx->shadow_offset_y = st->color_regen_shadow_offset_y;
    ctx->ball_offset_x   = st->color_regen_ball_offset_x;
    ctx->ball_offset_y   = st->color_regen_ball_offset_y;
    poingo_atomic_set(&ctx->next_seq, 0);
    poingo_atomic_set(&ctx->stop, 0);
    poingo_atomic_set(&ctx->failed, 0);

    st->regen_worker_ctx   = ctx;
    st->regen_units_uploaded = 0;

    int thread_count = poingo_cpu_count() - 1;
    if (thread_count < 1) thread_count = 1;
    if (thread_count > (int)total_units) thread_count = (int)total_units;

    st->regen_threads = st->regen_thread_storage;
    memset(st->regen_threads, 0,
           (size_t)thread_count * sizeof(*st->regen_threads));

    int spawned = 0;
    for (int t = 0; t < thread_count; t++) {
        st->regen_threads[t] = poingo_thread_create(freerange_regen_worker, "regen_worker", ctx);
        if (!st->regen_threads[t]) {
            break;
        }
        spawned++;
    }
    st->regen_thread_count = spawned;

    if (spawned == 0) {
        freerange_regen_workers_shutdown(st);
        return;
    }

    st->color_regen_active = true;
}

static void freerange_regen_upload_step(FreedomState *st, FreedomFrameSet *frames,
                                        int max_units, double target_ms) {
    if (!st || !frames || !st->color_regen_active || !st->regen_worker_ctx || target_ms <= 0.0) {
        return;
    }

    RegenWorkerCtx *ctx = st->regen_worker_ctx;

    /* A worker that could not allocate its scratch image leaves units that
       never complete. Drop the regen rather than stalling on them forever;
       the frames keep whatever they already held. */
    if (poingo_atomic_get(&ctx->failed)) {
        freerange_regen_workers_shutdown(st);
        return;
    }

    const int total        = (int)ctx->regen_total;
    const int block_size   = ctx->block_size;
    const int num_blocks   = ctx->num_blocks;

    uint64_t upload_start = poingo_perf_counter();
    int uploaded_this_tick = 0;

    while (st->regen_units_uploaded < total) {
        if (max_units > 0 && uploaded_this_tick >= max_units) break;
        if (get_perf_seconds(upload_start, poingo_perf_counter()) * 1000.0 >= target_ms) {
            break;
        }

        int seq = st->regen_units_uploaded;
        if (!poingo_atomic_get(&ctx->unit_done[seq])) {
            break;  
        }

        uint32_t unit;
        if (ctx->regen_order) {
            unit = ctx->regen_order[seq];
        } else {
            unit =
                (ctx->regen_start +
                 (uint32_t)((uint64_t)seq * (uint64_t)ctx->regen_stride % ctx->regen_total)) %
                ctx->regen_total;
        }
        int frame_index = (int)(unit / (uint32_t)num_blocks);
        int block_index = (int)(unit % (uint32_t)num_blocks);

        if (frame_index >= 0 && frame_index < frames->frame_count &&
            st->gl_ready && st->gl_textures && frame_index < st->gl_texture_count) {
            int y_start = block_index * block_size;
            int y_end   = y_start + block_size;
            if (y_end > frames->frame_h) y_end = frames->frame_h;
            const uint8_t *src = frames->frames
                                 + (size_t)frame_index * frames->frame_size
                                 + (size_t)y_start * (size_t)frames->frame_w * 4;
            glBindTexture(GL_TEXTURE_2D, st->gl_textures[frame_index]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y_start,
                            frames->frame_w, y_end - y_start,
                            GL_RGBA, GL_UNSIGNED_BYTE, src);
        }

        st->regen_units_uploaded++;
        uploaded_this_tick++;
    }

    if (st->regen_units_uploaded >= total) {
        freerange_regen_workers_shutdown(st);
        st->color_regen_active = false;
        return;
    }

    if (g_debug_mode && uploaded_this_tick > 0) {
        static int regen_log_countdown = 0;
        if (--regen_log_countdown <= 0) {
            regen_log_countdown = 60;
            int workers_at = poingo_atomic_get(&ctx->next_seq);
            fprintf(stderr, "[regen] uploaded %d  workers at %d/%d  total uploaded %d/%d\n",
                    uploaded_this_tick, workers_at, total,
                    st->regen_units_uploaded, total);
        }
    }
}


#define CURSOR_BUF 92             /* cursor buffer: blade + hilt */
#define CURSOR_BALL_SIZE 72       /* hilt ball diameter */
#define CURSOR_BALL_FRAMES 20
#define CURSOR_TIP 2              /* streak point = hotspot (x = y) */
#define STREAK_GAP 4.0f           /* float clearance between streak and ball */
#define STREAK_BORDER 2.2f        /* dark rim width just inside the edge */
#define STREAK_TAPER 0.90f        /* higher = more acute tip */
#define STREAK_NOSE 5.5f          /* rounds the point off, in taper units */
#define STREAK_R_TIP 10.0f        /* corner radius at the point */
#define STREAK_R_HEEL 12.0f       /* corner radius where the flanks meet the ball */
#define STREAK_SPAN 50.0f         /* how far round the ball the heels sit, degrees */

static struct {
    struct wl_surface *surface;
    struct wl_buffer *buffers[CURSOR_BALL_FRAMES];
    uint8_t *map;
    size_t map_size;
    uint32_t *blade;   
    int current;
    bool ready;
} g_ball_cursor;

/* Intersect two inside-positive edge fields, rolling a circle of radius r
   into the corner they form so it comes out rounded instead of sharp. */
static inline float round_isect(float e1, float e2, float r) {
    float u1 = fmaxf(r - e1, 0.0f);
    float u2 = fmaxf(r - e2, 0.0f);
    return fmaxf(fminf(e1, e2), r) - sqrtf(u1 * u1 + u2 * u2);
}

static void blade_render(void) {
    uint32_t *px = g_ball_cursor.blade;
    if (!px) return;
    memset(px, 0, (size_t)CURSOR_BUF * CURSOR_BUF * 4);

    const float s2 = 0.70710678f;
    const float tipx = CURSOR_TIP + 0.5f, tipy = CURSOR_TIP + 0.5f;
    const float bc = (float)CURSOR_BUF - CURSOR_BALL_SIZE / 2.0f - 0.5f;
    const float len = (bc - tipx) / s2;
    const float arcr = CURSOR_BALL_SIZE / 2.0f - 1.0f + STREAK_GAP;

    /* The heels are placed on the ball's clearance arc, STREAK_SPAN off the
       axis; the taper is then scaled to pass through them, so the base always
       lands on the arc instead of sailing past the ball. */
    const float span = STREAK_SPAN * PI / 180.0f;
    const float heel_t = len - arcr * cosf(span);
    const float heel_s = arcr * sinf(span);
    const float wend = heel_s / powf((heel_t + STREAK_NOSE) /
                                     (len + STREAK_NOSE), STREAK_TAPER);

    float lx3 = -0.3f, ly3 = -0.5f, lz3 = 1.0f;
    float il = 1.0f / sqrtf(lx3 * lx3 + ly3 * ly3 + lz3 * lz3);
    lx3 *= il; ly3 *= il; lz3 *= il;
    const float vdotl = s2 * lx3 - s2 * ly3;

    for (int y = 0; y < CURSOR_BUF; y++) {
        for (int x = 0; x < CURSOR_BUF; x++) {
            float rx = (float)x - tipx, ry = (float)y - tipy;
            float t = (rx + ry) * s2;      
            float s = (rx - ry) * s2;      
            if (t < -0.8f || t > len) continue;
            float tt = t < 0.0f ? 0.0f : t;
            float w = wend * powf((tt + STREAK_NOSE) / (len + STREAK_NOSE),
                                  STREAK_TAPER);

            float bdx = (float)x - bc, bdy = (float)y - bc;
            float e = round_isect(w - fabsf(s), t + 0.8f, STREAK_R_TIP);
            if (heel_t - t < e) e = heel_t - t;   /* stop at the heels */
            float eb = sqrtf(bdx * bdx + bdy * bdy) - arcr;
            e = round_isect(e, eb, STREAK_R_HEEL);
            float cov = e + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            float q = w > 0.01f ? clampf(s / w, -1.0f, 1.0f) : 0.0f;
            float nz = sqrtf(1.0f - 0.85f * q * q);
            float dot = q * vdotl + nz * lz3;
            float bright = 0.58f + 0.52f * fmaxf(0.0f, dot);
            bright *= 1.0f + 0.05f * (1.0f - tt / len);

            float r = 214.0f * bright, g = 218.0f * bright, b = 226.0f * bright;

            if (e < STREAK_BORDER) {
                float ol = (STREAK_BORDER - e) / STREAK_BORDER;
                if (ol > 1.0f) ol = 1.0f;
                ol *= 0.8f;
                r += (92.0f - r) * ol;
                g += (97.0f - g) * ol;
                b += (107.0f - b) * ol;
            }

            if (r < 0.0f) r = 0.0f;
            if (g < 0.0f) g = 0.0f;
            if (b < 0.0f) b = 0.0f;
            if (r > 255.0f) r = 255.0f;
            if (g > 255.0f) g = 255.0f;
            if (b > 255.0f) b = 255.0f;
            uint32_t a8 = (uint32_t)(cov * 255.0f + 0.5f);
            uint32_t r8 = (uint32_t)(r * cov + 0.5f);
            uint32_t g8 = (uint32_t)(g * cov + 0.5f);
            uint32_t b8 = (uint32_t)(b * cov + 0.5f);
            px[y * CURSOR_BUF + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }
}

static void ball_cursor_render_frames(void) {
    if (!g_ball_cursor.map) return;

    const int size = CURSOR_BUF;
    const float radius = CURSOR_BALL_SIZE / 2.0f - 1.0f;
    const float cx = (float)CURSOR_BUF - CURSOR_BALL_SIZE / 2.0f - 0.5f;
    const float cy = cx;
    const float inv_radius = 1.0f / radius;
    const float angle_period = (4.0f * PI) / LON_TILES;

    int bx0 = (int)(cx - radius) - 1, bx1 = (int)(cx + radius) + 2;
    int by0 = bx0, by1 = bx1;

    float light_x = -0.3f, light_y = -0.5f, light_z = 1.0f;
    float inv_len = 1.0f / sqrtf(light_x * light_x + light_y * light_y +
                                 light_z * light_z);
    light_x *= inv_len; light_y *= inv_len; light_z *= inv_len;

    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        uint32_t *px = (uint32_t *)(g_ball_cursor.map +
                                    (size_t)f * size * size * 4);
        memcpy(px, g_ball_cursor.blade, (size_t)size * size * 4);
        float phase_norm = ((float)f / (float)CURSOR_BALL_FRAMES) *
                           angle_period / (2.0f * PI);
        for (int y = by0; y < by1; y++) {
            for (int x = bx0; x < bx1; x++) {
                float dx = (float)x - cx;
                float dy = (float)y - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float cov = radius - dist + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;

                float vx = dx * inv_radius;
                float vy = dy * inv_radius;
                float r2 = vx * vx + vy * vy;
                if (r2 > 1.0f) r2 = 1.0f;
                float nz = sqrtf(1.0f - r2);

                float dot = vx * light_x + vy * light_y + nz * light_z;
                float brightness = AMBIENT_LIGHT +
                                   DIFFUSE_LIGHT * fmaxf(0.0f, dot);
                brightness = clampf(brightness, MIN_BRIGHTNESS, 1.0f);

                float na = vx * axis_ax + vy * axis_ay + nz * axis_az;
                float nu = vx * axis_ux + vy * axis_uy + nz * axis_uz;
                float nv = vx * axis_vx + vy * axis_vy + nz * axis_vz;

                float lat = asinf(clampf(na, -1.0f, 1.0f));
                int v = (int)floorf((lat + PI * 0.5f) *
                                    ((float)LAT_TILES / PI));
                if (v < 0) v = 0;
                if (v >= LAT_TILES) v = LAT_TILES - 1;

                float lon_norm = atan2f(nv, nu) / (2.0f * PI) + phase_norm;
                lon_norm -= floorf(lon_norm);
                int u = (int)(lon_norm * (float)LON_TILES);
                if (u >= LON_TILES) u = LON_TILES - 1;

                const uint8_t *rgb = (((u + v) & 1) == 0) ? g_color_light_rgb
                                                          : g_color_dark_rgb;
                uint32_t under = px[y * size + x];
                float inv = 1.0f - cov;
                uint32_t a8 = (uint32_t)(cov * 255.0f +
                                         inv * ((under >> 24) & 0xFF) + 0.5f);
                uint32_t r8 = (uint32_t)(rgb[0] * brightness * cov +
                                         inv * ((under >> 16) & 0xFF) + 0.5f);
                uint32_t g8 = (uint32_t)(rgb[1] * brightness * cov +
                                         inv * ((under >> 8) & 0xFF) + 0.5f);
                uint32_t b8 = (uint32_t)(rgb[2] * brightness * cov +
                                         inv * (under & 0xFF) + 0.5f);
                px[y * size + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
            }
        }
    }
}

static bool ball_cursor_create(struct wl_shm *shm, struct wl_compositor *compositor) {
    if (!shm || !compositor) return false;

    int stride = CURSOR_BUF * 4;
    size_t frame_bytes = (size_t)stride * CURSOR_BUF;
    size_t total = frame_bytes * CURSOR_BALL_FRAMES;

    int fd = memfd_create("poingo-cursor", MFD_CLOEXEC);
    if (unlikely(fd < 0)) return false;
    if (unlikely(ftruncate(fd, (off_t)total) < 0)) { close(fd); return false; }
    void *data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (unlikely(data == MAP_FAILED)) { close(fd); return false; }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)total);
    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        g_ball_cursor.buffers[f] = wl_shm_pool_create_buffer(
            pool, (int32_t)(f * frame_bytes),
            CURSOR_BUF, CURSOR_BUF, stride, WL_SHM_FORMAT_ARGB8888);
    }
    wl_shm_pool_destroy(pool);
    close(fd);

    g_ball_cursor.map = data;
    g_ball_cursor.map_size = total;

    g_ball_cursor.blade = malloc((size_t)CURSOR_BUF * CURSOR_BUF * 4);
    if (!g_ball_cursor.blade) return false;
    blade_render();

    compute_axis_vectors();
    ball_cursor_render_frames();

    g_ball_cursor.surface = wl_compositor_create_surface(compositor);
    if (!g_ball_cursor.surface) return false;
    wl_surface_attach(g_ball_cursor.surface, g_ball_cursor.buffers[0], 0, 0);
    wl_surface_damage(g_ball_cursor.surface, 0, 0, CURSOR_BUF, CURSOR_BUF);
    wl_surface_commit(g_ball_cursor.surface);
    g_ball_cursor.current = 0;
    g_ball_cursor.ready = true;
    return true;
}

static void ball_cursor_set(struct wl_pointer *pointer, uint32_t serial) {
    if (!g_ball_cursor.ready) return;
    wl_pointer_set_cursor(pointer, serial, g_ball_cursor.surface,
                          CURSOR_TIP, CURSOR_TIP);
}

static void ball_cursor_animate(int phase_i, int frame_count) {
    if (!g_ball_cursor.ready || frame_count <= 0) return;
    int f = (int)(((long)phase_i * CURSOR_BALL_FRAMES) / frame_count);
    f %= CURSOR_BALL_FRAMES;
    if (f < 0) f += CURSOR_BALL_FRAMES;
    if (f == g_ball_cursor.current) return;
    g_ball_cursor.current = f;
    wl_surface_attach(g_ball_cursor.surface, g_ball_cursor.buffers[f], 0, 0);
    wl_surface_damage(g_ball_cursor.surface, 0, 0, CURSOR_BUF, CURSOR_BUF);
    wl_surface_commit(g_ball_cursor.surface);
}

static void ball_cursor_destroy(void) {
    for (int f = 0; f < CURSOR_BALL_FRAMES; f++) {
        if (g_ball_cursor.buffers[f]) wl_buffer_destroy(g_ball_cursor.buffers[f]);
        g_ball_cursor.buffers[f] = NULL;
    }
    if (g_ball_cursor.surface) wl_surface_destroy(g_ball_cursor.surface);
    if (g_ball_cursor.map) munmap(g_ball_cursor.map, g_ball_cursor.map_size);
    free(g_ball_cursor.blade);
    memset(&g_ball_cursor, 0, sizeof(g_ball_cursor));
}

/* Which look the transition should switch to, alongside the palette. */
typedef enum {
    POINGO_MODE_KEEP,        /* colour only; leave the tilt and grid alone */
    POINGO_MODE_NOSTALGIA,
    POINGO_MODE_POINGO,
} PoingoModeChange;

/*
 * The only path allowed to change the ball's look. Everything it touches --
 * the global palette, the axis vectors, the shared sphere cache, the frame
 * set -- is also read by the regeneration workers, so the order is fixed:
 *
 *     join old workers -> change palette/mode -> rebuild cache -> start new
 *
 * Both halves of the menu reach the same colours by repeatable clicks
 * (NEW COLOR, NOSTALGIA, POINGO, the pickers), so a second change landing
 * mid-regen is ordinary use, not an edge case. Doing any of the middle steps
 * with workers still running is a data race.
 *
 * A NULL light_rgb means "pick a fresh colour" rather than "set this one".
 */
static void __attribute__((unused)) freerange_regen_transition(FreedomState *st, PoingoModeChange mode,
                                       const uint8_t light_rgb[3],
                                       const uint8_t dark_rgb[3]) {
    if (!st || !st->frames_ref) {
        return;
    }

    freerange_regen_workers_shutdown(st);

    if (mode == POINGO_MODE_NOSTALGIA) {
        set_a_mode_enabled(true);
    } else if (mode == POINGO_MODE_POINGO) {
        set_a_mode_enabled(false);
    }

    if (light_rgb && dark_rgb) {
        memcpy(g_color_light_rgb, light_rgb, sizeof(g_color_light_rgb));
        memcpy(g_color_dark_rgb, dark_rgb, sizeof(g_color_dark_rgb));
    } else {
        float light_value = fmaxf(g_color_light_rgb[0],
                                  fmaxf(g_color_light_rgb[1], g_color_light_rgb[2])) / 255.0f;
        pick_palette_colors(light_value,
                            g_color_light_rgb, g_color_dark_rgb,
                            false);
    }

    invalidate_sphere_pixel_cache();
    if (!ensure_sphere_pixel_cache(BALL_W)) {
        return;
    }

    if (!freerange_color_regen_prepare_assets(st, st->frames_ref)) {
        return;
    }
    freerange_color_regen_start(st, st->frames_ref);
}

static void freerange_request_graceful_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    if (st->ball_cleared || st->shutdown_pending) {
        return;
    }
    if (st->ghost_mode) {
        /* The Ghost badge is rendered from frame texture 0.  Clearing the
         * ball frames during shutdown would make the badge disintegrate. */
        st->ball_cleared = true;
        st->shutdown_pending = true;
        st->shutdown_start_ticks = poingo_ticks_ms();
        audio_begin_shutdown_fade(POINGO_EXIT_FADE_SECONDS);
        return;
    }
    if (st->color_regen_mode == BALL_REGEN_CLEAR && st->color_regen_active) {
        return;
    }
    /* Fade out rather than running the clear/regen dissolve. The regen is
     * still used for colour changes; it is only the exit that fades, so quit
     * looks the same as balloons and splat.
     *
     * Deliberately does NOT set ball_cleared: that flag skips
     * update_ball_physics, which would freeze the ball in mid-air for the
     * whole half second it is fading. The ball keeps bouncing as it goes.
     * Re-entry is guarded by shutdown_pending above instead. */
    st->shutdown_pending = true;
    st->exit_fade = POINGO_EXIT_FADE_SECONDS;
    st->shutdown_start_ticks = poingo_ticks_ms();
    audio_begin_shutdown_fade(POINGO_EXIT_FADE_SECONDS);
}

static void ball_size(PoingoBall *ball, int width, int height) {
    if (!ball || width <= 0 || height <= 0) {
        return;
    }

    float prop_x = (float)width / CANVAS_WIDTH;
    float prop_y = (float)height / CANVAS_HEIGHT;
    ball->diameter = BALL_BASE_DIAMETER * fminf(prop_x, prop_y) * ball->scale;
    ball->diameter_norm = ball->diameter / (float)height;
}

static int ball_at(const FreedomState *st, int x, int y) {
    if (!st || st->ghost_mode) {
        return -1;
    }

    for (int i = st->ball_count - 1; i >= 0; i--) {
        const PoingoBall *ball = &st->balls[i];
        float cx = ball->x + ball->diameter * 0.5f;
        float cy = ball->y * (float)st->height + ball->diameter * 0.5f;
        float dx = (float)x - cx;
        float dy = (float)y - cy;
        float radius = ball->diameter * 0.5f;
        if (dx * dx + dy * dy <= radius * radius) {
            return i;
        }
    }
    return -1;
}

static void ball_velocity(const PoingoBall *ball, int height,
                          float *vx, float *vy) {
    *vx = ball->vx * (float)ball->vx_direction;
    *vy = ball->vy * (float)height;
}

static void ball_set_velocity(PoingoBall *ball, int height,
                              float vx, float vy) {
    ball->vx_direction = vx < 0.0f ? -1 : 1;
    ball->vx = fabsf(vx);
    ball->vy = vy / (float)height;
}

static void ball_sound(const PoingoBall *ball, int index) {
    g_bounce_slot = index;
    BallAudio *audio = current_ball_audio();
    BounceSoundStyle style = ball->mode == BALL_MODE_NOSTALGIA
                                 ? BOUNCE_SOUND_NOSTALGIA
                                 : BOUNCE_SOUND_POINGO;
    float size_scale = remap_normalized_scale_to_sound(
        ball->diameter / BALL_BASE_DIAMETER);
    float selected_scale = audio->pair.dirty
                               ? audio->pending_size_scale
                               : audio->current_size_scale;
    if (audio->style == style &&
        fabsf(selected_scale - size_scale) <= 0.001f) {
        return;
    }

    audio->style = style;
    mark_sounds_dirty(size_scale);
}

static void solve_ball_pair(FreedomState *st, int a_index, int b_index,
                            BallSound sound) {
    PoingoBall *a = &st->balls[a_index];
    PoingoBall *b = &st->balls[b_index];
    float ax = a->x + a->diameter * 0.5f;
    float ay = a->y * (float)st->height + a->diameter * 0.5f;
    float bx = b->x + b->diameter * 0.5f;
    float by = b->y * (float)st->height + b->diameter * 0.5f;
    float dx = bx - ax;
    float dy = by - ay;
    float radius_sum = (a->diameter + b->diameter) * 0.5f;
    float distance_sq = dx * dx + dy * dy;
    if (distance_sq >= radius_sum * radius_sum) {
        return;
    }

    float distance = sqrtf(distance_sq);
    float nx = distance > 0.001f ? dx / distance : 1.0f;
    float ny = distance > 0.001f ? dy / distance : 0.0f;
    float avx, avy, bvx, bvy;
    ball_velocity(a, st->height, &avx, &avy);
    ball_velocity(b, st->height, &bvx, &bvy);
    float approach_speed = -((bvx - avx) * nx + (bvy - avy) * ny);
    float inv_a = a_index == st->grabbed_ball
                      ? 0.0f : 1.0f / (a->diameter * a->diameter);
    float inv_b = b_index == st->grabbed_ball
                      ? 0.0f : 1.0f / (b->diameter * b->diameter);
    float inv_sum = inv_a + inv_b;
    if (inv_sum <= 0.0f) {
        return;
    }

    if (approach_speed > 0.0f) {
        float impulse = (1.0f + POINGO_COLLISION_RESTITUTION) *
                        approach_speed / inv_sum;
        avx -= impulse * inv_a * nx;
        avy -= impulse * inv_a * ny;
        bvx += impulse * inv_b * nx;
        bvy += impulse * inv_b * ny;
        if (inv_a > 0.0f) {
            ball_set_velocity(a, st->height, avx, avy);
        }
        if (inv_b > 0.0f) {
            ball_set_velocity(b, st->height, bvx, bvy);
        }

        float baseline = get_natural_vx(st->width);
        if (sound == BALL_SOUND_PLAY &&
            impact_speed_is_audible(approach_speed, baseline)) {
            float pan = ((ax + bx) * 0.5f) / (float)st->width;
            play_ting_sound(approach_speed / baseline, pan);
        }
    }

    /* Separate overlap in inverse-mass proportion so unequal hollow shells
     * do not gain energy from positional correction. */
    float correction = (radius_sum - distance) / inv_sum;
    a->x -= correction * inv_a * nx;
    a->y -= correction * inv_a * ny / (float)st->height;
    b->x += correction * inv_b * nx;
    b->y += correction * inv_b * ny / (float)st->height;
    a->x = clampf(a->x, 0.0f, (float)st->width - a->diameter);
    b->x = clampf(b->x, 0.0f, (float)st->width - b->diameter);
    a->y = clampf(a->y, 0.0f, g_floor_y_normalized - a->diameter_norm);
    b->y = clampf(b->y, 0.0f, g_floor_y_normalized - b->diameter_norm);
}

static void sweep_grabbed_ball(FreedomState *st, float old_x, float old_y,
                               float new_x, float new_y) {
    if (!st || st->grabbed_ball < 0 ||
        st->grabbed_ball >= st->ball_count) {
        return;
    }

    PoingoBall *grabbed = &st->balls[st->grabbed_ball];
    float dx = new_x - old_x;
    float dy = (new_y - old_y) * (float)st->height;
    float step_travel = grabbed->diameter *
                        POINGO_COLLISION_TRAVEL_FRACTION;
    int steps = step_travel > 0.0f
                    ? (int)ceilf(sqrtf(dx * dx + dy * dy) / step_travel)
                    : 1;
    steps = steps < 1 ? 1 : steps;
    steps = steps > POINGO_COLLISION_MAX_STEPS
                ? POINGO_COLLISION_MAX_STEPS : steps;

    for (int step = 1; step <= steps; step++) {
        float progress = (float)step / (float)steps;
        grabbed->x = old_x + (new_x - old_x) * progress;
        grabbed->y = old_y + (new_y - old_y) * progress;
        for (int i = 0; i < st->ball_count; i++) {
            if (i == st->grabbed_ball) {
                continue;
            }
            int a = i < st->grabbed_ball ? i : st->grabbed_ball;
            int b = i < st->grabbed_ball ? st->grabbed_ball : i;
            solve_ball_pair(st, a, b,
                            st->make_noise ? BALL_SOUND_PLAY : BALL_SOUND_SILENT);
        }
    }
}

static int ball_substeps(const FreedomState *st, double sim_delta) {
    float max_velocity = 0.0f;
    float min_diameter = st->balls[0].diameter;
    for (int i = 0; i < st->ball_count; i++) {
        const PoingoBall *ball = &st->balls[i];
        max_velocity = fmaxf(max_velocity, ball->vx);
        max_velocity = fmaxf(max_velocity, fabsf(ball->vy) * (float)st->height);
        min_diameter = fminf(min_diameter, ball->diameter);
    }

    float travel = max_velocity * (float)(sim_delta * 60.0 *
                                           g_speed_multiplier);
    float step_travel = min_diameter * POINGO_COLLISION_TRAVEL_FRACTION;
    int steps = step_travel > 0.0f ? (int)ceilf(travel / step_travel) : 1;
    if (steps < 1) {
        return 1;
    }
    if (steps > POINGO_COLLISION_MAX_STEPS) {
        return POINGO_COLLISION_MAX_STEPS;
    }
    return steps;
}

static void step_balls(FreedomState *st, double sim_delta, BallSound sound) {
    if (!st || st->ball_count <= 0) {
        return;
    }

    int steps = st->ball_count > 1 ? ball_substeps(st, sim_delta) : 1;
    double step_delta = sim_delta / (double)steps;
    for (int step = 0; step < steps; step++) {
        for (int i = 0; i < st->ball_count; i++) {
            PoingoBall *ball = &st->balls[i];
            ball_sound(ball, i);
            update_ball_physics(&ball->x, &ball->y,
                                &ball->vx, &ball->vy,
                                &ball->vx_direction,
                                st->width, 0.0f, 0.0f,
                                ball->diameter, ball->diameter_norm,
                                step_delta, i == st->grabbed_ball,
                                sound == BALL_SOUND_PLAY,
                                g_actual_bounce_serial,
                                NULL, NULL, 0, 0.0f);
        }

        for (int a = 0; a < st->ball_count; a++) {
            for (int b = a + 1; b < st->ball_count; b++) {
                solve_ball_pair(st, a, b, sound);
            }
        }
    }
}

static float random_unit(void) {
    return (float)random() / (float)RAND_MAX;
}

static void random_ball_palette(PoingoBall *ball) {
    uint8_t saved_light[3];
    uint8_t saved_dark[3];
    memcpy(saved_light, g_color_light_rgb, sizeof(saved_light));
    memcpy(saved_dark, g_color_dark_rgb, sizeof(saved_dark));
    memcpy(g_color_light_rgb, ball->light_rgb, sizeof(g_color_light_rgb));
    memcpy(g_color_dark_rgb, ball->dark_rgb, sizeof(g_color_dark_rgb));
    pick_palette_colors(0.75f + random_unit() * 0.25f,
                        ball->light_rgb, ball->dark_rgb, false);
    memcpy(g_color_light_rgb, saved_light, sizeof(g_color_light_rgb));
    memcpy(g_color_dark_rgb, saved_dark, sizeof(g_color_dark_rgb));
}

static void set_ball_mode(PoingoBall *ball, BallMode mode) {
    if (!ball) {
        return;
    }

    static const uint8_t poingo_light[3] = {
        COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B
    };
    static const uint8_t poingo_dark[3] = {
        COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B
    };
    static const uint8_t nostalgia_light[3] = {255, 255, 255};
    static const uint8_t nostalgia_dark[3] = {255, 0, 0};
    const uint8_t *light = mode == BALL_MODE_NOSTALGIA
                               ? nostalgia_light : poingo_light;
    const uint8_t *dark = mode == BALL_MODE_NOSTALGIA
                              ? nostalgia_dark : poingo_dark;
    memcpy(ball->light_rgb, light, 3);
    memcpy(ball->dark_rgb, dark, 3);
    ball->mode = mode;
}

static bool ball_fits(const FreedomState *st, const PoingoBall *candidate) {
    float cx = candidate->x + candidate->diameter * 0.5f;
    float cy = candidate->y * (float)st->height + candidate->diameter * 0.5f;
    for (int i = 0; i < st->ball_count; i++) {
        const PoingoBall *ball = &st->balls[i];
        float bx = ball->x + ball->diameter * 0.5f;
        float by = ball->y * (float)st->height + ball->diameter * 0.5f;
        float dx = bx - cx;
        float dy = by - cy;
        float gap = (ball->diameter + candidate->diameter) * 0.5f +
                    POINGO_SPAWN_GAP;
        if (dx * dx + dy * dy < gap * gap) {
            return false;
        }
    }
    return true;
}

static bool add_ball(FreedomState *st) {
    if (!st || st->ball_count >= POINGO_MAX_BALLS ||
        st->width <= 0 || st->height <= 0) {
        return false;
    }

    PoingoBall ball = {
        .vx_direction = 1,
        .scale = 1.0f,
        .light_rgb = {COLOR_LIGHT_R, COLOR_LIGHT_G, COLOR_LIGHT_B},
        .dark_rgb = {COLOR_DARK_R, COLOR_DARK_G, COLOR_DARK_B},
        .mode = BALL_MODE_POINGO,
    };
    for (int attempt = 0; attempt < POINGO_SPAWN_ATTEMPTS; attempt++) {
        float room_bias = 1.0f - (float)attempt /
                          (float)POINGO_SPAWN_ATTEMPTS;
        ball.scale = FREEDOM_BALL_SCALE_MIN + random_unit() * room_bias *
                     (FREEDOM_BALL_SCALE_MAX - FREEDOM_BALL_SCALE_MIN);
        ball_size(&ball, st->width, st->height);
        float max_x = (float)st->width - ball.diameter;
        float max_y = g_floor_y_normalized - ball.diameter_norm;
        if (max_x < 0.0f || max_y < 0.0f) {
            continue;
        }
        ball.x = random_unit() * max_x;
        ball.y = random_unit() * max_y;
        if (!ball_fits(st, &ball)) {
            continue;
        }

        float natural_vx = get_natural_vx(st->width);
        float ideal_vy = get_ideal_bounce_vy(ball.diameter_norm, GRAVITY);
        ball.vx = natural_vx * (0.5f + random_unit() * 2.5f);
        ball.vx_direction = random() & 1 ? 1 : -1;
        ball.vy = ideal_vy * (random_unit() * 2.0f - 1.0f);
        random_ball_palette(&ball);
        st->balls[st->ball_count++] = ball;
        audio_predict_reset(g_audio_sim_time);
        return true;
    }
    return false;
}

static void remove_ball(FreedomState *st, int index) {
    if (!st || index < 0 || index >= st->ball_count) {
        return;
    }
    if (st->ball_count == 1) {
        freerange_request_graceful_shutdown(st);
        return;
    }

    if (index + 1 < st->ball_count) {
        memmove(&st->balls[index], &st->balls[index + 1],
                (size_t)(st->ball_count - index - 1) * sizeof(st->balls[0]));
    }
    st->ball_count--;
    if (st->grabbed_ball == index) {
        st->grabbed_ball = -1;
        st->pointer_down = false;
    } else if (st->grabbed_ball > index) {
        st->grabbed_ball--;
    }
    st->menu_ball = -1;
    audio_predict_reset(g_audio_sim_time);
}


/* Returns 0 on failure. Without the status check a driver that rejects the
   GLSL still links a do-nothing program, and Poingo opens as an empty
   window -- which is close to undiagnosable from a bug report. */
static GLuint freerange_gl_create_shader(const char *source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "Failed to compile %s shader: %s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}



static bool freerange_gl_upload_frames(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames || !frames->frames || frames->frame_count <= 0) {
        return false;
    }

    st->gl_textures = calloc((size_t)frames->frame_count, sizeof(GLuint));
    if (!st->gl_textures) {
        return false;
    }

    st->gl_texture_count = frames->frame_count;
    glGenTextures(frames->frame_count, st->gl_textures);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    uint8_t *blank = NULL;
    if (st->color_regen_active && st->color_regen_mode == BALL_REGEN_FILL &&
        st->regen_units_uploaded == 0) {
        blank = calloc(frames->frame_size, 1);
    }

    for (int i = 0; i < frames->frame_count; i++) {
        const uint8_t *frame = blank ? blank
                                     : frames->frames + (size_t)i * frames->frame_size;
        glBindTexture(GL_TEXTURE_2D, st->gl_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     frames->frame_w, frames->frame_h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, frame);
    }
    free(blank);
    return true;
}

static bool freerange_gl_init(FreedomState *st, const FreedomFrameSet *frames) {
    if (!st || !frames) {
        return false;
    }

    GLuint vert = freerange_gl_create_shader(freerange_vert_shader_text, GL_VERTEX_SHADER);
    GLuint frag = freerange_gl_create_shader(freerange_frag_shader_text, GL_FRAGMENT_SHADER);
    if (!vert || !frag) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    st->gl_program = glCreateProgram();
    glAttachShader(st->gl_program, vert);
    glAttachShader(st->gl_program, frag);
    glLinkProgram(st->gl_program);

    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint linked = GL_FALSE;
    glGetProgramiv(st->gl_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {0};
        glGetProgramInfoLog(st->gl_program, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "Failed to link GL program: %s\n", log);
        return false;
    }

    glUseProgram(st->gl_program);

    st->gl_pos_loc = glGetAttribLocation(st->gl_program, "pos");
    st->gl_uv_loc = glGetAttribLocation(st->gl_program, "uv_in");
    st->gl_tex_loc = glGetUniformLocation(st->gl_program, "tex");
    /* alpha carries the startup beam and the exit fade, so a missing location
       is as fatal as a missing texture sampler. */
    st->gl_alpha_loc = glGetUniformLocation(st->gl_program, "alpha");
    st->gl_recolor_loc = glGetUniformLocation(st->gl_program, "recolor");
    st->gl_light_loc = glGetUniformLocation(st->gl_program, "light_color");
    st->gl_dark_loc = glGetUniformLocation(st->gl_program, "dark_color");
    if (st->gl_pos_loc < 0 || st->gl_uv_loc < 0 || st->gl_tex_loc < 0 ||
        st->gl_alpha_loc < 0 || st->gl_recolor_loc < 0 ||
        st->gl_light_loc < 0 || st->gl_dark_loc < 0) {
        fprintf(stderr, "GL program is missing an expected attribute or uniform\n");
        return false;
    }

    glUniform1i(st->gl_tex_loc, 0);
    glUniform1f(st->gl_recolor_loc, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    freerange_gl_blend_straight();

    if (!freerange_gl_upload_frames(st, frames)) {
        return false;
    }
    
    {
        uint32_t *bg = ghost_icon_create_bg(false);
        if (bg) {
            glGenTextures(1, &st->gl_ghost_bg_texture);
            glBindTexture(GL_TEXTURE_2D, st->gl_ghost_bg_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GHOST_ICON_SIZE, GHOST_ICON_SIZE,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            free(bg);
        }
    }

    st->gl_ready = true;
    return true;
}

static void freerange_gl_shutdown(FreedomState *st) {
    if (!st) {
        return;
    }
    if (st->gl_textures && st->gl_texture_count > 0) {
        glDeleteTextures(st->gl_texture_count, st->gl_textures);
        free(st->gl_textures);
        st->gl_textures = NULL;
        st->gl_texture_count = 0;
    }
    if (st->gl_program) {
        glDeleteProgram(st->gl_program);
        st->gl_program = 0;
    }
    if (st->gl_hud_texture) {
        glDeleteTextures(1, &st->gl_hud_texture);
        st->gl_hud_texture = 0;
    }
    if (st->gl_ghost_bg_texture) {
        glDeleteTextures(1, &st->gl_ghost_bg_texture);
        st->gl_ghost_bg_texture = 0;
    }
    free(st->hud_pixels);
    st->hud_pixels = NULL;
    st->hud_pixels_size = 0;
    st->gl_hud_w = 0;
    st->gl_hud_h = 0;
    st->gl_ready = false;
}


typedef struct {
    int x, y, w, h;
} FreerangeRect;

static void freerange_rect_union(FreerangeRect *acc, const FreerangeRect *r) {
    if (!r || r->w <= 0 || r->h <= 0) {
        return;
    }
    if (acc->w <= 0 || acc->h <= 0) {
        *acc = *r;
        return;
    }
    int x0 = acc->x < r->x ? acc->x : r->x;
    int y0 = acc->y < r->y ? acc->y : r->y;
    int x1 = (acc->x + acc->w) > (r->x + r->w) ? (acc->x + acc->w) : (r->x + r->w);
    int y1 = (acc->y + acc->h) > (r->y + r->h) ? (acc->y + acc->h) : (r->y + r->h);
    acc->x = x0;
    acc->y = y0;
    acc->w = x1 - x0;
    acc->h = y1 - y0;
}

static void freerange_rect_clamp(FreerangeRect *r, int width, int height) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > width)  r->w = width  - r->x;
    if (r->y + r->h > height) r->h = height - r->y;
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
}

static bool freerange_ball_dest_quad(const FreedomState *st, const PoingoBall *ball,
                                     const FreedomFrameSet *frames,
                                     float extrap_dt,
                                     float *out_x, float *out_y, float *out_w, float *out_h) {
    if (!st || !ball || !frames || frames->frame_count <= 0 ||
        st->width <= 0 || st->height <= 0) {
        return false;
    }

    float exscale = extrap_dt * 60.0f * g_speed_multiplier;
    float rx = ball->x + (ball->vx * (float)ball->vx_direction) * exscale;
    float ry = ball->y + ball->vy * exscale;
    float rx_max = (float)st->width - ball->diameter;
    float ry_max = 1.0f - ball->diameter_norm;
    if (rx < 0.0f) rx = 0.0f;
    if (rx > rx_max) rx = rx_max;
    if (ry < 0.0f) ry = 0.0f;
    if (ry > ry_max) ry = ry_max;

    float ball_y_pixels = ry * (float)st->height;
    float composite_scale = ball->diameter / (float)BALL_W;
    float composite_offset_x = (float)g_ball_texture_offset_x * composite_scale;
    float composite_offset_y = (float)g_ball_texture_offset_y * composite_scale;

    *out_x = rx - composite_offset_x;
    *out_y = ball_y_pixels - composite_offset_y;
    *out_w = (float)frames->frame_w * composite_scale;
    *out_h = (float)frames->frame_h * composite_scale;
    return true;
}

static void freerange_gl_draw_ball(FreedomState *st, const PoingoBall *ball,
                                   const FreedomFrameSet *frames, float extrap_dt) {
    if (!st || !ball || !st->gl_ready) {
        return;
    }

    float dest_x, dest_y, composite_w, composite_h;
    if (!freerange_ball_dest_quad(st, ball, frames, extrap_dt,
                                  &dest_x, &dest_y, &composite_w, &composite_h)) {
        return;
    }

    int mode_frame_count = frames->frame_count / BALL_MODE_COUNT;
    int frame_index = (int)ball->mode * mode_frame_count + st->phase_i;
    if (frame_index < 0 || frame_index >= frames->frame_count) {
        return;
    }

    float x0 = (dest_x / (float)st->width) * 2.0f - 1.0f;
    float x1 = ((dest_x + composite_w) / (float)st->width) * 2.0f - 1.0f;
    float y0 = 1.0f - (dest_y / (float)st->height) * 2.0f;
    float y1 = 1.0f - ((dest_y + composite_h) / (float)st->height) * 2.0f;

    GLfloat verts[] = {
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f
    };

    glUseProgram(st->gl_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->gl_textures[frame_index]);
    glUniform1f(st->gl_recolor_loc, 1.0f);
    glUniform3f(st->gl_light_loc,
                ball->light_rgb[0] / 255.0f,
                ball->light_rgb[1] / 255.0f,
                ball->light_rgb[2] / 255.0f);
    glUniform3f(st->gl_dark_loc,
                ball->dark_rgb[0] / 255.0f,
                ball->dark_rgb[1] / 255.0f,
                ball->dark_rgb[2] / 255.0f);
    glVertexAttribPointer(st->gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(st->gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(st->gl_pos_loc);
    glEnableVertexAttribArray(st->gl_uv_loc);
    float base_alpha = st->ghost_mode ? 0.4f : 1.0f;
    if (st->shutdown_pending) {
        float k = st->exit_fade / POINGO_EXIT_FADE_SECONDS;
        base_alpha *= k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    }
    /* Fade in alongside the first beam. It fills the ball in random chunks, so
     * on a machine where a chunk lands badly the seams show; riding an alpha
     * ramp over the top hides that for nothing -- the blend is already
     * happening, only the uniform changes. Tied to how much has actually been
     * uploaded rather than to a clock, so it tracks the beam exactly however
     * fast the machine runs it.
     *
     * Not applied to a colour change: there the ball is already on screen, and
     * dimming it to nothing and back is a flicker, not an arrival. */
    if (st->color_regen_fade_in && st->color_regen_active &&
        st->color_regen_units_total > 0) {
        float progress = (float)st->regen_units_uploaded /
                         (float)st->color_regen_units_total;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        base_alpha *= progress;
    }
    glUniform1f(st->gl_alpha_loc, base_alpha);
    freerange_gl_blend_premul();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    freerange_gl_blend_straight();
}

static void freerange_gl_draw_frame(FreedomState *st, const FreedomFrameSet *frames,
                                    float extrap_dt) {
    if (!st) {
        return;
    }

    for (int i = 0; i < st->ball_count; i++) {
        if (i == st->grabbed_ball) {
            continue;
        }
        freerange_gl_draw_ball(st, &st->balls[i], frames, extrap_dt);
    }
    if (st->grabbed_ball >= 0 && st->grabbed_ball < st->ball_count) {
        freerange_gl_draw_ball(st, &st->balls[st->grabbed_ball], frames,
                               extrap_dt);
    }
}

static void hud_blend_pixel(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) {
        return;
    }

    uint8_t dst_a = dst[3];
    if (dst_a == 0) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = a;
        return;
    }

    uint16_t inv_a = (uint16_t)(255 - a);
    uint16_t out_a = (uint16_t)a + (uint16_t)(dst_a * inv_a) / 255;
    if (out_a == 0) {
        return;
    }
    dst[0] = (uint8_t)(((uint16_t)r * a + (uint16_t)dst[0] * dst_a * inv_a / 255) / out_a);
    dst[1] = (uint8_t)(((uint16_t)g * a + (uint16_t)dst[1] * dst_a * inv_a / 255) / out_a);
    dst[2] = (uint8_t)(((uint16_t)b * a + (uint16_t)dst[2] * dst_a * inv_a / 255) / out_a);
    dst[3] = (uint8_t)out_a;
}

static void hud_fill_rect(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!buf || buf_w <= 0 || buf_h <= 0 || w <= 0 || h <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > buf_w) x1 = buf_w;
    if (y1 > buf_h) y1 = buf_h;

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = buf + ((size_t)yy * (size_t)buf_w + (size_t)x0) * 4;
        for (int xx = x0; xx < x1; xx++) {
            hud_blend_pixel(row, r, g, b, a);
            row += 4;
        }
    }
}

static void hud_draw_text(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, const char *text, int scale,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!buf || buf_w <= 0 || buf_h <= 0 || !text || *text == '\0' || scale <= 0) {
        return;
    }

    int cursor_x = x;
    for (const char *p = text; *p != '\0'; ++p) {
        const GlyphDef *glyph = lookup_volume_glyph(*p);
        if (glyph) {
            for (int row = 0; row < GLYPH_ROWS; row++) {
                const char *pattern = glyph->rows[row];
                for (int col = 0; col < GLYPH_COLS; col++) {
                    if (pattern[col] != '.' && pattern[col] != ' ') {
                        int px = cursor_x + col * scale;
                        int py = y + row * scale;
                        hud_fill_rect(buf, buf_w, buf_h, px, py, scale, scale, r, g, b, a);
                    }
                }
            }
        }
        cursor_x += GLYPH_COLS * scale;
        if (p[1] != '\0') {
            cursor_x += scale;
        }
    }
}

static bool freerange_gl_update_hud(FreedomState *st) {
    if (!st || !st->gl_ready || st->width <= 0 || st->height <= 0) {
        return false;
    }

    float hud_alpha = fmaxf(g_volume_hud_alpha, g_speed_hud_alpha);
    if (hud_alpha <= 0.0f) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    float hud_width = fminf(380.0f, st->width * 0.7f);
    float hud_x = (st->width - hud_width) * 0.5f;
    float hud_y = st->height * 0.08f;
    float inner_margin = 10.0f;
    float bar_height = 14.0f;
    float bar_text_gap = 6.0f;
    float text_scale_f = 4.0f;
    float text_height = GLYPH_ROWS * text_scale_f;
    float section_gap = 12.0f;

    float volume_bar_y = hud_y + inner_margin;
    float volume_text_y = volume_bar_y + bar_height + bar_text_gap;
    float speed_bar_y = volume_text_y + text_height + section_gap;
    float speed_text_y = speed_bar_y + bar_height + bar_text_gap;
    float hud_height = (speed_text_y + text_height + inner_margin) - hud_y;

    char volume_label[32];
    if (g_volume_muted) {
        snprintf(volume_label, sizeof(volume_label), "^ MUTED v");
    } else {
        int percent = (int)lroundf((g_volume_hud_value / MASTER_VOLUME_MAX) * 100.0f);
        percent = percent < 0 ? 0 : (percent > 999 ? 999 : percent);
        snprintf(volume_label, sizeof(volume_label), "^ VOL %d%% v", percent);
    }

    char speed_label[48];
    float display_speed = clamp_speed(g_speed_multiplier);
    snprintf(speed_label, sizeof(speed_label), "< SPEED %.1fX>", display_speed);

    float volume_text_width = measure_text(volume_label, text_scale_f);
    float speed_text_width = measure_text(speed_label, text_scale_f);
    float max_text_width = fmaxf(volume_text_width, speed_text_width);
    float bar_width = hud_width - inner_margin * 2.0f;
    bool hud_fits =
        hud_width >= max_text_width + inner_margin * 2.0f &&
        bar_width > 1.0f &&
        hud_x >= 2.0f &&
        hud_x + hud_width <= (float)st->width - 2.0f &&
        hud_y >= 2.0f &&
        hud_y + hud_height <= (float)st->height - 2.0f;
    if (!hud_fits) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    int hud_w = (int)ceilf(hud_width);
    int hud_h = (int)ceilf(hud_height);
    if (hud_w <= 0 || hud_h <= 0) {
        st->gl_hud_w = 0;
        st->gl_hud_h = 0;
        return false;
    }

    size_t needed = (size_t)hud_w * (size_t)hud_h * 4;
    if (needed > st->hud_pixels_size) {
        uint8_t *new_pixels = realloc(st->hud_pixels, needed);
        if (!new_pixels) {
            st->gl_hud_w = 0;
            st->gl_hud_h = 0;
            return false;
        }
        st->hud_pixels = new_pixels;
        st->hud_pixels_size = needed;
    }
    memset(st->hud_pixels, 0, needed);

    int text_scale = (int)lroundf(text_scale_f);
    float volume_ratio = (g_volume_hud_value - MASTER_VOLUME_MIN) /
                         (MASTER_VOLUME_MAX - MASTER_VOLUME_MIN);
    volume_ratio = fmaxf(0.0f, fminf(1.0f, volume_ratio));

    int bar_x = (int)lroundf(inner_margin);
    int bar_y = (int)lroundf(inner_margin);
    int bar_w = (int)lroundf(bar_width);
    int bar_h = (int)lroundf(bar_height);
    int volume_text_x = (int)lroundf((hud_width - volume_text_width) * 0.5f);
    int volume_text_y_int = (int)lroundf(volume_text_y - hud_y);
    int speed_text_x = (int)lroundf((hud_width - speed_text_width) * 0.5f);
    int speed_text_y_int = (int)lroundf(speed_text_y - hud_y);

    uint8_t bg_alpha = (uint8_t)lroundf(hud_alpha * 180.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, 0, 0, hud_w, hud_h, 0, 0, 0, bg_alpha);

    uint8_t bar_bg_alpha = (uint8_t)lroundf(hud_alpha * 120.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, bar_w, bar_h,
                  255, 255, 255, bar_bg_alpha);

    int fill_w = (int)lroundf(bar_w * volume_ratio);
    uint8_t fill_alpha = (uint8_t)lroundf(hud_alpha * 230.0f);
    if (fill_w > 0) {
        if (g_volume_muted) {
            hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, fill_w, bar_h,
                          255, 90, 90, fill_alpha);
        } else {
            hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, bar_y, fill_w, bar_h,
                          80, 200, 255, fill_alpha);
        }
    }

    uint8_t text_alpha = (uint8_t)lroundf(hud_alpha * 255.0f);
    if (g_volume_muted) {
        hud_draw_text(st->hud_pixels, hud_w, hud_h, volume_text_x, volume_text_y_int,
                      volume_label, text_scale, 255, 200, 200, text_alpha);
    } else {
        hud_draw_text(st->hud_pixels, hud_w, hud_h, volume_text_x, volume_text_y_int,
                      volume_label, text_scale, 220, 240, 255, text_alpha);
    }

    int speed_bar_y_int = (int)lroundf(speed_bar_y - hud_y);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, speed_bar_y_int, bar_w, bar_h,
                  255, 255, 255, bar_bg_alpha);

    float speed_ratio = (g_speed_multiplier - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
    speed_ratio = fmaxf(0.0f, fminf(1.0f, speed_ratio));
    int speed_fill_w = (int)lroundf(bar_w * speed_ratio);
    uint8_t speed_fill_alpha = (uint8_t)lroundf(hud_alpha * 230.0f);
    if (speed_fill_w > 0) {
        hud_fill_rect(st->hud_pixels, hud_w, hud_h, bar_x, speed_bar_y_int,
                      speed_fill_w, bar_h, 50, 220, 150, speed_fill_alpha);
    }

    float normal_speed_ratio = (1.0f - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
    normal_speed_ratio = fmaxf(0.0f, fminf(1.0f, normal_speed_ratio));
    int crossbar_x = bar_x + (int)lroundf(bar_w * normal_speed_ratio);
    uint8_t crossbar_alpha = (uint8_t)lroundf(hud_alpha * 0.9f * 255.0f);
    hud_fill_rect(st->hud_pixels, hud_w, hud_h, crossbar_x, speed_bar_y_int, 2, bar_h,
                  255, 255, 255, crossbar_alpha);

    hud_draw_text(st->hud_pixels, hud_w, hud_h, speed_text_x, speed_text_y_int,
                  speed_label, text_scale, 210, 255, 210, text_alpha);

    if (!st->gl_hud_texture) {
        glGenTextures(1, &st->gl_hud_texture);
    }
    glBindTexture(GL_TEXTURE_2D, st->gl_hud_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (st->gl_hud_w != hud_w || st->gl_hud_h != hud_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hud_w, hud_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, st->hud_pixels);
        st->gl_hud_w = hud_w;
        st->gl_hud_h = hud_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud_w, hud_h, GL_RGBA,
                        GL_UNSIGNED_BYTE, st->hud_pixels);
    }

    st->hud_x = hud_x;
    st->hud_y = hud_y;
    return true;
}

static void freerange_gl_draw_hud(FreedomState *st) {
    if (!st || !st->gl_hud_texture || st->gl_hud_w <= 0 || st->gl_hud_h <= 0) {
        return;
    }

    float x0 = (st->hud_x / (float)st->width) * 2.0f - 1.0f;
    float x1 = ((st->hud_x + st->gl_hud_w) / (float)st->width) * 2.0f - 1.0f;
    float y0 = 1.0f - (st->hud_y / (float)st->height) * 2.0f;
    float y1 = 1.0f - ((st->hud_y + st->gl_hud_h) / (float)st->height) * 2.0f;

    GLfloat verts[] = {
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f
    };

    glUseProgram(st->gl_program);
    glUniform1f(st->gl_recolor_loc, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->gl_hud_texture);
    glVertexAttribPointer(st->gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(st->gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(st->gl_pos_loc);
    glEnableVertexAttribArray(st->gl_uv_loc);
    glUniform1f(st->gl_alpha_loc, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Returns false once the connection is gone, so the caller can wind down
   instead of spinning on a dead socket. */
static bool freerange_pump_events(struct wl_display *display, int timeout_ms) {
    if (!display) {
        return false;
    }

    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
            return false;
        }
    }

    /* The moving input region alone is many wl_region_add requests per frame,
       so the outgoing buffer really can fill. EAGAIN means "not all of it
       went"; waiting only for POLLIN there would sit on an unsent batch until
       the timeout. Wait for writability too and let the next flush finish. */
    short events = POLLIN;
    if (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        events |= POLLOUT;
    }

    struct pollfd pfd = {
        .fd = wl_display_get_fd(display),
        .events = events,
        .revents = 0
    };

    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result < 0 && errno != EINTR) {
        wl_display_cancel_read(display);
        return false;
    }
    if (poll_result <= 0 || !(pfd.revents & POLLIN)) {
        wl_display_cancel_read(display);
    } else if (wl_display_read_events(display) < 0) {
        return false;
    }

    return wl_display_dispatch_pending(display) >= 0;
}

static void add_ball_region(struct wl_region *region, const PoingoBall *ball,
                            int width, int height) {
    float center_x = ball->x + ball->diameter * 0.5f;
    float center_y = ball->y * (float)height + ball->diameter * 0.5f;
    float radius = ball->diameter * 0.5f;
    const int row_step = 2;
    int y_start = (int)floorf(center_y - radius);
    int y_end = (int)ceilf(center_y + radius);
    y_start = y_start < 0 ? 0 : y_start;
    y_end = y_end > height ? height : y_end;

    for (int y = y_start; y < y_end; y += row_step) {
        float dy = (float)y + 0.5f - center_y;
        float inside = radius * radius - dy * dy;
        if (inside <= 0.0f) {
            continue;
        }
        float dx = sqrtf(inside);
        int x = (int)floorf(center_x - dx);
        int region_width = (int)ceilf(dx * 2.0f);
        if (x < 0) {
            region_width += x;
            x = 0;
        }
        if (x + region_width > width) {
            region_width = width - x;
        }
        if (region_width > 0) {
            wl_region_add(region, x, y, region_width, row_step);
        }
    }
}

static void freerange_update_input_region(FreedomState *st) {
    if (!st || !st->compositor || !st->surface ||
        st->width <= 0 || st->height <= 0) {
        return;
    }

    struct wl_region *region = wl_compositor_create_region(st->compositor);
    if (!region) {
        return;
    }

    if (g_menu && ringmenu_is_open(g_menu)) {
        wl_region_add(region, 0, 0, st->width, st->height);
    } else if (st->ghost_mode) {
        wl_region_add(region,
                      st->width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN,
                      GHOST_ICON_MARGIN, GHOST_ICON_SIZE, GHOST_ICON_SIZE);
    } else {
        for (int i = 0; i < st->ball_count; i++) {
            add_ball_region(region, &st->balls[i], st->width, st->height);
        }
    }

    wl_surface_set_input_region(st->surface, region);
    wl_region_destroy(region);
}

typedef struct {
    int sphere_size;
    int shadow_size;
    int shadow_offset_x;
    int shadow_offset_y;
} FreerangeFrameGeometry;

static bool freerange_prepare_frame_geometry(FreerangeFrameGeometry *geometry) {
    compute_axis_vectors();

    const int sphere_texture_size = BALL_W;
    const float canonical_ball_diameter = BALL_BASE_DIAMETER;
    const float canonical_ball_radius = canonical_ball_diameter * 0.5f;
    const float shadow_offset_x_screen = 8.0f * 0.6f;
    const float shadow_offset_y_screen = 8.0f;
    const float shadow_size_screen = canonical_ball_diameter * 1.1f + 20.0f;

    const float scale_to_texture = sphere_texture_size / canonical_ball_diameter;
    int shadow_size = (int)ceilf(shadow_size_screen * scale_to_texture);

    if (!ensure_sphere_pixel_cache(sphere_texture_size)) {
        fprintf(stderr, "Failed to build sphere pixel cache\n");
        return false;
    }

    const float shadow_x_screen = (canonical_ball_radius + shadow_offset_x_screen) - shadow_size_screen * 0.5f;
    const float shadow_y_screen = (canonical_ball_radius + shadow_offset_y_screen) - shadow_size_screen * 0.5f;
    const float shadow_x_tex = shadow_x_screen * scale_to_texture;
    const float shadow_y_tex = shadow_y_screen * scale_to_texture;

    const float min_x = fminf(0.0f, shadow_x_tex);
    const float min_y = fminf(0.0f, shadow_y_tex);
    const float max_x = fmaxf((float)sphere_texture_size, shadow_x_tex + shadow_size);
    const float max_y = fmaxf((float)sphere_texture_size, shadow_y_tex + shadow_size);

    g_frame_texture_w = (int)ceilf(max_x - min_x);
    g_frame_texture_h = (int)ceilf(max_y - min_y);
    g_ball_texture_offset_x = (int)lroundf(-min_x);
    g_ball_texture_offset_y = (int)lroundf(-min_y);

    geometry->sphere_size = sphere_texture_size;
    geometry->shadow_size = shadow_size;
    geometry->shadow_offset_x = (int)lroundf(shadow_x_tex + g_ball_texture_offset_x);
    geometry->shadow_offset_y = (int)lroundf(shadow_y_tex + g_ball_texture_offset_y);
    return true;
}

static bool freerange_generate_frames(FreedomFrameSet *out_frames, int frame_count, float angle_period) {
    if (!out_frames || frame_count <= 0) {
        return false;
    }

    FreerangeFrameGeometry geometry;
    if (!freerange_prepare_frame_geometry(&geometry)) {
        return false;
    }

    int shadow_w = 0, shadow_h = 0;
    uint8_t *shadow_pixels = generate_shadow_pixels(
        geometry.shadow_size, &shadow_w, &shadow_h);
    if (!shadow_pixels) {
        fprintf(stderr, "Failed to generate shadow texture\n");
        return false;
    }

    size_t frame_size = (size_t)g_frame_texture_w * (size_t)g_frame_texture_h * 4;
    size_t total_frame_bytes = frame_size * (size_t)frame_count;
    uint8_t *frame_buffer = NULL;
    if (posix_memalign((void**)&frame_buffer, 32, total_frame_bytes) != 0) {
        fprintf(stderr, "Failed to allocate frame pixel buffer\n");
        free(shadow_pixels);
        return false;
    }

    BallFrameJobContext job_ctx = {
        .frame_count = frame_count,
        .angle_period = angle_period,
        .sphere_texture_size = geometry.sphere_size,
        .shadow_pixels = shadow_pixels,
        .shadow_w = shadow_w,
        .shadow_h = shadow_h,
        .composite_w = g_frame_texture_w,
        .composite_h = g_frame_texture_h,
        .ball_offset_x = g_ball_texture_offset_x,
        .ball_offset_y = g_ball_texture_offset_y,
        .shadow_offset_x = geometry.shadow_offset_x,
        .shadow_offset_y = geometry.shadow_offset_y,
        .frame_buffer = frame_buffer,
        .frame_size = frame_size
    };

    poingo_atomic_set(&job_ctx.next_index, 0);
    poingo_atomic_set(&job_ctx.failure, 0);
    poingo_atomic_set(&job_ctx.completed, 0);

    int thread_count = poingo_cpu_count();
    if (thread_count < 1) {
        thread_count = 1;
    }
    if (thread_count > frame_count) {
        thread_count = frame_count;
    }

    pthread_t **threads = calloc((size_t)thread_count, sizeof(pthread_t *));
    if (!threads) {
        fprintf(stderr, "Failed to allocate frame threads\n");
        free(frame_buffer);
        free(shadow_pixels);
        return false;
    }

    for (int t = 0; t < thread_count; t++) {
        threads[t] = poingo_thread_create(generate_ball_frame_worker, "ball_frame_worker", &job_ctx);
        if (!threads[t]) {
            poingo_atomic_set(&job_ctx.failure, 1);
            thread_count = t;
            break;
        }
    }

    for (int t = 0; t < thread_count; t++) {
        if (threads[t]) {
            poingo_thread_wait(threads[t], NULL);
        }
    }

    bool failed = poingo_atomic_get(&job_ctx.failure) != 0 ||
                  poingo_atomic_get(&job_ctx.completed) != frame_count;

    free(threads);
    free(shadow_pixels);

    if (failed) {
        fprintf(stderr, "Ball frame generation failed\n");
        free(frame_buffer);
        return false;
    }

    out_frames->frames = frame_buffer;
    out_frames->frame_size = frame_size;
    out_frames->frame_count = frame_count;
    out_frames->frame_w = g_frame_texture_w;
    out_frames->frame_h = g_frame_texture_h;
    return true;
}

static bool __attribute__((unused)) freerange_prepare_blank_frames(FreedomFrameSet *out_frames,
        int frame_count) {
    if (!out_frames || frame_count <= 0) {
        return false;
    }

    FreerangeFrameGeometry geometry;
    if (!freerange_prepare_frame_geometry(&geometry)) {
        return false;
    }

    size_t frame_size = (size_t)g_frame_texture_w * (size_t)g_frame_texture_h * 4;
    size_t total_frame_bytes = frame_size * (size_t)frame_count;
    uint8_t *frame_buffer = NULL;
    if (posix_memalign((void**)&frame_buffer, 32, total_frame_bytes) != 0) {
        fprintf(stderr, "Failed to allocate blank frame buffer\n");
        return false;
    }
    memset(frame_buffer, 0, total_frame_bytes);

    out_frames->frames = frame_buffer;
    out_frames->frame_size = frame_size;
    out_frames->frame_count = frame_count;
    out_frames->frame_w = g_frame_texture_w;
    out_frames->frame_h = g_frame_texture_h;
    return true;
}

static void freerange_destroy_frames(FreedomFrameSet *frames) {
    if (!frames) {
        return;
    }
    free(frames->frames);
    frames->frames = NULL;
    frames->frame_size = 0;
    frames->frame_count = 0;
    frames->frame_w = 0;
    frames->frame_h = 0;
}

/* Red and green encode the two checker colors; the shader supplies each
 * ball's palette without duplicating the animation textures. */
static bool make_basis_frames(FreedomFrameSet *out_frames, int frame_count,
                              float angle_period) {
    uint8_t saved_light[3];
    uint8_t saved_dark[3];
    bool saved_mode = g_a_mode;
    FreedomFrameSet modes[BALL_MODE_COUNT] = {0};
    bool ok = true;

    memcpy(saved_light, g_color_light_rgb, sizeof(saved_light));
    memcpy(saved_dark, g_color_dark_rgb, sizeof(saved_dark));

    for (int mode = 0; mode < BALL_MODE_COUNT; mode++) {
        const uint8_t basis_light[3] = {255, 0, 0};
        const uint8_t basis_dark[3] = {0, 255, 0};
        memcpy(g_color_light_rgb, basis_light, sizeof(g_color_light_rgb));
        memcpy(g_color_dark_rgb, basis_dark, sizeof(g_color_dark_rgb));
        set_a_mode_enabled(mode == BALL_MODE_NOSTALGIA);
        invalidate_sphere_pixel_cache();

        if (!freerange_generate_frames(&modes[mode], frame_count,
                                       angle_period)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        size_t total_size = modes[0].frame_size * (size_t)frame_count *
                            BALL_MODE_COUNT;
        uint8_t *combined = malloc(total_size);
        if (!combined) {
            ok = false;
        } else {
            for (int mode = 0; mode < BALL_MODE_COUNT; mode++) {
                memcpy(combined + modes[mode].frame_size *
                       (size_t)frame_count * (size_t)mode,
                       modes[mode].frames,
                       modes[mode].frame_size * (size_t)frame_count);
            }
            out_frames->frames = combined;
            out_frames->frame_size = modes[0].frame_size;
            out_frames->frame_count = frame_count * BALL_MODE_COUNT;
            out_frames->frame_w = modes[0].frame_w;
            out_frames->frame_h = modes[0].frame_h;
        }
    }

    for (int mode = 0; mode < BALL_MODE_COUNT; mode++) {
        freerange_destroy_frames(&modes[mode]);
    }

    memcpy(g_color_light_rgb, saved_light, sizeof(g_color_light_rgb));
    memcpy(g_color_dark_rgb, saved_dark, sizeof(g_color_dark_rgb));
    set_a_mode_enabled(saved_mode);
    invalidate_sphere_pixel_cache();
    return ok;
}

static void freerange_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener freerange_wm_base_listener = {
    .ping = freerange_wm_base_ping,
};

static void freerange_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                       int32_t width, int32_t height, struct wl_array *states) {
    (void)toplevel;
    (void)states;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (width > 0 && height > 0) {
        st->pending_width = width;
        st->pending_height = height;
    }
}

static void freerange_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    FreedomState *st = data;
    if (st) {
        /* Treat compositor/taskbar close exactly like the in-app QUIT action. */
        freerange_request_graceful_shutdown(st);
    }
}

static const struct xdg_toplevel_listener freerange_toplevel_listener = {
    .configure = freerange_toplevel_configure,
    .close = freerange_toplevel_close,
};

static void freerange_xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    FreedomState *st = data;
    if (!st) {
        return;
    }
    xdg_surface_ack_configure(surface, serial);
    st->configured = true;
    if (st->pending_width > 0 && st->pending_height > 0) {
        st->width = st->pending_width;
        st->height = st->pending_height;
        st->pending_width = 0;
        st->pending_height = 0;
        st->resize_pending = true;
    }
}

static const struct xdg_surface_listener freerange_xdg_surface_listener = {
    .configure = freerange_xdg_surface_configure,
};

static void freerange_output_mode(void *data, struct wl_output *output,
                                uint32_t flags, int width, int height, int refresh) {
    (void)output;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        st->width = width;
        st->height = height;
        st->refresh_mhz = refresh;
    }
}

static void freerange_output_geometry(void *data, struct wl_output *output,
                                    int32_t x, int32_t y,
                                    int32_t phys_width, int32_t phys_height,
                                    int32_t subpixel,
                                    const char *make, const char *model,
                                    int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)phys_width;
    (void)phys_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void freerange_output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
}

static void freerange_output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data;
    (void)output;
    (void)factor;
}

static const struct wl_output_listener freerange_output_listener = {
    .geometry = freerange_output_geometry,
    .mode = freerange_output_mode,
    .done = freerange_output_done,
    .scale = freerange_output_scale,
};

static void freerange_pointer_enter(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, struct wl_surface *surface,
                                  wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)surface;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    st->pointer_x = wl_fixed_to_int(surface_x);
    st->pointer_y = wl_fixed_to_int(surface_y);
    ball_cursor_set(pointer, serial);
}

static void freerange_pointer_leave(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;

    if (!g_menu || !ringmenu_is_open(g_menu)) {
        return;
    }

    /*
     * A right release that arrived just before this, in the same batch, is
     * still waiting on freerange_resolve_pending_ring_release() to decide
     * what it meant -- flag that a leave came with it, so that function
     * cancels instead of committing. See g_ring_right_release_pending's own
     * comment for why the decision has to wait this long.
     *
     * If no release is pending, the pointer has left mid-hold, with the
     * button (if any) still down and nothing decided yet -- a genuine right
     * drag that swept off the edge of the window, or a left-button drag
     * still holding the colour picker open. Either way the menu has no
     * pointer to react to any more, so it cancels immediately rather than
     * being left open with no way to close it until the pointer happens to
     * come back.
     */
    g_ring_pointer_left_pending = true;
    if (!g_ring_right_release_pending) {
        ringmenu_button(g_menu, RINGMENU_BTN_MIDDLE, true);
        if (g_picker_slot >= 0) {
            poingo_menu_set_drop(g_picker_slot, g_picker_original);
            g_picker_slot = -1;
            g_picker_locked = false;
        }
        poingo_menu_sync_drops();
    }
}

static void freerange_pointer_motion(void *data, struct wl_pointer *pointer,
                                   uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    int x = wl_fixed_to_int(surface_x);
    int y = wl_fixed_to_int(surface_y);
    st->pointer_x = x;
    st->pointer_y = y;

    if (g_menu && ringmenu_is_open(g_menu)) {
        ringmenu_motion(g_menu, x, y);

        int raw = ringmenu_field_slot(g_menu, x, y);
        int slot = -1;
        if (raw >= 0) {
            if (g_picker_locked) {
                slot = g_picker_slot;
            } else {
                if (raw < 2) slot = raw;
                g_picker_locked = true;
            }
        } else {
            g_picker_locked = false;
        }

        if (slot != g_picker_slot) {
            if (g_picker_slot >= 0) {
                poingo_menu_set_drop(g_picker_slot, g_picker_original);
            }
            g_picker_slot = slot;
            if (slot >= 0) {
                const uint8_t *cur = (slot == 0) ? g_color_light_rgb
                                                 : g_color_dark_rgb;
                memcpy(g_picker_original, cur, 3);
                memcpy(g_picker_color, cur, 3);
            }
        }

        if (slot >= 0) {
            uint8_t pr, pg, pb;
            if (ringmenu_field_color(g_menu, slot, x, y, &pr, &pg, &pb) &&
                (pr != g_picker_color[0] || pg != g_picker_color[1] ||
                 pb != g_picker_color[2])) {
                /* Motion arrives at the mouse's report rate in 1/256ths of a
                   pixel, so plenty of events land on the colour already shown;
                   repainting for those is just a dropped frame. */
                g_picker_color[0] = pr;
                g_picker_color[1] = pg;
                g_picker_color[2] = pb;
                poingo_menu_set_drop(slot, g_picker_color);
            }
        }
        return;
    }

    uint32_t now = poingo_ticks_ms();
    st->mouse_history[st->mouse_history_index].x = x;
    st->mouse_history[st->mouse_history_index].y = y;
    st->mouse_history[st->mouse_history_index].time = now;
    st->mouse_history_index = (st->mouse_history_index + 1) % FREEDOM_MOUSE_HISTORY_SIZE;

    if (st->pointer_down && st->grabbed_ball >= 0 &&
        st->grabbed_ball < st->ball_count && st->height > 0) {
        PoingoBall *ball = &st->balls[st->grabbed_ball];
        float old_x = ball->x;
        float old_y = ball->y;
        float desired_ball_x = (float)x - st->grab_u * ball->diameter;
        float desired_ball_y = ((float)y - st->grab_v * ball->diameter) /
                               (float)st->height;

        float border_inset = 0.0f;
        float border_inset_norm = 0.0f;
        float ball_diameter = ball->diameter;
        float ball_diameter_norm = ball->diameter_norm;

        float new_x = fmaxf(border_inset, fminf(
                            desired_ball_x,
                            (float)st->width - ball_diameter - border_inset));
        float new_y = fmaxf(border_inset_norm, fminf(
                            desired_ball_y,
                            1.0f - ball_diameter_norm - border_inset_norm));

        st->slingshot_pull_x = desired_ball_x - new_x;
        st->slingshot_pull_y = desired_ball_y - new_y;

        int previous = (st->mouse_history_index + FREEDOM_MOUSE_HISTORY_SIZE - 2) %
                       FREEDOM_MOUSE_HISTORY_SIZE;
        uint32_t previous_time = st->mouse_history[previous].time;
        if (previous_time > 0 && now > previous_time) {
            float frames = 16.67f / (float)(now - previous_time);
            float vx = (new_x - old_x) * frames;
            float vy = (new_y - old_y) * (float)st->height * frames;
            ball_set_velocity(ball, st->height, vx, vy);
        }
        sweep_grabbed_ball(st, old_x, old_y, new_x, new_y);
    }
}

/* Ends a left-button grab: drops the ball and hands it whatever velocity
   the slingshot pull or the flick history calls for. Split out of the
   button handler because the ring menu swallows the release event and
   still has to let go of the ball. */
static void freerange_release_grab(FreedomState *st) {
    st->pointer_down = false;

    int grabbed = st->grabbed_ball;
    st->grabbed_ball = -1;
    if (grabbed >= 0 && grabbed < st->ball_count) {
        PoingoBall *ball = &st->balls[grabbed];

        float slingshot_threshold_x = 20.0f;
        float slingshot_threshold_y = 20.0f / (float)st->height;
        bool has_slingshot_x = fabsf(st->slingshot_pull_x) > slingshot_threshold_x;
        bool has_slingshot_y = fabsf(st->slingshot_pull_y) > slingshot_threshold_y;

        if (has_slingshot_x || has_slingshot_y) {
            float slingshot_scale = 0.3f * 3.0f;
            if (has_slingshot_x) {
                ball->vx = fabsf(st->slingshot_pull_x * slingshot_scale);
                ball->vx_direction = st->slingshot_pull_x > 0.0f ? -1 : 1;
            } else {
                ball->vx = 0.0f;
            }
            if (has_slingshot_y) {
                ball->vy = -(st->slingshot_pull_y * slingshot_scale);
            } else {
                ball->vy = 0.0f;
            }

            float max_vx = (float)st->width * 0.4f;
            float max_vy = 0.4f;
            if (ball->vx > max_vx) {
                ball->vx = max_vx;
            }
            if (fabsf(ball->vy) > max_vy) {
                ball->vy = ball->vy > 0.0f ? max_vy : -max_vy;
            }
        } else {
            uint32_t current_time = poingo_ticks_ms();
            uint32_t flick_time_window = 100;
            int oldest_valid_index = -1;
            uint32_t oldest_time = current_time;
            for (int i = 0; i < FREEDOM_MOUSE_HISTORY_SIZE; i++) {
                if (st->mouse_history[i].time > 0 &&
                    (current_time - st->mouse_history[i].time) <= flick_time_window &&
                    st->mouse_history[i].time < oldest_time) {
                    oldest_time = st->mouse_history[i].time;
                    oldest_valid_index = i;
                }
            }
            if (oldest_valid_index >= 0) {
                float dx = (float)st->pointer_x - (float)st->mouse_history[oldest_valid_index].x;
                float dy = (float)st->pointer_y - (float)st->mouse_history[oldest_valid_index].y;
                float dt = (float)(current_time - st->mouse_history[oldest_valid_index].time);
                if (dt > 0.001f) {
                    float total_distance = sqrtf(dx * dx + dy * dy);
                    float speed_pixels_per_ms = total_distance / dt;
                    if (speed_pixels_per_ms > 1.0f) {
                        float vx_pixels_per_ms = dx / dt;
                        float vy_pixels_per_ms = dy / dt;
                        float ms_to_frame_60fps = 16.67f;
                        float scale_factor_x = 2.0f * ms_to_frame_60fps;
                        float scale_factor_y = 1.0f * ms_to_frame_60fps;

                        ball->vx = fabsf(vx_pixels_per_ms * scale_factor_x);
                        ball->vx_direction = vx_pixels_per_ms >= 0.0f ? 1 : -1;
                        ball->vy = (vy_pixels_per_ms * scale_factor_y) /
                                   (float)st->height;

                        float max_vx = (float)st->width * 0.4f;
                        float max_vy = 0.4f;
                        if (ball->vx > max_vx) {
                            ball->vx = max_vx;
                        }
                        if (fabsf(ball->vy) > max_vy) {
                            ball->vy = ball->vy > 0.0f ? max_vy : -max_vy;
                        }
                    } else {
                        ball->vx = 0.0f;
                        ball->vy = 0.0f;
                    }
                } else {
                    ball->vx = 0.0f;
                    ball->vy = 0.0f;
                }
            } else {
                ball->vx = 0.0f;
                ball->vy = 0.0f;
            }
        }

        st->slingshot_pull_x = 0.0f;
        st->slingshot_pull_y = 0.0f;
    }
}

/*
 * Everything a resolved ring-menu button event does, whether the release
 * was acted on immediately (left, middle, right-press) or held back and
 * decided later by freerange_resolve_pending_ring_release(). Pulled out of
 * freerange_pointer_button() so the deferred right-release path -- see
 * g_ring_right_release_pending's comment -- can call exactly the same
 * logic a normal, immediate button event would have.
 */
static void freerange_ring_menu_button(FreedomState *st, int rbtn, bool pressed) {
    if (!pressed && g_picker_slot >= 0 &&
        (rbtn == RINGMENU_BTN_LEFT || rbtn == RINGMENU_BTN_RIGHT)) {
        int slot = g_picker_slot;
        g_picker_slot = -1;
        g_picker_locked = false;
        ringmenu_button(g_menu, rbtn, pressed);
        if (st->menu_ball >= 0 && st->menu_ball < st->ball_count) {
            PoingoBall *ball = &st->balls[st->menu_ball];
            uint8_t *target = slot == 0 ? ball->light_rgb : ball->dark_rgb;
            memcpy(target, g_picker_color, 3);
            memcpy(g_color_light_rgb, ball->light_rgb, 3);
            memcpy(g_color_dark_rgb, ball->dark_rgb, 3);
        }
        poingo_menu_sync_drops();
        return;
    }

    int result = ringmenu_button(g_menu, rbtn, pressed);
    if (result >= 0 && g_picker_slot >= 0) {
        poingo_menu_set_drop(g_picker_slot, g_picker_original);
        g_picker_slot = -1;
        g_picker_locked = false;
    }
    if (result > 0) {
        PoingoBall *ball = st->menu_ball >= 0 && st->menu_ball < st->ball_count
                               ? &st->balls[st->menu_ball] : NULL;
        if (result == POINGO_MENU_NOSTALGIA && ball) {
            set_ball_mode(ball, BALL_MODE_NOSTALGIA);
        } else if (result == POINGO_MENU_POINGO && ball) {
            set_ball_mode(ball, BALL_MODE_POINGO);
        } else if (result == POINGO_MENU_NEWCOLOR && ball) {
            random_ball_palette(ball);
        } else if (result == POINGO_MENU_ADD_BALL) {
            (void)add_ball(st);
        } else if (result == POINGO_MENU_REMOVE_BALL) {
            remove_ball(st, st->menu_ball);
        } else if (result == POINGO_MENU_MUTE) {
            toggle_master_mute();
        } else if (result == POINGO_MENU_GHOST) {
            st->ghost_mode = true;
            g_ghost_mute = true;
        } else if (result == POINGO_MENU_QUIT) {
            freerange_request_graceful_shutdown(st);
        }
        if (ball && result != POINGO_MENU_REMOVE_BALL) {
            memcpy(g_color_light_rgb, ball->light_rgb, 3);
            memcpy(g_color_dark_rgb, ball->dark_rgb, 3);
        }
        poingo_menu_sync_drops();
    }
}

/*
 * The FreedomState is a single, stable instance for the process's whole
 * run (main() keeps it on its own stack frame and every callback is reached
 * through the same &st), so holding a raw pointer to it across the short
 * window between a deferred right release and its resolution next frame is
 * safe -- there is only ever one, and it outlives every event this loop
 * will ever dispatch.
 */
static FreedomState *g_ring_deferred_st = NULL;

/*
 * Decides a right-button release that freerange_pointer_button() held back
 * instead of acting on immediately, once the whole event batch that could
 * contain an accompanying leave has been drained. See
 * g_ring_right_release_pending's comment for why the decision cannot be
 * made any earlier than this. A no-op when nothing is pending, which is
 * every call after an ordinary frame with no ring-menu right release in it.
 */
static void freerange_resolve_pending_ring_release(void) {
    if (!g_ring_right_release_pending) {
        return;
    }
    bool cancel = g_ring_pointer_left_pending;
    FreedomState *st = g_ring_deferred_st;
    g_ring_right_release_pending = false;
    g_ring_pointer_left_pending = false;
    g_ring_deferred_st = NULL;

    if (!st || !g_menu || !ringmenu_is_open(g_menu)) {
        return;
    }

    if (cancel) {
        /*
         * A leave arrived in the same batch as the release: this was
         * labwc correcting a press it delivered live and then had to
         * withdraw, not a real user decision, and committing whatever the
         * pointer happened to be over the instant it was withdrawn is
         * exactly the bug this whole deferral exists to avoid. Cancel
         * through the ring menu's own supported path instead of treating
         * the release as a selection.
         */
        ringmenu_button(g_menu, RINGMENU_BTN_MIDDLE, true);
        if (g_picker_slot >= 0) {
            poingo_menu_set_drop(g_picker_slot, g_picker_original);
            g_picker_slot = -1;
            g_picker_locked = false;
        }
        poingo_menu_sync_drops();
        return;
    }

    freerange_ring_menu_button(st, RINGMENU_BTN_RIGHT, false);
}

static void freerange_pointer_button(void *data, struct wl_pointer *pointer,
                                   uint32_t serial, uint32_t time, uint32_t button,
                                   uint32_t state) {
    (void)pointer;
    (void)serial;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (g_menu && ringmenu_is_open(g_menu)) {
        /* The grab and the menu are separate states that happen to share the
           pointer, so an open menu must not swallow the end of a grab: the
           release still drops the ball, then goes on to the menu as usual. */
        if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_RELEASED) {
            freerange_release_grab(st);
        }

        int rbtn = -1;
        if (button == BTN_LEFT) rbtn = RINGMENU_BTN_LEFT;
        else if (button == BTN_RIGHT) rbtn = RINGMENU_BTN_RIGHT;
        else if (button == BTN_MIDDLE) rbtn = RINGMENU_BTN_MIDDLE;

        if (rbtn >= 0) {
            bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);

            /*
             * Only a right release is ever at risk of being labwc's
             * synthetic correction rather than a real one -- see
             * g_ring_right_release_pending's comment -- so only that case
             * is held back. Left and middle are unaffected: nothing about
             * the compositor's screen-bar promotion touches them.
             */
            if (rbtn == RINGMENU_BTN_RIGHT && !pressed) {
                g_ring_right_release_pending = true;
                g_ring_pointer_left_pending = false;
                g_ring_deferred_st = st;
                return;
            }

            freerange_ring_menu_button(st, rbtn, pressed);
        }
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_RIGHT) {
        int target = ball_at(st, st->pointer_x, st->pointer_y);
        if (g_menu && target >= 0) {
            st->menu_ball = target;
            memcpy(g_color_light_rgb, st->balls[target].light_rgb, 3);
            memcpy(g_color_dark_rgb, st->balls[target].dark_rgb, 3);
            g_picker_slot = -1;
            g_picker_locked = false;
            poingo_menu_sync_drops();
            ringmenu_set_led(g_menu, POINGO_MENU_MUTE - 1,
                             g_mixer.muted ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, POINGO_MENU_GHOST - 1,
                             st->ghost_mode ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_open(g_menu, st->pointer_x, st->pointer_y, st->width, st->height);
        }
        return;
    }

    if (st->ghost_mode) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            st->ghost_mode = false;
            g_ghost_mute = false;
        }
        return;
    }

    if (button == BTN_MIDDLE) {
        return;
    }

    if (button != BTN_LEFT) {
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        st->pointer_down = true;

        int target = ball_at(st, st->pointer_x, st->pointer_y);
        if (target >= 0) {
            PoingoBall *ball = &st->balls[target];
            st->grabbed_ball = target;
            if (ball->diameter > 0.0f) {
                float ball_y_pixels = ball->y * (float)st->height;
                st->grab_u = ((float)st->pointer_x - ball->x) / ball->diameter;
                st->grab_v = ((float)st->pointer_y - ball_y_pixels) /
                             ball->diameter;
                st->grab_u = fmaxf(0.0f, fminf(1.0f, st->grab_u));
                st->grab_v = fmaxf(0.0f, fminf(1.0f, st->grab_v));
            } else {
                st->grab_u = 0.5f;
                st->grab_v = 0.5f;
            }
            st->slingshot_pull_x = 0.0f;
            st->slingshot_pull_y = 0.0f;
        }
    } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        freerange_release_grab(st);
    }
}

static void freerange_adjust_ball_scale(FreedomState *st, int direction) {
    if (!st || st->grabbed_ball < 0 ||
        st->grabbed_ball >= st->ball_count || direction == 0) {
        return;
    }

    PoingoBall *ball = &st->balls[st->grabbed_ball];
    ball->scale += (float)direction * FREEDOM_BALL_SCALE_STEP;
    ball->scale = clamp_freerange_ball_scale(ball->scale);

    if (st->width > 0 && st->height > 0) {
        float prop_x = (float)st->width / CANVAS_WIDTH;
        float prop_y = (float)st->height / CANVAS_HEIGHT;
        float total_prop = fminf(prop_x, prop_y);
        float ball_diameter = BALL_BASE_DIAMETER * total_prop * ball->scale;
        ball->diameter = ball_diameter;
        ball->diameter_norm = ball_diameter / (float)st->height;

        float border_inset = 0.0f;
        float border_inset_norm = 0.0f;
        float desired_ball_x = (float)st->pointer_x - (st->grab_u * ball_diameter);
        float desired_ball_y = ((float)st->pointer_y - (st->grab_v * ball_diameter)) / (float)st->height;

        ball->x = fmaxf(border_inset, fminf(desired_ball_x,
                        (float)st->width - ball_diameter - border_inset));
        ball->y = fmaxf(border_inset_norm, fminf(desired_ball_y,
                        1.0f - ball->diameter_norm - border_inset_norm));
        st->slingshot_pull_x = desired_ball_x - ball->x;
        st->slingshot_pull_y = desired_ball_y - ball->y;

        float rescaled_for_sound = remap_normalized_scale_to_sound(
            total_prop * ball->scale);
        g_bounce_slot = st->grabbed_ball;
        mark_sounds_dirty(rescaled_for_sound);
    }
}

static void freerange_pointer_axis(void *data, struct wl_pointer *pointer,
                                 uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)pointer;
    (void)time;
    FreedomState *st = data;
    if (!st || st->grabbed_ball < 0 || st->grabbed_ball >= st->ball_count) {
        return;
    }
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    double delta = wl_fixed_to_double(value);
    if (delta == 0.0) {
        return;
    }
    freerange_adjust_ball_scale(st, delta < 0.0 ? 1 : -1);
	}

static const struct wl_pointer_listener freerange_pointer_listener = {
    .enter = freerange_pointer_enter,
    .leave = freerange_pointer_leave,
    .motion = freerange_pointer_motion,
    .button = freerange_pointer_button,
    .axis = freerange_pointer_axis,
};

static void freerange_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                                    uint32_t format, int fd, uint32_t size) {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    if (fd >= 0) {
        close(fd);
    }
}

static void freerange_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

/* Key repeat runs off these flags and only a RELEASED event clears them, but
   a key held as focus leaves never sends one -- it would repeat forever. */
static void freerange_clear_held_keys(FreedomState *st) {
    if (!st) {
        return;
    }
    st->key_vol_up_pressed = false;
    st->key_vol_down_pressed = false;
    st->key_speed_up_pressed = false;
    st->key_speed_down_pressed = false;
}

static void freerange_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    freerange_clear_held_keys(data);
}

static void freerange_keyboard_key(void *data, struct wl_keyboard *keyboard,
                                 uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == KEY_Q) {
            freerange_request_graceful_shutdown(st);
        } else if (key == KEY_ESC) {
            freerange_request_graceful_shutdown(st);
        } else if (key == KEY_M) {
            toggle_master_mute();
        } else if (key == KEY_A) {
            int target = st->grabbed_ball >= 0
                             ? st->grabbed_ball
                             : ball_at(st, st->pointer_x, st->pointer_y);
            if (target >= 0) {
                set_ball_mode(&st->balls[target], BALL_MODE_NOSTALGIA);
            }
        } else if (key == KEY_P) {
            int target = st->grabbed_ball >= 0
                             ? st->grabbed_ball
                             : ball_at(st, st->pointer_x, st->pointer_y);
            if (target >= 0) {
                set_ball_mode(&st->balls[target], BALL_MODE_POINGO);
            }
        } else if (key == KEY_C) {
            int target = st->grabbed_ball >= 0
                             ? st->grabbed_ball
                             : ball_at(st, st->pointer_x, st->pointer_y);
            if (target >= 0) {
                random_ball_palette(&st->balls[target]);
            }
        } else if (key == KEY_SPACE) {
            st->ghost_mode = !st->ghost_mode;
            g_ghost_mute = st->ghost_mode;
        } else if (key == KEY_LEFTBRACE) {
            freerange_adjust_ball_scale(st, -1);
        } else if (key == KEY_RIGHTBRACE) {
            freerange_adjust_ball_scale(st, 1);
        } else if (key == KEY_UP) {
            st->key_vol_up_pressed = true;
        } else if (key == KEY_DOWN) {
            st->key_vol_down_pressed = true;
        } else if (key == KEY_RIGHT) {
            st->key_speed_up_pressed = true;
        } else if (key == KEY_LEFT) {
            st->key_speed_down_pressed = true;
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (key == KEY_UP) {
            st->key_vol_up_pressed = false;
        } else if (key == KEY_DOWN) {
            st->key_vol_down_pressed = false;
        } else if (key == KEY_RIGHT) {
            st->key_speed_up_pressed = false;
        } else if (key == KEY_LEFT) {
            st->key_speed_down_pressed = false;
        }
    }
}

static void freerange_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                                       uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
                                       uint32_t mods_locked, uint32_t group) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void freerange_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                         int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener freerange_keyboard_listener = {
    .keymap = freerange_keyboard_keymap,
    .enter = freerange_keyboard_enter,
    .leave = freerange_keyboard_leave,
    .key = freerange_keyboard_key,
    .modifiers = freerange_keyboard_modifiers,
    .repeat_info = freerange_keyboard_repeat_info,
};

static void freerange_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    FreedomState *st = data;
    if (!st) {
        return;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
        st->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(st->pointer, &freerange_pointer_listener, st);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
        st->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(st->keyboard, &freerange_keyboard_listener, st);
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && st->keyboard) {
        wl_keyboard_destroy(st->keyboard);
        st->keyboard = NULL;
        /* The keys held when it vanished will never see a release. */
        freerange_clear_held_keys(st);
    }
    /* The protocol asks clients to release the object whose capability has
       gone, the pointer included. */
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && st->pointer) {
        wl_pointer_destroy(st->pointer);
        st->pointer = NULL;
    }
}

static void freerange_seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener freerange_seat_listener = {
    .capabilities = freerange_seat_capabilities,
    .name = freerange_seat_name,
};

static void freerange_registry_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface, uint32_t version) {
    FreedomState *st = data;
    if (!st) {
        return;
    }

    /* version is the highest the compositor supports, not the one to use.
       Asking for more than it offers is a protocol error, and the client is
       killed for it. */
    #define BIND_VERSION(wanted) ((version) < (wanted) ? (version) : (uint32_t)(wanted))

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                          BIND_VERSION(4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(registry, name, &wl_shm_interface, BIND_VERSION(1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        st->seat = wl_registry_bind(registry, name, &wl_seat_interface, BIND_VERSION(1));
        wl_seat_add_listener(st->seat, &freerange_seat_listener, st);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !st->output) {
        /* Only the first. Poingo tracks a single output, and letting each new
           one overwrite it leaked the previous proxy and made the mode report
           that won -- and so target_fps -- depend on advertisement order on a
           mixed-refresh desktop. */
        st->output = wl_registry_bind(registry, name, &wl_output_interface,
                                      BIND_VERSION(2));
        wl_output_add_listener(st->output, &freerange_output_listener, st);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                       BIND_VERSION(1));
        xdg_wm_base_add_listener(st->wm_base, &freerange_wm_base_listener, st);
    }

    #undef BIND_VERSION
}

static void freerange_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener freerange_registry_listener = {
    .global = freerange_registry_global,
    .global_remove = freerange_registry_global_remove,
};

static void freerange_signal_handler(int signum) {
    (void)signum;
    /* First signal asks for the same graceful exit as Q or the menu, so the
     * ball fades out and the audio rides down with it. A second one means
     * whoever sent the first has run out of patience: drop the fade and go. */
    g_freerange_quit_requested = g_freerange_quit_requested ? 2 : 1;
}
typedef struct {
    bool     ready;
    uint32_t compositor_ms;   
    uint64_t delivery_ns;     
} FrameCbData;

static void wayland_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    FrameCbData *fcb = (FrameCbData *)data;
    fcb->delivery_ns  = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
    fcb->ready        = true;
    fcb->compositor_ms = time;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener wayland_frame_listener = {
    .done = wayland_frame_done
};


/* The menu's on-screen box: ring[] is the ring proper, full[] takes in the
   colour field when the picker is up. False when the menu is closed. */
static bool poingo_menu_rects(int ring[4], int full[4]) {
    if (!g_menu || !ringmenu_is_open(g_menu)) return false;
    int mx, my, mw, mh;
    ringmenu_rect(g_menu, &mx, &my, &mw, &mh);
    ring[0] = mx; ring[1] = my; ring[2] = mw; ring[3] = mh;
    if (g_picker_slot >= 0) {
        int mcx, mcy;
        ringmenu_geometry(g_menu, &mcx, &mcy, NULL, NULL);
        float fr = ringmenu_field_radius(g_menu);
        int x1 = mx + mw, y1 = my + mh;
        int fx0 = (int)floorf((float)mcx - fr);
        int fy0 = (int)floorf((float)mcy - fr);
        int fx1 = (int)ceilf((float)mcx + fr);
        int fy1 = (int)ceilf((float)mcy + fr);
        if (fx0 < mx) mx = fx0;
        if (fy0 < my) my = fy0;
        if (fx1 > x1) x1 = fx1;
        if (fy1 > y1) y1 = fy1;
        mw = x1 - mx;
        mh = y1 - my;
    }
    full[0] = mx; full[1] = my; full[2] = mw; full[3] = mh;
    return true;
}

static int run_freerange_wayland(bool start_muted) {
    FreedomState st = {0};
    st.running = true;
    st.make_noise = true;
    st.grabbed_ball = -1;
    st.menu_ball = -1;
    g_suppress_hud = false;
    g_freerange_ball_scale = clamp_freerange_ball_scale(g_freerange_ball_scale);

    g_floor_y_normalized = 1.0f;
    g_target_peak_y = 0.0f;

    g_freerange_quit_requested = 0;
    signal(SIGINT, freerange_signal_handler);
    signal(SIGTERM, freerange_signal_handler);

    static bool seeded = false;
    if (!seeded) {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ (unsigned int)poingo_ticks_ms();
        srandom(seed);
        seeded = true;
    }

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    st.registry = wl_display_get_registry(st.display);
    if (!st.registry) {
        fprintf(stderr, "Failed to get Wayland registry\n");
        wl_display_disconnect(st.display);
        return 1;
    }

    wl_registry_add_listener(st.registry, &freerange_registry_listener, &st);
    wl_display_roundtrip(st.display);
    if (st.output) {
        wl_display_roundtrip(st.display);
    }

    if (!st.compositor || !st.wm_base) {
        fprintf(stderr, "Wayland compositor/wm_base missing\n");
        wl_display_disconnect(st.display);
        return 1;
    }

    if (st.width <= 0 || st.height <= 0) {
        st.width = 1280;
        st.height = 720;
    }

    st.egl_display = eglGetDisplay((EGLNativeDisplayType)st.display);
    if (st.egl_display == EGL_NO_DISPLAY || !eglInitialize(st.egl_display, NULL, NULL)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        wl_display_disconnect(st.display);
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint egl_attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg = NULL;
    EGLint egl_num = 0;
    if (!eglChooseConfig(st.egl_display, egl_attr, &egl_cfg, 1, &egl_num) || egl_num < 1) {
        fprintf(stderr, "Failed to choose EGL config\n");
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    st.egl_context = eglCreateContext(st.egl_display, egl_cfg, EGL_NO_CONTEXT,
                                      (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
    if (st.egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    int target_fps = 60;
    if (st.refresh_mhz > 0) {
        int refresh = st.refresh_mhz / 1000;
        if (refresh >= 15 && refresh <= 240) {
            target_fps = refresh;
        }
    }

    float angle_period = (4.0f * PI) / LON_TILES;
    int frame_count = (int)(target_fps * ROT_PERIOD + 0.5f);
    float frames_per_second = (float)target_fps;

    if (!init_audio(start_muted)) {
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    FreedomFrameSet frames = {0};
    if (!make_basis_frames(&frames, frame_count, angle_period)) {
        fprintf(stderr, "Failed to generate ball frames\n");
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }
    st.frames_ref = &frames;

    st.surface = wl_compositor_create_surface(st.compositor);
    if (!st.surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    wl_surface_set_opaque_region(st.surface, NULL);

    if (!ball_cursor_create(st.shm, st.compositor)) {
        fprintf(stderr, "Poingo: no ball cursor, using the default pointer\n");
    }

    st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    if (!st.xdg_surface) {
        fprintf(stderr, "Failed to create xdg_surface\n");
        wl_surface_destroy(st.surface);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }

    xdg_surface_add_listener(st.xdg_surface, &freerange_xdg_surface_listener, &st);
    st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    if (!st.xdg_toplevel) {
        fprintf(stderr, "Failed to create xdg_toplevel\n");
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        wl_display_disconnect(st.display);
        return 1;
    }
    xdg_toplevel_add_listener(st.xdg_toplevel, &freerange_toplevel_listener, &st);
    xdg_toplevel_set_title(st.xdg_toplevel, "Poingo");
    xdg_toplevel_set_app_id(st.xdg_toplevel, "poingo");
    xdg_toplevel_set_maximized(st.xdg_toplevel);

    st.egl_window = wl_egl_window_create(st.surface, st.width, st.height);
    if (!st.egl_window) {
        fprintf(stderr, "Failed to create EGL window\n");
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    st.egl_surface = eglCreateWindowSurface(st.egl_display, egl_cfg,
                                            (EGLNativeWindowType)st.egl_window, NULL);
    if (st.egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL window surface\n");
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    if (!eglMakeCurrent(st.egl_display, st.egl_surface, st.egl_surface, st.egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        eglDestroySurface(st.egl_display, st.egl_surface);
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }
    eglSwapInterval(st.egl_display, 0);

    uint32_t *light_drop = g_color_drop_light;
    uint32_t *dark_drop = g_color_drop_dark;
    ringmenu_color_drop_into(light_drop, POINGO_MENU_DISC * POINGO_MENU_DISC,
                             g_color_light_rgb[0], g_color_light_rgb[1],
                             g_color_light_rgb[2], 0, POINGO_MENU_ITEMS,
                             POINGO_MENU_DISC);
    ringmenu_color_drop_into(dark_drop, POINGO_MENU_DISC * POINGO_MENU_DISC,
                             g_color_dark_rgb[0], g_color_dark_rgb[1],
                             g_color_dark_rgb[2], 1, POINGO_MENU_ITEMS,
                             POINGO_MENU_DISC);
    RingMenuItem menu_items[POINGO_MENU_ITEMS] = {
        { .image = light_drop, .image_w = POINGO_MENU_DISC, .image_h = POINGO_MENU_DISC },
        { .image = dark_drop, .image_w = POINGO_MENU_DISC, .image_h = POINGO_MENU_DISC },
        { .label = "NOSTALGIA" },
        { .label = "POINGO" },
        { .label = "NEW COLOR" },
        { .label = "+ BALL" },
        { .label = "- BALL" },
        { .label = "MUTE", .led = RINGMENU_LED_OFF },
        { .label = "GHOST", .led = RINGMENU_LED_OFF },
        { .label = "QUIT" },
    };
    g_menu = ringmenu_create(menu_items, POINGO_MENU_ITEMS);
    if (g_menu) {
        int menu_size = ringmenu_size(g_menu);
        /* Color-field mode renders a rectangle expanded from the menu's
         * normal square to include the wheel.  Reserve that larger working
         * area up front; otherwise the render below changes the quad size
         * without having room to upload its pixels, stretching the menu. */
        float field_radius = ringmenu_field_radius(g_menu);
        int scratch_side = menu_size + (int)ceilf(field_radius * 2.0f);
        g_menu_scratch_cap = (size_t)scratch_side * scratch_side;
        g_menu_scratch = malloc(g_menu_scratch_cap * 4);
        if (!g_menu_scratch) {
            fprintf(stderr, "Failed to allocate menu workspace\n");
            ringmenu_destroy(g_menu);
            g_menu = NULL;
        }
    }
    glGenTextures(1, &g_menu_tex);
    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC pfn_swap_damage = NULL;
    bool has_buffer_age = false;
    {
        const char *egl_exts = eglQueryString(st.egl_display, EGL_EXTENSIONS);
        if (egl_exts) {
            if (strstr(egl_exts, "EGL_KHR_swap_buffers_with_damage")) {
                pfn_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageKHR");
            } else if (strstr(egl_exts, "EGL_EXT_swap_buffers_with_damage")) {
                pfn_swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                    eglGetProcAddress("eglSwapBuffersWithDamageEXT");
            }
            has_buffer_age = strstr(egl_exts, "EGL_EXT_buffer_age") != NULL;
        }
        if (g_debug_mode) {
            fprintf(stderr, "[egl] swap_with_damage=%s buffer_age=%s\n",
                    pfn_swap_damage ? "yes" : "no",
                    has_buffer_age ? "yes" : "no");
        }
    }

    wl_surface_commit(st.surface);
    while (!st.configured) {
        if (wl_display_dispatch(st.display) < 0) {
            break;
        }
    }
    if (!st.configured) {
        fprintf(stderr, "Never received the initial xdg_surface configure\n");
        eglDestroySurface(st.egl_display, st.egl_surface);
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    if (st.resize_pending) {
        wl_egl_window_resize(st.egl_window, st.width, st.height, 0, 0);
        st.resize_pending = false;
    }
    glViewport(0, 0, st.width, st.height);

    if (!freerange_gl_init(&st, &frames)) {
        fprintf(stderr, "Failed to initialize GL resources\n");
        freerange_gl_shutdown(&st);
        eglDestroySurface(st.egl_display, st.egl_surface);
        wl_egl_window_destroy(st.egl_window);
        xdg_toplevel_destroy(st.xdg_toplevel);
        xdg_surface_destroy(st.xdg_surface);
        wl_surface_destroy(st.surface);
        eglDestroyContext(st.egl_display, st.egl_context);
        eglTerminate(st.egl_display);
        freerange_color_regen_shutdown(&st);
        freerange_destroy_frames(&frames);
        shutdown_audio();
        wl_display_disconnect(st.display);
        return 1;
    }

    /* Runtime colors are shader uniforms now; only the uploaded masks remain. */
    free(frames.frames);
    frames.frames = NULL;

    PoingoBall *first_ball = &st.balls[0];
    first_ball->scale = g_freerange_ball_scale;
    first_ball->mode = BALL_MODE_POINGO;
    memcpy(first_ball->light_rgb, g_color_light_rgb, 3);
    memcpy(first_ball->dark_rgb, g_color_dark_rgb, 3);
    ball_size(first_ball, st.width, st.height);
    calculate_equilibrium_state(st.width, st.height,
                                GRAVITY, g_floor_y_normalized,
                                g_target_peak_y,
                                &first_ball->y, &first_ball->vy, &(float){0});
    first_ball->x = 100.0f;
    first_ball->vx = get_natural_vx(st.width);
    first_ball->vx_direction = 1;
    st.ball_count = 1;
    st.shutdown_pending = false;
    st.exit_fade = 0.0f;
    st.ball_cleared = false;
    st.shutdown_start_ticks = 0;

    {
        g_audio_sim_time = 0.0f;
        audio_predict_reset(g_audio_sim_time);
        build_audio_predict_buffer(&g_audio_predict,
                                   g_audio_sim_time,
                                   first_ball->x, first_ball->y,
                                   first_ball->vx, first_ball->vy,
                                   first_ball->vx_direction,
                                   st.width,
                                   0.0f, 0.0f,
                                   first_ball->diameter,
                                   first_ball->diameter_norm,
                                   1.0f / frames_per_second);
    }

    st.performance_frequency = poingo_perf_freq();
    if (st.performance_frequency == 0) {
        st.performance_frequency = 1;
    }
    st.last_counter = poingo_perf_counter();

    const double TARGET_FRAME_SECONDS = 1.0 / (double)target_fps;
    const double SNAP_THRESHOLD = fmax(0.00075, TARGET_FRAME_SECONDS * 0.08);
    double delta_error_accum = 0.0;

    #define FREERANGE_DAMAGE_HISTORY 8
    FreerangeRect damage_hist[FREERANGE_DAMAGE_HISTORY] = {0};
    int damage_hist_depth = 0;

    bool  in_reg_ghost = false;

    FrameCbData fr_cb = { .ready = true, .compositor_ms = 0 };
    uint32_t prev_compositor_ms = 0;
    bool     has_prev_compositor_ms = false;
    int obscured_frames = 0; 

    FILE   *fr_log        = NULL;
    uint64_t  fr_last       = 0;
    double  fr_mean       = 0.0, fr_M2 = 0.0, fr_max = 0.0;
    int     fr_n          = 0;
    uint64_t  fr_report_at  = poingo_perf_counter();

    FILE   *ov_log        = NULL;
    double  ov_mean       = 0.0, ov_M2 = 0.0, ov_max = 0.0;
    int     ov_n          = 0;

    FILE   *gf_log        = NULL;  
    double  gf_mean       = 0.0, gf_M2 = 0.0, gf_max = 0.0;
    int     gf_n          = 0;

    FILE    *fcb_log      = NULL;
    uint64_t fcb_prev_del = 0;                          
    uint64_t pump_enter_ns;
    int      fcbi_n       = 0;                          
    double   fcbi_mean    = 0.0, fcbi_M2 = 0.0, fcbi_max = 0.0;
    int      pw_n         = 0;                          
    double   pw_mean      = 0.0, pw_M2  = 0.0, pw_max  = 0.0;
    int      wk_n         = 0;                          
    double   wk_mean      = 0.0, wk_M2  = 0.0, wk_max  = 0.0;
    int      cs_n         = 0;                          
    double   cs_mean      = 0.0, cs_M2  = 0.0;
    uint64_t   fcb_report_at = poingo_perf_counter();

    double  prev_non_upload_ms  = 0.0;
    double  regen_ms_per_unit   = 0.0;
    bool    prev_regen_active   = false;

    int    drop_red          = 0;
    int    drop_yellow       = 0;
    int    normal_count      = 0;
    uint32_t drop_window_start = poingo_ticks_ms();
    double apb_ms_sum        = 0.0;
    double apb_ms_max        = 0.0;
    int    apb_count         = 0;

    if (g_debug_mode) {
        char fr_path[64];
        snprintf(fr_path, sizeof(fr_path), "/tmp/poingo_frames_%d.csv", (int)getpid());
        fr_log = fopen(fr_path, "w");
        if (fr_log) {
            fprintf(fr_log, "# interval_ms\n");
            fflush(fr_log);
        }
        char ov_path[64];
        snprintf(ov_path, sizeof(ov_path), "/tmp/poingo_overhead_%d.csv", (int)getpid());
        ov_log = fopen(ov_path, "w");
        if (ov_log) {
            fprintf(ov_log, "# overhead_ms\n");
            fflush(ov_log);
        }
        fprintf(stderr, "[debug] frame log:    %s\n", fr_path);
        fprintf(stderr, "[debug] overhead log: %s\n", ov_path);
        char gf_path[64];
        snprintf(gf_path, sizeof(gf_path), "/tmp/poingo_glfinish_%d.csv", (int)getpid());
        gf_log = fopen(gf_path, "w");
        if (gf_log) {
            fprintf(gf_log, "# glfinish_ms\n");
            fflush(gf_log);
        }
        fprintf(stderr, "[debug] glfinish log: %s\n", gf_path);
        char fcb_path[64];
        snprintf(fcb_path, sizeof(fcb_path), "/tmp/poingo_fcb_%d.csv", (int)getpid());
        fcb_log = fopen(fcb_path, "w");
        if (fcb_log) {
            fprintf(fcb_log, "delivery_ns,interval_us,poll_wait_us,wake_us,cstamp_off_us\n");
            fflush(fcb_log);
        }
        fprintf(stderr, "[debug] fcb log:      %s\n", fcb_path);
    }

    while (st.running) {
        bool was_ball_grabbed = st.grabbed_ball >= 0;
        bool miss_this_frame  = false;
        {
            struct timespec _pe;
            clock_gettime(CLOCK_MONOTONIC, &_pe);
            pump_enter_ns = (uint64_t)_pe.tv_sec * 1000000000ULL + (uint64_t)_pe.tv_nsec;
        }
        if (!freerange_pump_events(st.display, 20)) {
            /* The compositor is gone; there is nothing left to fade out to. */
            st.running = false;
            break;
        }
        /*
         * After the pump, not inside any pointer callback: this is the
         * first point at which a leave that arrived alongside a deferred
         * right release (see g_ring_right_release_pending) is guaranteed
         * to have already been seen, whichever order the two callbacks ran
         * in within this batch.
         */
        freerange_resolve_pending_ring_release();
        uint64_t tick_start = poingo_perf_counter();
        if (unlikely(g_freerange_quit_requested)) {
            if (g_freerange_quit_requested > 1) {
                st.running = false;
                break;
            }
            /* Re-entry is guarded inside, so calling it every tick is fine. */
            freerange_request_graceful_shutdown(&st);
        }

        uint64_t current_counter = poingo_perf_counter();
        double delta_seconds = (double)(current_counter - st.last_counter) /
                               (double)st.performance_frequency;
        st.last_counter = current_counter;

        if (delta_seconds < 0.0) {
            delta_seconds = 0.0;
        } else if (delta_seconds > 0.25) {
            delta_seconds = 0.25;
        }

        double sim_delta;
        double delta_diff = delta_seconds - TARGET_FRAME_SECONDS;
        if (fabs(delta_diff) < SNAP_THRESHOLD) {
            sim_delta = TARGET_FRAME_SECONDS;
            delta_error_accum += delta_diff;
        } else if (delta_seconds < TARGET_FRAME_SECONDS) {
            sim_delta = delta_seconds;
        } else {
            double clamped = SNAP_THRESHOLD;
            sim_delta = TARGET_FRAME_SECONDS + clamped;
            delta_error_accum += (delta_diff - clamped);
            double accum_cap = TARGET_FRAME_SECONDS * 2.0;
            if (delta_error_accum >  accum_cap) delta_error_accum =  accum_cap;
            if (delta_error_accum < -accum_cap) delta_error_accum = -accum_cap;
        }
        if (delta_error_accum > SNAP_THRESHOLD) {
            sim_delta += SNAP_THRESHOLD;
            delta_error_accum -= SNAP_THRESHOLD;
        } else if (delta_error_accum < -SNAP_THRESHOLD) {
            sim_delta -= SNAP_THRESHOLD;
            delta_error_accum += SNAP_THRESHOLD;
        }
        if (sim_delta < 0.0) sim_delta = 0.0;
        if (sim_delta > 0.25) sim_delta = 0.25;

        if (fr_cb.ready && has_prev_compositor_ms && fr_cb.compositor_ms != 0) {
            uint32_t comp_delta_ms = fr_cb.compositor_ms - prev_compositor_ms; 
            double comp_delta_s    = (double)comp_delta_ms * 0.001;
            if (comp_delta_s >= TARGET_FRAME_SECONDS * 1.5 &&
                comp_delta_s <= TARGET_FRAME_SECONDS * 8.0) {
                miss_this_frame = true;
                if (comp_delta_ms >= 35) drop_red++;
                else drop_yellow++;
                if (g_debug_mode) {
                    bool use_color = isatty(STDERR_FILENO);
                    const char *col = use_color
                        ? (comp_delta_ms >= 35 ? "\033[91m"   
                          : comp_delta_ms < 32 ? "\033[93m"   
                          :                      "")           
                        : "";
                    const char *rst = use_color ? "\033[0m" : "";
                    fprintf(stderr, "%s[miss] T+%.1fs  %u ms%s\n",
                            col, poingo_ticks_ms() / 1000.0, comp_delta_ms, rst);
                }
                if (sim_delta > TARGET_FRAME_SECONDS) {
                    delta_error_accum += (sim_delta - TARGET_FRAME_SECONDS);
                    sim_delta = TARGET_FRAME_SECONDS;
                    double max_accum = TARGET_FRAME_SECONDS * 2.0;
                    if (delta_error_accum > max_accum) delta_error_accum = max_accum;
                }
            }
        }

        if (g_freerange_quit_requested) {
            if (g_freerange_quit_requested > 1) {
                st.running = false;
            } else {
                freerange_request_graceful_shutdown(&st);
            }
        }
        if (!st.running) {
            break;
        }

        update_audio_output_latency();
        bool use_audio_prediction = g_audio_output_latency_valid &&
                                    st.ball_count == 1;
        float frame_elapsed = (float)delta_seconds;
        update_volume_hud(frame_elapsed);
        update_speed_hud(frame_elapsed);
        int vol_up_count = process_key_repeat(&g_vol_up_key, st.key_vol_up_pressed, frame_elapsed);
        int vol_down_count = process_key_repeat(&g_vol_down_key, st.key_vol_down_pressed, frame_elapsed);
        int speed_up_count = process_key_repeat(&g_speed_up_key, st.key_speed_up_pressed, frame_elapsed);
        int speed_down_count = process_key_repeat(&g_speed_down_key, st.key_speed_down_pressed, frame_elapsed);

        for (int i = 0; i < vol_up_count; i++) {
            adjust_master_volume(MASTER_VOLUME_STEP);
        }
        for (int i = 0; i < vol_down_count; i++) {
            adjust_master_volume(-MASTER_VOLUME_STEP);
        }
        for (int i = 0; i < speed_up_count; i++) {
            adjust_speed(SPEED_STEP);
        }
        for (int i = 0; i < speed_down_count; i++) {
            adjust_speed(-SPEED_STEP);
        }

        bool hud_visible = freerange_gl_update_hud(&st);

        if (was_ball_grabbed && st.grabbed_ball < 0) {
            audio_predict_reset(g_audio_sim_time);
        }

        if (st.resize_pending) {
            wl_egl_window_resize(st.egl_window, st.width, st.height, 0, 0);
            glViewport(0, 0, st.width, st.height);
            st.resize_pending = false;
            damage_hist_depth = 0;
        }

        for (int i = 0; i < st.ball_count; i++) {
            ball_size(&st.balls[i], st.width, st.height);
            st.balls[i].x = clampf(st.balls[i].x, 0.0f,
                                   (float)st.width - st.balls[i].diameter);
            st.balls[i].y = clampf(st.balls[i].y, 0.0f,
                                   g_floor_y_normalized -
                                   st.balls[i].diameter_norm);
        }

        float frame_advance = (float)(sim_delta * frames_per_second);
        st.phase_f = fmodf(st.phase_f + frame_advance, (float)frame_count);
        if (st.phase_f < 0.0f) {
            st.phase_f += frame_count;
        }
        st.phase_i = (int)floorf(st.phase_f);
        if (st.phase_i < 0) st.phase_i = 0;
        if (st.phase_i >= frame_count) st.phase_i = frame_count - 1;
        ball_cursor_animate(st.phase_i, frame_count);

        if (!st.ball_cleared) {
            BallSound sound = !use_audio_prediction && st.make_noise
                                  ? BALL_SOUND_PLAY : BALL_SOUND_SILENT;
            step_balls(&st, sim_delta, sound);
        }

        g_audio_sim_time += (float)sim_delta;
        if (use_audio_prediction &&
            st.grabbed_ball < 0 && !st.ball_cleared) {
            PoingoBall *ball = &st.balls[0];
            ball_sound(ball, 0);
            uint64_t apb_t0 = poingo_perf_counter();
            build_audio_predict_buffer(&g_audio_predict,
                                       g_audio_sim_time,
                                       ball->x, ball->y,
                                       ball->vx, ball->vy,
                                       ball->vx_direction,
                                       st.width,
                                       0.0f, 0.0f,
                                       ball->diameter, ball->diameter_norm,
                                       (float)sim_delta);
            double apb_ms =
                get_perf_seconds(apb_t0, poingo_perf_counter()) * 1000.0;
            apb_ms_sum += apb_ms;
            apb_count++;
            if (apb_ms > apb_ms_max) apb_ms_max = apb_ms;
            trigger_predictive_audio(
                &g_audio_predict, g_audio_sim_time, st.make_noise);
        }

        if (st.shutdown_pending) {
            /* Driven from the tick clock rather than the simulation step: the
             * fade must finish even if the sim is paused or starved.
             *
             * The mixer is riding the same POINGO_EXIT_FADE_SECONDS down, so by
             * the time the fade is up it is at a fraction of a percent of gain
             * -- inaudible. There is nothing left to wait for and nothing to
             * poll. It is also why the ball now always completes its fade,
             * where the old quiet check could cut it short after a tick or two
             * if the ball happened to be between bounces. */
            uint32_t now_ticks = poingo_ticks_ms();
            float elapsed = (float)(now_ticks - st.shutdown_start_ticks) * 0.001f;
            if (st.exit_fade > 0.0f) {
                st.exit_fade = POINGO_EXIT_FADE_SECONDS - elapsed;
                if (st.exit_fade < 0.0f) st.exit_fade = 0.0f;
            }
            if (elapsed >= POINGO_EXIT_FADE_SECONDS) {
                st.running = false;
            }
        }

        if (fr_cb.ready) {
            bool ghost_changed = (st.ghost_mode != in_reg_ghost);
            if (!st.ball_cleared) {
                freerange_update_input_region(&st);
                in_reg_ghost = st.ghost_mode;
                if (ghost_changed) {
                    damage_hist_depth = 0;
                }
            }
            obscured_frames = 0;

            uint64_t fr_now = poingo_perf_counter();
            if (g_debug_mode && fr_last != 0) {
                double ms = (double)(fr_now - fr_last) /
                            (double)st.performance_frequency * 1000.0;
                if (fr_log) fprintf(fr_log, "%.3f\n", ms);
                fr_n++;
                double d1 = ms - fr_mean;
                fr_mean += d1 / fr_n;
                fr_M2   += d1 * (ms - fr_mean);
                if (ms > fr_max) fr_max = ms;
                double elapsed = (double)(fr_now - fr_report_at) /
                                 (double)st.performance_frequency;
                if (elapsed >= 2.0 && fr_n >= 2) {
                    double stdev = sqrt(fr_M2 / (fr_n - 1));
                    double ov_stdev = (ov_n >= 2) ? sqrt(ov_M2 / (ov_n - 1)) : 0.0;
                    double gf_stdev = (gf_n >= 2) ? sqrt(gf_M2 / (gf_n - 1)) : 0.0;
                    fprintf(stderr,
                            "[fps] %.1f fps  interval: mean=%.2fms stdev=%.2fms max=%.1fms"
                            "  overhead: mean=%.2fms stdev=%.2fms max=%.1fms"
                            "  glfinish: mean=%.2fms stdev=%.2fms max=%.1fms\n",
                            fr_n / elapsed,
                            fr_mean, stdev, fr_max,
                            ov_mean, ov_stdev, ov_max,
                            gf_mean, gf_stdev, gf_max);
                    fr_n = 0; fr_mean = 0.0; fr_M2 = 0.0; fr_max = 0.0;
                    ov_n = 0; ov_mean = 0.0; ov_M2 = 0.0; ov_max = 0.0;
                    gf_n = 0; gf_mean = 0.0; gf_M2 = 0.0; gf_max = 0.0;
                    fr_report_at = fr_now;
                }
            }
            fr_last = fr_now;

            if (g_debug_mode && fr_cb.delivery_ns != 0) {
                struct timespec _noticed;
                clock_gettime(CLOCK_MONOTONIC, &_noticed);
                uint64_t noticed_ns = (uint64_t)_noticed.tv_sec * 1000000000ULL
                                    + (uint64_t)_noticed.tv_nsec;

                double wake_us = (double)(int64_t)(noticed_ns - fr_cb.delivery_ns) / 1000.0;

                uint64_t del_ms_mod = (fr_cb.delivery_ns / 1000000ULL) & 0xFFFFFFFFULL;
                int32_t  cs_diff_ms = (int32_t)(del_ms_mod - (uint32_t)fr_cb.compositor_ms);
                double   cstamp_off_us = cs_diff_ms * 1000.0;

                double poll_wait_us = 0.0;
                if (pump_enter_ns != 0 && fr_cb.delivery_ns >= pump_enter_ns) {
                    poll_wait_us = (double)(fr_cb.delivery_ns - pump_enter_ns) / 1000.0;
                }

                if (fcb_prev_del != 0 && fr_cb.delivery_ns > fcb_prev_del) {
                    double interval_us = (double)(fr_cb.delivery_ns - fcb_prev_del) / 1000.0;
                    fcbi_n++;
                    double _d = interval_us - fcbi_mean;
                    fcbi_mean += _d / fcbi_n;
                    fcbi_M2   += _d * (interval_us - fcbi_mean);
                    if (interval_us > fcbi_max) fcbi_max = interval_us;
                    if (poll_wait_us >= 0.0 && poll_wait_us < 30000.0) {
                        pw_n++;
                        double _p = poll_wait_us - pw_mean;
                        pw_mean += _p / pw_n;
                        pw_M2   += _p * (poll_wait_us - pw_mean);
                        if (poll_wait_us > pw_max) pw_max = poll_wait_us;
                    }
                    if (wake_us >= 0.0 && wake_us < 30000.0) {
                        wk_n++;
                        double _w = wake_us - wk_mean;
                        wk_mean += _w / wk_n;
                        wk_M2   += _w * (wake_us - wk_mean);
                        if (wake_us > wk_max) wk_max = wake_us;
                    }
                    cs_n++;
                    double _c = cstamp_off_us - cs_mean;
                    cs_mean += _c / cs_n;
                    cs_M2   += _c * (cstamp_off_us - cs_mean);

                    if (fcb_log) {
                        fprintf(fcb_log, "%llu,%.1f,%.1f,%.1f,%.1f\n",
                                (unsigned long long)fr_cb.delivery_ns,
                                interval_us, poll_wait_us, wake_us, cstamp_off_us);
                    }
                }
                fcb_prev_del = fr_cb.delivery_ns;

                double fcb_elapsed = (double)(fr_now - fcb_report_at)
                                   / (double)st.performance_frequency;
                if (fcb_elapsed >= 10.0 && fcbi_n >= 10) {
                    double i_sd  = (fcbi_n > 1) ? sqrt(fcbi_M2 / (fcbi_n - 1)) : 0.0;
                    double pw_sd = (pw_n   > 1) ? sqrt(pw_M2   / (pw_n   - 1)) : 0.0;
                    double w_sd  = (wk_n   > 1) ? sqrt(wk_M2   / (wk_n   - 1)) : 0.0;
                    double c_sd  = (cs_n   > 1) ? sqrt(cs_M2   / (cs_n   - 1)) : 0.0;
                    fprintf(stderr,
                            "[fcb] interval mean=%.3fms \xc2\xb1%.3fms max=%.3fms"
                            "  poll_wait mean=%.3fms \xc2\xb1%.3fms max=%.3fms"
                            "  wake mean=%.0f\xc2\xb5s \xc2\xb1%.0f\xc2\xb5s"
                            "  cstamp off=%.0f\xc2\xb5s \xc2\xb1%.0f\xc2\xb5s\n",
                            fcbi_mean / 1000.0, i_sd / 1000.0, fcbi_max / 1000.0,
                            pw_mean / 1000.0, pw_sd / 1000.0, pw_max / 1000.0,
                            wk_mean, w_sd,
                            cs_mean, c_sd);
                    fcbi_n = 0; fcbi_mean = 0.0; fcbi_M2 = 0.0; fcbi_max = 0.0;
                    pw_n   = 0; pw_mean  = 0.0; pw_M2  = 0.0; pw_max  = 0.0;
                    wk_n   = 0; wk_mean  = 0.0; wk_M2  = 0.0; wk_max  = 0.0;
                    cs_n   = 0; cs_mean  = 0.0; cs_M2  = 0.0;
                    fcb_report_at = fr_now;
                }
            }

            if (has_prev_compositor_ms) {
            } else if (fr_cb.compositor_ms != 0) {
                prev_compositor_ms = fr_cb.compositor_ms;
                has_prev_compositor_ms = true;
            }

            struct wl_callback *callback = wl_surface_frame(st.surface);
            wl_callback_add_listener(callback, &wayland_frame_listener, &fr_cb);
            fr_cb.ready = false;

            float render_extrap_dt = (float)(TARGET_FRAME_SECONDS - sim_delta);
            float extrap_cap = (float)(SNAP_THRESHOLD * 4.0);
            if (render_extrap_dt >  extrap_cap) render_extrap_dt =  extrap_cap;
            if (render_extrap_dt < -extrap_cap) render_extrap_dt = -extrap_cap;

            if (st.color_regen_active && !prev_regen_active) {
                regen_ms_per_unit    = 0.0;
            }

            /* Everything left in the frame after the rest of the tick, less a
             * margin. Previously this was then clamped to a small fixed
             * figure, which is what made regen crawl on a machine with plenty
             * of frame to spare. */
            double upload_budget_ms = TARGET_FRAME_SECONDS * 1000.0
                                      - prev_non_upload_ms
                                      - FREERANGE_REGEN_HEADROOM_MS;
            if (upload_budget_ms < 0.5) upload_budget_ms = 0.5;

            int upload_max;
            if (regen_ms_per_unit <= 0.0) {
                upload_max = 1;  
            } else {
                upload_max = (int)(upload_budget_ms / regen_ms_per_unit);
                if (upload_max < 1) upload_max = 1;  
            }

            int units_before = (int)st.regen_units_uploaded;
            uint64_t upload_t0 = poingo_perf_counter();
            freerange_regen_upload_step(&st, &frames, upload_max, upload_budget_ms);
            double upload_actual_ms =
                get_perf_seconds(upload_t0, poingo_perf_counter()) * 1000.0;

            int units_done = (int)st.regen_units_uploaded - units_before;
            if (units_done > 0) {
                double mpu = upload_actual_ms / (double)units_done;
                regen_ms_per_unit = (regen_ms_per_unit <= 0.0)
                    ? mpu
                    : 0.2 * regen_ms_per_unit + 0.8 * mpu;
            }

            /* The regen used to re-chunk itself mid-flight -- measure a
             * couple of frames, decide the blocks were the wrong size for the
             * duration it was aiming at, and restart. Restarting threw away
             * the work already uploaded and made the ball visibly stutter.
             * There is no target duration to chase now, so it just runs. */
            prev_regen_active = st.color_regen_active;
            if (st.color_regen_mode == BALL_REGEN_CLEAR && !st.color_regen_active && !st.ball_cleared) {
                st.ball_cleared = true;
                st.shutdown_pending = true;
                st.shutdown_start_ticks = poingo_ticks_ms();
            }

            bool menu_open = g_menu && ringmenu_is_open(g_menu);
            bool menu_active = menu_open || g_menu_was_open;

            FreerangeRect cur_rect = {0, 0, 0, 0};
            {
                for (int i = 0; i < st.ball_count; i++) {
                    float bx, by, bw, bh;
                    if (!freerange_ball_dest_quad(&st, &st.balls[i], &frames,
                                                  render_extrap_dt,
                                                  &bx, &by, &bw, &bh)) {
                        continue;
                    }
                    FreerangeRect br = {
                        (int)floorf(bx) - 2, (int)floorf(by) - 2,
                        (int)ceilf(bw) + 4, (int)ceilf(bh) + 4
                    };
                    freerange_rect_union(&cur_rect, &br);
                }
                if (hud_visible && st.gl_hud_w > 0 && st.gl_hud_h > 0) {
                    FreerangeRect hr = { (int)floorf(st.hud_x) - 2,
                                         (int)floorf(st.hud_y) - 2,
                                         st.gl_hud_w + 4, st.gl_hud_h + 4 };
                    freerange_rect_union(&cur_rect, &hr);
                }
                if (st.ghost_mode && (!st.ball_cleared || st.shutdown_pending)) {
                    FreerangeRect br = { st.width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN, GHOST_ICON_MARGIN, GHOST_ICON_SIZE, GHOST_ICON_SIZE };
                    freerange_rect_union(&cur_rect, &br);
                }
                /* The menu used to damage the whole screen, which made the
                   compositor recomposite 1920x1080 every frame it was up and
                   cost more than drawing it. Its own box is enough; the frame
                   it closes on repaints the box it last occupied. */
                int ring[4], full[4];
                if (menu_open && poingo_menu_rects(ring, full)) {
                    FreerangeRect mr = { full[0] - 2, full[1] - 2,
                                         full[2] + 4, full[3] + 4 };
                    freerange_rect_union(&cur_rect, &mr);
                } else if (menu_active && g_menu_rect[2] > 0) {
                    FreerangeRect mr = { g_menu_rect[0] - 2, g_menu_rect[1] - 2,
                                         g_menu_rect[2] + 4, g_menu_rect[3] + 4 };
                    freerange_rect_union(&cur_rect, &mr);
                }
                freerange_rect_clamp(&cur_rect, st.width, st.height);
            }

            bool repaint_full = true;
            FreerangeRect repaint = cur_rect;
            if (has_buffer_age) {
                EGLint age = 0;
                if (eglQuerySurface(st.egl_display, st.egl_surface,
                                    EGL_BUFFER_AGE_EXT, &age) &&
                     age >= 1 && (int)age <= damage_hist_depth) {
                    for (int i = 0; i < (int)age; i++) {
                        freerange_rect_union(&repaint, &damage_hist[i]);
                    }
                    freerange_rect_clamp(&repaint, st.width, st.height);
                    repaint_full = false;
                }
            }
            if (!repaint_full) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(repaint.x, st.height - (repaint.y + repaint.h),
                          repaint.w, repaint.h);
            }

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            freerange_gl_draw_frame(&st, &frames, render_extrap_dt);
            if (hud_visible) {
                freerange_gl_draw_hud(&st);
            }

            if (st.ghost_mode && (!st.ball_cleared || st.shutdown_pending)) {
                float bg_size = (float)GHOST_ICON_SIZE;
                float bx = (float)st.width - bg_size - (float)GHOST_ICON_MARGIN;
                float by = (float)GHOST_ICON_MARGIN;
                
                glUseProgram(st.gl_program);
                glUniform1f(st.gl_recolor_loc, 0.0f);
                glActiveTexture(GL_TEXTURE0);
                glEnableVertexAttribArray(st.gl_pos_loc);
                glEnableVertexAttribArray(st.gl_uv_loc);
                glUniform1f(st.gl_alpha_loc, 1.0f);
                
                if (st.gl_ghost_bg_texture) {
                    float bg_x0 = (bx / (float)st.width) * 2.0f - 1.0f;
                    float bg_x1 = ((bx + bg_size) / (float)st.width) * 2.0f - 1.0f;
                    float bg_y0 = 1.0f - (by / (float)st.height) * 2.0f;
                    float bg_y1 = 1.0f - ((by + bg_size) / (float)st.height) * 2.0f;
                    GLfloat bg_verts[] = {
                        bg_x0, bg_y1, 0.0f, 1.0f,
                        bg_x1, bg_y1, 1.0f, 1.0f,
                        bg_x0, bg_y0, 0.0f, 0.0f,
                        bg_x1, bg_y0, 1.0f, 0.0f
                    };
                    glBindTexture(GL_TEXTURE_2D, st.gl_ghost_bg_texture);
                    glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_verts);
                    glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_verts + 2);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
                
                /* The frame contains the ball plus its complete soft
                 * shadow.  The previous 580px UV crop centered the ball,
                 * but cut off the lower edge of the shadow.  Draw the whole
                 * frame instead, positioning it from the ball's true center
                 * so the ball remains centered in the badge.  A 128px ball
                 * leaves the same comfortable visual weight as the other
                 * Ghost-mode tools. */
                const float ball_diameter = 128.0f;
                const float frame_scale = ball_diameter / (float)BALL_W;
                const float icon_w = frames.frame_w * frame_scale;
                const float icon_h = frames.frame_h * frame_scale;
                const float ball_center_x = g_ball_texture_offset_x + BALL_W * 0.5f;
                const float ball_center_y = g_ball_texture_offset_y + BALL_H * 0.5f;
                const float ix = bx + bg_size * 0.5f - ball_center_x * frame_scale;
                const float iy = by + bg_size * 0.5f - ball_center_y * frame_scale;
                float px0 = (ix / (float)st.width) * 2.0f - 1.0f;
                float px1 = ((ix + icon_w) / (float)st.width) * 2.0f - 1.0f;
                float py0 = 1.0f - (iy / (float)st.height) * 2.0f;
                float py1 = 1.0f - ((iy + icon_h) / (float)st.height) * 2.0f;
                GLfloat bverts[] = {
                    px0, py1, 0.0f, 1.0f,
                    px1, py1, 1.0f, 1.0f,
                    px0, py0, 0.0f, 0.0f,
                    px1, py0, 1.0f, 0.0f
                };
                PoingoBall *ghost_ball = &st.balls[0];
                int ghost_frame = (int)ghost_ball->mode * frame_count;
                glBindTexture(GL_TEXTURE_2D, st.gl_textures[ghost_frame]);
                glUniform1f(st.gl_recolor_loc, 1.0f);
                glUniform3f(st.gl_light_loc,
                            ghost_ball->light_rgb[0] / 255.0f,
                            ghost_ball->light_rgb[1] / 255.0f,
                            ghost_ball->light_rgb[2] / 255.0f);
                glUniform3f(st.gl_dark_loc,
                            ghost_ball->dark_rgb[0] / 255.0f,
                            ghost_ball->dark_rgb[1] / 255.0f,
                            ghost_ball->dark_rgb[2] / 255.0f);
                glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bverts);
                glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bverts + 2);
                freerange_gl_blend_premul();
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                freerange_gl_blend_straight();
            }

            g_menu_was_open = menu_open;

            int menu_ring[4], menu_full[4];
            if (menu_open && poingo_menu_rects(menu_ring, menu_full)) {
                int mx = menu_full[0], my = menu_full[1];
                int mw = menu_full[2], mh = menu_full[3];
                /* The ring is the only part that changes as the pointer
                   moves; everything outside it stays the field, which is
                   already cached. */
                const int rx = menu_ring[0], ry = menu_ring[1];
                const int rw = menu_ring[2], rh = menu_ring[3];
                bool menu_redraw = ringmenu_take_dirty(g_menu);
                if (mx != g_menu_rect[0] || my != g_menu_rect[1] ||
                    mw != g_menu_rect[2] || mh != g_menu_rect[3]) {
                    menu_redraw = true;
                    g_menu_rect[0] = mx;
                    g_menu_rect[1] = my;
                    g_menu_rect[2] = mw;
                    g_menu_rect[3] = mh;
                }
                if (menu_redraw) {
                    size_t need = (size_t)mw * mh;
                    if (g_menu_scratch && need <= g_menu_scratch_cap) {
                        bool field_cached = false;
                        int fc_i = (g_picker_slot >= 0 &&
                                    g_picker_slot < FIELD_CACHE_SLOTS)
                                       ? g_picker_slot : 0;
                        if (g_picker_slot >= 0) {
                            int mcx, mcy;
                            ringmenu_geometry(g_menu, &mcx, &mcy, NULL, NULL);
                            if (g_field_cache[fc_i].slot != g_picker_slot ||
                                g_field_cache[fc_i].w != mw ||
                                g_field_cache[fc_i].h != mh ||
                                g_field_cache[fc_i].off_x != mx - mcx ||
                                g_field_cache[fc_i].off_y != my - mcy ||
                                g_field_cache[fc_i].cap < need) {
                                if (g_field_cache[fc_i].cap < need) {
                                    uint32_t *grown = realloc(g_field_cache[fc_i].px,
                                                              need * 4);
                                    if (grown) {
                                        g_field_cache[fc_i].px = grown;
                                        g_field_cache[fc_i].cap = need;
                                    }
                                }
                                if (g_field_cache[fc_i].cap >= need) {
                                    memset(g_field_cache[fc_i].px, 0, need * 4);
                                    ringmenu_field_draw(g_menu, g_picker_slot,
                                                        g_field_cache[fc_i].px,
                                                        mw, mh, mx, my);
                                    g_field_cache[fc_i].slot = g_picker_slot;
                                    g_field_cache[fc_i].w = mw;
                                    g_field_cache[fc_i].h = mh;
                                    g_field_cache[fc_i].off_x = mx - mcx;
                                    g_field_cache[fc_i].off_y = my - mcy;
                                }
                            }
                            field_cached = (g_field_cache[fc_i].slot == g_picker_slot &&
                                            g_field_cache[fc_i].cap >= need);
                        }

                        /* Restoring, premultiplying and uploading the whole
                           rect costs ~5ms at picker size. Once the field is on
                           the texture, only the ring's own box can have
                           changed, so confine all three to it. */
                        bool whole = !field_cached || g_menu_full_upload ||
                                     g_menu_tex_slot != g_picker_slot ||
                                     mw != g_menu_tex_w || mh != g_menu_tex_h;
                        int ux = whole ? 0 : rx - mx;
                        int uy = whole ? 0 : ry - my;
                        int uw = whole ? mw : rw;
                        int uh = whole ? mh : rh;
                        if (ux < 0) { uw += ux; ux = 0; }
                        if (uy < 0) { uh += uy; uy = 0; }
                        if (ux + uw > mw) uw = mw - ux;
                        if (uy + uh > mh) uh = mh - uy;

                        /* Lay down straight-alpha pixels for whatever is about
                           to be recomposited; the rest of the buffer keeps the
                           premultiplied copy already sitting on the texture. */
                        if (!field_cached) {
                            memset(g_menu_scratch, 0, need * 4);
                            if (g_picker_slot >= 0) {
                                ringmenu_field_draw(g_menu, g_picker_slot,
                                                    g_menu_scratch, mw, mh, mx, my);
                            }
                        } else if (whole) {
                            memcpy(g_menu_scratch, g_field_cache[fc_i].px, need * 4);
                        } else {
                            for (int row = 0; row < uh; row++) {
                                size_t off = (size_t)(uy + row) * mw + ux;
                                memcpy(g_menu_scratch + off,
                                       g_field_cache[fc_i].px + off, (size_t)uw * 4);
                            }
                        }
                        ringmenu_draw(g_menu, g_menu_scratch, mw, mh, mx, my);
                        for (int row = 0; row < uh; row++) {
                            uint32_t *p = g_menu_scratch + (size_t)(uy + row) * mw + ux;
                            for (int col = 0; col < uw; col++) {
                                uint32_t c = p[col];
                                uint32_t a = c >> 24;
                                if (a == 0) { p[col] = 0; continue; }
                                if (a == 255) continue;
                                uint32_t r = ((c & 0xFF) * a + 127) / 255;
                                uint32_t g_val = (((c >> 8) & 0xFF) * a + 127) / 255;
                                uint32_t b = (((c >> 16) & 0xFF) * a + 127) / 255;
                                p[col] = r | (g_val << 8) | (b << 16) | (a << 24);
                            }
                        }

                        glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                        if (mw != g_menu_tex_w || mh != g_menu_tex_h) {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, g_menu_scratch);
                            g_menu_tex_w = mw;
                            g_menu_tex_h = mh;
                        } else if (whole) {
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh,
                                            GL_RGBA, GL_UNSIGNED_BYTE, g_menu_scratch);
                        } else if (uw > 0 && uh > 0) {
                            /* GLES2 has no GL_UNPACK_ROW_LENGTH, so the rows
                               have to be gathered before they go up. */
                            size_t rows = (size_t)uw * uh;
                            if (g_upload_cap < rows) {
                                uint32_t *grown = realloc(g_upload_staging, rows * 4);
                                if (grown) {
                                    g_upload_staging = grown;
                                    g_upload_cap = rows;
                                }
                            }
                            if (g_upload_cap >= rows) {
                                for (int row = 0; row < uh; row++) {
                                    memcpy(g_upload_staging + (size_t)row * uw,
                                           g_menu_scratch + (size_t)(uy + row) * mw + ux,
                                           (size_t)uw * 4);
                                }
                                glTexSubImage2D(GL_TEXTURE_2D, 0, ux, uy, uw, uh,
                                                GL_RGBA, GL_UNSIGNED_BYTE,
                                                g_upload_staging);
                            } else {
                                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh,
                                                GL_RGBA, GL_UNSIGNED_BYTE,
                                                g_menu_scratch);
                            }
                        }
                        g_menu_full_upload = false;
                        g_menu_tex_slot = g_picker_slot;
                    }
                }
                float x0 = 2.f * mx / st.width - 1.f;
                float x1 = 2.f * (mx + mw) / st.width - 1.f;
                float y0 = 1.f - 2.f * my / st.height;
                float y1 = 1.f - 2.f * (my + mh) / st.height;
                GLfloat mverts[16] = {
                    x0, y0, 0.f, 0.f,
                    x1, y0, 1.f, 0.f,
                    x0, y1, 0.f, 1.f,
                    x1, y1, 1.f, 1.f,
                };
                glUseProgram(st.gl_program);
                glUniform1f(st.gl_recolor_loc, 0.0f);
                glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                glVertexAttribPointer(st.gl_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts);
                glVertexAttribPointer(st.gl_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts + 2);
                glEnableVertexAttribArray(st.gl_pos_loc);
                glEnableVertexAttribArray(st.gl_uv_loc);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            if (!repaint_full) {
                glDisable(GL_SCISSOR_TEST);
            }
            if (g_debug_mode) {
                uint64_t gf_t0 = poingo_perf_counter();
                glFinish();
                uint64_t gf_t1 = poingo_perf_counter();
                double gf_ms = (double)(gf_t1 - gf_t0) /
                               (double)st.performance_frequency * 1000.0;
                if (gf_log) fprintf(gf_log, "%.3f\n", gf_ms);
                gf_n++;
                double gd1 = gf_ms - gf_mean;
                gf_mean += gd1 / gf_n;
                gf_M2   += gd1 * (gf_ms - gf_mean);
                if (gf_ms > gf_max) gf_max = gf_ms;
            }
            if (pfn_swap_damage) {
                FreerangeRect swap_dmg = cur_rect;
                if (damage_hist_depth >= 1) {
                    freerange_rect_union(&swap_dmg, &damage_hist[0]);
                }
                freerange_rect_clamp(&swap_dmg, st.width, st.height);
                EGLint dmg[4];
                EGLint n_dmg = 0;
                if (damage_hist_depth >= 1 && swap_dmg.w > 0 && swap_dmg.h > 0) {
                    dmg[0] = swap_dmg.x;
                    dmg[1] = st.height - (swap_dmg.y + swap_dmg.h);
                    dmg[2] = swap_dmg.w;
                    dmg[3] = swap_dmg.h;
                    n_dmg = 1;
                }
                pfn_swap_damage(st.egl_display, st.egl_surface,
                                n_dmg ? dmg : NULL, n_dmg);
            } else {
                eglSwapBuffers(st.egl_display, st.egl_surface);
            }
            wl_display_flush(st.display);

            for (int i = FREERANGE_DAMAGE_HISTORY - 1; i > 0; i--) {
                damage_hist[i] = damage_hist[i - 1];
            }
            damage_hist[0] = cur_rect;
            if (damage_hist_depth < FREERANGE_DAMAGE_HISTORY) {
                damage_hist_depth++;
            }

            if (fr_cb.compositor_ms != 0) {
                prev_compositor_ms = fr_cb.compositor_ms;
                has_prev_compositor_ms = true;
            }

            if (tick_start != 0) {
                double ov_ms = get_perf_seconds(tick_start, poingo_perf_counter()) * 1000.0;
                double non_upload = ov_ms - upload_actual_ms;
                prev_non_upload_ms = non_upload > 0.0 ? non_upload : 0.0;
                if (g_debug_mode) {
                    if (ov_log) fprintf(ov_log, "%.3f\n", ov_ms);
                    ov_n++;
                    double od1 = ov_ms - ov_mean;
                    ov_mean += od1 / ov_n;
                    ov_M2   += od1 * (ov_ms - ov_mean);
                    if (ov_ms > ov_max) ov_max = ov_ms;
                }
            }

            if (g_debug_mode && has_prev_compositor_ms) {
                if (!miss_this_frame) normal_count++;
                uint32_t now_ms = poingo_ticks_ms();
                if (now_ms - drop_window_start >= 10000) {
                    bool use_color = isatty(STDERR_FILENO);
                    double apb_mean = apb_count > 0 ? apb_ms_sum / apb_count : 0.0;
                    fprintf(stderr, "[drops] %s%dr%s  %s%dy%s  %dok  |  apb mean=%.3fms max=%.3fms\n",
                            (use_color && drop_red)    ? "\033[91m" : "", drop_red,
                            (use_color && drop_red)    ? "\033[0m"  : "",
                            (use_color && drop_yellow) ? "\033[93m" : "", drop_yellow,
                            (use_color && drop_yellow) ? "\033[0m"  : "",
                            normal_count, apb_mean, apb_ms_max);
                    drop_red = drop_yellow = normal_count = 0;
                    apb_ms_sum = apb_ms_max = 0.0; apb_count = 0;
                    drop_window_start = now_ms;
                }
            }

        } else {
            if (++obscured_frames % 300 == 0) {
                struct wl_callback *callback = wl_surface_frame(st.surface);
                wl_callback_add_listener(callback, &wayland_frame_listener, &fr_cb);
                wl_surface_commit(st.surface);
                wl_display_flush(st.display);
            }
        }
    } 

    if (g_predict_audit) {
        static const char *surface_names[BOUNCE_SURFACE_COUNT] = {
            "floor", "ceiling", "left-wall", "right-wall"
        };
        uint64_t expected_total = 0;
        uint64_t selected_total = 0;
        uint64_t committed_total = 0;
        for (int surface = 0; surface < BOUNCE_SURFACE_COUNT; ++surface) {
            expected_total += g_audit_expected[surface];
            selected_total += g_audit_selected[surface];
            committed_total += g_audit_committed[surface];
            fprintf(stderr,
                    "predict audit %s: expected=%llu selected=%llu committed=%llu\n",
                    surface_names[surface],
                    (unsigned long long)g_audit_expected[surface],
                    (unsigned long long)g_audit_selected[surface],
                    (unsigned long long)g_audit_committed[surface]);
        }
        fprintf(stderr,
                "predict audit total: expected=%llu selected=%llu committed=%llu\n",
                (unsigned long long)expected_total,
                (unsigned long long)selected_total,
                (unsigned long long)committed_total);
    }

    if (fr_log) {
        fflush(fr_log);
        fclose(fr_log);
    }
    if (ov_log) {
        fflush(ov_log);
        fclose(ov_log);
    }
    if (gf_log) {
        fflush(gf_log);
        fclose(gf_log);
    }
    if (fcb_log) {
        fflush(fcb_log);
        fclose(fcb_log);
    }

    freerange_gl_shutdown(&st);
    if (st.egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(st.egl_display, st.egl_surface);
    }
    if (st.egl_window) {
        wl_egl_window_destroy(st.egl_window);
    }
    if (st.egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(st.egl_display, st.egl_context);
    }
    if (st.egl_display != EGL_NO_DISPLAY) {
        eglTerminate(st.egl_display);
    }

    /* Owns a wl_surface and wl_buffers, so it has to go before the compositor
     * and shm that made them -- and well before wl_display_disconnect, which
     * frees every proxy on the connection. Destroying them afterwards is a
     * use-after-free, and it segfaulted on every single exit. */
    ball_cursor_destroy();

    if (st.pointer) {
        wl_pointer_destroy(st.pointer);
    }
    if (st.keyboard) {
        wl_keyboard_destroy(st.keyboard);
    }
    if (st.seat) {
        wl_seat_destroy(st.seat);
    }
    if (st.output) {
        wl_output_destroy(st.output);
    }
    if (st.xdg_toplevel) {
        xdg_toplevel_destroy(st.xdg_toplevel);
    }
    if (st.xdg_surface) {
        xdg_surface_destroy(st.xdg_surface);
    }
    if (st.surface) {
        wl_surface_destroy(st.surface);
    }
    if (st.wm_base) {
        xdg_wm_base_destroy(st.wm_base);
    }
    if (st.shm) {
        wl_shm_destroy(st.shm);
    }
    if (st.compositor) {
        wl_compositor_destroy(st.compositor);
    }
    if (st.registry) {
        wl_registry_destroy(st.registry);
    }
    if (st.display) {
        wl_display_disconnect(st.display);
    }

    freerange_color_regen_shutdown(&st);
    freerange_destroy_frames(&frames);
    shutdown_audio();
    if (g_menu) { ringmenu_destroy(g_menu); g_menu = NULL; }
    if (g_menu_scratch) { free(g_menu_scratch); g_menu_scratch = NULL; }
    for (int i = 0; i < FIELD_CACHE_SLOTS; i++) {
        free(g_field_cache[i].px);
        g_field_cache[i].px = NULL;
        g_field_cache[i].cap = 0;
    }
    if (g_upload_staging) { free(g_upload_staging); g_upload_staging = NULL; g_upload_cap = 0; }
    free(st.regen_unit_done_storage);
    free(st.regen_order_storage);
    free(st.regen_thread_storage);
    release_sphere_pixel_cache();
    return 0;
}



/* Outcome of the command line, so main() can tell "run" from "print help and
   stop" from "the user made a mistake". */
typedef enum {
    POINGO_ARGS_RUN,
    POINGO_ARGS_DONE,
    POINGO_ARGS_ERROR,
} PoingoArgsResult;

static void poingo_print_usage(const char *program) {
    printf("Usage: %s [options]\n", program);
    printf("Options:\n");
    printf("  --mute                Start with audio muted\n");
    printf("  --light-color <color> Set the light ball color (format: R,G,B or #RRGGBB)\n");
    printf("  --dark-color <color>  Set the dark ball color (format: R,G,B or #RRGGBB)\n");
    printf("  --start-size <scale>  Set initial ball size (%.2f to %.2f)\n",
           (double)FREEDOM_BALL_SCALE_MIN, (double)FREEDOM_BALL_SCALE_MAX);
    printf("  --audit-predict       Count expected and predicted impacts\n");
    printf("  --debug               Write performance diagnostics\n");
    printf("  --help, -h            Show this help message\n");
}

/* The value that follows an option, or NULL when the option was written
   last with nothing after it. */
static const char *poingo_option_value(int argc, char *argv[], int *i) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", argv[*i]);
        return NULL;
    }
    return argv[++(*i)];
}

static PoingoArgsResult poingo_parse_args(int argc, char *argv[], bool *start_muted) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "-h") == 0) {
            poingo_print_usage(argv[0]);
            return POINGO_ARGS_DONE;
        } else if (strcmp(argv[i], "--mute") == 0 || strcmp(argv[i], "-mute") == 0 || strcmp(argv[i], "mute") == 0) {
            *start_muted = true;
        } else if (strcmp(argv[i], "--light-color") == 0) {
            const char *value = poingo_option_value(argc, argv, &i);
            if (!value || !parse_color_arg(value, g_color_light_rgb)) {
                fprintf(stderr, "Invalid --light-color (use #RRGGBB or R,G,B)\n");
                return POINGO_ARGS_ERROR;
            }
        } else if (strcmp(argv[i], "--dark-color") == 0) {
            const char *value = poingo_option_value(argc, argv, &i);
            if (!value || !parse_color_arg(value, g_color_dark_rgb)) {
                fprintf(stderr, "Invalid --dark-color (use #RRGGBB or R,G,B)\n");
                return POINGO_ARGS_ERROR;
            }
        } else if (strcmp(argv[i], "--start-size") == 0) {
            const char *value_text = poingo_option_value(argc, argv, &i);
            char *end = NULL;
            /* strtod("nan") succeeds and consumes the whole string, and every
               comparison against a NaN is false -- so without the isfinite
               check the range test lets it through and it goes on to poison
               the ball geometry. */
            double value = value_text ? strtod(value_text, &end) : 0.0;
            if (!value_text || end == value_text || *end != '\0' ||
                !isfinite(value) ||
                value < FREEDOM_BALL_SCALE_MIN ||
                value > FREEDOM_BALL_SCALE_MAX) {
                fprintf(stderr, "Invalid --start-size (use %.2f to %.2f)\n",
                        (double)FREEDOM_BALL_SCALE_MIN,
                        (double)FREEDOM_BALL_SCALE_MAX);
                return POINGO_ARGS_ERROR;
            }
            g_freerange_ball_scale = (float)value;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
        } else if (strcmp(argv[i], "--audit-predict") == 0) {
            g_predict_audit = true;
        } else {
            /* Silently ignoring a typo makes a broken launcher entry look
               like it worked. */
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            poingo_print_usage(argv[0]);
            return POINGO_ARGS_ERROR;
        }
    }

    return POINGO_ARGS_RUN;
}

int main(int argc, char *argv[]) {
    bool start_muted = false;
    PoingoArgsResult args = poingo_parse_args(argc, argv, &start_muted);
    if (args == POINGO_ARGS_DONE) {
        return 0;
    }
    if (args == POINGO_ARGS_ERROR) {
        return 1;
    }

    const char *wayland_display_env = getenv("WAYLAND_DISPLAY");
    if (!wayland_display_env || wayland_display_env[0] == '\0') {
        fprintf(stderr, "poingo requires a Wayland compositor (WAYLAND_DISPLAY is not set).\n");
        return 1;
    }

    return run_freerange_wayland(start_muted);
}
