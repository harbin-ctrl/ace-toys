
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include <GLES2/gl2.h>
#include "platform.h"
#include "ringmenu.h"
#include "ghost_icon.h"
#include "toy_audio.h"
#include "toy_audio_stream.h"
#include "spraycan.h"

#ifndef PI
#define PI 3.14159265358979323846f
#endif


typedef struct {
    uint8_t r, g, b;
} PaintColor;

static const PaintColor DEFAULT_PALETTE[] = {
    {225, 30, 35},   
    {255, 130, 0},   
    {250, 215, 30},  
    {40, 200, 45},   
    {35, 90, 250},   
    {150, 40, 230},  
    {242, 242, 242}  
};
#define PALETTE_SIZE (sizeof(DEFAULT_PALETTE) / sizeof(DEFAULT_PALETTE[0]))

typedef enum {
    TOOL_SPRAY = 0,
    TOOL_SPLAT = 1,
} Tool;

#define MENU_RESULT_SPRAY  ((int)PALETTE_SIZE + 1)
#define MENU_RESULT_SPLAT  ((int)PALETTE_SIZE + 2)
#define MENU_RESULT_ERASER ((int)PALETTE_SIZE + 3)
#define MENU_RESULT_GHOST  ((int)PALETTE_SIZE + 4)
#define MENU_RESULT_CLEAR  ((int)PALETTE_SIZE + 5)
#define MENU_RESULT_QUIT   ((int)PALETTE_SIZE + 6)
#define MENU_ITEMS         ((int)PALETTE_SIZE + 6)

#define SPRAY_R_DEFAULT 70.0    // spray cloud radius in px
#define SPRAY_R_MIN 24.0
#define SPRAY_R_MAX 160.0
#define ERASER_R_DEFAULT 60.0
#define ERASER_R_MIN 18.0
#define ERASER_R_MAX 200.0
#define SIZE_STEP 1.13          // radius factor per wheel notch / [ ] press

#define SPRAY_FLOW 6.5          // density/sec at the cloud center
#define SPRAY_DROPS_PER_SEC 200.0  // speckle droplets/sec at default radius
#define SPRAY_SPITS_PER_SEC 1.0     // fat sputter droplets/sec
#define ERASE_RATE 11.0         // 1/e density decay per sec at eraser center
#define PAINT_EPS 0.004f        // below this a pixel counts as bare glass

#define LIGHT_DX 0.7071      // toward the right
#define LIGHT_DY 0.7071      // toward the bottom
#define FIELD_ISO 1.0        // metaball field value at the silhouette
#define EDGE_AA 1.4          // px: width of every anti-aliasing ramp
#define GLOSS_D_MIN 1.5      // px inside the silhouette where the gloss line starts
#define GLOSS_D_MAX 6.0      // px inside the silhouette where it ends
#define GLOSS_FACE_LO 0.25   // edge facing the light less than this: paint
#define GLOSS_FACE_HI 0.55   // more than this: full gloss (quick ramp between)

#define BALL_SUPPORT 2.5
#define BALL_Q (1.0 - 1.0 / (BALL_SUPPORT * BALL_SUPPORT))
#define BALL_AMP (FIELD_ISO / (BALL_Q * BALL_Q * BALL_Q))

#define SHADE_PAD 12
#define CHAMFER_PAD 16

#define COLOR_NONE 0xFF

#define SPLAT_S_DEFAULT 80.0
#define SPLAT_S_MIN 45.0
#define SPLAT_S_MAX 260.0
#define SPLAT_DRAG_SPACING 55.0
#define MENU_DISC_SIZE 36
#define SPLAT_SIZE_ALLOC_MAX (2.0 * SPLAT_S_MAX)
#define SPLAT_SCRATCH_SIDE ((int)(2.0 * SPLAT_SIZE_ALLOC_MAX) + 64)
#define SPLAT_SCRATCH_CAP ((size_t)SPLAT_SCRATCH_SIDE * SPLAT_SCRATCH_SIDE)

typedef struct {
    Plat *plat;

    int cursor;                 /* platform cursor id, -1 without one */
    uint32_t *cursor_pixels;    /* redrawn when the tool, size or colour changes */
    bool cursor_dirty;

    GLuint program;
    GLint pos_loc;
    GLint uv_loc;
    GLint tex_loc;
    GLint fade_loc;
    GLuint canvas_texture;

    int width;
    int height;
    int pending_width;
    int pending_height;
    bool resize_pending;
    bool running;

    uint32_t *canvas;

    float *paint;

    float *field;
    uint8_t *color_map;
    long splat_count;

    bool dirty;
    int dirty_x0;
    int dirty_y0;
    int dirty_x1;
    int dirty_y1;
    uint32_t *staging;
    float *shade_dist_u;
    float *shade_dist_c;
    float *shade_dist_s;
    size_t shade_scratch_cap;

    /* Reused by every splat; allocated once after the window is configured. */
    float *splat_own;
    float *splat_vdist;
    uint8_t *splat_vlabel;

    bool shade_pending;
    int shade_x0, shade_y0, shade_x1, shade_y1;

    RingMenu *menu;
    uint32_t *menu_scratch;
    size_t menu_scratch_cap;   
    bool menu_dirty;

    uint32_t *badge;
    uint32_t *badge_base;
    uint32_t *badge_icon;
    uint32_t *badge_full;
    bool frame_pending;

    bool paint_mode;    
    Tool tool;          
    bool eraser_mode;   
    bool pointer_down;
    int pointer_x;
    int pointer_y;
    double last_spray_x;
    double last_spray_y;
    bool has_last_spray;
    int last_splat_x;
    int last_splat_y;
    bool has_last_splat;
    double spray_radius;
    double eraser_radius;
    double splat_size;
    int palette_index;
    
    PaintColor palette[PALETTE_SIZE];
    uint32_t color_discs[PALETTE_SIZE][MENU_DISC_SIZE * MENU_DISC_SIZE];
    int color_picker_slot;
    PaintColor original_color;
    bool color_picker_locked;

#define DAMAGE_HISTORY 4
    struct {
        int x0, y0, x1, y1;
    } damage_hist[DAMAGE_HISTORY];
    int damage_hist_depth;

    struct timespec last_step;   
} PaintState;

static volatile sig_atomic_t g_quit_requested = 0;
static double g_quit_fade = 0.0;

static void handle_signal(int signum) {
    (void)signum;
    g_quit_requested = 1;
}

static double frand(void) {
    return rand() / ((double)RAND_MAX + 1.0);
}

static double ts_diff(const struct timespec *a, const struct timespec *b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) * 1e-9;
}


#define PAINT_MAX_THREADS 16
#define PARALLEL_MIN_ROWS 96

typedef void (*RowPassFn)(void *ctx, int y0, int y1, int band);

static struct {
    pthread_t workers[PAINT_MAX_THREADS - 1];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    RowPassFn fn;
    void *ctx;
    int rows;
    int generation;
    int remaining;
    int nthreads;
    bool running;
} g_pool = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static int paint_threads(void) {
    if (g_pool.nthreads == 0) {
        long cores = plat_cpu_count();
        int n = (cores > 2) ? (int)cores - 2 + 1 : 1;
        if (n > PAINT_MAX_THREADS) n = PAINT_MAX_THREADS;
        g_pool.nthreads = n;
    }
    return g_pool.nthreads;
}

static void pool_band(int rows, int band, int *y0, int *y1) {
    *y0 = (int)((long long)rows * band / g_pool.nthreads);
    *y1 = (int)((long long)rows * (band + 1) / g_pool.nthreads);
}

static void *pool_worker(void *arg) {
    int band = (int)(intptr_t)arg + 1;
    int seen = 0;
    pthread_mutex_lock(&g_pool.mu);
    for (;;) {
        while (g_pool.generation == seen) {
            pthread_cond_wait(&g_pool.cv, &g_pool.mu);
        }
        seen = g_pool.generation;
        RowPassFn fn = g_pool.fn;
        void *ctx = g_pool.ctx;
        int rows = g_pool.rows;
        pthread_mutex_unlock(&g_pool.mu);

        int y0, y1;
        pool_band(rows, band, &y0, &y1);
        if (y0 < y1) fn(ctx, y0, y1, band);

        pthread_mutex_lock(&g_pool.mu);
        if (--g_pool.remaining == 0) pthread_cond_broadcast(&g_pool.cv);
    }
    return NULL;
}

static void run_rows(RowPassFn fn, void *ctx, int rows) {
    if (rows <= 0) return;

    int nthreads = paint_threads();
    static bool tried_start = false;
    if (!g_pool.running && !tried_start && nthreads > 1) {
        tried_start = true;
        int ok = 0;
        for (int i = 0; i < nthreads - 1; i++) {
            if (pthread_create(&g_pool.workers[i], NULL, pool_worker,
                               (void *)(intptr_t)i) != 0) break;
            ok++;
        }
        g_pool.running = (ok == nthreads - 1);
        if (!g_pool.running) g_pool.nthreads = 1;
    }

    if (!g_pool.running || rows < PARALLEL_MIN_ROWS) {
        fn(ctx, 0, rows, 0);
        return;
    }

    pthread_mutex_lock(&g_pool.mu);
    g_pool.fn = fn;
    g_pool.ctx = ctx;
    g_pool.rows = rows;
    g_pool.remaining = nthreads - 1;
    g_pool.generation++;
    pthread_cond_broadcast(&g_pool.cv);
    pthread_mutex_unlock(&g_pool.mu);

    int y0, y1;
    pool_band(rows, 0, &y0, &y1);
    if (y0 < y1) fn(ctx, y0, y1, 0);

    pthread_mutex_lock(&g_pool.mu);
    while (g_pool.remaining > 0) {
        pthread_cond_wait(&g_pool.cv, &g_pool.mu);
    }
    pthread_mutex_unlock(&g_pool.mu);
}


static void mark_dirty(PaintState *st, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > st->width) x1 = st->width;
    if (y1 > st->height) y1 = st->height;
    if (x0 >= x1 || y0 >= y1) return;
    if (!st->dirty) {
        st->dirty = true;
        st->dirty_x0 = x0;
        st->dirty_y0 = y0;
        st->dirty_x1 = x1;
        st->dirty_y1 = y1;
    } else {
        if (x0 < st->dirty_x0) st->dirty_x0 = x0;
        if (y0 < st->dirty_y0) st->dirty_y0 = y0;
        if (x1 > st->dirty_x1) st->dirty_x1 = x1;
        if (y1 > st->dirty_y1) st->dirty_y1 = y1;
    }
}

static void queue_shade(PaintState *st, int x0, int y0, int x1, int y1) {
    if (!st->shade_pending) {
        st->shade_pending = true;
        st->shade_x0 = x0;
        st->shade_y0 = y0;
        st->shade_x1 = x1;
        st->shade_y1 = y1;
    } else {
        if (x0 < st->shade_x0) st->shade_x0 = x0;
        if (y0 < st->shade_y0) st->shade_y0 = y0;
        if (x1 > st->shade_x1) st->shade_x1 = x1;
        if (y1 > st->shade_y1) st->shade_y1 = y1;
    }
}


#define ALPHA_LUT_N 1024
static uint8_t g_alpha_lut[ALPHA_LUT_N];

static void alpha_lut_init(void) {
    for (int i = 0; i < ALPHA_LUT_N; i++) {
        double a = 1.0 - exp(-(i / 128.0));
        g_alpha_lut[i] = (uint8_t)(a * 255.0 + 0.5);
    }
}

static uint32_t straight_over(uint32_t src, uint32_t dst) {
    uint32_t sa = src >> 24;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = dst >> 24;
    uint32_t inv = 255 - sa;
    uint32_t oa = sa + (da * inv + 127) / 255;
    if (oa == 0) return 0;
    uint32_t out = oa << 24;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t s = (src >> shift) & 0xFF;
        uint32_t d = (dst >> shift) & 0xFF;
        uint32_t v = (s * sa * 255 + d * da * inv + oa * 127) / (oa * 255);
        if (v > 255) v = 255;
        out |= v << shift;
    }
    return out;
}


static void chamfer_sweep(float *dist, int bw, int bh) {
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            size_t bi = (size_t)by * bw + bx;
            float d = dist[bi];
            if (d == 0.0f) continue;
            if (bx > 0)                { float v = dist[bi - 1] + 1.0f;         if (v < d) d = v; }
            if (by > 0)                { float v = dist[bi - bw] + 1.0f;        if (v < d) d = v; }
            if (bx > 0 && by > 0)      { float v = dist[bi - bw - 1] + 1.4142f; if (v < d) d = v; }
            if (bx < bw - 1 && by > 0) { float v = dist[bi - bw + 1] + 1.4142f; if (v < d) d = v; }
            dist[bi] = d;
        }
    }
    for (int by = bh - 1; by >= 0; --by) {
        for (int bx = bw - 1; bx >= 0; --bx) {
            size_t bi = (size_t)by * bw + bx;
            float d = dist[bi];
            if (d == 0.0f) continue;
            if (bx < bw - 1)                { float v = dist[bi + 1] + 1.0f;         if (v < d) d = v; }
            if (by < bh - 1)                { float v = dist[bi + bw] + 1.0f;        if (v < d) d = v; }
            if (bx < bw - 1 && by < bh - 1) { float v = dist[bi + bw + 1] + 1.4142f; if (v < d) d = v; }
            if (bx > 0 && by < bh - 1)      { float v = dist[bi + bw - 1] + 1.4142f; if (v < d) d = v; }
            dist[bi] = d;
        }
    }
}

static float dist_at(const float *dist, int bw, int bh, int x, int y) {
    if (x < 0) x = 0; else if (x >= bw) x = bw - 1;
    if (y < 0) y = 0; else if (y >= bh) y = bh - 1;
    return dist[(size_t)y * bw + x];
}

typedef struct {
    uint32_t *pixels;
    const float *field;
    const uint8_t *cmap;
    const PaintColor *palette;
    int width, height;
    int cx0, cy0, bw, bh;        
    int rx0, rx1, ry0;           
    float *dist_u, *dist_c, *dist_s;
    uint8_t c;                   
    uint32_t flat_pixel;
    double base_r, base_g, base_b;
    uint8_t box_color[PAINT_MAX_THREADS];
    bool box_mixed[PAINT_MAX_THREADS];
    bool present[PAINT_MAX_THREADS][PALETTE_SIZE];
} ShadeCtx;

static void shade_seed_union_pass(void *vctx, int by0, int by1, int band) {
    ShadeCtx *k = vctx;
    uint8_t color = COLOR_NONE;
    bool mixed = false;
    for (int by = by0; by < by1; ++by) {
        const float *frow = k->field + (size_t)(k->cy0 + by) * k->width + k->cx0;
        const uint8_t *crow = k->cmap + (size_t)(k->cy0 + by) * k->width + k->cx0;
        float *drow = k->dist_u + (size_t)by * k->bw;
        for (int bx = 0; bx < k->bw; ++bx) {
            if (frow[bx] > (float)FIELD_ISO) {
                drow[bx] = 1e9f;
                if (color == COLOR_NONE) color = crow[bx];
                else if (crow[bx] != color) mixed = true;
            } else {
                drow[bx] = 0.0f;
            }
        }
    }
    k->box_color[band] = color;
    k->box_mixed[band] = mixed;
}

static void shade_scan_pass(void *vctx, int yo0, int yo1, int band) {
    ShadeCtx *k = vctx;
    for (int yo = yo0; yo < yo1; ++yo) {
        int y = k->ry0 + yo;
        for (int x = k->rx0; x <= k->rx1; ++x) {
            size_t idx = (size_t)y * k->width + x;
            if (k->field[idx] <= FIELD_ISO || k->cmap[idx] >= PALETTE_SIZE) {
                k->pixels[idx] = 0;
            } else {
                k->present[band][k->cmap[idx]] = true;
            }
        }
    }
}

static void shade_seed_color_pass(void *vctx, int by0, int by1, int band) {
    (void)band;
    ShadeCtx *k = vctx;
    for (int by = by0; by < by1; ++by) {
        const float *frow = k->field + (size_t)(k->cy0 + by) * k->width + k->cx0;
        const uint8_t *crow = k->cmap + (size_t)(k->cy0 + by) * k->width + k->cx0;
        float *drow = k->dist_c + (size_t)by * k->bw;
        for (int bx = 0; bx < k->bw; ++bx) {
            drow[bx] = (frow[bx] > (float)FIELD_ISO && crow[bx] == k->c) ? 1e9f : 0.0f;
        }
    }
}

static void shade_smooth_pass(void *vctx, int by0, int by1, int band) {
    (void)band;
    ShadeCtx *k = vctx;
    int bw = k->bw;
    for (int by = by0; by < by1; ++by) {
        const float *dc = k->dist_c;
        float *ds = k->dist_s;
        for (int bx = 0; bx < bw; ++bx) {
            size_t bi = (size_t)by * bw + bx;
            float d = dc[bi];
            if (d > (float)GLOSS_D_MAX + 6.0f ||
                bx == 0 || bx == bw - 1 || by == 0 || by == k->bh - 1) {
                ds[bi] = d;
            } else {
                const float *r0 = dc + bi - bw;
                const float *r1 = dc + bi;
                const float *r2 = dc + bi + bw;
                ds[bi] = (r0[-1] + r0[0] + r0[1] +
                          r1[-1] + r1[0] + r1[1] +
                          r2[-1] + r2[0] + r2[1]) * (1.0f / 9.0f);
            }
        }
    }
}

static void shade_color_pass(void *vctx, int yo0, int yo1, int band) {
    (void)band;
    ShadeCtx *k = vctx;
    const float *field = k->field;
    const uint8_t *cmap = k->cmap;
    int width = k->width;
    int height = k->height;
    int bw = k->bw, bh = k->bh;
    uint8_t c = k->c;

    for (int yo = yo0; yo < yo1; ++yo) {
        int y = k->ry0 + yo;
        for (int x = k->rx0; x <= k->rx1; ++x) {
            size_t idx = (size_t)y * width + x;
            double D = field[idx];
            if (D <= FIELD_ISO || cmap[idx] != c) continue;

            int bx = x - k->cx0, by = y - k->cy0;
            size_t bi = (size_t)by * bw + bx;
            double d_un = k->dist_u[bi];
            double d_col = k->dist_s[bi];

            if (d_un > GLOSS_D_MAX + 3.0 && d_col > GLOSS_D_MAX + 3.0) {
                k->pixels[idx] = k->flat_pixel;
                continue;
            }

            double fl = (x > 0)          ? field[idx - 1]     : D;
            double fr = (x < width - 1)  ? field[idx + 1]     : D;
            double fu = (y > 0)          ? field[idx - width] : D;
            double fd = (y < height - 1) ? field[idx + width] : D;
            double gx = 0.5 * (fr - fl);
            double gy = 0.5 * (fd - fu);
            double glen = sqrt(gx * gx + gy * gy);

            double d_outer = d_un;
            if (d_un < GLOSS_D_MAX + 3.0 && glen > 1e-9) {
                d_outer = (D - FIELD_ISO) / glen;
            }

            double alpha = d_outer / EDGE_AA;
            if (alpha > 1.0) alpha = 1.0;

            bool at_outer = d_col > d_un - 0.75;
            double d_edge = at_outer ? d_outer : d_col;

            double nx = 0.0, ny = 0.0;
            if (at_outer && glen > 1e-9) {
                nx = -gx / glen;
                ny = -gy / glen;
            } else {
                double gdx = dist_at(k->dist_s, bw, bh, bx + 2, by) -
                             dist_at(k->dist_s, bw, bh, bx - 2, by);
                double gdy = dist_at(k->dist_s, bw, bh, bx, by + 2) -
                             dist_at(k->dist_s, bw, bh, bx, by - 2);
                double gl = sqrt(gdx * gdx + gdy * gdy);
                if (gl > 1e-9) {
                    nx = -gdx / gl;
                    ny = -gdy / gl;
                }
            }

            double band_in = (d_edge - GLOSS_D_MIN) / EDGE_AA;
            if (band_in < 0.0) band_in = 0.0; else if (band_in > 1.0) band_in = 1.0;
            double band_out = (GLOSS_D_MAX - d_edge) / EDGE_AA;
            if (band_out < 0.0) band_out = 0.0; else if (band_out > 1.0) band_out = 1.0;

            double facing = nx * LIGHT_DX + ny * LIGHT_DY;
            double lit = (facing - GLOSS_FACE_LO) / (GLOSS_FACE_HI - GLOSS_FACE_LO);
            if (lit < 0.0) lit = 0.0; else if (lit > 1.0) lit = 1.0;

            double gloss = band_in * band_out * lit;

            double r_val = k->base_r + (255.0 - k->base_r) * gloss;
            double g_val = k->base_g + (255.0 - k->base_g) * gloss;
            double b_val = k->base_b + (255.0 - k->base_b) * gloss;

            uint8_t out_r = (uint8_t)r_val;
            uint8_t out_g = (uint8_t)g_val;
            uint8_t out_b = (uint8_t)b_val;
            uint8_t out_a = (uint8_t)(alpha * 255.0);
            k->pixels[idx] = out_r | (out_g << 8) | (out_b << 16) |
                             ((uint32_t)out_a << 24);
        }
    }
}

static void splat_shade_region(PaintState *st, int rx0, int ry0, int rx1, int ry1) {
    uint32_t *pixels = st->canvas;
    const float *field = st->field;
    const uint8_t *cmap = st->color_map;
    int width = st->width;
    int height = st->height;

    int cx0 = rx0 - CHAMFER_PAD; if (cx0 < 0) cx0 = 0;
    int cy0 = ry0 - CHAMFER_PAD; if (cy0 < 0) cy0 = 0;
    int cx1 = rx1 + CHAMFER_PAD; if (cx1 >= width) cx1 = width - 1;
    int cy1 = ry1 + CHAMFER_PAD; if (cy1 >= height) cy1 = height - 1;
    int bw = cx1 - cx0 + 1;
    int bh = cy1 - cy0 + 1;

    size_t shade_need = (size_t)bw * (size_t)bh;
    if (!st->shade_dist_u || !st->shade_dist_c || !st->shade_dist_s ||
        shade_need > st->shade_scratch_cap) return;
    float *dist_u = st->shade_dist_u;
    float *dist_c = st->shade_dist_c;
    float *dist_s = st->shade_dist_s;

    ShadeCtx k = {
        .pixels = pixels, .field = field, .cmap = cmap,
        .palette = st->palette,
        .width = width, .height = height,
        .cx0 = cx0, .cy0 = cy0, .bw = bw, .bh = bh,
        .rx0 = rx0, .rx1 = rx1, .ry0 = ry0,
        .dist_u = dist_u, .dist_c = dist_c, .dist_s = dist_s,
    };
    int nbands = paint_threads();
    for (int b = 0; b < nbands; ++b) {
        k.box_color[b] = COLOR_NONE;
        k.box_mixed[b] = false;
    }

    run_rows(shade_seed_union_pass, &k, bh);
    uint8_t box_color = COLOR_NONE;
    bool box_mixed = false;
    for (int b = 0; b < nbands; ++b) {
        if (k.box_mixed[b]) box_mixed = true;
        if (k.box_color[b] == COLOR_NONE) continue;
        if (box_color == COLOR_NONE) box_color = k.box_color[b];
        else if (k.box_color[b] != box_color) box_mixed = true;
    }
    chamfer_sweep(dist_u, bw, bh);

    memset(k.present, 0, sizeof(k.present));
    run_rows(shade_scan_pass, &k, ry1 - ry0 + 1);
    bool present[PALETTE_SIZE] = {false};
    for (int b = 0; b < nbands; ++b) {
        for (unsigned pc = 0; pc < PALETTE_SIZE; ++pc) {
            if (k.present[b][pc]) present[pc] = true;
        }
    }

    for (uint8_t c = 0; c < PALETTE_SIZE; ++c) {
        if (!present[c]) continue;

        k.c = c;
        if (!box_mixed && c == box_color) {
            k.dist_c = dist_u;
        } else {
            k.dist_c = dist_c;
            run_rows(shade_seed_color_pass, &k, bh);
            chamfer_sweep(dist_c, bw, bh);
        }
        run_rows(shade_smooth_pass, &k, bh);

        k.base_r = st->palette[c].r;
        k.base_g = st->palette[c].g;
        k.base_b = st->palette[c].b;
        k.flat_pixel = st->palette[c].r | ((uint32_t)st->palette[c].g << 8) |
                       ((uint32_t)st->palette[c].b << 16) | 0xFF000000u;
        run_rows(shade_color_pass, &k, ry1 - ry0 + 1);
    }

}


typedef struct {
    PaintState *st;
    int rx0, rx1, ry0;
    bool over_base;   
} ShadeRegionCtx;

static inline uint32_t spray_pixel(const float *row) {
    float d = row[0];
    int ai = (int)(d * 128.0f);
    uint8_t a8 = ai >= ALPHA_LUT_N ? 255 : g_alpha_lut[ai];
    float a = a8 * (1.0f / 255.0f);
    float inv = a > 0.001f ? 1.0f / a : 0.0f;
    float r = row[1] * inv;
    float g = row[2] * inv;
    float b = row[3] * inv;
    uint32_t r8 = r > 254.5f ? 255u : (uint32_t)(r + 0.5f);
    uint32_t g8 = g > 254.5f ? 255u : (uint32_t)(g + 0.5f);
    uint32_t b8 = b > 254.5f ? 255u : (uint32_t)(b + 0.5f);
    return r8 | (g8 << 8) | (b8 << 16) | ((uint32_t)a8 << 24);
}

static void shade_region_pass(void *vctx, int yo0, int yo1, int band) {
    (void)band;
    ShadeRegionCtx *k = vctx;
    PaintState *st = k->st;
    int rx0 = k->rx0;
    int rx1 = k->rx1;

    for (int yo = yo0; yo < yo1; yo++) {
        int y = k->ry0 + yo;
        const float *row = st->paint + ((size_t)y * st->width + rx0) * 4;
        uint32_t *out = st->canvas + (size_t)y * st->width + rx0;
        if (k->over_base) {
            for (int x = rx0; x <= rx1; x++, row += 4, out++) {
                if (row[0] <= PAINT_EPS) continue;
                *out = straight_over(spray_pixel(row), *out);
            }
        } else {
            for (int x = rx0; x <= rx1; x++, row += 4, out++) {
                *out = row[0] <= PAINT_EPS ? 0 : spray_pixel(row);
            }
        }
    }
}

static void shade_region(PaintState *st, int rx0, int ry0, int rx1, int ry1) {
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 >= st->width) rx1 = st->width - 1;
    if (ry1 >= st->height) ry1 = st->height - 1;
    if (rx0 > rx1 || ry0 > ry1) return;

    bool over_base = st->splat_count > 0 && st->field && st->color_map;
    if (over_base) {
        splat_shade_region(st, rx0, ry0, rx1, ry1);
    }

    ShadeRegionCtx k = { .st = st, .rx0 = rx0, .rx1 = rx1, .ry0 = ry0,
                         .over_base = over_base };
    run_rows(shade_region_pass, &k, ry1 - ry0 + 1);
    mark_dirty(st, rx0, ry0, rx1 + 1, ry1 + 1);
}

static void flush_pending_shade(PaintState *st) {
    if (!st->shade_pending) return;
    st->shade_pending = false;
    shade_region(st, st->shade_x0, st->shade_y0, st->shade_x1, st->shade_y1);
}


static inline void deposit(PaintState *st, int x, int y, float w,
                           float r, float g, float b) {
    if (x < 0 || y < 0 || x >= st->width || y >= st->height) return;
    float *p = st->paint + ((size_t)y * st->width + x) * 4;
    
    float f = 1.0f - expf(-w);
    float remain = 1.0f - f;
    
    p[0] += w;
    p[1] = p[1] * remain + r * f;
    p[2] = p[2] * remain + g * f;
    p[3] = p[3] * remain + b * f;
}

static void deposit_dot(PaintState *st, double cx, double cy, double rad,
                        float w, float r, float g, float b) {
    int x0 = (int)floor(cx - rad), x1 = (int)ceil(cx + rad);
    int y0 = (int)floor(cy - rad), y1 = (int)ceil(cy + rad);
    double inv_r2 = 1.0 / (rad * rad);
    for (int y = y0; y <= y1; y++) {
        double dy = y + 0.5 - cy;
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx;
            double q = 1.0 - (dx * dx + dy * dy) * inv_r2;
            if (q <= 0.0) continue;
            deposit(st, x, y, (float)(w * q), r, g, b);
        }
    }
}

typedef struct {
    PaintState *st;
    double cx, cy;
    double flow, inv_R2;
    int x0, x1, y0;
    float cr, cg, cb;
} SprayStampCtx;

static void spray_stamp_pass(void *vctx, int yo0, int yo1, int band) {
    (void)band;
    SprayStampCtx *k = vctx;
    PaintState *st = k->st;
    
    for (int yo = yo0; yo < yo1; yo++) {
        int y = k->y0 + yo;
        double dy = y + 0.5 - k->cy;
        float *p = st->paint + ((size_t)y * st->width + k->x0) * 4;
        for (int x = k->x0; x <= k->x1; x++, p += 4) {
            double dx = x + 0.5 - k->cx;
            double q = 1.0 - (dx * dx + dy * dy) * k->inv_R2;
            if (q <= 0.0) continue;
            float w = (float)(k->flow * q * q);
            float f = 1.0f - expf(-w);
            float remain = 1.0f - f;
            p[0] += w;
            p[1] = p[1] * remain + k->cr * f;
            p[2] = p[2] * remain + k->cg * f;
            p[3] = p[3] * remain + k->cb * f;
        }
    }
}

static void spray_stamp(PaintState *st, double px, double py, double dt) {
    if (!st->paint || st->width <= 0 || st->height <= 0) return;

    const PaintColor pc = st->palette[st->palette_index];
    const float cr = pc.r, cg = pc.g, cb = pc.b;

    double R = st->spray_radius * (0.94 + 0.12 * frand());
    double cx = px + (frand() - 0.5) * 0.12 * R;
    double cy = py + (frand() - 0.5) * 0.12 * R;

    double flow_scale = (SPRAY_R_DEFAULT * SPRAY_R_DEFAULT) / (R * R);
    double flow = SPRAY_FLOW * dt * flow_scale;
    int x0 = (int)floor(cx - R); if (x0 < 0) x0 = 0;
    int y0 = (int)floor(cy - R); if (y0 < 0) y0 = 0;
    int x1 = (int)ceil(cx + R); if (x1 >= st->width) x1 = st->width - 1;
    int y1 = (int)ceil(cy + R); if (y1 >= st->height) y1 = st->height - 1;
    double inv_R2 = 1.0 / (R * R);

    SprayStampCtx k = {
        .st = st, .cx = cx, .cy = cy, .flow = flow, .inv_R2 = inv_R2,
        .x0 = x0, .x1 = x1, .y0 = y0,
        .cr = cr, .cg = cg, .cb = cb
    };
    run_rows(spray_stamp_pass, &k, y1 - y0 + 1);

    double drops = SPRAY_DROPS_PER_SEC * dt;
    int n = (int)drops + (frand() < (drops - floor(drops)) ? 1 : 0);
    for (int i = 0; i < n; i++) {
        double u1 = frand() + 1e-9, u2 = frand();
        double rr = sqrt(-2.0 * log(u1)) * 0.55 * R;
        if (rr > 1.6 * R) rr = 1.6 * R;
        double ang = 2.0 * PI * u2;
        double u = frand();
        double drad = 0.7 + 1.6 * u * u * u;
        deposit_dot(st, px + rr * cos(ang), py + rr * sin(ang), drad,
                    (float)(0.18 + 0.5 * frand()), cr, cg, cb);
    }

    if (frand() < SPRAY_SPITS_PER_SEC * dt) {
        double rr = frand() * 1.35 * R;
        double ang = frand() * 2.0 * PI;
        deposit_dot(st, px + rr * cos(ang), py + rr * sin(ang),
                    2.2 + 2.0 * frand(), (float)(0.9 + 0.8 * frand()),
                    cr, cg, cb);
    }

    int pad = (int)(1.6 * R) + 8;
    queue_shade(st, (int)px - pad, (int)py - pad, (int)px + pad, (int)py + pad);
}

typedef struct {
    PaintState *st;
    double cx, cy, dt;
    int x0, x1, y0;
    double inv_R2;
} EraseStampCtx;

static void erase_stamp_pass(void *vctx, int yo0, int yo1, int band) {
    (void)band;
    EraseStampCtx *k = vctx;
    PaintState *st = k->st;
    bool scrub_splats = st->splat_count > 0;

    for (int yo = yo0; yo < yo1; yo++) {
        int y = k->y0 + yo;
        double dy = y + 0.5 - k->cy;
        size_t row_idx = (size_t)y * st->width + k->x0;
        float *p = st->paint + row_idx * 4;
        float *fld = st->field + row_idx;
        uint8_t *cm = st->color_map + row_idx;
        for (int x = k->x0; x <= k->x1; x++, p += 4, fld++, cm++) {
            bool has_mist = p[0] > 0.0f;
            bool has_mat = scrub_splats && *fld > 0.0f;
            if (!has_mist && !has_mat) continue;
            double dx = x + 0.5 - k->cx;
            double q = 1.0 - (dx * dx + dy * dy) * k->inv_R2;
            if (q <= 0.0) continue;
            float f = expf((float)(-ERASE_RATE * k->dt * q));
            if (has_mist) {
                float d_old = p[0];
                float d_new = d_old * f;
                float a_old = 1.0f - expf(-d_old);
                float a_new = 1.0f - expf(-d_new);
                float color_scale = a_old > 0.001f ? a_new / a_old : 0.0f;

                p[0] = d_new;
                p[1] *= color_scale;
                p[2] *= color_scale;
                p[3] *= color_scale;
                if (p[0] < PAINT_EPS) p[0] = p[1] = p[2] = p[3] = 0.0f;
            }
            if (has_mat) {
                *fld *= f;
                if (*fld <= (float)FIELD_ISO) *cm = COLOR_NONE;
                if (*fld < 0.01f) *fld = 0.0f;
            }
        }
    }
}

static void erase_stamp(PaintState *st, double cx, double cy, double dt) {
    if (!st->paint || !st->field || !st->color_map ||
        st->width <= 0 || st->height <= 0) return;

    double R = st->eraser_radius;
    int x0 = (int)floor(cx - R); if (x0 < 0) x0 = 0;
    int y0 = (int)floor(cy - R); if (y0 < 0) y0 = 0;
    int x1 = (int)ceil(cx + R); if (x1 >= st->width) x1 = st->width - 1;
    int y1 = (int)ceil(cy + R); if (y1 >= st->height) y1 = st->height - 1;
    double inv_R2 = 1.0 / (R * R);

    EraseStampCtx k = {
        .st = st, .cx = cx, .cy = cy, .dt = dt,
        .x0 = x0, .x1 = x1, .y0 = y0, .inv_R2 = inv_R2
    };
    run_rows(erase_stamp_pass, &k, y1 - y0 + 1);
    queue_shade(st, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}


typedef struct {
    const double *mx, *my, *mr;
    int count;
    int x_start, x_end, y_start;
    int bw;
    float *own;
} SplatFieldCtx;

static void splat_field_pass(void *vctx, int by0, int by1, int band) {
    (void)band;
    SplatFieldCtx *k = vctx;
    int row_lo = k->y_start + by0;
    int row_hi = k->y_start + by1 - 1;
    for (int i = 0; i < k->count; i++) {
        float bx_c = (float)k->mx[i];
        float by_c = (float)k->my[i];
        float R = (float)(BALL_SUPPORT * k->mr[i]);
        float R_sq = R * R;
        float inv_R_sq = 1.0f / R_sq;
        int iy0 = (int)ceilf(by_c - R);
        int iy1 = (int)floorf(by_c + R);
        if (iy0 < row_lo) iy0 = row_lo;
        if (iy1 > row_hi) iy1 = row_hi;
        for (int y = iy0; y <= iy1; ++y) {
            float dy = (float)y - by_c;
            float span_sq = R_sq - dy * dy;
            if (span_sq <= 0.0f) continue;
            float span = sqrtf(span_sq);
            int ix0 = (int)ceilf(bx_c - span);
            int ix1 = (int)floorf(bx_c + span);
            if (ix0 < k->x_start) ix0 = k->x_start;
            if (ix1 > k->x_end) ix1 = k->x_end;
            float *row = k->own + (size_t)(y - k->y_start) * k->bw - k->x_start;
            float dy_sq = dy * dy;
            for (int x = ix0; x <= ix1; ++x) {
                float dx = (float)x - bx_c;
                float q = 1.0f - (dx * dx + dy_sq) * inv_R_sq;
                row[x] += (float)BALL_AMP * q * q * q;
            }
        }
    }
}

typedef struct {
    PaintState *st;
    const float *own;
    int x_start, y_start;
    int bw;
    uint8_t color_index;
    int neck[PAINT_MAX_THREADS][4];
} SplatClaimCtx;

static void splat_claim_pass(void *vctx, int by0, int by1, int band) {
    SplatClaimCtx *k = vctx;
    PaintState *st = k->st;
    int width = st->width;
    int *neck = k->neck[band];
    for (int by = by0; by < by1; ++by) {
        int y = k->y_start + by;
        for (int bx = 0; bx < k->bw; ++bx) {
            float s = k->own[(size_t)by * k->bw + bx];
            if (s <= 0.0f) continue;
            size_t idx = (size_t)y * width + (k->x_start + bx);
            float f_old = st->field[idx];
            float f_new = f_old + s;
            st->field[idx] = f_new;
            bool newly_painted = false;
            if (s > (float)FIELD_ISO) {
                st->color_map[idx] = k->color_index;
                newly_painted = true;
            } else if (f_old <= (float)FIELD_ISO && f_new > (float)FIELD_ISO) {
                st->color_map[idx] = COLOR_NONE;   
                newly_painted = true;
                if (bx < neck[0]) neck[0] = bx;
                if (by < neck[1]) neck[1] = by;
                if (bx > neck[2]) neck[2] = bx;
                if (by > neck[3]) neck[3] = by;
            }
            if (newly_painted) {
                float *p = st->paint + idx * 4;
                p[0] = p[1] = p[2] = p[3] = 0.0f;
            }
        }
    }
}

static void draw_splat(PaintState *st, int cx, int cy, int size, uint8_t color_index) {
    int width = st->width;
    int height = st->height;

    double mx[30];
    double my[30];
    double mr[30];
    int count = 0;

    mx[count] = cx;
    my[count] = cy;
    mr[count] = size * 0.30;
    count++;

    int num_arms = 5 + rand() % 4;
    for (int j = 0; j < num_arms && count < 28; j++) {
        double angle = j * (2.0 * PI / num_arms) + 0.35 * sin(j * 1.7) +
                       ((rand() % 100) / 100.0) * 0.25;
        double dist = size * (0.30 + 0.08 * cos(j * 2.1) +
                              ((rand() % 100) / 100.0) * 0.10);
        mx[count] = cx + dist * cos(angle);
        my[count] = cy + dist * sin(angle);
        mr[count] = size * (0.13 + 0.03 * sin(j * 1.3) +
                            ((rand() % 100) / 100.0) * 0.02);
        count++;

        double sat_dist = dist + size * (0.22 + ((rand() % 100) / 100.0) * 0.08);
        mx[count] = cx + sat_dist * cos(angle + 0.18);
        my[count] = cy + sat_dist * sin(angle + 0.18);
        mr[count] = size * (0.030 + ((rand() % 100) / 100.0) * 0.015);
        count++;
    }

    double first_r = BALL_SUPPORT * mr[0];
    double fx0 = mx[0] - first_r, fy0 = my[0] - first_r;
    double fx1 = mx[0] + first_r, fy1 = my[0] + first_r;
    for (int i = 1; i < count; i++) {
        double R = BALL_SUPPORT * mr[i];
        if (mx[i] - R < fx0) fx0 = mx[i] - R;
        if (my[i] - R < fy0) fy0 = my[i] - R;
        if (mx[i] + R > fx1) fx1 = mx[i] + R;
        if (my[i] + R > fy1) fy1 = my[i] + R;
    }
    int x_start = (int)floor(fx0);
    int y_start = (int)floor(fy0);
    int x_end = (int)ceil(fx1);
    int y_end = (int)ceil(fy1);
    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;
    if (x_end >= width) x_end = width - 1;
    if (y_end >= height) y_end = height - 1;
    if (x_start > x_end || y_start > y_end) return;

    int bw = x_end - x_start + 1;
    int bh = y_end - y_start + 1;
    size_t scratch_need = (size_t)bw * (size_t)bh;
    if (!st->splat_own || !st->splat_vdist || !st->splat_vlabel ||
        scratch_need > SPLAT_SCRATCH_CAP) return;
    float *own = st->splat_own;
    float *vdist = st->splat_vdist;
    uint8_t *vlabel = st->splat_vlabel;
    memset(own, 0, scratch_need * sizeof(*own));

    SplatFieldCtx fctx = {
        .mx = mx, .my = my, .mr = mr, .count = count,
        .x_start = x_start, .x_end = x_end, .y_start = y_start,
        .bw = bw, .own = own,
    };
    run_rows(splat_field_pass, &fctx, bh);

    SplatClaimCtx cctx = {
        .st = st, .own = own,
        .x_start = x_start, .y_start = y_start,
        .bw = bw, .color_index = color_index,
    };
    int nbands = paint_threads();
    for (int b = 0; b < nbands; ++b) {
        cctx.neck[b][0] = bw;
        cctx.neck[b][1] = bh;
        cctx.neck[b][2] = -1;
        cctx.neck[b][3] = -1;
    }
    run_rows(splat_claim_pass, &cctx, bh);
    int neck_x0 = bw, neck_y0 = bh, neck_x1 = -1, neck_y1 = -1;
    for (int b = 0; b < nbands; ++b) {
        if (cctx.neck[b][0] < neck_x0) neck_x0 = cctx.neck[b][0];
        if (cctx.neck[b][1] < neck_y0) neck_y0 = cctx.neck[b][1];
        if (cctx.neck[b][2] > neck_x1) neck_x1 = cctx.neck[b][2];
        if (cctx.neck[b][3] > neck_y1) neck_y1 = cctx.neck[b][3];
    }

    if (neck_x1 >= 0) {
        int vx0 = x_start + neck_x0 - 16; if (vx0 < 0) vx0 = 0;
        int vy0 = y_start + neck_y0 - 16; if (vy0 < 0) vy0 = 0;
        int vx1 = x_start + neck_x1 + 16; if (vx1 >= width) vx1 = width - 1;
        int vy1 = y_start + neck_y1 + 16; if (vy1 >= height) vy1 = height - 1;
        int vbw = vx1 - vx0 + 1;
        int vbh = vy1 - vy0 + 1;

        size_t vneed = (size_t)vbw * (size_t)vbh;
        if (vneed <= SPLAT_SCRATCH_CAP) {
            for (int by = 0; by < vbh; ++by) {
                for (int bx = 0; bx < vbw; ++bx) {
                    size_t idx = (size_t)(vy0 + by) * width + (vx0 + bx);
                    size_t bi = (size_t)by * vbw + bx;
                    if (st->field[idx] > (float)FIELD_ISO &&
                        st->color_map[idx] < PALETTE_SIZE) {
                        vdist[bi] = 0.0f;
                        vlabel[bi] = st->color_map[idx];
                    } else {
                        vdist[bi] = 1e9f;
                        vlabel[bi] = COLOR_NONE;
                    }
                }
            }
            for (int by = 0; by < vbh; ++by) {
                for (int bx = 0; bx < vbw; ++bx) {
                    size_t bi = (size_t)by * vbw + bx;
                    float d = vdist[bi];
                    if (d == 0.0f) continue;
                    if (bx > 0 && vdist[bi - 1] + 1.0f < d)                       { d = vdist[bi - 1] + 1.0f;         vlabel[bi] = vlabel[bi - 1]; }
                    if (by > 0 && vdist[bi - vbw] + 1.0f < d)                     { d = vdist[bi - vbw] + 1.0f;       vlabel[bi] = vlabel[bi - vbw]; }
                    if (bx > 0 && by > 0 && vdist[bi - vbw - 1] + 1.4142f < d)    { d = vdist[bi - vbw - 1] + 1.4142f; vlabel[bi] = vlabel[bi - vbw - 1]; }
                    if (bx < vbw - 1 && by > 0 && vdist[bi - vbw + 1] + 1.4142f < d) { d = vdist[bi - vbw + 1] + 1.4142f; vlabel[bi] = vlabel[bi - vbw + 1]; }
                    vdist[bi] = d;
                }
            }
            for (int by = vbh - 1; by >= 0; --by) {
                for (int bx = vbw - 1; bx >= 0; --bx) {
                    size_t bi = (size_t)by * vbw + bx;
                    float d = vdist[bi];
                    if (d == 0.0f) continue;
                    if (bx < vbw - 1 && vdist[bi + 1] + 1.0f < d)                       { d = vdist[bi + 1] + 1.0f;         vlabel[bi] = vlabel[bi + 1]; }
                    if (by < vbh - 1 && vdist[bi + vbw] + 1.0f < d)                     { d = vdist[bi + vbw] + 1.0f;       vlabel[bi] = vlabel[bi + vbw]; }
                    if (bx < vbw - 1 && by < vbh - 1 && vdist[bi + vbw + 1] + 1.4142f < d) { d = vdist[bi + vbw + 1] + 1.4142f; vlabel[bi] = vlabel[bi + vbw + 1]; }
                    if (bx > 0 && by < vbh - 1 && vdist[bi + vbw - 1] + 1.4142f < d)    { d = vdist[bi + vbw - 1] + 1.4142f; vlabel[bi] = vlabel[bi + vbw - 1]; }
                    vdist[bi] = d;
                }
            }
            for (int by = 0; by < vbh; ++by) {
                for (int bx = 0; bx < vbw; ++bx) {
                    size_t idx = (size_t)(vy0 + by) * width + (vx0 + bx);
                    if (st->field[idx] > (float)FIELD_ISO &&
                        st->color_map[idx] == COLOR_NONE) {
                        uint8_t l = vlabel[(size_t)by * vbw + bx];
                        st->color_map[idx] = (l < PALETTE_SIZE) ? l : color_index;
                    }
                }
            }
        }
    }

    st->splat_count++;

    queue_shade(st, x_start - SHADE_PAD, y_start - SHADE_PAD,
                x_end + SHADE_PAD, y_end + SHADE_PAD);
}

static void splat_at(PaintState *st, int x, int y) {
    if (!st->canvas || !st->field || !st->color_map ||
        st->width <= 0 || st->height <= 0) return;
    int size = (int)(st->splat_size * (1.0 + frand()));
    draw_splat(st, x, y, size, (uint8_t)st->palette_index);
    st->last_splat_x = x;
    st->last_splat_y = y;
    st->has_last_splat = true;
}


static void clear_canvas(PaintState *st) {
    if (!st->canvas || st->width <= 0 || st->height <= 0) return;
    memset(st->canvas, 0, (size_t)st->width * (size_t)st->height * 4);
    if (st->paint) {
        memset(st->paint, 0,
               (size_t)st->width * (size_t)st->height * 4 * sizeof(float));
    }
    if (st->field) {
        memset(st->field, 0,
               (size_t)st->width * (size_t)st->height * sizeof(float));
    }
    if (st->color_map) {
        memset(st->color_map, COLOR_NONE, (size_t)st->width * (size_t)st->height);
    }
    st->splat_count = 0;
    st->has_last_splat = false;
    st->shade_pending = false;   
    mark_dirty(st, 0, 0, st->width, st->height);
}


typedef struct {
    ToyAudioStream *stream;
    pthread_mutex_t mu;
    ToyHiss synth;
    bool spraying;
    float size;
    bool running;
} Hiss;

static Hiss g_hiss = { .mu = PTHREAD_MUTEX_INITIALIZER };

#define HISS_RATE TOY_AUDIO_SAMPLE_RATE

static void hiss_render(void *userdata, float *output,
                        uint32_t nframes, uint32_t channels) {
    Hiss *h = userdata;
    (void)channels;
    if (!h || !output || nframes == 0) return;

    pthread_mutex_lock(&h->mu);
    toy_hiss_render(&h->synth, output, (int)nframes,
                    h->spraying, h->size, HISS_RATE);
    pthread_mutex_unlock(&h->mu);
}

static bool hiss_start(void) {
    toy_hiss_init(&g_hiss.synth);
    ToyAudioStreamConfig stream_config = {
        .name = "splat-hiss",
        .description = "Splat Spray",
        .sample_rate = HISS_RATE,
        .channels = 1,
        .render = hiss_render,
        .userdata = &g_hiss,
    };
    g_hiss.stream = toy_audio_stream_start(&stream_config);
    if (!g_hiss.stream) {
        fprintf(stderr, "Splat: failed to start audio; quitting.\n");
        return false;
    }
    g_hiss.running = true;
    return true;
}

static void hiss_set(bool spraying, float size01) {
    if (!g_hiss.running) return;
    pthread_mutex_lock(&g_hiss.mu);
    g_hiss.spraying = spraying;
    g_hiss.size = size01;
    pthread_mutex_unlock(&g_hiss.mu);
}

static void hiss_stop(void) {
    if (!g_hiss.running) return;
    toy_audio_stream_stop(g_hiss.stream);
    g_hiss.stream = NULL;
    g_hiss.running = false;
}


#define CURSOR_BUF 448
#define CURSOR_HOT (CURSOR_BUF / 2)

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static inline void argb_blend(uint32_t *px, int stride, int x, int y,
                              double r, double g, double b, double a) {
    if (a <= 0.0) return;
    if (a > 1.0) a = 1.0;
    uint32_t *dst = px + (size_t)y * stride + x;
    uint32_t d = *dst;
    double da = ((d >> 24) & 0xFF) / 255.0;
    double dr = ((d >> 16) & 0xFF) / 255.0;
    double dg = ((d >> 8) & 0xFF) / 255.0;
    double db = (d & 0xFF) / 255.0;
    double oa = a + da * (1.0 - a);
    double onr = (r / 255.0) * a + dr * (1.0 - a);
    double ong = (g / 255.0) * a + dg * (1.0 - a);
    double onb = (b / 255.0) * a + db * (1.0 - a);
    *dst = ((uint32_t)(oa * 255.0 + 0.5) << 24) |
           ((uint32_t)(onr * 255.0 + 0.5) << 16) |
           ((uint32_t)(ong * 255.0 + 0.5) << 8) |
           (uint32_t)(onb * 255.0 + 0.5);
}

static inline void cursor_blend(uint32_t *px, int x, int y,
                                double r, double g, double b, double a) {
    argb_blend(px, CURSOR_BUF, x, y, r, g, b, a);
}


#define BRUSH_LEN 34.0      // bristle head length, tip to round base
#define BRUSH_W 10.0        // head max half-width
#define HANDLE_LEN (1.5 * BRUSH_LEN)
#define HANDLE_W 8.0        // handle half-width
#define BRUSH_PAINT_LEN 16.0 // how far up the head the paint dip reaches
#define BRUSH_AXIS_X 0.70710678   // brush axis unit vector, tip -> handle
#define BRUSH_AXIS_Y 0.70710678

#define BRUSH_LIGHT_X 0.94
#define BRUSH_LIGHT_Y 0.34

static double bristle_sdf(double t, double s) {
    if (t < 0.0) return sqrt(t * t + s * s);   
    if (t > BRUSH_LEN) {                       
        double dt = t - BRUSH_LEN;
        return sqrt(dt * dt + s * s) - BRUSH_W;
    }
    double h = BRUSH_W * pow(sin((t / BRUSH_LEN) * (PI / 2.0)), 0.75);
    return fabs(s) - h;
}

static double handle_sdf(double t, double s) {
    double a = HANDLE_LEN / 2.0;
    double dt = (t - (BRUSH_LEN + a - 3.0)) / a;
    double ds = s / HANDLE_W;
    return (sqrt(dt * dt + ds * ds) - 1.0) * HANDLE_W;
}

static void render_brush(uint32_t *px, int stride, int buf_w, int buf_h,
                         double tip_x, double tip_y, double k,
                         PaintColor paint) {
    double reach = (BRUSH_LEN + HANDLE_LEN + 4.0) * k;
    double halfw = (BRUSH_W + 3.0) * k;
    int c0x = (int)floor(tip_x - halfw * BRUSH_AXIS_Y) - 2;
    int c0y = (int)floor(tip_y - halfw * BRUSH_AXIS_X) - 2;
    int c1x = (int)ceil(tip_x + reach * BRUSH_AXIS_X + halfw * BRUSH_AXIS_Y) + 2;
    int c1y = (int)ceil(tip_y + reach * BRUSH_AXIS_Y + halfw * BRUSH_AXIS_X) + 2;
    if (c0x < 0) c0x = 0;
    if (c0y < 0) c0y = 0;
    if (c1x > buf_w - 1) c1x = buf_w - 1;
    if (c1y > buf_h - 1) c1y = buf_h - 1;

    for (int y = c0y; y <= c1y; y++) {
        double ry = (y + 0.5) - tip_y;
        for (int x = c0x; x <= c1x; x++) {
            double rx = (x + 0.5) - tip_x;
            double t = (rx * BRUSH_AXIS_X + ry * BRUSH_AXIS_Y) / k;
            double s = (-rx * BRUSH_AXIS_Y + ry * BRUSH_AXIS_X) / k;

            double d_b = bristle_sdf(t, s);
            double d_h = handle_sdf(t, s);
            double d = d_b < d_h ? d_b : d_h;

            double cov = (0.5 - d) / 1.0;   
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;

            double fr, fg, fb;
            double d_sel;   

            if (d_b < 0.0) {
                d_sel = d_b;

                double strand = 0.94 + 0.06 * sin(s * 1.9 + t * 0.25);
                fr = 182.0 * strand;
                fg = 182.0 * strand;
                fb = 188.0 * strand;

                double dip_edge = BRUSH_PAINT_LEN + 2.5 * (s / BRUSH_W) * (s / BRUSH_W);
                double pm = dip_edge - t;   
                if (pm < 0.0) pm = 0.0; else if (pm > 1.0) pm = 1.0;

                if (pm > 0.0) {
                    double gt = bristle_sdf(t + 0.5, s) - bristle_sdf(t - 0.5, s);
                    double gs = bristle_sdf(t, s + 0.5) - bristle_sdf(t, s - 0.5);
                    double gx = gt * BRUSH_AXIS_X - gs * BRUSH_AXIS_Y;
                    double gy = gt * BRUSH_AXIS_Y + gs * BRUSH_AXIS_X;
                    double glen = sqrt(gx * gx + gy * gy);
                    double facing = glen > 1e-9
                        ? (gx / glen) * BRUSH_LIGHT_X + (gy / glen) * BRUSH_LIGHT_Y : 0.0;

                    double depth = -d_b;
                    double band_in = (depth - 1.3) / 0.8;
                    if (band_in < 0.0) band_in = 0.0; else if (band_in > 1.0) band_in = 1.0;
                    double band_out = (4.0 - depth) / 0.8;
                    if (band_out < 0.0) band_out = 0.0; else if (band_out > 1.0) band_out = 1.0;
                    double lit = (facing - GLOSS_FACE_LO) / (GLOSS_FACE_HI - GLOSS_FACE_LO);
                    if (lit < 0.0) lit = 0.0; else if (lit > 1.0) lit = 1.0;
                    double gloss = band_in * band_out * lit;

                    double gt2 = t - 11.5, gs2 = s - 2.6;
                    double ax = 4.0, ay = 0.8;   
                    double seg = (gt2 * ax + gs2 * ay) / (ax * ax + ay * ay);
                    if (seg < 0.0) seg = 0.0; else if (seg > 1.0) seg = 1.0;
                    double qx = gt2 - seg * ax, qy = gs2 - seg * ay;
                    double glint = (1.1 - sqrt(qx * qx + qy * qy)) / 0.7;
                    if (glint < 0.0) glint = 0.0; else if (glint > 1.0) glint = 1.0;
                    gloss += (1.0 - gloss) * glint * 0.9;

                    double pr = paint.r + (255.0 - paint.r) * gloss;
                    double pg = paint.g + (255.0 - paint.g) * gloss;
                    double pb = paint.b + (255.0 - paint.b) * gloss;
                    fr = pr * pm + fr * (1.0 - pm);
                    fg = pg * pm + fg * (1.0 - pm);
                    fb = pb * pm + fb * (1.0 - pm);
                }
            } else {
                d_sel = d_h;

                fr = 196.0; fg = 120.0; fb = 58.0;

                double gt = handle_sdf(t + 0.5, s) - handle_sdf(t - 0.5, s);
                double gs = handle_sdf(t, s + 0.5) - handle_sdf(t, s - 0.5);
                double gx = gt * BRUSH_AXIS_X - gs * BRUSH_AXIS_Y;
                double gy = gt * BRUSH_AXIS_Y + gs * BRUSH_AXIS_X;
                double glen = sqrt(gx * gx + gy * gy);
                double facing = glen > 1e-9
                    ? (gx / glen) * BRUSH_LIGHT_X + (gy / glen) * BRUSH_LIGHT_Y : 0.0;

                double depth = -d_h;
                double band_in = (depth - 1.0) / 0.8;
                if (band_in < 0.0) band_in = 0.0; else if (band_in > 1.0) band_in = 1.0;
                double band_out = (3.2 - depth) / 0.8;
                if (band_out < 0.0) band_out = 0.0; else if (band_out > 1.0) band_out = 1.0;
                double lit = (facing - GLOSS_FACE_LO) / (GLOSS_FACE_HI - GLOSS_FACE_LO);
                if (lit < 0.0) lit = 0.0; else if (lit > 1.0) lit = 1.0;
                double rim = band_in * band_out * lit * 0.5;

                fr += (255.0 - fr) * rim;
                fg += (255.0 - fg) * rim;
                fb += (255.0 - fb) * rim;

            }

            double ot = (d_sel + 1.2) / 0.8;
            if (ot < 0.0) ot = 0.0; else if (ot > 1.0) ot = 1.0;
            if (d_b > 0.0 && d_h < 0.0) {
                double jt = (1.2 - d_b) / 0.8;
                if (jt < 0.0) jt = 0.0; else if (jt > 1.0) jt = 1.0;
                if (jt > ot) ot = jt;
            }
            fr = fr * (1.0 - ot) + 42.0 * ot;
            fg = fg * (1.0 - ot) + 40.0 * ot;
            fb = fb * (1.0 - ot) + 46.0 * ot;

            argb_blend(px, stride, x, y, fr, fg, fb, cov);
        }
    }
}

static double can_scale(const PaintState *st) {
    return 0.62 + 0.90 * (st->spray_radius - SPRAY_R_MIN) /
                        (SPRAY_R_MAX - SPRAY_R_MIN);
}

static void render_cursor(PaintState *st, uint32_t *px) {
    memset(px, 0, (size_t)CURSOR_BUF * CURSOR_BUF * 4);

    double rr = st->eraser_mode ? st->eraser_radius
              : st->tool == TOOL_SPLAT ? st->splat_size * 0.8
                                       : st->spray_radius;
    if (rr > CURSOR_HOT - 4) rr = CURSOR_HOT - 4;

    int b0 = CURSOR_HOT - (int)rr - 4;
    int b1 = CURSOR_HOT + (int)rr + 4;
    if (b0 < 0) b0 = 0;
    if (b1 > CURSOR_BUF - 1) b1 = CURSOR_BUF - 1;
    for (int y = b0; y <= b1; y++) {
        double dy = y + 0.5 - CURSOR_HOT;
        for (int x = b0; x <= b1; x++) {
            double dx = x + 0.5 - CURSOR_HOT;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist > rr + 3.0) continue;
            if (st->eraser_mode && dist < rr - 0.5) {
                cursor_blend(px, x, y, 255, 150, 170, 0.10);
            }
            double a_dark = clamp01(1.5 - fabs(dist - (rr + 0.8))) * 0.55;
            if (a_dark > 0.0) cursor_blend(px, x, y, 25, 25, 30, a_dark);
            double a_lite = clamp01(1.5 - fabs(dist - (rr - 0.8))) * 0.60;
            if (a_lite > 0.0) cursor_blend(px, x, y, 255, 255, 255, a_lite);
        }
    }

    if (!st->eraser_mode && st->tool == TOOL_SPLAT) {
        int preview_idx = st->palette_index;
        if (st->color_picker_slot >= 0) preview_idx = st->color_picker_slot;
        double k = 0.85 + 1.15 * (st->splat_size - SPLAT_S_MIN) /
                                 (SPLAT_S_MAX - SPLAT_S_MIN);
        render_brush(px, CURSOR_BUF, CURSOR_BUF, CURSOR_BUF,
                     CURSOR_HOT, CURSOR_HOT, k, st->palette[preview_idx]);
    } else if (!st->eraser_mode) {
        double k = can_scale(st);
        int preview_idx = st->palette_index;
        if (st->color_picker_slot >= 0) preview_idx = st->color_picker_slot;
        const PaintColor pc = st->palette[preview_idx];
        double tmin = SPRAYCAN_T_MIN * k, tmax = SPRAYCAN_T_MAX * k;
        double smax = SPRAYCAN_S_MAX * k;
        int c0 = CURSOR_HOT +
                 (int)floor(tmin * SPRAYCAN_AXIS_X - smax * SPRAYCAN_AXIS_Y) - 2;
        int c1 = CURSOR_HOT +
                 (int)ceil(tmax * SPRAYCAN_AXIS_X + smax * SPRAYCAN_AXIS_Y) + 2;
        if (c0 < 0) c0 = 0;
        if (c1 > CURSOR_BUF - 1) c1 = CURSOR_BUF - 1;
        for (int y = c0; y <= c1; y++) {
            double ry = y + 0.5 - CURSOR_HOT;
            for (int x = c0; x <= c1; x++) {
                double rx = x + 0.5 - CURSOR_HOT;
                double t = rx * SPRAYCAN_AXIS_X + ry * SPRAYCAN_AXIS_Y;
                double s = -rx * SPRAYCAN_AXIS_Y + ry * SPRAYCAN_AXIS_X;
                if (t < tmin || t > tmax || fabs(s) > smax) continue;
                double r, g, b, a;
                spraycan_sample(t / k, s / k, pc.r, pc.g, pc.b, 1,
                                &r, &g, &b, &a);
                cursor_blend(px, x, y, r, g, b, a);
            }
        }
    } else {
        double k = 0.55 + 0.75 * (st->eraser_radius - ERASER_R_MIN) /
                                (ERASER_R_MAX - ERASER_R_MIN);
        double tmin = 8.0 * k, tmax = 50.0 * k, smax = 12.0 * k;
        int c0 = CURSOR_HOT +
                 (int)floor(tmin * SPRAYCAN_AXIS_X - smax * SPRAYCAN_AXIS_Y) - 2;
        int c1 = CURSOR_HOT +
                 (int)ceil(tmax * SPRAYCAN_AXIS_X + smax * SPRAYCAN_AXIS_Y) + 2;
        if (c0 < 0) c0 = 0;
        if (c1 > CURSOR_BUF - 1) c1 = CURSOR_BUF - 1;
        for (int y = c0; y <= c1; y++) {
            double ry = y + 0.5 - CURSOR_HOT;
            for (int x = c0; x <= c1; x++) {
                double rx = x + 0.5 - CURSOR_HOT;
                double t = (rx * SPRAYCAN_AXIS_X + ry * SPRAYCAN_AXIS_Y) / k;
                double s = (-rx * SPRAYCAN_AXIS_Y + ry * SPRAYCAN_AXIS_X) / k;
                double d = spraycan_rrect_(t, s, 10.0, 48.0, 9.0, 3.0);
                double cov = clamp01(0.5 - d);
                if (cov <= 0.0) continue;
                double fr, fg, fb;
                if (t < 26.0) {
                    fr = 247; fg = 143; fb = 167;   
                } else {
                    fr = 178; fg = 192; fb = 214;   
                    if (t < 28.0) { fr *= 0.85; fg *= 0.85; fb *= 0.85; }
                }
                double streak = exp(-((s + 4.5) * (s + 4.5)) / (2.4 * 2.4)) * 0.35;
                fr += (255.0 - fr) * streak;
                fg += (255.0 - fg) * streak;
                fb += (255.0 - fb) * streak;
                double ot = clamp01((d + 1.3) / 0.9);
                fr = fr * (1.0 - ot) + 42.0 * ot;
                fg = fg * (1.0 - ot) + 40.0 * ot;
                fb = fb * (1.0 - ot) + 46.0 * ot;
                cursor_blend(px, x, y, fr, fg, fb, cov);
            }
        }
    }
}

static bool create_cursor(PaintState *st) {
    st->cursor = -1;
    st->cursor_pixels = malloc((size_t)CURSOR_BUF * CURSOR_BUF * sizeof(*st->cursor_pixels));
    if (!st->cursor_pixels) return false;

    render_cursor(st, st->cursor_pixels);
    st->cursor = plat_cursor_create(st->plat, st->cursor_pixels, CURSOR_BUF, 1,
                                    CURSOR_HOT, CURSOR_HOT);
    return st->cursor >= 0;
}

static void update_cursor(PaintState *st) {
    if (st->cursor < 0) return;
    render_cursor(st, st->cursor_pixels);
    plat_cursor_update(st->plat, st->cursor, st->cursor_pixels);
}


#define BADGE_SIZE 192
#define BADGE_MARGIN GHOST_ICON_MARGIN

static void badge_pos(const PaintState *st, int *x, int *y) {
    *x = st->width - BADGE_SIZE - BADGE_MARGIN;
    *y = BADGE_MARGIN;
}

static void build_badge(PaintState *st) {
    if (!st->badge || !st->badge_base || !st->badge_icon || !st->badge_full) return;
    memcpy(st->badge, st->badge_base, (size_t)BADGE_SIZE * BADGE_SIZE * 4);
    int preview_idx = st->palette_index;
    if (st->color_picker_slot >= 0) preview_idx = st->color_picker_slot;
    const PaintColor pc = st->palette[preview_idx];
    
    uint32_t *icon = st->badge_icon;
    memset(icon, 0, (size_t)BADGE_SIZE * BADGE_SIZE * 4);

    if (st->tool == TOOL_SPLAT) {
        int full_n = BADGE_SIZE * 2;
        uint32_t *full = st->badge_full;
        memset(full, 0, (size_t)full_n * full_n * 4);
        {
            double k = (double)full_n / 120.0;   
            double tip = (double)full_n / 2.0 - 29.0 * k;
            render_brush(full, full_n, full_n, full_n, tip, tip, k, pc);
            for (int y = 0; y < BADGE_SIZE; y++) {
                for (int x = 0; x < BADGE_SIZE; x++) {
                    uint32_t a = 0, r = 0, g = 0, b = 0;
                    for (int sy = 0; sy < 2; sy++) {
                        for (int sx = 0; sx < 2; sx++) {
                            uint32_t p = full[(size_t)(y * 2 + sy) * full_n + x * 2 + sx];
                            a += p >> 24;
                            r += (p >> 16) & 0xFF;
                            g += (p >> 8) & 0xFF;
                            b += p & 0xFF;
                        }
                    }
                    a = (a + 2) / 4; r = (r + 2) / 4; g = (g + 2) / 4; b = (b + 2) / 4;
                    uint32_t out = 0;
                    if (a) {
                        r = r * 255 / a; if (r > 255) r = 255;
                        g = g * 255 / a; if (g > 255) g = 255;
                        b = b * 255 / a; if (b > 255) b = 255;
                        out = r | (g << 8) | (b << 16) | (a << 24);
                    }
                    icon[(size_t)y * BADGE_SIZE + x] = out;
                }
            }
        }
    } else {
        const double k = 0.68;       
        const double hot = 29.0;      
        const double step = 106.0 / (BADGE_SIZE * 2);

        for (int y = 0; y < BADGE_SIZE; y++) {
            for (int x = 0; x < BADGE_SIZE; x++) {
                double ar = 0, ag = 0, ab = 0, aa = 0;
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        double rx = (x * 2 + sx + 0.5) * step - hot;
                        double ry = (y * 2 + sy + 0.5) * step - hot;
                        double t = rx * SPRAYCAN_AXIS_X + ry * SPRAYCAN_AXIS_Y;
                        double s = -rx * SPRAYCAN_AXIS_Y + ry * SPRAYCAN_AXIS_X;
                        double r, g, b, a;
                        spraycan_sample(t / k, s / k, pc.r, pc.g, pc.b, 1, &r, &g, &b, &a);
                        ar += r * a; ag += g * a; ab += b * a; aa += a;
                    }
                }
                uint32_t out = 0;
                if (aa > 0.0) {
                    uint32_t r8 = (uint32_t)(ar / aa + 0.5);
                    uint32_t g8 = (uint32_t)(ag / aa + 0.5);
                    uint32_t b8 = (uint32_t)(ab / aa + 0.5);
                    uint32_t a8 = (uint32_t)(aa / 4.0 * 255.0 + 0.5);
                    out = r8 | (g8 << 8) | (b8 << 16) | (a8 << 24);
                }
                icon[(size_t)y * BADGE_SIZE + x] = out;
            }
        }
    }
    
    /* The old hard duplicate looked like a second tool.  Use the shared
     * lower-right, 40%-black soft shadow that matches Poingo's lighting. */
    ghost_icon_composite_shadow(st->badge, icon, BADGE_SIZE, BADGE_SIZE);
    // Icon pass
    for (int y = 0; y < BADGE_SIZE; y++) {
        for (int x = 0; x < BADGE_SIZE; x++) {
            uint32_t px = icon[y * BADGE_SIZE + x];
            if (px >> 24) {
                st->badge[y * BADGE_SIZE + x] = straight_over(px, st->badge[y * BADGE_SIZE + x]);
            }
        }
    }
}

static void update_input_region(PaintState *st);

static void set_paint_mode(PaintState *st, bool on) {
    if (st->paint_mode == on) return;
    st->paint_mode = on;
    st->pointer_down = false;
    if (!on) {
        build_badge(st);
    } else if (st->badge) {
        int bx, by;
        badge_pos(st, &bx, &by);
        mark_dirty(st, bx, by, bx + BADGE_SIZE, by + BADGE_SIZE);
    }
    st->frame_pending = true;   
    mark_dirty(st, 0, 0, st->width, st->height);
    update_input_region(st);
    printf("Splat: %s\n", on ? "paint mode"
                             : "click-through mode (click the tool badge in "
                               "the top-right corner to paint again)");
    fflush(stdout);
}



static bool make_color_disc(PaintState *st, PaintColor c, int slot) {
    return ringmenu_color_drop_into(st->color_discs[slot],
                                    (size_t)MENU_DISC_SIZE * MENU_DISC_SIZE,
                                    c.r, c.g, c.b, slot, MENU_ITEMS,
                                    MENU_DISC_SIZE);
}

static bool create_color_menu(PaintState *st) {
    RingMenuItem items[MENU_ITEMS] = {0};
    bool ok = true;
    for (size_t i = 0; i < PALETTE_SIZE; i++) {
        if (!make_color_disc(st, st->palette[i], (int)i)) ok = false;
        items[i].image = st->color_discs[i];
        items[i].image_w = MENU_DISC_SIZE;
        items[i].image_h = MENU_DISC_SIZE;
    }
    items[MENU_RESULT_SPRAY - 1].label = "SPRAY";
    items[MENU_RESULT_SPLAT - 1].label = "SPLAT";
    items[MENU_RESULT_ERASER - 1].label = "ERASER";
    items[MENU_RESULT_GHOST - 1].label = "GHOST";
    items[MENU_RESULT_GHOST - 1].led = RINGMENU_LED_OFF;
    items[MENU_RESULT_CLEAR - 1].label = "CLEAR";
    items[MENU_RESULT_QUIT - 1].label = "QUIT";
    RingMenuGroup modes = {
        .first = MENU_RESULT_SPRAY - 1,
        .count = 3,
        .selected = MENU_RESULT_SPRAY - 1,
    };
    if (ok) {
        st->menu = ringmenu_create_grouped(items, MENU_ITEMS, &modes, 1);
    }
    return st->menu != NULL;
}

static void sync_menu_mode_leds(PaintState *st) {
    if (!st->menu) return;
    int selected = st->eraser_mode  ? MENU_RESULT_ERASER
               : st->tool == TOOL_SPLAT ? MENU_RESULT_SPLAT
                                        : MENU_RESULT_SPRAY;
    ringmenu_set_group_selected(st->menu, 0, selected - 1);
    ringmenu_set_led(st->menu, MENU_RESULT_GHOST - 1,
                     st->paint_mode ? RINGMENU_LED_OFF : RINGMENU_LED_ON);
}

static void menu_closed(PaintState *st, int result) {
    int mx, my;
    ringmenu_geometry(st->menu, &mx, &my, NULL, NULL);
    float arc_r = ringmenu_field_radius(st->menu);
    mark_dirty(st, (int)floorf(mx - arc_r), (int)floorf(my - arc_r),
               (int)ceilf(mx + arc_r), (int)ceilf(my + arc_r));
    ringmenu_take_dirty(st->menu);
    st->menu_dirty = false;
    if (result >= 1 && result <= (int)PALETTE_SIZE) {
        st->palette_index = result - 1;
        st->eraser_mode = false;
        st->cursor_dirty = true;
    } else if (result == MENU_RESULT_SPRAY) {
        st->tool = TOOL_SPRAY;
        st->eraser_mode = false;
        st->cursor_dirty = true;
    } else if (result == MENU_RESULT_SPLAT) {
        st->tool = TOOL_SPLAT;
        st->eraser_mode = false;
        st->cursor_dirty = true;
    } else if (result == MENU_RESULT_ERASER) {
        st->eraser_mode = true;
        st->cursor_dirty = true;
    } else if (result == MENU_RESULT_GHOST) {
        set_paint_mode(st, false);
    } else if (result == MENU_RESULT_CLEAR) {
        clear_canvas(st);
    } else if (result == MENU_RESULT_QUIT) {
        st->running = false;
    }
}


static const char *VERT_SHADER =
    "attribute vec2 pos;\n"
    "attribute vec2 uv_in;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    uv = uv_in;\n"
    "}\n";

static const char *FRAG_SHADER =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform float fade;\n"   
    "void main() {\n"
    "    vec4 c = texture2D(tex, uv);\n"
    "    gl_FragColor = vec4(c.rgb, c.a * fade);\n"
    "}\n";

static GLuint compile_shader(const char *src, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    return shader;
}

static bool gl_init(PaintState *st) {
    GLuint vert = compile_shader(VERT_SHADER, GL_VERTEX_SHADER);
    GLuint frag = compile_shader(FRAG_SHADER, GL_FRAGMENT_SHADER);

    st->program = glCreateProgram();
    glAttachShader(st->program, vert);
    glAttachShader(st->program, frag);
    glLinkProgram(st->program);
    glUseProgram(st->program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    st->pos_loc = glGetAttribLocation(st->program, "pos");
    st->uv_loc = glGetAttribLocation(st->program, "uv_in");
    st->tex_loc = glGetUniformLocation(st->program, "tex");
    st->fade_loc = glGetUniformLocation(st->program, "fade");
    if (st->pos_loc < 0 || st->uv_loc < 0 || st->tex_loc < 0 ||
        st->fade_loc < 0) {
        return false;
    }

    glUniform1i(st->tex_loc, 0);
    glUniform1f(st->fade_loc, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

static bool canvas_resize(PaintState *st, int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return false;

    uint32_t *new_canvas = calloc((size_t)new_w * (size_t)new_h, 4);
    float *new_paint = calloc((size_t)new_w * (size_t)new_h * 4, sizeof(float));
    float *new_field = calloc((size_t)new_w * (size_t)new_h, sizeof(float));
    uint8_t *new_cmap = malloc((size_t)new_w * (size_t)new_h);
    uint32_t *new_staging = malloc((size_t)new_w * (size_t)new_h * 4);
    size_t new_shade_cap = (size_t)new_w * (size_t)new_h;
    float *new_shade_u = malloc(new_shade_cap * sizeof(float));
    float *new_shade_c = malloc(new_shade_cap * sizeof(float));
    float *new_shade_s = malloc(new_shade_cap * sizeof(float));
    if (!new_canvas || !new_paint || !new_field || !new_cmap || !new_staging ||
        !new_shade_u || !new_shade_c || !new_shade_s) {
        free(new_canvas);
        free(new_paint);
        free(new_field);
        free(new_cmap);
        free(new_staging);
        free(new_shade_u);
        free(new_shade_c);
        free(new_shade_s);
        return false;
    }
    memset(new_cmap, COLOR_NONE, (size_t)new_w * (size_t)new_h);
    free(st->staging);
    free(st->shade_dist_u);
    free(st->shade_dist_c);
    free(st->shade_dist_s);
    st->staging = new_staging;
    st->shade_dist_u = new_shade_u;
    st->shade_dist_c = new_shade_c;
    st->shade_dist_s = new_shade_s;
    st->shade_scratch_cap = new_shade_cap;

    if (st->canvas) {
        int min_w = new_w < st->width ? new_w : st->width;
        int min_h = new_h < st->height ? new_h : st->height;
        for (int y = 0; y < min_h; y++) {
            memcpy(new_canvas + (size_t)y * new_w,
                   st->canvas + (size_t)y * st->width,
                   (size_t)min_w * 4);
            if (st->paint) {
                memcpy(new_paint + (size_t)y * new_w * 4,
                       st->paint + (size_t)y * st->width * 4,
                       (size_t)min_w * 4 * sizeof(float));
            }
            if (st->field) {
                memcpy(new_field + (size_t)y * new_w,
                       st->field + (size_t)y * st->width,
                       (size_t)min_w * sizeof(float));
            }
            if (st->color_map) {
                memcpy(new_cmap + (size_t)y * new_w,
                       st->color_map + (size_t)y * st->width,
                       (size_t)min_w);
            }
        }
        free(st->canvas);
        free(st->paint);
        free(st->field);
        free(st->color_map);
    }
    st->canvas = new_canvas;
    st->paint = new_paint;
    st->field = new_field;
    st->color_map = new_cmap;
    st->width = new_w;
    st->height = new_h;

    if (st->canvas_texture) {
        glDeleteTextures(1, &st->canvas_texture);
    }
    glGenTextures(1, &st->canvas_texture);
    glBindTexture(GL_TEXTURE_2D, st->canvas_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, new_w, new_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, st->canvas);

    st->dirty = false;
    return true;
}

static void gl_draw(PaintState *st, bool repaint_full, int rx0, int ry0, int rx1, int ry1) {
    if (st->dirty && st->dirty_y1 > st->dirty_y0 && st->dirty_x1 > st->dirty_x0) {
        int dx = st->dirty_x0;
        int dy = st->dirty_y0;
        int dw = st->dirty_x1 - st->dirty_x0;
        int dh = st->dirty_y1 - st->dirty_y0;
        glBindTexture(GL_TEXTURE_2D, st->canvas_texture);
        if (st->staging && dw < st->width / 2) {
            for (int row = 0; row < dh; ++row) {
                memcpy(st->staging + (size_t)row * dw,
                       st->canvas + (size_t)(dy + row) * st->width + dx,
                       (size_t)dw * 4);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh,
                            GL_RGBA, GL_UNSIGNED_BYTE, st->staging);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dy, st->width, dh,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            st->canvas + (size_t)dy * st->width);
        }
    }
    st->dirty = false;

    if (ringmenu_is_open(st->menu)) {
        int mx, my, mw, mh;
        ringmenu_rect(st->menu, &mx, &my, &mw, &mh);
        int x0 = mx < 0 ? 0 : mx;
        int y0 = my < 0 ? 0 : my;
        int x1 = mx + mw > st->width ? st->width : mx + mw;
        int y1 = my + mh > st->height ? st->height : my + mh;
        
        int mcx, mcy;
        ringmenu_geometry(st->menu, &mcx, &mcy, NULL, NULL);

        if (st->color_picker_slot >= 0) {
            float arc_r = ringmenu_field_radius(st->menu);
            int ax0 = (int)floorf(mcx - arc_r);
            int ay0 = (int)floorf(mcy - arc_r);
            int ax1 = (int)ceilf(mcx + arc_r);
            int ay1 = (int)ceilf(mcy + arc_r);
            if (ax0 < x0) x0 = ax0 < 0 ? 0 : ax0;
            if (ay0 < y0) y0 = ay0 < 0 ? 0 : ay0;
            if (ax1 > x1) x1 = ax1 > st->width ? st->width : ax1;
            if (ay1 > y1) y1 = ay1 > st->height ? st->height : ay1;
        }
        
        if (x0 < x1 && y0 < y1) {
            int cw = x1 - x0;
            int ch = y1 - y0;
            size_t need = (size_t)cw * ch;
            if (st->menu_scratch && need <= st->menu_scratch_cap) {
                for (int row = 0; row < ch; row++) {
                    memcpy(st->menu_scratch + (size_t)row * cw,
                           st->canvas + (size_t)(y0 + row) * st->width + x0,
                           (size_t)cw * 4);
                }
                if (st->color_picker_slot >= 0) {
                    ringmenu_field_draw(st->menu, st->color_picker_slot,
                                        st->menu_scratch, cw, ch, x0, y0);
                }
                ringmenu_draw(st->menu, st->menu_scratch, cw, ch, x0, y0);
                glBindTexture(GL_TEXTURE_2D, st->canvas_texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, cw, ch,
                                GL_RGBA, GL_UNSIGNED_BYTE, st->menu_scratch);
            }
        }
    }
    st->menu_dirty = false;

    int bx = 0, by = 0;
    bool badge_up = false;
    if (!st->paint_mode && st->badge) {
        badge_pos(st, &bx, &by);
        if (bx >= 0 && by >= 0 && by + BADGE_SIZE <= st->height) {
            size_t need = (size_t)BADGE_SIZE * BADGE_SIZE;
            if (st->menu_scratch && need <= st->menu_scratch_cap) {
                for (int row = 0; row < BADGE_SIZE; row++) {
                    uint32_t *dst = st->menu_scratch + (size_t)row * BADGE_SIZE;
                    const uint32_t *cnv =
                        st->canvas + (size_t)(by + row) * st->width + bx;
                    const uint32_t *src =
                        st->badge + (size_t)row * BADGE_SIZE;
                    for (int col = 0; col < BADGE_SIZE; col++) {
                        dst[col] = straight_over(src[col], cnv[col]);
                    }
                }
                glBindTexture(GL_TEXTURE_2D, st->canvas_texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, bx, by, BADGE_SIZE,
                                BADGE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE,
                                st->menu_scratch);
                badge_up = true;
            }
        }
    }

    static const GLfloat verts[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    if (!repaint_full && rx1 > rx0 && ry1 > ry0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(rx0, st->height - ry1, rx1 - rx0, ry1 - ry0);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(st->program);
    float fade_val = st->paint_mode ? 1.0f : 0.4f;
    if (g_quit_fade > 0.0) {
        fade_val *= (float)(g_quit_fade / 1.0);
    }
    glUniform1f(st->fade_loc, fade_val);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->canvas_texture);
    glVertexAttribPointer(st->pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(st->uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(st->pos_loc);
    glEnableVertexAttribArray(st->uv_loc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!repaint_full && rx1 > rx0 && ry1 > ry0) {
        glDisable(GL_SCISSOR_TEST);
    }

    if (badge_up) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(bx, st->height - by - BADGE_SIZE, BADGE_SIZE, BADGE_SIZE);
        float badge_fade = 1.0f;
        if (g_quit_fade > 0.0) badge_fade *= (float)(g_quit_fade / 1.0);
        glUniform1f(st->fade_loc, badge_fade);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_SCISSOR_TEST);
    }
}


static void update_input_region(PaintState *st) {
    if (!st->plat) return;
    PlatRect region = { 0, 0, st->width, st->height };
    if (!st->paint_mode) {
        badge_pos(st, &region.x, &region.y);
        region.w = BADGE_SIZE;
        region.h = BADGE_SIZE;
    }
    plat_input_region(st->plat, &region, 1);
    plat_apply(st->plat);
}


static void on_resize(void *data, int width, int height) {
    PaintState *st = data;
    st->pending_width = width;
    st->pending_height = height;
    if (width != st->width || height != st->height) {
        st->resize_pending = true;
    }
}
static void on_close(void *data) {
    ((PaintState *)data)->running = false;
}

static void pointer_enter(void *data, int x, int y) {
    PaintState *st = data;
    st->pointer_x = x;
    st->pointer_y = y;
}
static void pointer_leave(void *data) {
    ((PaintState *)data)->pointer_down = false;
}
static void pointer_motion(void *data, int x, int y) {
    PaintState *st = data;
    st->pointer_x = x;
    st->pointer_y = y;

    if (ringmenu_is_open(st->menu)) {
        ringmenu_motion(st->menu, st->pointer_x, st->pointer_y);
        if (ringmenu_take_dirty(st->menu)) st->menu_dirty = true;
        
        int mx, my;
        ringmenu_geometry(st->menu, &mx, &my, NULL, NULL);

        int raw = ringmenu_field_slot(st->menu, st->pointer_x, st->pointer_y);
        int slot = -1;
        if (raw >= 0) {
            if (st->color_picker_locked) {
                slot = st->color_picker_slot;
            } else {
                if (raw < (int)PALETTE_SIZE) slot = raw;
                st->color_picker_locked = true;
            }
        } else {
            st->color_picker_locked = false;
        }

        if (slot != st->color_picker_slot) {
            float arc_r = ringmenu_field_radius(st->menu);
            mark_dirty(st, (int)floorf(mx - arc_r), (int)floorf(my - arc_r),
                       (int)ceilf(mx + arc_r), (int)ceilf(my + arc_r));
            if (st->color_picker_slot >= 0) {
                st->palette[st->color_picker_slot] = st->original_color;
                make_color_disc(st, st->original_color, st->color_picker_slot);
                ringmenu_update_image(st->menu, st->color_picker_slot,
                                      st->color_discs[st->color_picker_slot]);
            }
            st->color_picker_slot = slot;
            if (slot >= 0) {
                st->original_color = st->palette[slot];
            }
            st->menu_dirty = true;
            st->cursor_dirty = true;
            if (!st->paint_mode) build_badge(st);
        }

        if (slot >= 0) {
            PaintColor new_color;
            if (ringmenu_field_color(st->menu, slot, st->pointer_x,
                                     st->pointer_y, &new_color.r,
                                     &new_color.g, &new_color.b)) {
                st->palette[slot] = new_color;
                make_color_disc(st, new_color, slot);
                ringmenu_update_image(st->menu, slot, st->color_discs[slot]);

                if (!st->paint_mode) build_badge(st);
                st->cursor_dirty = true;
                st->menu_dirty = true;
            }
        }
    } else if (st->pointer_down && st->paint_mode && !st->eraser_mode &&
               st->tool == TOOL_SPLAT) {
        int dx = st->pointer_x - st->last_splat_x;
        int dy = st->pointer_y - st->last_splat_y;
        if (!st->has_last_splat ||
            (double)(dx * dx + dy * dy) >=
                SPLAT_DRAG_SPACING * SPLAT_DRAG_SPACING) {
            splat_at(st, st->pointer_x, st->pointer_y);
        }
    }
}
static void pointer_button(void *data, PlatButton button, PlatPress state) {
    PaintState *st = data;
    if (!st->paint_mode) {
        if (button == PLAT_BTN_LEFT && state == PLAT_PRESSED) {
            int bx, by;
            badge_pos(st, &bx, &by);
            if (st->pointer_x >= bx && st->pointer_x < bx + BADGE_SIZE &&
                st->pointer_y >= by && st->pointer_y < by + BADGE_SIZE) {
                set_paint_mode(st, true);
            }
        }
        return;
    }

    if (ringmenu_is_open(st->menu)) {
        int btn = -1;
        if (button == PLAT_BTN_LEFT) btn = RINGMENU_BTN_LEFT;
        else if (button == PLAT_BTN_RIGHT) btn = RINGMENU_BTN_RIGHT;
        else if (button == PLAT_BTN_MIDDLE) btn = RINGMENU_BTN_MIDDLE;
        if (btn >= 0) {
            bool pressed = (state == PLAT_PRESSED);
            
            if (!pressed && st->color_picker_slot >= 0 && (btn == RINGMENU_BTN_LEFT || btn == RINGMENU_BTN_RIGHT)) {
                int selected = st->color_picker_slot + 1;
                PaintColor before = st->original_color;
                PaintColor after = st->palette[st->color_picker_slot];
                st->color_picker_slot = -1;
                ringmenu_button(st->menu, btn, pressed); 
                menu_closed(st, selected);
                if (st->splat_count > 0 &&
                    (before.r != after.r || before.g != after.g ||
                     before.b != after.b)) {
                    queue_shade(st, 0, 0, st->width - 1, st->height - 1);
                }
                return;
            }
            
            int result = ringmenu_button(st->menu, btn, pressed);
            if (result >= 0) {
                if (st->color_picker_slot >= 0) {
                    st->palette[st->color_picker_slot] = st->original_color;
                    st->color_picker_slot = -1;
                    if (!st->paint_mode) build_badge(st);
                }
                menu_closed(st, result);
            } else if (ringmenu_take_dirty(st->menu)) {
                st->menu_dirty = true;
            }
        }
        return;
    }

    if (button == PLAT_BTN_RIGHT) {
        if (state == PLAT_PRESSED && st->menu) {
            st->pointer_down = false;
            st->color_picker_slot = -1;
            st->color_picker_locked = false;
            ringmenu_open(st->menu, st->pointer_x, st->pointer_y,
                          st->width, st->height);
            sync_menu_mode_leds(st);
            if (ringmenu_take_dirty(st->menu)) st->menu_dirty = true;
        }
        return;
    }
    if (button != PLAT_BTN_LEFT) return;

    if (state == PLAT_PRESSED) {
        st->pointer_down = true;
        clock_gettime(CLOCK_MONOTONIC, &st->last_step);
        if (st->eraser_mode) {
            erase_stamp(st, st->pointer_x, st->pointer_y, 0.06);
        } else if (st->tool == TOOL_SPLAT) {
            splat_at(st, st->pointer_x, st->pointer_y);
        } else {
            spray_stamp(st, st->pointer_x, st->pointer_y, 0.09);
        }
    } else {
        st->pointer_down = false;
        st->has_last_splat = false;
    }
}
static void pointer_scroll(void *data, double value) {
    PaintState *st = data;
    if (!st->paint_mode || ringmenu_is_open(st->menu)) return;

    double notches = value / 10.0;
    double factor = pow(SIZE_STEP, -notches);   
    if (st->eraser_mode) {
        st->eraser_radius *= factor;
        if (st->eraser_radius < ERASER_R_MIN) st->eraser_radius = ERASER_R_MIN;
        if (st->eraser_radius > ERASER_R_MAX) st->eraser_radius = ERASER_R_MAX;
    } else if (st->tool == TOOL_SPLAT) {
        st->splat_size *= factor;
        if (st->splat_size < SPLAT_S_MIN) st->splat_size = SPLAT_S_MIN;
        if (st->splat_size > SPLAT_S_MAX) st->splat_size = SPLAT_S_MAX;
    } else {
        st->spray_radius *= factor;
        if (st->spray_radius < SPRAY_R_MIN) st->spray_radius = SPRAY_R_MIN;
        if (st->spray_radius > SPRAY_R_MAX) st->spray_radius = SPRAY_R_MAX;
    }
    st->cursor_dirty = true;
}
/* No release will follow a button held when the pointer went away. */
static void pointer_lost(void *data) {
    ((PaintState *)data)->pointer_down = false;
}

static void keyboard_key(void *data, PlatKey key, PlatPress state) {
    PaintState *st = data;
    if (state != PLAT_PRESSED) return;

    switch (key) {
        case PLAT_KEY_ESC:
        case PLAT_KEY_Q:
            st->running = false;
            break;
        case PLAT_KEY_C:
            clear_canvas(st);
            break;
        case PLAT_KEY_E:
            st->eraser_mode = !st->eraser_mode;
            st->cursor_dirty = true;
            break;
        case PLAT_KEY_T:
            st->tool = st->tool == TOOL_SPRAY ? TOOL_SPLAT : TOOL_SPRAY;
            st->eraser_mode = false;
            st->cursor_dirty = true;
            break;
        case PLAT_KEY_LEFTBRACE:
        case PLAT_KEY_RIGHTBRACE: {
            double f = key == PLAT_KEY_RIGHTBRACE ? SIZE_STEP : 1.0 / SIZE_STEP;
            if (st->eraser_mode) {
                st->eraser_radius *= f;
                if (st->eraser_radius < ERASER_R_MIN) st->eraser_radius = ERASER_R_MIN;
                if (st->eraser_radius > ERASER_R_MAX) st->eraser_radius = ERASER_R_MAX;
            } else if (st->tool == TOOL_SPLAT) {
                st->splat_size *= f;
                if (st->splat_size < SPLAT_S_MIN) st->splat_size = SPLAT_S_MIN;
                if (st->splat_size > SPLAT_S_MAX) st->splat_size = SPLAT_S_MAX;
            } else {
                st->spray_radius *= f;
                if (st->spray_radius < SPRAY_R_MIN) st->spray_radius = SPRAY_R_MIN;
                if (st->spray_radius > SPRAY_R_MAX) st->spray_radius = SPRAY_R_MAX;
            }
            st->cursor_dirty = true;
            break;
        }
        case PLAT_KEY_SPACE:
            if (ringmenu_is_open(st->menu)) {
                menu_closed(st, ringmenu_button(st->menu,
                                                RINGMENU_BTN_MIDDLE, true));
            }
            set_paint_mode(st, !st->paint_mode);
            break;
        default:
            if (key >= PLAT_KEY_1 && key < PLAT_KEY_1 + (int)PALETTE_SIZE) {
                st->palette_index = (int)(key - PLAT_KEY_1);
                st->eraser_mode = false;
                st->cursor_dirty = true;
            }
            break;
    }
}
static void keyboard_lost(void *data) {
    (void)data;
}

static const PlatHandlers g_handlers = {
    .pointer_enter = pointer_enter,
    .pointer_leave = pointer_leave,
    .pointer_motion = pointer_motion,
    .pointer_button = pointer_button,
    .pointer_scroll = pointer_scroll,
    .pointer_lost = pointer_lost,
    .key = keyboard_key,
    .keyboard_lost = keyboard_lost,
    .resize = on_resize,
    .close = on_close,
};

int main(void) {
    PaintState st = {0};
    st.running = true;
    st.paint_mode = true;
    st.width = 1280;
    st.height = 720;
    st.spray_radius = SPRAY_R_DEFAULT;
    st.eraser_radius = ERASER_R_DEFAULT;
    st.splat_size = SPLAT_S_DEFAULT;
    st.tool = TOOL_SPLAT;   

    for (size_t i = 0; i < PALETTE_SIZE; i++) st.palette[i] = DEFAULT_PALETTE[i];
    st.color_picker_slot = -1;

    srand((unsigned int)(time(NULL) ^ getpid()));
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    alpha_lut_init();

    PlatConfig plat_config = {
        .title = "Splat",
        .app_id = "splat",
        .log = PLAT_LOG_QUIET,
        .damage = PLAT_DAMAGE_AUTO,
    };
    st.plat = plat_open(&plat_config, &g_handlers, &st);
    if (!st.plat) return 1;

    int output_width = 0, output_height = 0;
    plat_output_size(st.plat, &output_width, &output_height);
    if (output_width > 0 && output_height > 0) {
        st.width = output_width;
        st.height = output_height;
    }
    if (!plat_window_create(st.plat, st.width, st.height) ||
        !plat_window_show(st.plat)) {
        plat_close(st.plat);
        return 1;
    }

    if (st.pending_width > 0 && st.pending_height > 0) {
        st.width = st.pending_width;
        st.height = st.pending_height;
    }
    st.pending_width = st.pending_height = 0;
    st.resize_pending = false;
    plat_surface_resize(st.plat, st.width, st.height);
    glViewport(0, 0, st.width, st.height);

    if (!gl_init(&st) || !canvas_resize(&st, st.width, st.height)) {
        fprintf(stderr, "Failed to initialize GL resources\n");
        return 1;
    }
    st.splat_own = calloc(SPLAT_SCRATCH_CAP, sizeof(*st.splat_own));
    st.splat_vdist = malloc(SPLAT_SCRATCH_CAP * sizeof(*st.splat_vdist));
    st.splat_vlabel = malloc(SPLAT_SCRATCH_CAP * sizeof(*st.splat_vlabel));
    if (!st.splat_own || !st.splat_vdist || !st.splat_vlabel) {
        fprintf(stderr, "Failed to allocate splat workspace\n");
        free(st.splat_own);
        free(st.splat_vdist);
        free(st.splat_vlabel);
        return 1;
    }
    update_input_region(&st);

    if (!create_cursor(&st)) {
        fprintf(stderr, "Warning: could not create the tool cursor, "
                        "using the default pointer\n");
    }

    if (!create_color_menu(&st)) {
        fprintf(stderr, "Warning: could not create the menu, "
                        "right-click will do nothing\n");
    }

    if (st.menu) {
        float field_r = ringmenu_field_radius(st.menu);
        int side = ringmenu_size(st.menu) + (int)ceilf(field_r * 2.0f);
        size_t menu_cap = (size_t)side * side;
        if (menu_cap < (size_t)BADGE_SIZE * BADGE_SIZE) {
            menu_cap = (size_t)BADGE_SIZE * BADGE_SIZE;
        }
        st.menu_scratch = malloc(menu_cap * 4);
        st.menu_scratch_cap = st.menu_scratch ? menu_cap : 0;
        st.badge_base = ghost_icon_create_bg(false);
        st.badge = st.badge_base ? malloc((size_t)BADGE_SIZE * BADGE_SIZE * 4) : NULL;
        st.badge_icon = calloc((size_t)BADGE_SIZE * BADGE_SIZE, 4);
        st.badge_full = calloc((size_t)BADGE_SIZE * 2 * BADGE_SIZE * 2, 4);
        if (!st.menu_scratch || !st.badge_base || !st.badge ||
            !st.badge_icon || !st.badge_full) {
            fprintf(stderr, "Failed to allocate menu and badge workspace\n");
            free(st.menu_scratch);
            free(st.badge);
            free(st.badge_base);
            return 1;
        }
        build_badge(&st);
    }

    if (!hiss_start()) {
        return 1;
    }

    printf("Splat running. SPLAT: click or drag to throw glossy paint blobs.\n"
           "SPRAY: hold the left button to mist (hold still to build up\n"
           "opacity). Scroll to change the tool\n"
           "size. Right-click for the menu (drag to a slot and release):\n"
           "colors, SPRAY, SPLAT, ERASER, GHOST, CLEAR, QUIT, CANCEL.\n"
           "Keys: 1-7 colors, T tool, E eraser, [ ] size, C clear,\n"
           "Space click-through, Esc/Q quit.\n");
    fflush(stdout);

    st.dirty = true;

    while (st.running) {
        bool spraying = st.pointer_down && st.paint_mode &&
                        !ringmenu_is_open(st.menu) &&
                        (st.eraser_mode || st.tool == TOOL_SPRAY);
        bool animating = spraying || g_quit_fade > 0.0;

        hiss_set(spraying && !st.eraser_mode,
                 (float)((st.spray_radius - SPRAY_R_MIN) /
                         (SPRAY_R_MAX - SPRAY_R_MIN)));

        bool want_draw = (st.dirty || st.shade_pending || st.menu_dirty ||
                          st.frame_pending || g_quit_fade > 0.0) &&
                         plat_frame(st.plat)->ready;
        if (!plat_pump(st.plat, want_draw ? 0 : (animating ? 8 : -1))) {
            break;
        }

        if (g_quit_requested == 1 || (!st.running && g_quit_fade == 0.0)) {
            st.running = true;
            g_quit_requested = 2;
            g_quit_fade = 1.0;
        }

        if (st.resize_pending && st.pending_width > 0 && st.pending_height > 0) {
            int nw = st.pending_width, nh = st.pending_height;
            st.pending_width = st.pending_height = 0;
            st.resize_pending = false;
            if (nw != st.width || nh != st.height) {
                plat_surface_resize(st.plat, nw, nh);
                glViewport(0, 0, nw, nh);
                canvas_resize(&st, nw, nh);
                st.damage_hist_depth = 0;
                st.dirty = true;
                update_input_region(&st);
            }
        }

        if (st.cursor_dirty) {
            st.cursor_dirty = false;
            update_cursor(&st);
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (animating) {
            double dt = ts_diff(&now, &st.last_step);
            if (dt > 0.05) dt = 0.05;
            if (dt > 0.0) {
                if (g_quit_fade > 0.0) {
                    g_quit_fade -= dt;
                    if (g_quit_fade <= 0.0) {
                        break;
                    }
                }

                if (spraying) {
                    if (!st.has_last_spray) {
                        st.last_spray_x = st.pointer_x;
                        st.last_spray_y = st.pointer_y;
                        st.has_last_spray = true;
                    }

                    double dx = st.pointer_x - st.last_spray_x;
                    double dy = st.pointer_y - st.last_spray_y;
                    double dist = sqrt(dx * dx + dy * dy);

                    double R = st.eraser_mode ? st.eraser_radius : st.spray_radius;
                    int steps = (int)ceil(dist / (R * 0.25));
                    if (steps < 1) steps = 1;

                    double step_dt = dt / steps;
                    for (int i = 1; i <= steps; i++) {
                        double t = (double)i / steps;
                        double px = st.last_spray_x + dx * t;
                        double py = st.last_spray_y + dy * t;

                        if (st.eraser_mode) {
                            erase_stamp(&st, px, py, step_dt);
                        } else {
                            spray_stamp(&st, px, py, step_dt);
                        }
                    }
                    st.last_spray_x = st.pointer_x;
                    st.last_spray_y = st.pointer_y;
                } else {
                    st.has_last_spray = false;
                }
            }
        }
        st.last_step = now;

        if ((st.dirty || st.shade_pending || st.menu_dirty ||
             st.frame_pending || g_quit_fade > 0.0) &&
            plat_frame(st.plat)->ready) {
            flush_pending_shade(&st);
            st.frame_pending = false;

            (void)plat_frame_request(st.plat);

            bool full_damage = st.menu_dirty || ringmenu_is_open(st.menu) || !st.paint_mode || g_quit_fade > 0.0;
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (st.dirty) {
                dx = st.dirty_x0;
                dy = st.dirty_y0;
                dw = st.dirty_x1 - st.dirty_x0;
                dh = st.dirty_y1 - st.dirty_y0;
            }

            if (st.damage_hist_depth < DAMAGE_HISTORY) st.damage_hist_depth++;
            for (int i = DAMAGE_HISTORY - 1; i > 0; i--) {
                st.damage_hist[i] = st.damage_hist[i - 1];
            }
            if (full_damage || (dw == 0 && dh == 0 && st.dirty)) {
                st.damage_hist[0].x0 = 0;
                st.damage_hist[0].y0 = 0;
                st.damage_hist[0].x1 = st.width;
                st.damage_hist[0].y1 = st.height;
            } else {
                st.damage_hist[0].x0 = dx;
                st.damage_hist[0].y0 = dy;
                st.damage_hist[0].x1 = dx + dw;
                st.damage_hist[0].y1 = dy + dh;
            }

            bool repaint_full = true;
            int rx0 = 0, ry0 = 0, rx1 = st.width, ry1 = st.height;
            int age = plat_buffer_age(st.plat);
            if (!full_damage &&
                age >= 1 && age <= st.damage_hist_depth) {
                repaint_full = false;
                rx0 = st.width; ry0 = st.height; rx1 = 0; ry1 = 0;
                for (int k = 0; k < age; k++) {
                    if (st.damage_hist[k].x0 < rx0) rx0 = st.damage_hist[k].x0;
                    if (st.damage_hist[k].y0 < ry0) ry0 = st.damage_hist[k].y0;
                    if (st.damage_hist[k].x1 > rx1) rx1 = st.damage_hist[k].x1;
                    if (st.damage_hist[k].y1 > ry1) ry1 = st.damage_hist[k].y1;
                }
                if (rx0 >= rx1 || ry0 >= ry1) {
                    rx0 = 0; ry0 = 0; rx1 = 0; ry1 = 0;
                }
            }

            gl_draw(&st, repaint_full, rx0, ry0, rx1, ry1);

            if (plat_has_damage(st.plat) && !full_damage) {
                PlatRect damage = { dx, dy, dw, dh };
                if (damage.w == 0 || damage.h == 0) {
                    damage = (PlatRect){ 0, 0, 1, 1 };
                }
                plat_swap(st.plat, &damage, 1);
            } else {
                plat_swap(st.plat, NULL, 0);
            }
        }
    }

    hiss_stop();
    ringmenu_destroy(st.menu);
    free(st.menu_scratch);
    free(st.badge);
    free(st.badge_base);
    free(st.badge_icon);
    free(st.badge_full);
    free(st.canvas);
    free(st.paint);
    free(st.field);
    free(st.color_map);
    free(st.staging);
    free(st.shade_dist_u);
    free(st.shade_dist_c);
    free(st.shade_dist_s);
    free(st.splat_own);
    free(st.splat_vdist);
    free(st.splat_vlabel);
    free(st.cursor_pixels);
    if (st.canvas_texture) glDeleteTextures(1, &st.canvas_texture);
    if (st.program) glDeleteProgram(st.program);
    plat_close(st.plat);
    return 0;
}
