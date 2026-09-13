/*
 * Win32 implementation of platform.h.
 *
 * A toy is a transparent overlay over the work area. On Windows that takes
 * three pieces:
 *
 *   Pixels   ANGLE renders GL ES 2 on Direct3D 11 and presents through
 *            DirectComposition, which keeps the swap chain's premultiplied
 *            alpha. WS_EX_NOREDIRECTIONBITMAP: no GDI surface sits behind it.
 *
 *   Input    Windows has no input region. WS_EX_LAYERED | WS_EX_TRANSPARENT
 *            hides the window from the mouse, and every pump clears both
 *            flags while the cursor is inside the toy's region or a button
 *            is held. Clearing WS_EX_LAYERED matters when a valid region
 *            covers pixels whose rendered alpha is zero:
 *
 *                cursor over a sprite  ->  the window takes input
 *                cursor elsewhere      ->  input falls through to what is below
 *
 *   Pacing   Swaps wait for vsync. There are no frame callbacks, so a frame
 *            is ready whenever the window is not minimized.
 */
#define WIN32_LEAN_AND_MEAN
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <windowsx.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>

#define WIN_CLASS_NAME L"ToyOverlay"
/* A toy's .rc file puts its icon at this id. Without one, Windows' default. */
#define WIN_ICON_ID 1
#define WIN_TITLE_MAX 128
#define WIN_REGION_MIN_CAP 256
#define WIN_CURSORS_MAX 4
#define WIN_LOG_ROTATE_BYTES (4u * 1024 * 1024)
/* One wheel notch, in the axis units Wayland reports for it. */
#define WIN_WHEEL_UNITS 10.0
/* A click-through window gets no pointer messages, so poll for region entry. */
#define WIN_CURSOR_POLL_MS 16
#define WIN_EX_STYLE (WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | \
                      WS_EX_TRANSPARENT | WS_EX_APPWINDOW)

/* lParam bits of WM_KEYDOWN and WM_KEYUP. */
#define WIN_KEY_EXTENDED (1UL << 24)
#define WIN_KEY_WAS_DOWN (1UL << 30)

/* Set-1 scan codes. Like Linux key codes they name physical keys, so the
   bindings sit in the same place on every keyboard layout. */
enum {
    WIN_SC_ESC = 0x01,
    WIN_SC_1 = 0x02,
    WIN_SC_9 = 0x0A,
    WIN_SC_Q = 0x10,
    WIN_SC_E = 0x12,
    WIN_SC_T = 0x14,
    WIN_SC_P = 0x19,
    WIN_SC_LEFTBRACE = 0x1A,
    WIN_SC_RIGHTBRACE = 0x1B,
    WIN_SC_A = 0x1E,
    WIN_SC_C = 0x2E,
    WIN_SC_M = 0x32,
    WIN_SC_SPACE = 0x39,
    WIN_SC_UP = 0x48,
    WIN_SC_LEFT = 0x4B,
    WIN_SC_RIGHT = 0x4D,
    WIN_SC_DOWN = 0x50,
};

typedef enum {
    WIN_HIT_PASS,   /* WS_EX_TRANSPARENT: input goes to the window below */
    WIN_HIT_TAKE,
} WinHit;

typedef struct {
    HCURSOR *frames;
    int frame_count;
    int size;
    int hot_x;
    int hot_y;
} WinCursor;

struct Plat {
    PlatHandlers handlers;
    void *userdata;
    PlatLog log;
    wchar_t title[WIN_TITLE_MAX];

    HINSTANCE instance;
    bool class_registered;
    HWND hwnd;
    RECT work_area;
    int refresh_mhz;
    bool destroyed;
    bool swap_failed;

    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    PlatFrame frame;

    WinHit hit;
    bool pointer_inside;
    int pointer_x;
    int pointer_y;
    int buttons_held;

    PlatRect *region;
    int region_count;
    int region_cap;

    WinCursor cursors[WIN_CURSORS_MAX];
    int cursor_count;
    int cursor_in_use;
    int cursor_frame;
};

static PlatKey win_key(LPARAM lp)
{
    UINT scan = (UINT)((lp >> 16) & 0xFF);

    if (lp & WIN_KEY_EXTENDED) {
        switch (scan) {
        case WIN_SC_UP:
            return PLAT_KEY_UP;
        case WIN_SC_DOWN:
            return PLAT_KEY_DOWN;
        case WIN_SC_LEFT:
            return PLAT_KEY_LEFT;
        case WIN_SC_RIGHT:
            return PLAT_KEY_RIGHT;
        default:
            return PLAT_KEY_OTHER;
        }
    }

    if (scan >= WIN_SC_1 && scan <= WIN_SC_9) {
        return (PlatKey)(PLAT_KEY_1 + (int)(scan - WIN_SC_1));
    }

    switch (scan) {
    case WIN_SC_ESC:
        return PLAT_KEY_ESC;
    case WIN_SC_Q:
        return PLAT_KEY_Q;
    case WIN_SC_E:
        return PLAT_KEY_E;
    case WIN_SC_T:
        return PLAT_KEY_T;
    case WIN_SC_P:
        return PLAT_KEY_P;
    case WIN_SC_LEFTBRACE:
        return PLAT_KEY_LEFTBRACE;
    case WIN_SC_RIGHTBRACE:
        return PLAT_KEY_RIGHTBRACE;
    case WIN_SC_A:
        return PLAT_KEY_A;
    case WIN_SC_C:
        return PLAT_KEY_C;
    case WIN_SC_M:
        return PLAT_KEY_M;
    case WIN_SC_SPACE:
        return PLAT_KEY_SPACE;
    default:
        return PLAT_KEY_OTHER;
    }
}

static bool win_region_has(const Plat *p, int x, int y)
{
    for (int i = 0; i < p->region_count; i++) {
        const PlatRect *r = &p->region[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            return true;
        }
    }
    return false;
}

/* Let input through unless the cursor is over the region or a press is in
   progress. Polled, because Windows has nothing that would tell us. */
static void win_update_hit(Plat *p)
{
    if (!p->hwnd) {
        return;
    }

    POINT cursor;
    if (!GetCursorPos(&cursor) || !ScreenToClient(p->hwnd, &cursor)) {
        return;
    }

    bool take = p->buttons_held > 0 || win_region_has(p, cursor.x, cursor.y);
    WinHit hit = take ? WIN_HIT_TAKE : WIN_HIT_PASS;
    if (hit == p->hit) {
        return;
    }

    p->hit = hit;
    LONG_PTR style = GetWindowLongPtrW(p->hwnd, GWL_EXSTYLE);
    if (hit == WIN_HIT_TAKE) {
        style &= ~((LONG_PTR)WS_EX_TRANSPARENT | WS_EX_LAYERED);
    } else {
        style |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    }
    SetWindowLongPtrW(p->hwnd, GWL_EXSTYLE, style);
    if (hit == WIN_HIT_PASS) {
        SetLayeredWindowAttributes(p->hwnd, 0, 255, LWA_ALPHA);
    }
    SetWindowPos(p->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void win_fit_work_area(Plat *p)
{
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &p->work_area, 0);
    if (!p->hwnd) {
        return;
    }

    const RECT *area = &p->work_area;
    SetWindowPos(p->hwnd, NULL, area->left, area->top,
                 area->right - area->left, area->bottom - area->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The frame of the cursor in use, NULL when there is no cursor. */
static HCURSOR win_cursor_now(const Plat *p)
{
    if (p->cursor_count == 0) {
        return NULL;
    }
    const WinCursor *cursor = &p->cursors[p->cursor_in_use];
    return cursor->frames[p->cursor_frame % cursor->frame_count];
}

static void win_cursor_show(const Plat *p)
{
    HCURSOR cursor = win_cursor_now(p);
    if (cursor && p->pointer_inside && p->hit == WIN_HIT_TAKE) {
        SetCursor(cursor);
    }
}

/* Wayland sends enter once, with a position, then motion. Mirror that. */
static void win_pointer_at(Plat *p, int x, int y)
{
    if (!p->pointer_inside) {
        TRACKMOUSEEVENT track = {
            .cbSize = sizeof(track),
            .dwFlags = TME_LEAVE,
            .hwndTrack = p->hwnd,
        };
        TrackMouseEvent(&track);
        p->pointer_inside = true;
        p->pointer_x = x;
        p->pointer_y = y;
        p->handlers.pointer_enter(p->userdata, x, y);
        return;
    }

    /* Windows repeats WM_MOUSEMOVE for a cursor that has not moved. */
    if (x == p->pointer_x && y == p->pointer_y) {
        return;
    }
    p->pointer_x = x;
    p->pointer_y = y;
    p->handlers.pointer_motion(p->userdata, x, y);
}

/* Capture while any button is down, so a drag keeps its input off the
   region, as a Wayland implicit grab does. */
static void win_button(Plat *p, LPARAM lp, PlatButton button, PlatPress press)
{
    win_pointer_at(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

    if (press == PLAT_PRESSED) {
        if (p->buttons_held == 0) {
            SetCapture(p->hwnd);
        }
        p->buttons_held++;
    } else if (p->buttons_held > 0) {
        p->buttons_held--;
        if (p->buttons_held == 0) {
            ReleaseCapture();
        }
    }

    p->handlers.pointer_button(p->userdata, button, press);
}

static LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Plat *p = (Plat *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_MOUSEMOVE:
        win_pointer_at(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        p->pointer_inside = false;
        p->handlers.pointer_leave(p->userdata);
        return 0;
    case WM_LBUTTONDOWN:
        win_button(p, lp, PLAT_BTN_LEFT, PLAT_PRESSED);
        return 0;
    case WM_LBUTTONUP:
        win_button(p, lp, PLAT_BTN_LEFT, PLAT_RELEASED);
        return 0;
    case WM_RBUTTONDOWN:
        win_button(p, lp, PLAT_BTN_RIGHT, PLAT_PRESSED);
        return 0;
    case WM_RBUTTONUP:
        win_button(p, lp, PLAT_BTN_RIGHT, PLAT_RELEASED);
        return 0;
    case WM_MBUTTONDOWN:
        win_button(p, lp, PLAT_BTN_MIDDLE, PLAT_PRESSED);
        return 0;
    case WM_MBUTTONUP:
        win_button(p, lp, PLAT_BTN_MIDDLE, PLAT_RELEASED);
        return 0;
    case WM_XBUTTONDOWN:
        win_button(p, lp, PLAT_BTN_OTHER, PLAT_PRESSED);
        return TRUE;
    case WM_XBUTTONUP:
        win_button(p, lp, PLAT_BTN_OTHER, PLAT_RELEASED);
        return TRUE;
    case WM_MOUSEWHEEL: {
        double notches = (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        p->handlers.pointer_scroll(p->userdata, -notches * WIN_WHEEL_UNITS);
        return 0;
    }
    case WM_CAPTURECHANGED:
        /* Something took the mouse mid-press; the release will not come here. */
        if (p->buttons_held > 0 && (HWND)lp != hwnd) {
            p->buttons_held = 0;
            p->handlers.pointer_lost(p->userdata);
        }
        return 0;
    case WM_SETCURSOR: {
        HCURSOR cursor = win_cursor_now(p);
        if (LOWORD(lp) == HTCLIENT && cursor) {
            SetCursor(cursor);
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        /* The toys run their own key repeat, as they do under Wayland. */
        if (!(lp & WIN_KEY_WAS_DOWN)) {
            p->handlers.key(p->userdata, win_key(lp), PLAT_PRESSED);
        }
        return 0;
    case WM_KEYUP:
        p->handlers.key(p->userdata, win_key(lp), PLAT_RELEASED);
        return 0;
    case WM_KILLFOCUS:
        p->handlers.keyboard_lost(p->userdata);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0) {
            p->handlers.resize(p->userdata, LOWORD(lp), HIWORD(lp));
        }
        return 0;
    case WM_DISPLAYCHANGE:
        win_fit_work_area(p);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) {
            win_fit_work_area(p);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        p->handlers.close(p->userdata);
        return 0;
    case WM_DESTROY:
        p->destroyed = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A program launched from the Start menu or Explorer has no stderr. Append it
   to %LOCALAPPDATA%\<app_id>\session.log instead, as poingo-logged does on
   Linux; one previous generation is kept as session.log.1. */
static void win_log_to_file(const char *app_id)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != NULL && err != INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t dir[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    wchar_t app[WIN_TITLE_MAX];
    MultiByteToWideChar(CP_UTF8, 0, app_id, -1, app, WIN_TITLE_MAX);

    wchar_t log[MAX_PATH];
    wchar_t old[MAX_PATH];
    _snwprintf(dir + length, MAX_PATH - length, L"\\%ls", app);
    dir[MAX_PATH - 1] = L'\0';
    CreateDirectoryW(dir, NULL);
    _snwprintf(log, MAX_PATH, L"%ls\\session.log", dir);
    _snwprintf(old, MAX_PATH, L"%ls\\session.log.1", dir);
    log[MAX_PATH - 1] = L'\0';
    old[MAX_PATH - 1] = L'\0';

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(log, GetFileExInfoStandard, &info) &&
            (info.nFileSizeHigh > 0 || info.nFileSizeLow > WIN_LOG_ROTATE_BYTES)) {
        MoveFileExW(log, old, MOVEFILE_REPLACE_EXISTING);
    }

    if (!_wfreopen(log, L"a", stderr)) {
        return;
    }
    setvbuf(stderr, NULL, _IONBF, 0);

    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(stderr, "=== %04u-%02u-%02uT%02u:%02u:%02u start ===\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

static bool win_egl_open(Plat *p)
{
    EGLAttrib display_attr[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_NONE
    };
    p->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE,
                                           (void *)EGL_DEFAULT_DISPLAY, display_attr);
    if (p->egl_display == EGL_NO_DISPLAY || !eglInitialize(p->egl_display, NULL, NULL)) {
        fprintf(stderr, "Failed to initialize EGL (ANGLE on Direct3D 11)\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint config_attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLint config_count = 0;
    if (!eglChooseConfig(p->egl_display, config_attr, &p->egl_config, 1, &config_count) ||
            config_count < 1) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return false;
    }

    EGLint context_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    p->egl_context = eglCreateContext(p->egl_display, p->egl_config, EGL_NO_CONTEXT, context_attr);
    if (p->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return false;
    }

    if (p->log == PLAT_LOG_DEBUG) {
        fprintf(stderr, "[egl] %s %s\n", eglQueryString(p->egl_display, EGL_VENDOR),
                eglQueryString(p->egl_display, EGL_VERSION));
    }
    return true;
}

Plat *plat_open(const PlatConfig *config, const PlatHandlers *handlers, void *userdata)
{
    win_log_to_file(config->app_id);

    Plat *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->handlers = *handlers;
    p->userdata = userdata;
    p->log = config->log;
    p->instance = GetModuleHandleW(NULL);
    p->frame.ready = true;
    p->hit = WIN_HIT_PASS;
    MultiByteToWideChar(CP_UTF8, 0, config->title, -1, p->title, WIN_TITLE_MAX);

    /* Physical pixels. Otherwise Windows scales the overlay up and blurs it. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    win_fit_work_area(p);

    DEVMODEW mode = { .dmSize = sizeof(mode) };
    /* 0 and 1 both mean "the hardware default", which says nothing. */
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
        p->refresh_mhz = (int)mode.dmDisplayFrequency * 1000;
    }

    if (!win_egl_open(p)) {
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

    if (p->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (p->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(p->egl_display, p->egl_surface);
        }
        if (p->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(p->egl_display, p->egl_context);
        }
        eglTerminate(p->egl_display);
    }

    /* Detach first: the toy may already be half torn down, and destroying a
       focused window still sends it focus and capture messages. */
    if (p->hwnd) {
        SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(p->hwnd);
    }
    /* After the window, so none of these is the cursor on screen. */
    for (int c = 0; c < p->cursor_count; c++) {
        for (int f = 0; f < p->cursors[c].frame_count; f++) {
            DestroyCursor(p->cursors[c].frames[f]);
        }
        free(p->cursors[c].frames);
    }
    if (p->class_registered) {
        UnregisterClassW(WIN_CLASS_NAME, p->instance);
    }
    free(p->region);
    free(p);
}

void plat_output_size(const Plat *p, int *width, int *height)
{
    *width = p->work_area.right - p->work_area.left;
    *height = p->work_area.bottom - p->work_area.top;
}

int plat_refresh_mhz(const Plat *p)
{
    return p->refresh_mhz;
}

bool plat_window_create(Plat *p, int width, int height)
{
    /* The work area decides the size, as maximizing does under Wayland. */
    (void)width;
    (void)height;

    WNDCLASSEXW window_class = {
        .cbSize = sizeof(window_class),
        .lpfnWndProc = win_proc,
        .hInstance = p->instance,
        .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
        .hIcon = LoadIconW(p->instance, MAKEINTRESOURCEW(WIN_ICON_ID)),
        .lpszClassName = WIN_CLASS_NAME,
    };
    if (!RegisterClassExW(&window_class)) {
        fprintf(stderr, "Failed to register the window class\n");
        return false;
    }
    p->class_registered = true;

    const RECT *area = &p->work_area;
    p->hwnd = CreateWindowExW(WIN_EX_STYLE, WIN_CLASS_NAME, p->title, WS_POPUP,
                              area->left, area->top,
                              area->right - area->left, area->bottom - area->top,
                              NULL, NULL, p->instance, p);
    if (!p->hwnd) {
        fprintf(stderr, "Failed to create the window\n");
        return false;
    }
    /* Layered only for click-through; the swap chain carries the alpha. */
    SetLayeredWindowAttributes(p->hwnd, 0, 255, LWA_ALPHA);

    EGLint surface_attr[] = { EGL_DIRECT_COMPOSITION_ANGLE, EGL_TRUE, EGL_NONE };
    p->egl_surface = eglCreateWindowSurface(p->egl_display, p->egl_config,
                                            (EGLNativeWindowType)p->hwnd, surface_attr);
    if (p->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL window surface\n");
        return false;
    }

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }
    eglSwapInterval(p->egl_display, 1);
    return true;
}

bool plat_window_show(Plat *p)
{
    ShowWindow(p->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(p->hwnd);

    RECT client;
    if (!GetClientRect(p->hwnd, &client)) {
        fprintf(stderr, "Failed to read the window size\n");
        return false;
    }
    p->handlers.resize(p->userdata, client.right - client.left, client.bottom - client.top);
    return true;
}

static uint32_t win_unpremultiply(uint32_t channel, uint32_t alpha)
{
    uint32_t value = (channel * 255 + alpha / 2) / alpha;
    return value > 255 ? 255 : value;
}

static HCURSOR win_cursor_make(const uint32_t *argb, int size, int hot_x, int hot_y)
{
    BITMAPV5HEADER header = {
        .bV5Size = sizeof(header),
        .bV5Width = size,
        .bV5Height = -size,     /* top-down, like the source */
        .bV5Planes = 1,
        .bV5BitCount = 32,
        .bV5Compression = BI_BITFIELDS,
        .bV5RedMask = 0x00FF0000,
        .bV5GreenMask = 0x0000FF00,
        .bV5BlueMask = 0x000000FF,
        .bV5AlphaMask = 0xFF000000,
    };
    HDC screen = GetDC(NULL);
    void *bits = NULL;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&header, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color) {
        return NULL;
    }

    /* Cursors take straight alpha. */
    uint32_t *dst = bits;
    for (int i = 0; i < size * size; i++) {
        uint32_t pixel = argb[i];
        uint32_t alpha = pixel >> 24;
        if (alpha == 0) {
            dst[i] = 0;
            continue;
        }
        uint32_t r = win_unpremultiply((pixel >> 16) & 0xFF, alpha);
        uint32_t g = win_unpremultiply((pixel >> 8) & 0xFF, alpha);
        uint32_t b = win_unpremultiply(pixel & 0xFF, alpha);
        dst[i] = (alpha << 24) | (r << 16) | (g << 8) | b;
    }

    /* Ignored beside 32-bit colour, but CreateIconIndirect insists on one.
       Monochrome rows are padded to 16 bits. */
    size_t mask_bytes = (size_t)((size + 15) / 16 * 2) * (size_t)size;
    uint8_t *mask_bits = calloc(mask_bytes, 1);
    HBITMAP mask = mask_bits ? CreateBitmap(size, size, 1, 1, mask_bits) : NULL;
    free(mask_bits);

    HCURSOR cursor = NULL;
    if (mask) {
        ICONINFO info = {
            .fIcon = FALSE,
            .xHotspot = (DWORD)hot_x,
            .yHotspot = (DWORD)hot_y,
            .hbmMask = mask,
            .hbmColor = color,
        };
        cursor = (HCURSOR)CreateIconIndirect(&info);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return cursor;
}

int plat_cursor_create(Plat *p, const uint32_t *argb, int size,
                       int frame_count, int hot_x, int hot_y)
{
    if (size <= 0 || frame_count <= 0 || p->cursor_count >= WIN_CURSORS_MAX) {
        return -1;
    }

    WinCursor *cursor = &p->cursors[p->cursor_count];
    cursor->frames = calloc((size_t)frame_count, sizeof(*cursor->frames));
    if (!cursor->frames) {
        return -1;
    }

    size_t frame_pixels = (size_t)size * (size_t)size;
    for (int f = 0; f < frame_count; f++) {
        cursor->frames[f] = win_cursor_make(argb + (size_t)f * frame_pixels, size, hot_x, hot_y);
        if (cursor->frames[f]) {
            continue;
        }
        for (int i = 0; i < f; i++) {
            DestroyCursor(cursor->frames[i]);
        }
        free(cursor->frames);
        cursor->frames = NULL;
        return -1;
    }
    cursor->frame_count = frame_count;
    cursor->size = size;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    return p->cursor_count++;
}

void plat_cursor_update(Plat *p, int cursor_id, const uint32_t *argb)
{
    if (cursor_id < 0 || cursor_id >= p->cursor_count) {
        return;
    }
    WinCursor *cursor = &p->cursors[cursor_id];
    HCURSOR *old = calloc((size_t)cursor->frame_count, sizeof(*old));
    if (!old) {
        return;
    }

    /* A cursor's pixels are fixed once made, so each frame is made again. The
       old ones go only after the new ones are showing. */
    size_t frame_pixels = (size_t)cursor->size * (size_t)cursor->size;
    for (int f = 0; f < cursor->frame_count; f++) {
        HCURSOR fresh = win_cursor_make(argb + (size_t)f * frame_pixels, cursor->size,
                                        cursor->hot_x, cursor->hot_y);
        if (!fresh) {
            continue;
        }
        old[f] = cursor->frames[f];
        cursor->frames[f] = fresh;
    }
    if (cursor_id == p->cursor_in_use) {
        win_cursor_show(p);
    }
    for (int f = 0; f < cursor->frame_count; f++) {
        if (old[f]) {
            DestroyCursor(old[f]);
        }
    }
    free(old);
}

void plat_cursor_use(Plat *p, int cursor)
{
    if (cursor < 0 || cursor >= p->cursor_count || cursor == p->cursor_in_use) {
        return;
    }
    p->cursor_in_use = cursor;
    p->cursor_frame = 0;
    win_cursor_show(p);
}

void plat_cursor_frame(Plat *p, int frame)
{
    if (p->cursor_count == 0 || frame < 0 ||
            frame >= p->cursors[p->cursor_in_use].frame_count || frame == p->cursor_frame) {
        return;
    }
    p->cursor_frame = frame;
    win_cursor_show(p);
}

void plat_surface_resize(Plat *p, int width, int height)
{
    /* ANGLE resizes the swap chain to the client area on the next swap. */
    (void)p;
    (void)width;
    (void)height;
}

void plat_input_region(Plat *p, const PlatRect *rects, int count)
{
    if (count > p->region_cap) {
        int cap = p->region_cap ? p->region_cap : WIN_REGION_MIN_CAP;
        while (cap < count) {
            cap *= 2;
        }
        PlatRect *grown = realloc(p->region, (size_t)cap * sizeof(*grown));
        if (!grown) {
            /* Keep the previous region rather than none. */
            return;
        }
        p->region = grown;
        p->region_cap = cap;
    }
    if (count > 0) {
        memcpy(p->region, rects, (size_t)count * sizeof(*rects));
    }
    p->region_count = count;
    win_update_hit(p);
}

void plat_apply(Plat *p)
{
    /* Win32 state takes effect as it is set. */
    (void)p;
}

bool plat_pump(Plat *p, int timeout_ms)
{
    bool minimized = p->hwnd && IsIconic(p->hwnd);
    p->frame.ready = !minimized;

    /* A draw passes zero and is paced by its swap. Otherwise wait here so a
       static toy does not spin. Poll a visible click-through window because
       its thread receives no mouse message when the pointer enters a region. */
    if (timeout_ms != 0) {
        DWORD wait = timeout_ms < 0
                         ? (minimized ? INFINITE : WIN_CURSOR_POLL_MS)
                         : (DWORD)timeout_ms;
        MsgWaitForMultipleObjectsEx(0, NULL, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            p->destroyed = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    win_update_hit(p);
    return !p->destroyed;
}

const PlatFrame *plat_frame(const Plat *p)
{
    return &p->frame;
}

bool plat_frame_request(Plat *p)
{
    p->frame.ready = !(p->hwnd && IsIconic(p->hwnd));
    return p->frame.ready;
}

void plat_frame_poke(Plat *p)
{
    (void)plat_frame_request(p);
}

int plat_buffer_age(const Plat *p)
{
    /* A DirectComposition swap chain never reports it. */
    (void)p;
    return 0;
}

bool plat_has_damage(const Plat *p)
{
    (void)p;
    return false;
}

void plat_swap(Plat *p, const PlatRect *damage, int count)
{
    /* DirectComposition presents the whole buffer. */
    (void)damage;
    (void)count;
    if (eglSwapBuffers(p->egl_display, p->egl_surface) || p->swap_failed) {
        return;
    }
    p->swap_failed = true;
    fprintf(stderr, "eglSwapBuffers failed: 0x%04x\n", (unsigned)eglGetError());
}

int plat_cpu_count(void)
{
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 ? (int)count : 1;
}
