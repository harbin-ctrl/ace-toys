/*
 * Wayland implementation of platform.h: an xdg-shell toplevel, EGL on a
 * wl_egl_window, wl_seat input and wl_surface frame callbacks.
 *
 * Moved out of poingo.c without changing behaviour; the notes on protocol
 * corner cases came with it.
 */
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/memfd.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"

enum {
    WAY_GLOBAL_NONE = 0,
    WAY_CURSORS_MAX = 4,
};

typedef struct {
    struct wl_surface *surface;
    struct wl_buffer **buffers;
    int frame_count;
    uint8_t *map;
    size_t map_size;
    int size;
    int hot_x;
    int hot_y;
    int current;
} WayCursor;

struct Plat {
    PlatHandlers handlers;
    void *userdata;
    PlatLog log;
    PlatDamage damage;
    const char *title;
    const char *app_id;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    uint32_t compositor_name;
    uint32_t shm_name;
    uint32_t seat_name;
    uint32_t output_name;
    uint32_t wm_base_name;
    /* The compositor or wm_base went away; nothing can be drawn again. */
    bool lost;

    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct wl_egl_window *egl_window;
    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC swap_damage;
    bool has_buffer_age;
    EGLint *damage_rects;
    int damage_cap;

    int output_width;
    int output_height;
    int refresh_mhz;
    int pending_width;
    int pending_height;
    int surface_height;
    bool configured;

    PlatFrame frame;
    struct wl_callback *frame_callback;

    WayCursor cursors[WAY_CURSORS_MAX];
    int cursor_count;
    int cursor_in_use;
    /* set_cursor takes the serial of the enter it answers. */
    uint32_t enter_serial;
    bool pointer_inside;
};

static PlatKey way_key(uint32_t key)
{
    if (key >= KEY_1 && key <= KEY_9) {
        return (PlatKey)(PLAT_KEY_1 + (int)(key - KEY_1));
    }

    switch (key) {
    case KEY_E:
        return PLAT_KEY_E;
    case KEY_T:
        return PLAT_KEY_T;
    case KEY_Q:
        return PLAT_KEY_Q;
    case KEY_ESC:
        return PLAT_KEY_ESC;
    case KEY_ENTER:
        return PLAT_KEY_ENTER;
    case KEY_BACKSPACE:
        return PLAT_KEY_BACKSPACE;
    case KEY_DELETE:
        return PLAT_KEY_DELETE;
    case KEY_M:
        return PLAT_KEY_M;
    case KEY_A:
        return PLAT_KEY_A;
    case KEY_P:
        return PLAT_KEY_P;
    case KEY_C:
        return PLAT_KEY_C;
    case KEY_SPACE:
        return PLAT_KEY_SPACE;
    case KEY_LEFTBRACE:
        return PLAT_KEY_LEFTBRACE;
    case KEY_RIGHTBRACE:
        return PLAT_KEY_RIGHTBRACE;
    case KEY_UP:
        return PLAT_KEY_UP;
    case KEY_DOWN:
        return PLAT_KEY_DOWN;
    case KEY_LEFT:
        return PLAT_KEY_LEFT;
    case KEY_RIGHT:
        return PLAT_KEY_RIGHT;
    default:
        return PLAT_KEY_OTHER;
    }
}

static PlatButton way_button(uint32_t button)
{
    switch (button) {
    case BTN_LEFT:
        return PLAT_BTN_LEFT;
    case BTN_RIGHT:
        return PLAT_BTN_RIGHT;
    case BTN_MIDDLE:
        return PLAT_BTN_MIDDLE;
    default:
        return PLAT_BTN_OTHER;
    }
}

static void way_cursor_show(const Plat *p)
{
    if (!p->pointer || !p->pointer_inside || p->cursor_count == 0) {
        return;
    }
    const WayCursor *cursor = &p->cursors[p->cursor_in_use];
    wl_pointer_set_cursor(p->pointer, p->enter_serial, cursor->surface,
                          cursor->hot_x, cursor->hot_y);
}

static void way_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener way_wm_base_listener = {
    .ping = way_wm_base_ping,
};

static void way_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    Plat *p = data;
    if (width > 0 && height > 0) {
        p->pending_width = width;
        p->pending_height = height;
    }
}

static void way_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    Plat *p = data;
    p->handlers.close(p->userdata);
}

static const struct xdg_toplevel_listener way_toplevel_listener = {
    .configure = way_toplevel_configure,
    .close = way_toplevel_close,
};

static void way_xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    Plat *p = data;
    xdg_surface_ack_configure(surface, serial);
    p->configured = true;
    if (p->pending_width > 0 && p->pending_height > 0) {
        int width = p->pending_width;
        int height = p->pending_height;
        p->pending_width = 0;
        p->pending_height = 0;
        p->handlers.resize(p->userdata, width, height);
    }
}

static const struct xdg_surface_listener way_xdg_surface_listener = {
    .configure = way_xdg_surface_configure,
};

static void way_output_mode(void *data, struct wl_output *output,
                            uint32_t flags, int width, int height, int refresh)
{
    (void)output;
    Plat *p = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        p->output_width = width;
        p->output_height = height;
        p->refresh_mhz = refresh;
    }
}

static void way_output_geometry(void *data, struct wl_output *output,
                                int32_t x, int32_t y,
                                int32_t phys_width, int32_t phys_height,
                                int32_t subpixel,
                                const char *make, const char *model,
                                int32_t transform)
{
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

static void way_output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void way_output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    (void)factor;
}

static const struct wl_output_listener way_output_listener = {
    .geometry = way_output_geometry,
    .mode = way_output_mode,
    .done = way_output_done,
    .scale = way_output_scale,
};

static void way_pointer_enter(void *data, struct wl_pointer *pointer,
                              uint32_t serial, struct wl_surface *surface,
                              wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer;
    (void)surface;
    Plat *p = data;
    p->enter_serial = serial;
    p->pointer_inside = true;
    way_cursor_show(p);
    p->handlers.pointer_enter(p->userdata, wl_fixed_to_int(surface_x),
                              wl_fixed_to_int(surface_y));
}

static void way_pointer_leave(void *data, struct wl_pointer *pointer,
                              uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    Plat *p = data;
    p->pointer_inside = false;
    p->handlers.pointer_leave(p->userdata);
}

static void way_pointer_motion(void *data, struct wl_pointer *pointer,
                               uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer;
    (void)time;
    Plat *p = data;
    p->handlers.pointer_motion(p->userdata, wl_fixed_to_int(surface_x),
                               wl_fixed_to_int(surface_y));
}

static void way_pointer_button(void *data, struct wl_pointer *pointer,
                               uint32_t serial, uint32_t time, uint32_t button,
                               uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    Plat *p = data;
    PlatPress press = state == WL_POINTER_BUTTON_STATE_PRESSED ? PLAT_PRESSED : PLAT_RELEASED;
    p->handlers.pointer_button(p->userdata, way_button(button), press);
}

static void way_pointer_axis(void *data, struct wl_pointer *pointer,
                             uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    Plat *p = data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    p->handlers.pointer_scroll(p->userdata, wl_fixed_to_double(value));
}

static const struct wl_pointer_listener way_pointer_listener = {
    .enter = way_pointer_enter,
    .leave = way_pointer_leave,
    .motion = way_pointer_motion,
    .button = way_pointer_button,
    .axis = way_pointer_axis,
};

static void way_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                                uint32_t format, int fd, uint32_t size)
{
    (void)keyboard;
    Plat *p = data;
    if (fd < 0) {
        return;
    }
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }

    char *source = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (source == MAP_FAILED) {
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
                                    p->xkb_context, source, XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(source, size);
    if (!keymap) {
        return;
    }
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }

    xkb_state_unref(p->xkb_state);
    xkb_keymap_unref(p->xkb_keymap);
    p->xkb_keymap = keymap;
    p->xkb_state = state;
}

static void way_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void way_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    Plat *p = data;
    p->handlers.keyboard_lost(p->userdata);
}

static void way_keyboard_key(void *data, struct wl_keyboard *keyboard,
                             uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    Plat *p = data;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        p->handlers.key(p->userdata, way_key(key), PLAT_PRESSED);
        if (p->handlers.text && p->xkb_state) {
            char utf8[64];
            int length = xkb_state_key_get_utf8(p->xkb_state, key + 8,
                                                utf8, sizeof(utf8));
            if (length > 0 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F) {
                p->handlers.text(p->userdata, utf8);
            }
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        p->handlers.key(p->userdata, way_key(key), PLAT_RELEASED);
    }
}

static void way_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
                                   uint32_t mods_locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    Plat *p = data;
    if (p->xkb_state) {
        xkb_state_update_mask(p->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}

static void way_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                     int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener way_keyboard_listener = {
    .keymap = way_keyboard_keymap,
    .enter = way_keyboard_enter,
    .leave = way_keyboard_leave,
    .key = way_keyboard_key,
    .modifiers = way_keyboard_modifiers,
    .repeat_info = way_keyboard_repeat_info,
};

static void way_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    Plat *p = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !p->pointer) {
        p->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p->pointer, &way_pointer_listener, p);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !p->keyboard) {
        p->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(p->keyboard, &way_keyboard_listener, p);
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        if (p->keyboard) {
            wl_keyboard_destroy(p->keyboard);
            p->keyboard = NULL;
        }
        /* The keys held when it vanished will never see a release. */
        p->handlers.keyboard_lost(p->userdata);
    }
    /* The protocol asks clients to release the object whose capability has
       gone, the pointer included. */
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        if (p->pointer) {
            wl_pointer_destroy(p->pointer);
            p->pointer = NULL;
        }
        p->pointer_inside = false;
        p->handlers.pointer_lost(p->userdata);
    }
}

static void way_seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener way_seat_listener = {
    .capabilities = way_seat_capabilities,
    .name = way_seat_name,
};

static void way_registry_global(void *data, struct wl_registry *registry,
                                uint32_t name, const char *interface, uint32_t version)
{
    Plat *p = data;

    /* version is the highest the compositor supports, not the one to use.
       Asking for more than it offers is a protocol error, and the client is
       killed for it. */
    #define BIND_VERSION(wanted) ((version) < (wanted) ? (version) : (uint32_t)(wanted))

    if (strcmp(interface, wl_compositor_interface.name) == 0 && !p->compositor) {
        p->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                         BIND_VERSION(4));
        if (p->compositor) {
            p->compositor_name = name;
        }
    } else if (strcmp(interface, wl_shm_interface.name) == 0 && !p->shm) {
        p->shm = wl_registry_bind(registry, name, &wl_shm_interface, BIND_VERSION(1));
        if (p->shm) {
            p->shm_name = name;
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !p->seat) {
        /* One seat owns the single pointer/keyboard state. */
        p->seat = wl_registry_bind(registry, name, &wl_seat_interface, BIND_VERSION(1));
        if (p->seat) {
            p->seat_name = name;
            wl_seat_add_listener(p->seat, &way_seat_listener, p);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !p->output) {
        /* Only the first. A toy tracks a single output, and letting each new
           one overwrite it leaked the previous proxy and made the mode report
           that won -- and so target_fps -- depend on advertisement order on a
           mixed-refresh desktop. */
        p->output = wl_registry_bind(registry, name, &wl_output_interface,
                                     BIND_VERSION(2));
        if (p->output) {
            p->output_name = name;
            wl_output_add_listener(p->output, &way_output_listener, p);
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !p->wm_base) {
        p->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                      BIND_VERSION(1));
        if (p->wm_base) {
            p->wm_base_name = name;
            xdg_wm_base_add_listener(p->wm_base, &way_wm_base_listener, p);
        }
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 &&
               !p->deco_manager) {
        p->deco_manager = wl_registry_bind(registry, name,
                                           &zxdg_decoration_manager_v1_interface,
                                           BIND_VERSION(1));
    }

    #undef BIND_VERSION
}

static void way_registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    Plat *p = data;

    if (name == p->seat_name) {
        if (p->pointer) {
            wl_pointer_destroy(p->pointer);
            p->pointer = NULL;
        }
        if (p->keyboard) {
            wl_keyboard_destroy(p->keyboard);
            p->keyboard = NULL;
        }
        if (p->seat) {
            wl_seat_destroy(p->seat);
            p->seat = NULL;
        }
        p->seat_name = WAY_GLOBAL_NONE;
        p->pointer_inside = false;
        p->handlers.keyboard_lost(p->userdata);
        p->handlers.pointer_lost(p->userdata);
        return;
    }
    if (name == p->output_name) {
        if (p->output) {
            wl_output_destroy(p->output);
            p->output = NULL;
        }
        p->output_name = WAY_GLOBAL_NONE;
        p->refresh_mhz = 0;
        return;
    }
    if (name == p->shm_name) {
        if (p->shm) {
            wl_shm_destroy(p->shm);
            p->shm = NULL;
        }
        p->shm_name = WAY_GLOBAL_NONE;
        return;
    }
    if (name == p->compositor_name) {
        /* Its surfaces must die before the proxy. */
        p->compositor_name = WAY_GLOBAL_NONE;
        p->lost = true;
        return;
    }
    if (name == p->wm_base_name) {
        /* Destroying this with xdg_surfaces alive is a protocol error. */
        p->wm_base_name = WAY_GLOBAL_NONE;
        p->lost = true;
    }
}

static const struct wl_registry_listener way_registry_listener = {
    .global = way_registry_global,
    .global_remove = way_registry_remove,
};

static void way_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    Plat *p = data;
    if (p->frame_callback == callback) {
        p->frame_callback = NULL;
    }
    p->frame.delivered_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    p->frame.ready = true;
    p->frame.presented_ms = time;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener way_frame_listener = {
    .done = way_frame_done,
};

/* Hidden surfaces may withhold a callback indefinitely. */
static bool way_frame_can_request(const Plat *p)
{
    return p && !p->frame_callback;
}

static void way_frame_cancel(Plat *p)
{
    if (!p->frame_callback) {
        return;
    }

    wl_callback_destroy(p->frame_callback);
    p->frame_callback = NULL;
}

static void way_cursor_destroy(WayCursor *cursor)
{
    for (int f = 0; f < cursor->frame_count; f++) {
        if (cursor->buffers[f]) {
            wl_buffer_destroy(cursor->buffers[f]);
        }
    }
    free(cursor->buffers);
    if (cursor->surface) {
        wl_surface_destroy(cursor->surface);
    }
    if (cursor->map) {
        munmap(cursor->map, cursor->map_size);
    }
    memset(cursor, 0, sizeof(*cursor));
}

static bool way_cursor_fail(WayCursor *cursor)
{
    way_cursor_destroy(cursor);
    return false;
}

/* Destroy bound globals before disconnecting their display. */
static void way_globals_destroy(Plat *p)
{
    if (p->pointer) {
        wl_pointer_destroy(p->pointer);
        p->pointer = NULL;
    }
    if (p->keyboard) {
        wl_keyboard_destroy(p->keyboard);
        p->keyboard = NULL;
    }
    if (p->seat) {
        wl_seat_destroy(p->seat);
        p->seat = NULL;
    }
    if (p->output) {
        wl_output_destroy(p->output);
        p->output = NULL;
    }
    if (p->deco_manager) {
        zxdg_decoration_manager_v1_destroy(p->deco_manager);
        p->deco_manager = NULL;
    }
    if (p->wm_base) {
        xdg_wm_base_destroy(p->wm_base);
        p->wm_base = NULL;
    }
    if (p->shm) {
        wl_shm_destroy(p->shm);
        p->shm = NULL;
    }
    if (p->compositor) {
        wl_compositor_destroy(p->compositor);
        p->compositor = NULL;
    }
    if (p->registry) {
        wl_registry_destroy(p->registry);
        p->registry = NULL;
    }
    if (p->display) {
        wl_display_disconnect(p->display);
        p->display = NULL;
    }

    p->compositor_name = WAY_GLOBAL_NONE;
    p->shm_name = WAY_GLOBAL_NONE;
    p->seat_name = WAY_GLOBAL_NONE;
    p->output_name = WAY_GLOBAL_NONE;
    p->wm_base_name = WAY_GLOBAL_NONE;
}

static bool way_egl_open(Plat *p)
{
    p->egl_display = eglGetDisplay((EGLNativeDisplayType)p->display);
    if (p->egl_display == EGL_NO_DISPLAY || !eglInitialize(p->egl_display, NULL, NULL)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return false;
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
    EGLint egl_num = 0;
    if (!eglChooseConfig(p->egl_display, egl_attr, &p->egl_config, 1, &egl_num) || egl_num < 1) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return false;
    }

    p->egl_context = eglCreateContext(p->egl_display, p->egl_config, EGL_NO_CONTEXT,
                                      (EGLint[]) {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
    if (p->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }
    return true;
}

Plat *plat_open(const PlatConfig *config, const PlatHandlers *handlers, void *userdata)
{
    const char *wayland_display_env = getenv("WAYLAND_DISPLAY");
    if (!wayland_display_env || wayland_display_env[0] == '\0') {
        fprintf(stderr, "%s requires a Wayland compositor (WAYLAND_DISPLAY is not set).\n",
                config->app_id);
        return NULL;
    }

    Plat *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->handlers = *handlers;
    p->userdata = userdata;
    p->log = config->log;
    p->damage = config->damage;
    p->title = config->title;
    p->app_id = config->app_id;
    p->frame.ready = true;
    p->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!p->xkb_context) {
        plat_close(p);
        return NULL;
    }

    p->display = wl_display_connect(NULL);
    if (!p->display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        plat_close(p);
        return NULL;
    }

    p->registry = wl_display_get_registry(p->display);
    if (!p->registry) {
        fprintf(stderr, "Failed to get Wayland registry\n");
        plat_close(p);
        return NULL;
    }

    wl_registry_add_listener(p->registry, &way_registry_listener, p);
    wl_display_roundtrip(p->display);
    if (p->output) {
        wl_display_roundtrip(p->display);
    }

    if (!p->compositor || !p->wm_base) {
        fprintf(stderr, "Wayland compositor/wm_base missing\n");
        plat_close(p);
        return NULL;
    }

    if (!way_egl_open(p)) {
        plat_close(p);
        return NULL;
    }
    return p;
}

void plat_close(Plat *p)
{
    if (!p) {
        return;
    }

    way_frame_cancel(p);
    if (p->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_window) {
        wl_egl_window_destroy(p->egl_window);
    }
    if (p->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(p->egl_display, p->egl_context);
    }
    if (p->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(p->egl_display);
    }
    free(p->damage_rects);
    xkb_state_unref(p->xkb_state);
    xkb_keymap_unref(p->xkb_keymap);
    xkb_context_unref(p->xkb_context);

    /* Their surfaces and buffers must die before their globals and display. */
    for (int c = 0; c < p->cursor_count; c++) {
        way_cursor_destroy(&p->cursors[c]);
    }

    if (p->decoration) {
        zxdg_toplevel_decoration_v1_destroy(p->decoration);
    }
    if (p->xdg_toplevel) {
        xdg_toplevel_destroy(p->xdg_toplevel);
    }
    if (p->xdg_surface) {
        xdg_surface_destroy(p->xdg_surface);
    }
    if (p->surface) {
        wl_surface_destroy(p->surface);
    }
    way_globals_destroy(p);
    free(p);
}

void plat_output_size(const Plat *p, int *width, int *height)
{
    *width = p->output_width;
    *height = p->output_height;
}

int plat_refresh_mhz(const Plat *p)
{
    return p->refresh_mhz;
}

bool plat_window_create(Plat *p, int width, int height)
{
    p->surface = wl_compositor_create_surface(p->compositor);
    if (!p->surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        return false;
    }

    wl_surface_set_opaque_region(p->surface, NULL);

    p->xdg_surface = xdg_wm_base_get_xdg_surface(p->wm_base, p->surface);
    if (!p->xdg_surface) {
        fprintf(stderr, "Failed to create xdg_surface\n");
        return false;
    }

    xdg_surface_add_listener(p->xdg_surface, &way_xdg_surface_listener, p);
    p->xdg_toplevel = xdg_surface_get_toplevel(p->xdg_surface);
    if (!p->xdg_toplevel) {
        fprintf(stderr, "Failed to create xdg_toplevel\n");
        return false;
    }
    xdg_toplevel_add_listener(p->xdg_toplevel, &way_toplevel_listener, p);
    xdg_toplevel_set_title(p->xdg_toplevel, p->title);
    xdg_toplevel_set_app_id(p->xdg_toplevel, p->app_id);
    xdg_toplevel_set_maximized(p->xdg_toplevel);

    /* An overlay wants no title bar: ask the compositor to leave decoration
       to the client, which draws none. */
    if (p->deco_manager) {
        p->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(p->deco_manager,
                        p->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(p->decoration,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    p->egl_window = wl_egl_window_create(p->surface, width, height);
    if (!p->egl_window) {
        fprintf(stderr, "Failed to create EGL window\n");
        return false;
    }
    p->surface_height = height;

    p->egl_surface = eglCreateWindowSurface(p->egl_display, p->egl_config,
                                            (EGLNativeWindowType)p->egl_window, NULL);
    if (p->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL window surface\n");
        return false;
    }

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }
    eglSwapInterval(p->egl_display, 0);

    const char *egl_exts = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    if (egl_exts && p->damage == PLAT_DAMAGE_AUTO) {
        if (strstr(egl_exts, "EGL_KHR_swap_buffers_with_damage")) {
            p->swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                             eglGetProcAddress("eglSwapBuffersWithDamageKHR");
        } else if (strstr(egl_exts, "EGL_EXT_swap_buffers_with_damage")) {
            p->swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)
                             eglGetProcAddress("eglSwapBuffersWithDamageEXT");
        }
        p->has_buffer_age = strstr(egl_exts, "EGL_EXT_buffer_age") != NULL;
    }
    if (p->log == PLAT_LOG_DEBUG) {
        fprintf(stderr, "[egl] swap_with_damage=%s buffer_age=%s\n",
                p->swap_damage ? "yes" : "no",
                p->has_buffer_age ? "yes" : "no");
    }
    return true;
}

bool plat_window_show(Plat *p)
{
    wl_surface_commit(p->surface);
    while (!p->configured) {
        if (wl_display_dispatch(p->display) < 0) {
            break;
        }
    }
    if (!p->configured) {
        fprintf(stderr, "Never received the initial xdg_surface configure\n");
        return false;
    }
    return true;
}

int plat_cursor_create(Plat *p, const uint32_t *argb, int size,
                       int frame_count, int hot_x, int hot_y)
{
    if (!p->shm || !p->compositor || size <= 0 || frame_count <= 0 ||
            p->cursor_count >= WAY_CURSORS_MAX) {
        return -1;
    }

    WayCursor *cursor = &p->cursors[p->cursor_count];
    int stride = size * 4;
    size_t frame_bytes = (size_t)stride * (size_t)size;
    size_t total = frame_bytes * (size_t)frame_count;

    cursor->buffers = calloc((size_t)frame_count, sizeof(*cursor->buffers));
    if (!cursor->buffers) {
        return -1;
    }
    cursor->frame_count = frame_count;

    int fd = memfd_create("toy-cursor", MFD_CLOEXEC);
    if (fd < 0) {
        way_cursor_fail(cursor);
        return -1;
    }
    if (ftruncate(fd, (off_t)total) < 0) {
        close(fd);
        way_cursor_fail(cursor);
        return -1;
    }
    void *data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        way_cursor_fail(cursor);
        return -1;
    }
    cursor->map = data;
    cursor->map_size = total;
    memcpy(data, argb, total);

    struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, (int32_t)total);
    if (!pool) {
        close(fd);
        way_cursor_fail(cursor);
        return -1;
    }
    for (int f = 0; f < frame_count; f++) {
        cursor->buffers[f] = wl_shm_pool_create_buffer(
                                 pool, (int32_t)((size_t)f * frame_bytes),
                                 size, size, stride, WL_SHM_FORMAT_ARGB8888);
        if (!cursor->buffers[f]) {
            wl_shm_pool_destroy(pool);
            close(fd);
            way_cursor_fail(cursor);
            return -1;
        }
    }
    wl_shm_pool_destroy(pool);
    close(fd);

    cursor->surface = wl_compositor_create_surface(p->compositor);
    if (!cursor->surface) {
        way_cursor_fail(cursor);
        return -1;
    }
    wl_surface_attach(cursor->surface, cursor->buffers[0], 0, 0);
    wl_surface_damage(cursor->surface, 0, 0, size, size);
    wl_surface_commit(cursor->surface);
    cursor->size = size;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    cursor->current = 0;
    return p->cursor_count++;
}

void plat_cursor_use(Plat *p, int cursor)
{
    if (cursor < 0 || cursor >= p->cursor_count || cursor == p->cursor_in_use) {
        return;
    }
    p->cursor_in_use = cursor;
    way_cursor_show(p);
}

void plat_cursor_frame(Plat *p, int frame)
{
    if (p->cursor_count == 0) {
        return;
    }
    WayCursor *cursor = &p->cursors[p->cursor_in_use];
    if (frame < 0 || frame >= cursor->frame_count || frame == cursor->current) {
        return;
    }
    cursor->current = frame;
    wl_surface_attach(cursor->surface, cursor->buffers[frame], 0, 0);
    wl_surface_damage(cursor->surface, 0, 0, cursor->size, cursor->size);
    wl_surface_commit(cursor->surface);
}

void plat_cursor_update(Plat *p, int cursor_id, const uint32_t *argb)
{
    if (cursor_id < 0 || cursor_id >= p->cursor_count) {
        return;
    }
    WayCursor *cursor = &p->cursors[cursor_id];
    memcpy(cursor->map, argb, cursor->map_size);
    wl_surface_attach(cursor->surface, cursor->buffers[cursor->current], 0, 0);
    wl_surface_damage(cursor->surface, 0, 0, cursor->size, cursor->size);
    wl_surface_commit(cursor->surface);
}

void plat_surface_resize(Plat *p, int width, int height)
{
    wl_egl_window_resize(p->egl_window, width, height, 0, 0);
    p->surface_height = height;
}

void plat_input_region(Plat *p, const PlatRect *rects, int count)
{
    if (!p->compositor || !p->surface) {
        return;
    }

    struct wl_region *region = wl_compositor_create_region(p->compositor);
    if (!region) {
        return;
    }
    for (int i = 0; i < count; i++) {
        wl_region_add(region, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
    }
    wl_surface_set_input_region(p->surface, region);
    wl_region_destroy(region);
}

void plat_apply(Plat *p)
{
    if (p->surface) {
        wl_surface_commit(p->surface);
    }
}

/* Returns false once the connection is gone, so the caller can wind down
   instead of spinning on a dead socket. */
bool plat_pump(Plat *p, int timeout_ms)
{
    struct wl_display *display = p->display;
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

    int poll_result = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (poll_result < 0 && errno != EINTR) {
        wl_display_cancel_read(display);
        return false;
    }
    if (poll_result <= 0 || !(pfd.revents & POLLIN)) {
        wl_display_cancel_read(display);
    } else if (wl_display_read_events(display) < 0) {
        return false;
    }

    return wl_display_dispatch_pending(display) >= 0 && !p->lost;
}

const PlatFrame *plat_frame(const Plat *p)
{
    return &p->frame;
}

bool plat_frame_request(Plat *p)
{
    if (!p->surface || !way_frame_can_request(p)) {
        return false;
    }

    struct wl_callback *callback = wl_surface_frame(p->surface);
    if (!callback) {
        return false;
    }
    if (wl_callback_add_listener(callback, &way_frame_listener, p) < 0) {
        wl_callback_destroy(callback);
        return false;
    }

    p->frame_callback = callback;
    p->frame.ready = false;
    return true;
}

void plat_frame_poke(Plat *p)
{
    if (plat_frame_request(p)) {
        wl_surface_commit(p->surface);
        wl_display_flush(p->display);
    }
}

int plat_buffer_age(const Plat *p)
{
    if (!p->has_buffer_age) {
        return 0;
    }
    EGLint age = 0;
    if (!eglQuerySurface(p->egl_display, p->egl_surface, EGL_BUFFER_AGE_EXT, &age) || age < 1) {
        return 0;
    }
    return (int)age;
}

bool plat_has_damage(const Plat *p)
{
    return p->swap_damage != NULL;
}

static bool way_damage_reserve(Plat *p, int count)
{
    if (count <= p->damage_cap) {
        return true;
    }
    EGLint *grown = realloc(p->damage_rects, (size_t)count * 4 * sizeof(*grown));
    if (!grown) {
        return false;
    }
    p->damage_rects = grown;
    p->damage_cap = count;
    return true;
}

void plat_swap(Plat *p, const PlatRect *damage, int count)
{
    if (p->swap_damage && damage && count > 0 && way_damage_reserve(p, count)) {
        /* EGL counts rows from the bottom. */
        for (int i = 0; i < count; i++) {
            p->damage_rects[i * 4 + 0] = damage[i].x;
            p->damage_rects[i * 4 + 1] = p->surface_height - (damage[i].y + damage[i].h);
            p->damage_rects[i * 4 + 2] = damage[i].w;
            p->damage_rects[i * 4 + 3] = damage[i].h;
        }
        p->swap_damage(p->egl_display, p->egl_surface, p->damage_rects, count);
    } else {
        eglSwapBuffers(p->egl_display, p->egl_surface);
    }
    wl_display_flush(p->display);
}

int plat_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
