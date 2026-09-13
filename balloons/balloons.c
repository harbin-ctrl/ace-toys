
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <malloc.h>

#include <GLES2/gl2.h>

#include "compat.h"
#include "platform.h"
#include "balloon_gen.h"
#include "ghost_icon.h"
#include "audio.h"
#include "ringmenu.h"
#include "cursor_hand_grab.h"

#define BALLOON_SOUND_SCALE 0.45f
#define POP_SOUND_VOLUME 32   /* full whack (0-64 scale, typical 8-32) */
#define BOP_SOUND_VOLUME 14   /* gentle knuckle tap */

static bool g_startup_trace;
static struct timespec g_startup_t0;

static double startup_elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - g_startup_t0.tv_sec) * 1000.0 +
           (double)(now.tv_nsec - g_startup_t0.tv_nsec) * 1e-6;
}

static void startup_mark(const char *phase) {
    if (g_startup_trace) {
        fprintf(stderr, "[balloons startup] %-24s %8.1f ms\n",
                phase, startup_elapsed_ms());
    }
}

typedef struct {
    Anim *anims;
    Anim string_anim;
    Anim *pop_anims;
    int anim_count;
    int pop_count;
    bool assets_ok;
    bool audio_ok;
} StartupJobs;

static void *startup_assets_worker(void *userdata) {
    StartupJobs *jobs = userdata;
    jobs->assets_ok = balloon_generate_assets(&jobs->anims, &jobs->anim_count,
                                              &jobs->string_anim,
                                              &jobs->pop_anims, &jobs->pop_count);
    return NULL;
}

static void *startup_audio_worker(void *userdata) {
    StartupJobs *jobs = userdata;
    jobs->audio_ok = audio_init();
    return NULL;
}


typedef struct {
    int min_x, min_y;
    int max_x, max_y;
} AlphaBounds;

static AlphaBounds g_ghost_balloon_bounds;

static bool find_alpha_bounds(const Anim *animation, AlphaBounds *bounds) {
    if (!animation || !animation->frames || !animation->frames[0].rgba || !bounds) {
        return false;
    }

    int min_x = animation->w;
    int min_y = animation->h;
    int max_x = -1;
    int max_y = -1;
    const uint8_t *pixels = animation->frames[0].rgba;
    for (int y = 0; y < animation->h; y++) {
        for (int x = 0; x < animation->w; x++) {
            if (pixels[((size_t)y * animation->w + x) * 4 + 3] == 0) continue;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;
    *bounds = (AlphaBounds){ min_x, min_y, max_x, max_y };
    return true;
}


typedef enum { MODE_BOUNCE, MODE_FLOAT, MODE_SCURRY, MODE_DRIFT } Mode;
typedef enum { INTERACTION_GRAB, INTERACTION_POP } InteractionMode;

typedef struct {
    const Anim *anim;
    GLuint *textures;      
    float x, y;            
    float vx, vy;
    float base_x;          
    float phase;
    int facing;            
    int frame;
    float frame_time;      
    float action_timer;    
    int action;            
    float dash;            
    bool grabbed;          
    float grab_dx, grab_dy;
    float fall_v;          
    int pr_x, pr_y, pr_w, pr_h; 
    bool pr_valid;
    bool dead;
    bool popped;
    float pop_timer;
    float scheduled_pop_time;
    float scale;           
} Sprite;

static float frandf(void) { return (float)rand() / (float)RAND_MAX; }
static float frandf_sq(void) { float r = frandf(); return r * r; }

static bool g_trace;

static bool g_full_damage = true;

typedef PlatRect Rect;

static void rect_union(Rect *acc, const Rect *r) {
    if (!r || r->w <= 0 || r->h <= 0) return;
    if (acc->w <= 0 || acc->h <= 0) { *acc = *r; return; }
    int x0 = acc->x < r->x ? acc->x : r->x;
    int y0 = acc->y < r->y ? acc->y : r->y;
    int x1 = (acc->x + acc->w) > (r->x + r->w) ? (acc->x + acc->w) : (r->x + r->w);
    int y1 = (acc->y + acc->h) > (r->y + r->h) ? (acc->y + acc->h) : (r->y + r->h);
    acc->x = x0; acc->y = y0; acc->w = x1 - x0; acc->h = y1 - y0;
}

static void rect_clamp(Rect *r, int max_w, int max_h) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
    if (r->x + r->w > max_w) r->w = max_w - r->x;
    if (r->y + r->h > max_h) r->h = max_h - r->y;
}

static bool rect_intersects(const Rect *a, const Rect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

#define DAMAGE_HISTORY 4
static Rect *g_damage_hist[DAMAGE_HISTORY]; 
static int g_damage_hist_n[DAMAGE_HISTORY];
static int g_damage_hist_depth = 0;
static Rect *g_cur_damage;                  
static int g_ncur_damage;
static Rect *g_sprite_rects;                

static void damage_push(Rect r, int max_w, int max_h) {
    rect_clamp(&r, max_w, max_h);
    if (r.w <= 0 || r.h <= 0) return;
    g_cur_damage[g_ncur_damage++] = r;
}

static Anim *g_anims;
static GLuint **g_anim_tex;
static int g_nanims;

static Anim *g_pop_anims;
static GLuint **g_pop_tex;
static int g_npop_anims;

static Anim g_str_anim;
static GLuint *g_str_tex;

static GLuint g_ghost_bg_tex;

static float g_wind;           
static float g_wind_target;
static float g_breeze_timer = 8.f;  
static bool g_breezing;

static bool g_gust_synthesizing; 
static float g_gust_delay;     
static float g_gust_target;    
static float g_gust_duration;

static void start_gust(void) {
    g_breezing = true;
    g_wind_target = g_gust_target;
    g_breeze_timer = g_gust_duration;
}

static bool g_storm_active;
static float g_storm_timer;      
static float g_storm_leg_timer;  
static float g_storm_pop_timer;  
static float g_storm_duration;   
static float g_storm_alpha;      
static int g_over_w8;            
static int g_over_a8;            
static float g_storm_countdown;  

#ifndef STORM_PERIOD_MIN_S
#define STORM_PERIOD_MIN_S 420.f
#endif
#ifndef STORM_PERIOD_MAX_S
#define STORM_PERIOD_MAX_S 660.f
#endif
static float roll_storm_countdown(void) {
    return STORM_PERIOD_MIN_S +
           frandf() * (STORM_PERIOD_MAX_S - STORM_PERIOD_MIN_S);
}

#define LIGHTNING_MAX_STROKES 3
static float g_lightning_t = -1.f;   
static int   g_lightning_nstrokes;
static float g_lightning_start[LIGHTNING_MAX_STROKES];
static float g_lightning_hold[LIGHTNING_MAX_STROKES];
static float g_lightning_peak[LIGHTNING_MAX_STROKES];
static float g_lightning_total;
static float g_flash01;              
static float g_dry_flash_delay;      

#define THUNDER_MIN_DELAY_S 0.85f
#define THUNDER_MAX_DELAY_S 3.60f
static float g_thunder_delay = -1.f; 
static float g_thunder_dist;         

static void lightning_strike(void) {
    float dist01 = frandf();          
    g_lightning_nstrokes = 1 + rand() % LIGHTNING_MAX_STROKES;
    float t = 0.f;
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        g_lightning_start[k] = t;
        g_lightning_hold[k] = 0.03f + frandf() * 0.05f;
        g_lightning_peak[k] = 0.55f + 0.45f * frandf();
        t += g_lightning_hold[k] + 0.04f + frandf() * 0.09f;
    }
    g_lightning_peak[0] = fmaxf(g_lightning_peak[0], 0.85f);
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        g_lightning_peak[k] *= 1.f - 0.35f * dist01;
    }
    g_lightning_total = t + 0.45f;   
    g_lightning_t = 0.f;
    g_thunder_dist = dist01;
    g_thunder_delay = THUNDER_MIN_DELAY_S +
                      dist01 * (THUNDER_MAX_DELAY_S - THUNDER_MIN_DELAY_S);
    if (g_trace) fprintf(stderr,
                         "[trace] lightning: %d strokes over %.0f ms, dist %.2f, "
                         "thunder in %.0f ms\n",
                         g_lightning_nstrokes, (double)g_lightning_total * 1e3,
                         (double)dist01, (double)g_thunder_delay * 1e3);
}

static void thunder_update(float dt) {
    if (g_thunder_delay < 0.f) return;
    g_thunder_delay -= dt;
    if (g_thunder_delay <= 0.f) {
        g_thunder_delay = -1.f;
        play_thunder_sound(g_thunder_dist);
    }
}

static void lightning_update(float dt) {
    g_flash01 = 0.f;
    if (g_lightning_t < 0.f) return;
    g_lightning_t += dt;
    if (g_lightning_t >= g_lightning_total) {
        g_lightning_t = -1.f;
        return;
    }
    for (int k = 0; k < g_lightning_nstrokes; k++) {
        float ts = g_lightning_t - g_lightning_start[k];
        if (ts < 0.f) break;
        float v;
        if (ts < g_lightning_hold[k]) {
            v = g_lightning_peak[k];
        } else {
            float tau = (k == g_lightning_nstrokes - 1) ? 0.11f : 0.045f;
            v = g_lightning_peak[k] * expf(-(ts - g_lightning_hold[k]) / tau);
        }
        if (v > g_flash01) g_flash01 = v;
    }
}

static void pop_random_sprite(void);   

static void storm_new_leg(bool first) {
    float strength = 170.f + frandf() * 110.f;
    float dir;
    if (first) {
        dir = (frandf() < 0.5f) ? -1.f : 1.f;
    } else {
        dir = (g_wind_target > 0.f) ? -1.f : 1.f;
        if (frandf() < 0.25f) dir = -dir;
    }
    g_wind_target = dir * strength;
    g_storm_leg_timer = 4.5f + frandf() * 4.5f;
    if (g_storm_leg_timer > g_storm_timer) {
        g_storm_leg_timer = g_storm_timer;
    }
    /* Storm wind is the continuous bed; keep enough headroom for lightning's
     * thunder to move decisively into the foreground. */
    play_whoosh_async(1.0f, g_storm_leg_timer, 1.5f, false);
}

static void start_storm(void) {
    g_storm_timer = 37.5f + frandf() * 15.f;
    g_storm_duration = g_storm_timer;
    g_storm_pop_timer = 4.6f + frandf() * 7.7f;
    if (!g_storm_active) {
        g_storm_active = true;
        g_breezing = false;
        g_gust_delay = 0.f;
        g_gust_synthesizing = false;
        whoosh_gust_cancel();
        storm_new_leg(true);
    }
}

static void stop_storm(void) {
    if (g_storm_active) {
        g_storm_active = false;
        g_wind_target = 0.f;
        g_breeze_timer = 20.f + frandf_sq() * 20.f;
        g_storm_alpha = 0.0f;
        g_dry_flash_delay = 0.f;
        g_storm_countdown = roll_storm_countdown();
        whoosh_gust_cancel();
        whoosh_stop_playing();
    }
}

static void breeze_update(float dt) {
    lightning_update(dt);   
    thunder_update(dt);     
    if (g_storm_active) {
        g_storm_timer -= dt;
        g_storm_leg_timer -= dt;
        g_storm_pop_timer -= dt;
        if (g_storm_pop_timer <= 0.f) {
            g_storm_pop_timer = 4.6f + frandf() * 7.7f;
            if (frandf() < 0.55f) lightning_strike();
            else if (frandf() < 0.20f)
                g_dry_flash_delay = 0.4f + frandf() * 1.2f;
            float r = frandf();
            int casualties = (r < 0.5f) ? 1 : (r < 0.75f) ? 2 : 3;
            for (int k = 0; k < casualties; k++)
                pop_random_sprite();
        }
        if (g_dry_flash_delay > 0.f) {
            g_dry_flash_delay -= dt;
            if (g_dry_flash_delay <= 0.f) lightning_strike();
        }
        if (g_storm_timer <= 0.f) {
            g_storm_active = false;
            g_wind_target = 0.f;
            g_breeze_timer = 20.f + frandf_sq() * 20.f;
            g_storm_alpha = 0.0f;
            g_dry_flash_delay = 0.f;
            g_storm_countdown = roll_storm_countdown();
        } else if (g_storm_leg_timer <= 0.f) {
            storm_new_leg(false);
        }
        g_wind += (g_wind_target - g_wind) * fminf(1.f, dt * 1.6f);

        if (g_storm_active) {
            float elapsed = g_storm_duration - g_storm_timer;
            float remaining = g_storm_timer;
            float fade_in_time = 4.0f;
            float fade_out_time = 4.0f;
            float max_alpha = 0.6f;

            if (elapsed < fade_in_time) {
                g_storm_alpha = (elapsed / fade_in_time) * max_alpha;
            } else if (remaining < fade_out_time) {
                g_storm_alpha = (remaining / fade_out_time) * max_alpha;
            } else {
                g_storm_alpha = max_alpha;
            }
        }
        return;
    }
    g_storm_alpha = 0.0f;
    g_storm_countdown -= dt;
    if (g_storm_countdown <= 0.f) {
        start_storm();   
        return;
    }
    if (g_gust_synthesizing) {
        float latency = 0.f;
        if (whoosh_gust_poll(&latency)) {
            g_gust_synthesizing = false;
            g_gust_delay = latency;
            if (g_gust_delay <= 0.f) start_gust();
        }
    } else if (g_gust_delay > 0.f) {
        g_gust_delay -= dt;
        if (g_gust_delay <= 0.f) start_gust();
    } else {
        g_breeze_timer -= dt;
        if (g_breeze_timer <= 0.f) {
            if (g_breezing) {
                g_breezing = false;
                g_wind_target = 0.f;
                g_breeze_timer = 20.f + frandf_sq() * 20.f;
            } else {
                float strength = 70.f + frandf() * 80.f;
                g_gust_target = (frandf() < 0.5f) ? -strength : strength;
                g_gust_duration = 4.0f + frandf_sq() * 5.0f;
                if (play_whoosh_async((strength - 90.f) / 110.f,
                                      g_gust_duration, 1.0f, true))
                    g_gust_synthesizing = true;
                else
                    start_gust();  
            }
        }
    }
    g_wind += (g_wind_target - g_wind) * fminf(1.f, dt * 1.6f);
}

#define BALLOON_SCALE_MIN 0.833333f
#define BALLOON_SCALE_MAX 1.0f

#define BALLOON_TIE_X (36.0f / 72.0f)
#define BALLOON_TIE_Y (71.5f / 128.0f)

#define BALLOON_BODY_CX (36.0f / 72.0f)
#define BALLOON_BODY_CY (34.0f / 128.0f)
#define BALLOON_BOB_FRAC (3.0f / 128.0f)
#define GHOST_BALLOON_BODY_HEIGHT_FRAC 0.70f

static float sprite_w(const Sprite *s, float scale) { return s->anim->w * scale * s->scale; }
static float sprite_h(const Sprite *s, float scale) { return s->anim->h * scale * s->scale; }

static void sprite_roll_anim(Sprite *s) {
    int i = rand() % g_nanims;
    s->anim = &g_anims[i];
    s->textures = g_anim_tex[i];
    s->frame = rand() % s->anim->nframes;
    s->frame_time = frandf() * s->anim->frames[s->frame].delay_ms;
    s->scale = BALLOON_SCALE_MIN + frandf() * (BALLOON_SCALE_MAX - BALLOON_SCALE_MIN);
}

static void sprite_init(Sprite *s, Mode mode, float scale, float speed, int sw, int sh) {
    float w = sprite_w(s, scale), h = sprite_h(s, scale);
    s->frame = rand() % s->anim->nframes;
    s->frame_time = frandf() * s->anim->frames[s->frame].delay_ms;
    s->facing = 1;
    s->phase = frandf() * 6.2831853f;
    switch (mode) {
    case MODE_BOUNCE:
        s->x = frandf() * (sw - w);
        s->y = frandf() * (sh - h);
        s->vx = (frandf() < 0.5f ? -1.f : 1.f) * (60.f + frandf() * 60.f) * speed;
        s->vy = (frandf() < 0.5f ? -1.f : 1.f) * (60.f + frandf() * 60.f) * speed;
        break;
    case MODE_FLOAT:
        s->base_x = frandf() * (sw - w);
        s->x = s->base_x;
        s->y = sh - frandf() * (sh + h);
        s->vy = -(20.f + frandf() * 25.f) * speed;
        break;
    case MODE_SCURRY:
        s->x = frandf() * (sw - w);
        s->y = sh - h;
        s->vx = (100.f + frandf() * 80.f) * speed;
        if (frandf() < 0.5f) s->vx = -s->vx;
        s->action = 0;
        s->action_timer = 1.0f + frandf_sq() * 1.5f;
        break;
    case MODE_DRIFT:
        s->x = frandf() * (sw - w);
        s->y = frandf() * (sh - h);
        break;
    }
    if (s->vx < 0) s->facing = -1;
}

static void sprite_update(Sprite *s, Mode mode, float dt, float t,
                          float scale, float speed, int sw, int sh) {
    if (s->dead) return;
    if (s->popped) {
        s->pop_timer -= dt;
        if (s->pop_timer <= 0.f) {
            s->dead = true;
        }
    }
    float w = sprite_w(s, scale), h = sprite_h(s, scale);

    float anim_rate = 1.f + (mode == MODE_SCURRY ? s->dash / 300.f : 0.f);
    s->frame_time += dt * 1000.f * anim_rate;
    while (s->frame_time >= s->anim->frames[s->frame].delay_ms) {
        s->frame_time -= s->anim->frames[s->frame].delay_ms;
        s->frame++;
        if (s->frame >= s->anim->nframes) {
            s->frame = 0;
        }
    }

    if (s->grabbed) return; 

    switch (mode) {
    case MODE_BOUNCE:
        s->x += s->vx * dt;
        s->y += s->vy * dt;
        if (s->x < 0)       { s->x = 0;       s->vx = fabsf(s->vx); }
        if (s->x + w > sw)  { s->x = sw - w;  s->vx = -fabsf(s->vx); }
        if (s->y < 0)       { s->y = 0;       s->vy = fabsf(s->vy); }
        if (s->y + h > sh)  { s->y = sh - h;  s->vy = -fabsf(s->vy); }
        s->facing = (s->vx < 0) ? -1 : 1;
        break;
    case MODE_FLOAT:
        s->y += s->vy * dt;
        s->base_x += g_wind * (0.75f + 0.5f * fabsf(sinf(s->phase))) * dt;
        if (s->base_x > sw + 40.f)     s->base_x = -w - 40.f;
        if (s->base_x < -w - 40.f)     s->base_x = sw + 40.f;
        s->x = s->base_x + sinf(t * 0.9f + s->phase) * 24.f;
        if (s->y + h < 0) { 
            sprite_roll_anim(s); 
            s->y = sh;
            s->base_x = frandf() * (sw - sprite_w(s, scale));
            s->vy = -(20.f + frandf() * 25.f) * speed;
        }
        break;
    case MODE_SCURRY:
        s->action_timer -= dt;
        if (s->action_timer <= 0.f) {
            if (s->action == 0 && frandf() < 0.4f) {
                s->action = 1; 
                s->action_timer = 0.3f + frandf_sq() * 1.7f;
            } else {
                s->action = 0;
                s->action_timer = 0.6f + frandf_sq() * 2.4f;
                float v = (100.f + frandf() * 120.f) * speed;
                s->vx = (frandf() < 0.5f ? -v : v);
            }
        }
        s->dash *= exp2f(-2.0f * dt); 
        if (s->action == 0 || s->dash > 1.f) {
            float dir = (s->vx < 0) ? -1.f : 1.f;
            s->x += (s->vx + dir * s->dash) * dt;
        }
        if (s->x < 0)      { s->x = 0;      s->vx = fabsf(s->vx); }
        if (s->x + w > sw) { s->x = sw - w; s->vx = -fabsf(s->vx); }
        if (s->y < sh - h - 0.5f) { 
            s->fall_v += 1400.f * dt;
            s->y += s->fall_v * dt;
        }
        if (s->y >= sh - h) { s->y = sh - h; s->fall_v = 0.f; }
        s->facing = (s->vx < 0) ? -1 : 1;
        break;
    case MODE_DRIFT: {
        float ang = sinf(t * 0.31f + s->phase) * 2.2f + sinf(t * 0.13f + s->phase * 2.f) * 1.7f;
        float v = (30.f + 20.f * sinf(t * 0.21f + s->phase)) * speed;
        s->x += cosf(ang) * v * dt;
        s->y += sinf(ang) * v * dt * 0.6f;
        if (s->x < 0)      s->x = 0;
        if (s->x + w > sw) s->x = sw - w;
        if (s->y < 0)      s->y = 0;
        if (s->y + h > sh) s->y = sh - h;
        s->facing = (cosf(ang) < 0) ? -1 : 1;
        break;
    }
    }
}


typedef struct {
    Plat *plat;
    /* Input region scratch: a body box per balloon, or one for menu or ghost. */
    PlatRect *input_rects;

    int width, height;
    bool resize_pending;
    bool running;
    bool need_redraw;

    double ptr_x, ptr_y;

    int needle_cursor;
    int hand_cursor;
} Ctx;

static volatile sig_atomic_t g_signal_quit = 0;
static double g_quit_fade = 0.0;
static bool g_ghost = false;
static double g_startup_fade = 0.0;
static GLint g_fade_loc = -1;
static GLint g_color_loc = -1;
static void on_signal(int sig) { (void)sig; g_signal_quit = 1; }

static void on_resize(void *data, int width, int height) {
    Ctx *ctx = data;
    ctx->width = width;
    ctx->height = height;
    ctx->resize_pending = true;
    g_full_damage = true;
    ctx->need_redraw = true;
}
static Ctx *g_ctx;
static Sprite *g_sprites;
static int g_nsprites;
static Mode g_mode = MODE_BOUNCE;
static float g_scale = 1.0f;
static float g_speed = 1.0f;
static float g_time = 0.0f;
static int g_grab_index = -1;
static InteractionMode g_interaction_mode = INTERACTION_GRAB;
static double g_press_x, g_press_y;
static struct timespec g_press_ts;

static void pop_sprite(Sprite *s, float pop_x, float pop_y) {
    int pop_idx = rand() % g_npop_anims;
    s->popped = true;
    s->pop_timer = 0.15f;
    s->anim = &g_pop_anims[pop_idx];
    s->textures = g_pop_tex[pop_idx];
    s->frame = 0;
    s->frame_time = 0;
    float pw = sprite_w(s, g_scale);
    float ph = sprite_h(s, g_scale);
    s->x = pop_x - pw * 0.5f;
    s->y = pop_y - ph * 0.5f;
    play_pop_sound(POP_SOUND_VOLUME);
}

static void pop_random_sprite(void) {
    int pick = -1, seen = 0;
    for (int i = 0; i < g_nsprites; i++) {
        if (g_sprites[i].dead || g_sprites[i].popped) continue;
        seen++;
        if (rand() % seen == 0) pick = i;
    }
    if (pick >= 0) {
        Sprite *s = &g_sprites[pick];
        float cx = s->x + sprite_w(s, g_scale) * 0.5f;
        float cy = s->y + sprite_h(s, g_scale) * (32.f / 128.f); 
        pop_sprite(s, cx, cy);
    }
}

static bool g_mass_pop_active = false;
static bool g_mass_pop_quit = false;   
static float g_mass_pop_timer = 0.f;

static void start_mass_pop(bool then_quit) {
    if (g_mass_pop_active) {
        g_mass_pop_quit = g_mass_pop_quit || then_quit;
        return;
    }
    g_mass_pop_active = true;
    g_mass_pop_quit = then_quit;
    g_mass_pop_timer = 0.f;
    for (int i = 0; i < g_nsprites; i++) {
        if (!g_sprites[i].dead && !g_sprites[i].popped) {
            float u1 = frandf();
            if (u1 < 0.0001f) u1 = 0.0001f;
            float u2 = frandf();
            float z0 = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
            float val = fabsf(z0) * 1.5f;
            if (val > 5.0f) val = 5.0f;
            g_sprites[i].scheduled_pop_time = val;
        }
    }
}

static void trigger_quit(void) {
    g_signal_quit = 1;
}

static RingMenu *g_menu;
static GLuint g_menu_tex;
static uint32_t *g_menu_scratch;
static bool g_menu_was_open;   

enum { MENU_STORM = 1, MENU_GRAB, MENU_POP, MENU_POP_ALL, MENU_GHOST, MENU_QUIT };

static void update_pointer_cursor(void);

static void menu_result(int r) {
    if (r == MENU_STORM) {
        if (g_storm_active) {
            stop_storm();
        } else {
            start_storm();
        }
    }
    else if (r == MENU_GRAB || r == MENU_POP) {
        if (g_grab_index >= 0) {
            g_sprites[g_grab_index].grabbed = false;
            g_grab_index = -1;
        }
        g_interaction_mode = r == MENU_GRAB ? INTERACTION_GRAB : INTERACTION_POP;
        update_pointer_cursor();
    }
    else if (r == MENU_POP_ALL) start_mass_pop(false);
    else if (r == MENU_GHOST) g_ghost = true;
    else if (r == MENU_QUIT) trigger_quit();
}

static void on_close(void *data) {
    (void)data;
    trigger_quit();
}

static int sprite_hit(double px, double py) {
    for (int i = g_nsprites - 1; i >= 0; i--) {
        Sprite *s = &g_sprites[i];
        if (s->dead || s->popped) continue;
        float w = sprite_w(s, g_scale), h = sprite_h(s, g_scale);
        
        float bx = s->x + w * (12.f / 72.f);
        float by = s->y + h * (4.f / 128.f);
        float bw = w * (48.f / 72.f);
        float bh = h * (60.f / 128.f);
        
        if (px >= bx && px < bx + bw && py >= by && py < by + bh)
            return i;
    }
    return -1;
}

#define NEEDLE_SIZE 64
#define NEEDLE_TIP 4.5      /* tip position (x == y) inside the buffer */
#define NEEDLE_LEN 38.0     /* tip to shaft end, along the diagonal */
#define NEEDLE_W 2.6        /* shaft half-width once fully tapered */
#define NEEDLE_HEAD_R 7.0
#define NAXIS 0.70710678

static double clampd01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void render_needle_cursor(uint32_t *px, int size) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double rx = (x + 0.5) - NEEDLE_TIP;
            double ry = (y + 0.5) - NEEDLE_TIP;
            double t = (rx + ry) * NAXIS;   
            double s = (ry - rx) * NAXIS;   

            double half = NEEDLE_W * (t < 12.0 ? clampd01(t / 12.0) : 1.0);
            double d_sh = fabs(s) - half;
            double dcap = t - (NEEDLE_LEN + 2.0);
            if (dcap > d_sh) d_sh = dcap;
            if (t < 0.0) d_sh = sqrt(t * t + s * s);

            double ht = t - (NEEDLE_LEN + 3.0);
            double d_hd = sqrt(ht * ht + s * s) - NEEDLE_HEAD_R;

            double d = d_sh < d_hd ? d_sh : d_hd;
            double cov = clampd01(0.5 - d);
            if (cov <= 0.0) {
                px[(size_t)y * size + x] = 0;
                continue;
            }

            double fr, fg, fb;
            if (d_hd < d_sh) {
                fr = 210.0; fg = 45.0; fb = 62.0;
                double glint = clampd01((2.4 - sqrt((ht + 2.4) * (ht + 2.4) +
                                                    (s + 2.4) * (s + 2.4))) / 1.4);
                fr += (255.0 - fr) * glint * 0.9;
                fg += (255.0 - fg) * glint * 0.9;
                fb += (255.0 - fb) * glint * 0.9;
            } else {
                fr = 205.0; fg = 210.0; fb = 222.0;
                double line = clampd01(1.0 - fabs(s + half * 0.4) / 0.9);
                fr += (255.0 - fr) * line * 0.7;
                fg += (255.0 - fg) * line * 0.7;
                fb += (255.0 - fb) * line * 0.7;
            }

            double ot = clampd01((d + 1.1) / 0.8);
            fr = fr * (1.0 - ot) + 40.0 * ot;
            fg = fg * (1.0 - ot) + 40.0 * ot;
            fb = fb * (1.0 - ot) + 46.0 * ot;

            uint32_t a8 = (uint32_t)(cov * 255.0 + 0.5);
            uint32_t r8 = (uint32_t)(fr * cov + 0.5);
            uint32_t g8 = (uint32_t)(fg * cov + 0.5);
            uint32_t b8 = (uint32_t)(fb * cov + 0.5);
            px[(size_t)y * size + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }
}

/* The cursor art is smaller than the shared cursor buffer, so it is blitted
 * into the top-left corner and the rest left transparent. The hotspot is
 * expressed in buffer coordinates and so needs no adjustment for the margin.
 *
 * The pixels arrive already premultiplied, which is what WL_SHM_FORMAT_ARGB8888
 * wants. (The PNG this replaced was packed straight into the buffer without
 * premultiplying, so its semi-transparent edge pixels were a little too bright
 * against dark backgrounds.) */
static void render_hand_cursor(uint32_t *px, int size) {
    memset(px, 0, (size_t)size * size * sizeof(*px));
    if (size < CURSOR_HAND_GRAB_W || size < CURSOR_HAND_GRAB_H) return;

    for (int y = 0; y < CURSOR_HAND_GRAB_H; ++y) {
        memcpy(px + (size_t)y * size,
               balloons_cursor_hand_grab_argb + (size_t)y * CURSOR_HAND_GRAB_W,
               (size_t)CURSOR_HAND_GRAB_W * sizeof(*px));
    }
}

/* The grab cursor's hotspot, in its pixels. */
#define HAND_HOT_X 9
#define HAND_HOT_Y 7

typedef void (*CursorRenderer)(uint32_t *pixels, int size);

/* Renders one NEEDLE_SIZE square and hands it to the platform, which keeps
   its own copy. Returns the cursor's id, or -1. */
static int create_cursor(Ctx *ctx, CursorRenderer render, int hot_x, int hot_y) {
    uint32_t *pixels = malloc((size_t)NEEDLE_SIZE * NEEDLE_SIZE * sizeof(*pixels));
    if (!pixels) return -1;
    render(pixels, NEEDLE_SIZE);
    int cursor = plat_cursor_create(ctx->plat, pixels, NEEDLE_SIZE, 1, hot_x, hot_y);
    free(pixels);
    return cursor;
}

static bool create_needle_cursor(Ctx *ctx) {
    ctx->needle_cursor = create_cursor(ctx, render_needle_cursor,
                                       (int)NEEDLE_TIP, (int)NEEDLE_TIP);
    return ctx->needle_cursor >= 0;
}

static bool create_hand_cursor(Ctx *ctx) {
    ctx->hand_cursor = create_cursor(ctx, render_hand_cursor, HAND_HOT_X, HAND_HOT_Y);
    return ctx->hand_cursor >= 0;
}

static void update_pointer_cursor(void) {
    if (!g_ctx || !g_ctx->plat) return;
    int cursor = g_interaction_mode == INTERACTION_GRAB
                     ? g_ctx->hand_cursor
                     : g_ctx->needle_cursor;
    if (cursor >= 0) plat_cursor_use(g_ctx->plat, cursor);
}

static void pointer_enter(void *d, int x, int y) {
    (void)d;
    g_ctx->ptr_x = x;
    g_ctx->ptr_y = y;
    if (g_trace) fprintf(stderr, "[trace] enter %.0f,%.0f\n", g_ctx->ptr_x, g_ctx->ptr_y);
}
static void pointer_leave(void *d) {
    (void)d;
}
static void grabbed_follow_pointer(void) {
    Sprite *s = &g_sprites[g_grab_index];
    s->x = (float)g_ctx->ptr_x - s->grab_dx;
    s->y = (float)g_ctx->ptr_y - s->grab_dy;
}
static void pointer_motion(void *d, int x, int y) {
    (void)d;
    g_ctx->ptr_x = x;
    g_ctx->ptr_y = y;
    if (g_grab_index >= 0) grabbed_follow_pointer();
    if (ringmenu_is_open(g_menu))
        ringmenu_motion(g_menu, (int)g_ctx->ptr_x, (int)g_ctx->ptr_y);
}
static void pointer_button(void *d, PlatButton button, PlatPress press) {
    (void)d;
    bool pressed = press == PLAT_PRESSED;

    if (g_ghost) {
        if (pressed) {
            g_ghost = false;
        }
        return;
    }

    if (g_menu && ringmenu_is_open(g_menu)) {
        int btn = -1;
        if (button == PLAT_BTN_LEFT) btn = RINGMENU_BTN_LEFT;
        else if (button == PLAT_BTN_RIGHT) btn = RINGMENU_BTN_RIGHT;
        if (btn >= 0) {
            int r = ringmenu_button(g_menu, btn, pressed);
            if (r >= 0) menu_result(r);
        }
        return;
    }

    if (button == PLAT_BTN_MIDDLE) {
        /* Middle-click has no action outside the ring menu. */
        return;
    }

    if (button == PLAT_BTN_LEFT && g_interaction_mode == INTERACTION_GRAB) {
        if (pressed) {
            int hit = sprite_hit(g_ctx->ptr_x, g_ctx->ptr_y);
            if (hit >= 0 && !g_sprites[hit].popped) {
                Sprite *s = &g_sprites[hit];
                g_grab_index = hit;
                s->grabbed = true;
                s->grab_dx = (float)g_ctx->ptr_x - s->x;
                s->grab_dy = (float)g_ctx->ptr_y - s->y;
                g_press_x = g_ctx->ptr_x;
                g_press_y = g_ctx->ptr_y;
                clock_gettime(CLOCK_MONOTONIC, &g_press_ts);
            }
        } else if (g_grab_index >= 0) { 
            Sprite *s = &g_sprites[g_grab_index];
            s->grabbed = false;
            g_grab_index = -1;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double held = (now.tv_sec - g_press_ts.tv_sec) +
                          (now.tv_nsec - g_press_ts.tv_nsec) * 1e-9;
            double moved = fabs(g_ctx->ptr_x - g_press_x) + fabs(g_ctx->ptr_y - g_press_y);
            if (held >= 0.3 || moved >= 6.0) {
                if (g_mode == MODE_FLOAT) {
                    s->base_x = s->x;
                    s->phase = -0.9f * g_time;
                } else if (g_mode == MODE_SCURRY) {
                    s->fall_v = 0.f; 
                }
            }
        }
        return;
    }
    if (button == PLAT_BTN_RIGHT && pressed) {
        if (g_menu) {
            ringmenu_set_led(g_menu, MENU_STORM - 1,
                             g_storm_active ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, MENU_GRAB - 1,
                             g_interaction_mode == INTERACTION_GRAB
                                 ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, MENU_POP - 1,
                             g_interaction_mode == INTERACTION_POP
                                 ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_set_led(g_menu, MENU_GHOST - 1,
                             g_ghost ? RINGMENU_LED_ON : RINGMENU_LED_OFF);
            ringmenu_open(g_menu, (int)g_ctx->ptr_x, (int)g_ctx->ptr_y,
                          g_ctx->width, g_ctx->height);
        }
        return;
    }
    if (button != PLAT_BTN_LEFT || !pressed ||
        g_interaction_mode != INTERACTION_POP) return;

    int hit = sprite_hit(g_ctx->ptr_x, g_ctx->ptr_y);
    if (g_trace) fprintf(stderr, "[trace] press at %.0f,%.0f hit=%d\n",
                         g_ctx->ptr_x, g_ctx->ptr_y, hit);
    if (hit >= 0 && !g_sprites[hit].popped)
        pop_sprite(&g_sprites[hit], (float)g_ctx->ptr_x, (float)g_ctx->ptr_y);
}
static void pointer_scroll(void *d, double delta) {
    (void)d; (void)delta;
}
static void pointer_lost(void *d) {
    (void)d;
}
static void key_press(void *d, PlatKey key, PlatPress press) {
    (void)d;
    if (press != PLAT_PRESSED) return;
    if (key == PLAT_KEY_ESC || key == PLAT_KEY_Q)
        trigger_quit();
    else if (key == PLAT_KEY_SPACE)
        g_ghost = !g_ghost;
    else if (key == PLAT_KEY_M)
        toggle_master_mute();
    else if (key == PLAT_KEY_UP)
        adjust_master_volume(0.05f);
    else if (key == PLAT_KEY_DOWN)
        adjust_master_volume(-0.05f);
}
static void keyboard_lost(void *d) {
    (void)d;
}

static const PlatHandlers g_handlers = {
    .pointer_enter = pointer_enter,
    .pointer_leave = pointer_leave,
    .pointer_motion = pointer_motion,
    .pointer_button = pointer_button,
    .pointer_scroll = pointer_scroll,
    .pointer_lost = pointer_lost,
    .key = key_press,
    .keyboard_lost = keyboard_lost,
    .resize = on_resize,
    .close = on_close,
};


static const char *vert_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *frag_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_fade;\n"
    "uniform vec4 u_color_mult;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color_mult * u_fade; }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "apngo: shader error: %s\n", log);
        exit(1);
    }
    return sh;
}

static void draw_tex_quad(GLuint tex, float x, float y, float w, float h,
                          int sw, int sh, int facing) {
    float x0 = 2.f * x / sw - 1.f;
    float x1 = 2.f * (x + w) / sw - 1.f;
    float y0 = 1.f - 2.f * y / sh;
    float y1 = 1.f - 2.f * (y + h) / sh;
    float u0 = facing < 0 ? 1.f : 0.f;
    float u1 = facing < 0 ? 0.f : 1.f;
    GLfloat verts[16] = {
        x0, y0, u0, 0.f,
        x1, y0, u1, 0.f,
        x0, y1, u0, 1.f,
        x1, y1, u1, 1.f,
    };
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_ghost_shadow(GLuint texture, float x, float y, float w, float h,
                              int screen_width, int screen_height) {
    enum { radius = 3 };
    const float sigma = 1.65f;
    const float two_sigma_squared = 2.0f * sigma * sigma;
    float weight_sum = 0.0f;

    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++)
            weight_sum += expf(-(float)(dx * dx + dy * dy) / two_sigma_squared);

    /* Source-over blending makes several samples less opaque than their
     * arithmetic sum. Convert each normalized weight so a solid silhouette
     * still reaches Poingo's 40% black core. */
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            float weight = expf(-(float)(dx * dx + dy * dy) / two_sigma_squared) / weight_sum;
            float alpha = 1.0f - powf(1.0f - GHOST_ICON_SHADOW_OPACITY, weight);
            if (g_color_loc >= 0) glUniform4f(g_color_loc, 0.0f, 0.0f, 0.0f, alpha);
            draw_tex_quad(texture, x + GHOST_ICON_SHADOW_OFFSET_X + dx,
                          y + GHOST_ICON_SHADOW_OFFSET_Y + dy,
                          w, h, screen_width, screen_height, 1);
        }
    }
    if (g_color_loc >= 0) glUniform4f(g_color_loc, 1.0f, 1.0f, 1.0f, 1.0f);
}

static void draw_sprite(const Sprite *s, float scale, int sw, int sh) {
    draw_tex_quad(s->textures[s->frame], s->x, s->y,
                  sprite_w(s, scale), sprite_h(s, scale), sw, sh, s->facing);
}

static void string_quad(const Sprite *s, float *x, float *y, float *w, float *h) {
    float bw = sprite_w(s, g_scale), bh = sprite_h(s, g_scale);
    *w = (float)g_str_anim.w;
    *h = (float)g_str_anim.h;
    *x = s->x + bw * BALLOON_TIE_X - *w * BALLOON_TIE_X;
    *y = s->y + bh * BALLOON_TIE_Y - *h * BALLOON_TIE_Y;
}

static void draw_string(const Sprite *s, int sw, int sh) {
    if (!g_str_tex || g_str_anim.nframes <= 0) return;
    float x, y, w, h;
    string_quad(s, &x, &y, &w, &h);
    int f = s->frame % g_str_anim.nframes;   
    draw_tex_quad(g_str_tex[f], x, y, w, h, sw, sh, 1);
}



int main(int argc, char **argv) {
    (void)argc; (void)argv;                   
    const Mode mode = MODE_FLOAT;
    const int count = 30;
    const float scale = 1.0f, speed = 1.0f;  
    const bool pixel = false;
    int nfiles = 0;

    clock_gettime(CLOCK_MONOTONIC, &g_startup_t0);
    g_startup_trace = getenv("BALLOONS_STARTUP_TRACE") != NULL;
    startup_mark("begin");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    g_trace = getenv("APNGO_TRACE") != NULL;
    g_storm_countdown = roll_storm_countdown();

    StartupJobs startup_jobs = {0};
    pthread_t audio_thread, assets_thread;
    bool audio_thread_started =
        pthread_create(&audio_thread, NULL, startup_audio_worker, &startup_jobs) == 0;
    bool assets_thread_started =
        pthread_create(&assets_thread, NULL, startup_assets_worker, &startup_jobs) == 0;

    if (!audio_thread_started) startup_jobs.audio_ok = audio_init();
    if (!assets_thread_started) {
        startup_jobs.assets_ok = balloon_generate_assets(
            &startup_jobs.anims, &startup_jobs.anim_count,
            &startup_jobs.string_anim, &startup_jobs.pop_anims,
            &startup_jobs.pop_count);
    }
    startup_mark("startup jobs launched");

    /* While those jobs run, continue with the independent window-system setup
     * below. They are joined before any generated pixels are uploaded. */
    Anim *anims = NULL;
    if (!startup_jobs.assets_ok && !assets_thread_started) {
        fprintf(stderr, "balloons: failed to generate runtime assets\n");
        return 1;
    }

    Ctx ctx = {0};
    g_ctx = &ctx;
    ctx.running = true;
    ctx.width = 1280;
    ctx.height = 720;
    g_mode = mode;
    g_scale = scale;
    g_speed = speed;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ctx.needle_cursor = -1;
    ctx.hand_cursor = -1;
    PlatConfig plat_config = {
        .title = "balloons",
        .app_id = "balloons",
        .log = g_trace ? PLAT_LOG_DEBUG : PLAT_LOG_QUIET,
        .damage = getenv("APNGO_NO_DAMAGE") ? PLAT_DAMAGE_OFF : PLAT_DAMAGE_AUTO,
    };
    ctx.plat = plat_open(&plat_config, &g_handlers, &ctx);
    if (!ctx.plat) return 1;
    startup_mark("window system");

    if (audio_thread_started) pthread_join(audio_thread, NULL);
    if (assets_thread_started) pthread_join(assets_thread, NULL);
    if (!startup_jobs.audio_ok) {
        fprintf(stderr, "balloons: audio initialization failed; continuing silently\n");
    }
    if (!startup_jobs.assets_ok) {
        fprintf(stderr, "balloons: failed to generate runtime assets\n");
        return 1;
    }
    anims = startup_jobs.anims;
    nfiles = startup_jobs.anim_count;
    g_str_anim = startup_jobs.string_anim;
    g_pop_anims = startup_jobs.pop_anims;
    g_npop_anims = startup_jobs.pop_count;
    startup_mark("audio and assets ready");
    mark_sounds_dirty(BALLOON_SOUND_SCALE);
    audio_pregen_async();

    if (!find_alpha_bounds(&anims[0], &g_ghost_balloon_bounds)) {
        fprintf(stderr, "balloons: unable to measure ghost icon source\n");
        return 1;
    }

    startup_mark("external assets");

    int output_w = 0, output_h = 0;
    plat_output_size(ctx.plat, &output_w, &output_h);
    if (output_w > 0 && output_h > 0) {
        ctx.width = output_w;
        ctx.height = output_h;
    }
    if (!plat_window_create(ctx.plat, ctx.width, ctx.height) ||
        !plat_window_show(ctx.plat)) {
        plat_close(ctx.plat);
        return 1;
    }
    if (ctx.resize_pending) {
        plat_surface_resize(ctx.plat, ctx.width, ctx.height);
        ctx.resize_pending = false;
    }
    startup_mark("EGL initialized");

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile_shader(GL_VERTEX_SHADER, vert_src));
    glAttachShader(prog, compile_shader(GL_FRAGMENT_SHADER, frag_src));
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glUseProgram(prog);
    g_fade_loc = glGetUniformLocation(prog, "u_fade");
    g_color_loc = glGetUniformLocation(prog, "u_color_mult");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
    if (g_color_loc >= 0) glUniform4f(g_color_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); 
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    startup_mark("GL pipeline");

    {
        RingMenuItem items[6] = {
            { .label = "STORM", .led = RINGMENU_LED_OFF },
            { .label = "GRAB", .led = RINGMENU_LED_ON },
            { .label = "POP", .led = RINGMENU_LED_OFF },
            { .label = "POP ALL" },
            { .label = "GHOST", .led = RINGMENU_LED_OFF },
            { .label = "QUIT" },
        };
        g_menu = ringmenu_create(items, 6);
    if (!g_menu) fprintf(stderr, "balloons: no ring menu\n");
    if (g_menu) {
        int menu_size = ringmenu_size(g_menu);
        g_menu_scratch = malloc((size_t)menu_size * menu_size * 4);
        if (!g_menu_scratch) {
            fprintf(stderr, "balloons: failed to allocate menu workspace\n");
            ringmenu_destroy(g_menu);
            g_menu = NULL;
        }
    }
    }
    glGenTextures(1, &g_menu_tex);
    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!create_needle_cursor(&ctx)) {
        fprintf(stderr, "balloons: no needle cursor, using the default pointer\n");
    }
    if (!create_hand_cursor(&ctx)) {
        fprintf(stderr, "balloons: no hand cursor, using the default pointer\n");
    }
    update_pointer_cursor();

    GLuint **anim_tex = calloc((size_t)nfiles, sizeof(GLuint *));
    if (!anim_tex) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < nfiles; i++) {
        anim_tex[i] = malloc((size_t)anims[i].nframes * sizeof(GLuint));
        glGenTextures(anims[i].nframes, anim_tex[i]);
        for (int f = 0; f < anims[i].nframes; f++) {
            glBindTexture(GL_TEXTURE_2D, anim_tex[i][f]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, anims[i].w, anims[i].h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, anims[i].frames[f].rgba);
            GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            free(anims[i].frames[f].rgba);   
            anims[i].frames[f].rgba = NULL;
        }
    }

    g_str_tex = malloc((size_t)g_str_anim.nframes * sizeof(GLuint));
    if (!g_str_tex) { fprintf(stderr, "out of memory\n"); return 1; }
    glGenTextures(g_str_anim.nframes, g_str_tex);
    for (int f = 0; f < g_str_anim.nframes; f++) {
        glBindTexture(GL_TEXTURE_2D, g_str_tex[f]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_str_anim.w, g_str_anim.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_str_anim.frames[f].rgba);
        GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(g_str_anim.frames[f].rgba);
        g_str_anim.frames[f].rgba = NULL;
    }

    g_pop_tex = calloc((size_t)g_npop_anims, sizeof(GLuint *));
    for (int i = 0; i < g_npop_anims; i++) {
        g_pop_tex[i] = malloc((size_t)g_pop_anims[i].nframes * sizeof(GLuint));
        glGenTextures(g_pop_anims[i].nframes, g_pop_tex[i]);
        for (int f = 0; f < g_pop_anims[i].nframes; f++) {
            glBindTexture(GL_TEXTURE_2D, g_pop_tex[i][f]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_pop_anims[i].w, g_pop_anims[i].h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, g_pop_anims[i].frames[f].rgba);
            GLint filt = pixel ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            free(g_pop_anims[i].frames[f].rgba);
        }
    }
    startup_mark("texture uploads");
    
    {
        uint32_t *bg = ghost_icon_create_bg(true);
        if (bg) {
            glGenTextures(1, &g_ghost_bg_tex);
            glBindTexture(GL_TEXTURE_2D, g_ghost_bg_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GHOST_ICON_SIZE, GHOST_ICON_SIZE,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            free(bg);
        }
    }
    
    malloc_trim(0);   


    g_anims = anims;
    g_anim_tex = anim_tex;
    g_nanims = nfiles;
    g_nsprites = count;
    g_sprites = calloc((size_t)g_nsprites, sizeof(Sprite));
    int max_damage = 2 * g_nsprites;   
    g_cur_damage = calloc((size_t)max_damage, sizeof(Rect));
    g_sprite_rects = calloc((size_t)g_nsprites, sizeof(Rect));
    ctx.input_rects = calloc((size_t)g_nsprites + 1, sizeof(PlatRect));
    bool dmg_ok = g_cur_damage && g_sprite_rects && ctx.input_rects;
    for (int i = 0; i < DAMAGE_HISTORY; i++) {
        g_damage_hist[i] = calloc((size_t)max_damage, sizeof(Rect));
        if (!g_damage_hist[i]) dmg_ok = false;
    }
    if (!g_sprites || !dmg_ok) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < g_nsprites; i++) {
        Sprite *s = &g_sprites[i];
        sprite_roll_anim(s);
        sprite_init(s, mode, scale, speed, ctx.width, ctx.height);
    }
    startup_mark("scene initialized");

    if (g_ghost) {
        plat_input_region(ctx.plat, NULL, 0);
    }

    if (getenv("BALLOONS_TEST_STORM")) start_storm();
    if (getenv("BALLOONS_TEST_THUNDER")) {
        start_storm();
        lightning_strike();
    }

    struct timespec ts_prev;
    clock_gettime(CLOCK_MONOTONIC, &ts_prev);
    float t = 0.f;
    float respawn_timer = 10.0f + frandf() * 5.0f;

    ctx.need_redraw = true;
    startup_mark("ready");
    while (ctx.running) {
        if (g_signal_quit == 1 || (!ctx.running && g_quit_fade == 0.0)) {
            g_signal_quit = 2;
            ctx.running = true;
            g_quit_fade = 1.0;
        }
        
        if (g_quit_fade > 0.0 || g_startup_fade < 0.5) {
            ctx.need_redraw = true;
        }

        if (!plat_pump(ctx.plat, 0)) break;
        if (!ctx.need_redraw && !plat_frame(ctx.plat)->ready) {
            if (!plat_pump(ctx.plat, -1)) break;
            continue;
        }
        ctx.need_redraw = false;
        if (ctx.resize_pending) {
            plat_surface_resize(ctx.plat, ctx.width, ctx.height);
            ctx.resize_pending = false;
        }

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        float dt = (float)(ts_now.tv_sec - ts_prev.tv_sec) +
                   (float)(ts_now.tv_nsec - ts_prev.tv_nsec) * 1e-9f;
        ts_prev = ts_now;
        if (dt > 0.1f) dt = 0.1f;
        t += dt;
        g_time = t;

        if (g_quit_fade > 0.0) {
            g_quit_fade -= dt;
            if (g_quit_fade <= 0.0) break;
        }

        if (g_startup_fade < 0.5) {
            g_startup_fade += dt;
            if (g_startup_fade > 0.5) g_startup_fade = 0.5;
        }

        if (g_mass_pop_active) {
            g_mass_pop_timer += dt;
            bool any_alive = false;
            for (int i = 0; i < g_nsprites; i++) {
                if (!g_sprites[i].dead && !g_sprites[i].popped) {
                    if (g_mass_pop_timer >= g_sprites[i].scheduled_pop_time) {
                        Sprite *s = &g_sprites[i];
                        float cx = s->x + sprite_w(s, scale) * 0.5f;
                        float cy = s->y + sprite_h(s, scale) * (32.f / 128.f);
                        pop_sprite(s, cx, cy);
                    }
                    any_alive = true; 
                } else if (!g_sprites[i].dead) {
                    any_alive = true;
                }
            }
            if (!any_alive) {
                if (g_mass_pop_quit) {
                    ctx.running = false;
                } else {
                    g_mass_pop_active = false;
                }
            }
        } else {
            respawn_timer -= dt;
            if (respawn_timer <= 0.f) {
                respawn_timer = 10.0f + frandf() * 5.0f;
                int live_count = 0;
                for (int i = 0; i < g_nsprites; i++) {
                    if (!g_sprites[i].dead && !g_sprites[i].popped) {
                        live_count++;
                    }
                }
                if (live_count < count) {
                    for (int i = 0; i < g_nsprites; i++) {
                        if (g_sprites[i].dead) {
                            Sprite *s = &g_sprites[i];
                            sprite_roll_anim(s);
                            sprite_init(s, mode, scale, speed, ctx.width, ctx.height);
                            s->y = (float)ctx.height;
                            s->dead = false;
                            s->popped = false;
                            s->grabbed = false;
                            break;
                        }
                    }
                }
            }
        }

        breeze_update(dt);
        for (int i = 0; i < g_nsprites; i++)
            sprite_update(&g_sprites[i], mode, dt, t, scale, speed, ctx.width, ctx.height);

        bool menu_open = ringmenu_is_open(g_menu);
        int region_count = 0;
        if (menu_open) {
            ctx.input_rects[region_count++] = (PlatRect){ 0, 0, ctx.width, ctx.height };
        } else if (g_ghost) {
            ctx.input_rects[region_count++] = (PlatRect){
                ctx.width - GHOST_ICON_SIZE - GHOST_ICON_MARGIN, GHOST_ICON_MARGIN,
                GHOST_ICON_SIZE, GHOST_ICON_SIZE
            };
        } else {
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead || s->popped) continue;
                float w = sprite_w(s, scale), h = sprite_h(s, scale);
                float bx = s->x + w * (12.f / 72.f);
                float by = s->y + h * (4.f / 128.f);
                float bw = w * (48.f / 72.f);
                float bh = h * (60.f / 128.f);
                ctx.input_rects[region_count++] = (PlatRect){
                    (int)floorf(bx), (int)floorf(by), (int)ceilf(bw), (int)ceilf(bh)
                };
            }
        }
        plat_input_region(ctx.plat, ctx.input_rects, region_count);

        if (menu_open || g_menu_was_open) g_full_damage = true;
        g_menu_was_open = menu_open;

        float veil = g_storm_alpha * (1.f - g_flash01);
        float white = 0.22f * g_flash01;
        float over_a = white + veil - white * veil;
        int over_w8 = (int)(white * 255.f + 0.5f);
        int over_a8 = (int)(over_a * 255.f + 0.5f);
        if (over_w8 != g_over_w8 || over_a8 != g_over_a8) {
            g_over_w8 = over_w8;
            g_over_a8 = over_a8;
            g_full_damage = true;
        }

        bool frame_was_full_damage = g_full_damage;

        g_ncur_damage = 0;
        for (int i = 0; i < g_nsprites; i++) {
            Sprite *s = &g_sprites[i];
            Rect nr = {0, 0, 0, 0};
            if (!s->dead) {
                float x0 = s->x, y0 = s->y;
                float x1 = s->x + sprite_w(s, scale), y1 = s->y + sprite_h(s, scale);
                if (!s->popped) {
                    float sx, sy, sw2, sh2;
                    string_quad(s, &sx, &sy, &sw2, &sh2);
                    if (sx < x0) x0 = sx;
                    if (sy < y0) y0 = sy;
                    if (sx + sw2 > x1) x1 = sx + sw2;
                    if (sy + sh2 > y1) y1 = sy + sh2;
                }
                nr.x = (int)floorf(x0) - 2;
                nr.y = (int)floorf(y0) - 2;
                nr.w = (int)ceilf(x1 - x0) + 4;
                nr.h = (int)ceilf(y1 - y0) + 4;
            }
            g_sprite_rects[i] = nr;
            Rect pr = {0, 0, 0, 0};
            if (s->pr_valid) pr = (Rect){ s->pr_x, s->pr_y, s->pr_w, s->pr_h };
            if (nr.w > 0 && pr.w > 0 && rect_intersects(&nr, &pr)) {
                rect_union(&pr, &nr);
                damage_push(pr, ctx.width, ctx.height);
            } else {
                if (nr.w > 0) damage_push(nr, ctx.width, ctx.height);
                if (pr.w > 0) damage_push(pr, ctx.width, ctx.height);
            }
            if (s->dead) {
                s->pr_valid = false;  
            } else {
                s->pr_x = nr.x; s->pr_y = nr.y;
                s->pr_w = nr.w; s->pr_h = nr.h;
                s->pr_valid = true;
            }
        }

        bool repaint_full = true;
        Rect repaint = {0, 0, 0, 0};
        int age = frame_was_full_damage ? 0 : plat_buffer_age(ctx.plat);
        if (age >= 1) {
            if (age <= g_damage_hist_depth) {
                for (int d = 0; d < g_ncur_damage; d++)
                    rect_union(&repaint, &g_cur_damage[d]);
                for (int k = 0; k < age; k++)
                    for (int d = 0; d < g_damage_hist_n[k]; d++)
                        rect_union(&repaint, &g_damage_hist[k][d]);
                if ((long long)repaint.w * repaint.h <=
                    (long long)ctx.width * ctx.height / 2)
                    repaint_full = false;
            }
        }

        glViewport(0, 0, ctx.width, ctx.height);
        static bool g_ghost_was = false;
        if (g_ghost != g_ghost_was) {
            g_full_damage = true;
            g_ghost_was = g_ghost;
        }

        float fade_val = 1.0f;
        if (g_startup_fade < 0.5) {
            fade_val *= (float)(g_startup_fade / 0.5);
            repaint_full = true;
        }
        if (g_quit_fade > 0.0) {
            fade_val *= (float)(g_quit_fade / 1.0);
            repaint_full = true; 
        }
        float actual_fade = fade_val * (g_ghost ? 0.4f : 1.0f);
        if (g_fade_loc >= 0) glUniform1f(g_fade_loc, actual_fade);
        glClearColor((float)g_over_w8 / 255.f * fade_val, (float)g_over_w8 / 255.f * fade_val,
                     (float)g_over_w8 / 255.f * fade_val, (float)g_over_a8 / 255.f * fade_val);
        if (repaint_full) {
            glClear(GL_COLOR_BUFFER_BIT);
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead) continue;
                draw_sprite(s, scale, ctx.width, ctx.height);
                if (!s->popped) {
                    draw_string(s, ctx.width, ctx.height);
                }
            }
        } else if (repaint.w > 0 && repaint.h > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(repaint.x, ctx.height - (repaint.y + repaint.h), repaint.w, repaint.h);
            glClear(GL_COLOR_BUFFER_BIT);
            for (int i = 0; i < g_nsprites; i++) {
                Sprite *s = &g_sprites[i];
                if (s->dead || g_sprite_rects[i].w <= 0) continue;
                if (!rect_intersects(&repaint, &g_sprite_rects[i])) continue;
                draw_sprite(s, scale, ctx.width, ctx.height);
                if (!s->popped) {
                    draw_string(s, ctx.width, ctx.height);
                }
            }
            glDisable(GL_SCISSOR_TEST);
        }

        if (g_ghost) {
            if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
            
            float bg_size = (float)GHOST_ICON_SIZE;
            float bg_x = (float)ctx.width - bg_size - (float)GHOST_ICON_MARGIN;
            float bg_y = (float)GHOST_ICON_MARGIN;
            if (g_ghost_bg_tex) {
                draw_tex_quad(g_ghost_bg_tex, bg_x, bg_y, bg_size, bg_size, ctx.width, ctx.height, 1);
            }
            
            /* Center and size the visible balloon body, not its 186x329
             * texture rectangle. The texture deliberately has transparent
             * space below the knot where the separately-rendered string
             * would attach; that space must not make the badge look high or
             * undersized. */
            const Anim *ghost_anim = &g_anims[0];
            const float source_body_height =
                (float)(g_ghost_balloon_bounds.max_y - g_ghost_balloon_bounds.min_y + 1);
            const float body_scale =
                (GHOST_ICON_SIZE * GHOST_BALLOON_BODY_HEIGHT_FRAC) / source_body_height;
            const float bw = ghost_anim->w * body_scale;
            const float bh = ghost_anim->h * body_scale;
            const float source_center_x =
                (g_ghost_balloon_bounds.min_x + g_ghost_balloon_bounds.max_x + 1) * 0.5f;
            const float source_center_y =
                (g_ghost_balloon_bounds.min_y + g_ghost_balloon_bounds.max_y + 1) * 0.5f;
            const float bx = bg_x + bg_size * 0.5f - source_center_x * body_scale;
            const float by = bg_y + bg_size * 0.5f - source_center_y * body_scale;
            
            draw_ghost_shadow(anim_tex[0][0], bx, by, bw, bh, ctx.width, ctx.height);
            
            // Draw icon
            draw_tex_quad(anim_tex[0][0], bx, by, bw, bh, ctx.width, ctx.height, 1);
        }

        if (menu_open) {
            if (g_fade_loc >= 0) glUniform1f(g_fade_loc, 1.0f);
            int mx, my, mw, mh;
            ringmenu_rect(g_menu, &mx, &my, &mw, &mh);
            if (ringmenu_take_dirty(g_menu)) {
                if (g_menu_scratch && (size_t)mw * mh <=
                    (size_t)ringmenu_size(g_menu) * ringmenu_size(g_menu)) {
                    memset(g_menu_scratch, 0, (size_t)mw * mh * 4);
                    ringmenu_draw(g_menu, g_menu_scratch, mw, mh, mx, my);
                    for (size_t i = 0; i < (size_t)mw * mh; i++) {
                        uint32_t c = g_menu_scratch[i];
                        uint32_t a = c >> 24;
                        if (a == 0) { g_menu_scratch[i] = 0; continue; }
                        if (a == 255) continue;
                        uint32_t r = ((c & 0xFF) * a + 127) / 255;
                        uint32_t g = (((c >> 8) & 0xFF) * a + 127) / 255;
                        uint32_t b = (((c >> 16) & 0xFF) * a + 127) / 255;
                        g_menu_scratch[i] = r | (g << 8) | (b << 16) | (a << 24);
                    }
                    glBindTexture(GL_TEXTURE_2D, g_menu_tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, g_menu_scratch);
                }
            }
            float x0 = 2.f * mx / ctx.width - 1.f;
            float x1 = 2.f * (mx + mw) / ctx.width - 1.f;
            float y0 = 1.f - 2.f * my / ctx.height;
            float y1 = 1.f - 2.f * (my + mh) / ctx.height;
            GLfloat mverts[16] = {
                x0, y0, 0.f, 0.f,
                x1, y0, 1.f, 0.f,
                x0, y1, 0.f, 1.f,
                x1, y1, 1.f, 1.f,
            };
            glBindTexture(GL_TEXTURE_2D, g_menu_tex);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), mverts + 2);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        plat_frame_request(ctx.plat);
        if (plat_has_damage(ctx.plat) && !frame_was_full_damage) {
            /* No damage at all still has to name something, or it means all. */
            static const PlatRect one_pixel = { 0, 0, 1, 1 };
            if (g_ncur_damage > 0)
                plat_swap(ctx.plat, g_cur_damage, g_ncur_damage);
            else
                plat_swap(ctx.plat, &one_pixel, 1);
        } else {
            g_full_damage = false;
            plat_swap(ctx.plat, NULL, 0);
        }

        if (g_damage_hist_depth < DAMAGE_HISTORY) g_damage_hist_depth++;
        Rect *oldest = g_damage_hist[DAMAGE_HISTORY - 1];
        for (int i = DAMAGE_HISTORY - 1; i > 0; i--) {
            g_damage_hist[i] = g_damage_hist[i - 1];
            g_damage_hist_n[i] = g_damage_hist_n[i - 1];
        }
        g_damage_hist[0] = oldest;
        if (frame_was_full_damage) {
            g_damage_hist[0][0] = (Rect){0, 0, ctx.width, ctx.height};
            g_damage_hist_n[0] = 1;
        } else {
            memcpy(g_damage_hist[0], g_cur_damage,
                   (size_t)g_ncur_damage * sizeof(Rect));
            g_damage_hist_n[0] = g_ncur_damage;
        }

    }

    ringmenu_destroy(g_menu);
    free(g_menu_scratch);
    free(ctx.input_rects);
    /* The window goes first, so the audio's fade plays out over the desktop. */
    plat_close(ctx.plat);

    audio_shutdown_graceful();
    return 0;
}
