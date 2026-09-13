#ifndef TOY_PLATFORM_H
#define TOY_PLATFORM_H

/*
 * The toys' window-system layer: window, GL ES 2 context, input, frame
 * pacing and cursors. A toy above it sees pixels, events and GL -- never
 * Wayland, EGL or Win32.
 *
 *     poingo.c, balloons.c   physics, rendering, menus, audio mixing
 *        |  plat_*()                       ^  PlatHandlers
 *        v                                 |
 *     platform_wayland.c                platform_win32.c
 *     wl_*, xdg-shell, EGL              Win32, ANGLE (D3D11), DirectComposition
 *
 * The surface is a transparent overlay the size of the work area. Pixels are
 * premultiplied RGBA. Pointer input reaches the toy only inside the region
 * last passed to plat_input_region(); everywhere else it falls through to
 * whatever is underneath.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct Plat Plat;

/* Surface pixels, origin top-left. */
typedef struct {
    int x, y, w, h;
} PlatRect;

typedef enum {
    PLAT_BTN_LEFT,
    PLAT_BTN_RIGHT,
    PLAT_BTN_MIDDLE,
    PLAT_BTN_OTHER,
} PlatButton;

typedef enum {
    PLAT_RELEASED,
    PLAT_PRESSED,
} PlatPress;

typedef enum {
    PLAT_KEY_OTHER,
    PLAT_KEY_Q,
    PLAT_KEY_ESC,
    PLAT_KEY_M,
    PLAT_KEY_A,
    PLAT_KEY_P,
    PLAT_KEY_C,
    PLAT_KEY_SPACE,
    PLAT_KEY_LEFTBRACE,
    PLAT_KEY_RIGHTBRACE,
    PLAT_KEY_UP,
    PLAT_KEY_DOWN,
    PLAT_KEY_LEFT,
    PLAT_KEY_RIGHT,
    PLAT_KEY_E,
    PLAT_KEY_T,
    /* The digit row, contiguous: PLAT_KEY_1 + n is the key for n + 1. */
    PLAT_KEY_1,
    PLAT_KEY_2,
    PLAT_KEY_3,
    PLAT_KEY_4,
    PLAT_KEY_5,
    PLAT_KEY_6,
    PLAT_KEY_7,
    PLAT_KEY_8,
    PLAT_KEY_9,
} PlatKey;

typedef enum {
    PLAT_LOG_QUIET,
    PLAT_LOG_DEBUG,
} PlatLog;

typedef enum {
    PLAT_DAMAGE_AUTO,   /* use damage and buffer age where the system has them */
    PLAT_DAMAGE_OFF,    /* always present and repaint everything */
} PlatDamage;

/* Called from inside plat_pump(), on the thread that calls it. */
typedef struct {
    void (*pointer_enter)(void *userdata, int x, int y);
    void (*pointer_leave)(void *userdata);
    void (*pointer_motion)(void *userdata, int x, int y);
    void (*pointer_button)(void *userdata, PlatButton button, PlatPress press);
    /* Negative scrolls away from the user (wheel up). */
    void (*pointer_scroll)(void *userdata, double delta);
    /* The pointer device is gone; no release will follow a held button. */
    void (*pointer_lost)(void *userdata);
    void (*key)(void *userdata, PlatKey key, PlatPress press);
    /* Focus left; no release will follow a held key. */
    void (*keyboard_lost)(void *userdata);
    void (*resize)(void *userdata, int width, int height);
    /* The user asked to close the window (taskbar, Alt+F4, compositor). */
    void (*close)(void *userdata);
} PlatHandlers;

typedef struct {
    const char *title;
    const char *app_id;
    PlatLog log;
    PlatDamage damage;
} PlatConfig;

/* Presentation feedback for the last requested frame. */
typedef struct {
    bool ready;             /* presented; safe to draw the next one */
    uint32_t presented_ms;  /* compositor clock, 0 when the system has none */
    uint64_t delivered_ns;  /* monotonic arrival time, 0 when unknown */
} PlatFrame;

/* Connect to the window system and create the GL context. NULL on failure,
   with the reason already on stderr. */
Plat *plat_open(const PlatConfig *config, const PlatHandlers *handlers, void *userdata);

/* Tear down whatever plat_open() and later calls built. NULL is harmless. */
void plat_close(Plat *plat);

/* Output mode, 0 when the window system has not said. */
void plat_output_size(const Plat *plat, int *width, int *height);
int plat_refresh_mhz(const Plat *plat);

/* Create the surface and make the GL context current on it. Not yet shown. */
bool plat_window_create(Plat *plat, int width, int height);

/* Map the window and wait until it has its size (resize handler fires). */
bool plat_window_show(Plat *plat);

/* Add a cursor: frame_count premultiplied ARGB8888 squares of side size,
   hotspot at (hot_x, hot_y). The pixels are copied. Returns the cursor's
   id, or -1. The first cursor added is the one in use. */
int plat_cursor_create(Plat *plat, const uint32_t *argb, int size,
                       int frame_count, int hot_x, int hot_y);

/* Show this cursor while the pointer is over the surface. */
void plat_cursor_use(Plat *plat, int cursor);

/* Show this frame of the cursor in use. */
void plat_cursor_frame(Plat *plat, int frame);

/* Redraw a cursor: new pixels in the same layout it was created with. Its
   hotspot stays. */
void plat_cursor_update(Plat *plat, int cursor, const uint32_t *argb);

/* Follow a resize the handler reported. */
void plat_surface_resize(Plat *plat, int width, int height);

/* Where pointer input is accepted until the next call. */
void plat_input_region(Plat *plat, const PlatRect *rects, int count);

/* Apply pending surface state, the input region included, without drawing. */
void plat_apply(Plat *plat);

/* Dispatch pending events, waiting up to timeout_ms for the first; negative
   waits until something arrives. False once the window system is gone. */
bool plat_pump(Plat *plat, int timeout_ms);

const PlatFrame *plat_frame(const Plat *plat);

/* Ask for presentation feedback on the next swap. */
bool plat_frame_request(Plat *plat);

/* A hidden surface may never answer; ask again without drawing. */
void plat_frame_poke(Plat *plat);

/* Frames since the back buffer's contents were current, 0 when unknown. */
int plat_buffer_age(const Plat *plat);

/* Whether plat_swap() hands damage on to the compositor. */
bool plat_has_damage(const Plat *plat);

/* Present. count rectangles say what changed; none means everything. */
void plat_swap(Plat *plat, const PlatRect *damage, int count);

int plat_cpu_count(void);

#endif
