/*
 * SerenityOS implementation of platform.h.
 *
 * A toy is a transparent overlay over the desktop, less the taskbar. On
 * SerenityOS that takes three pieces:
 *
 *   Pixels   gles2_soft.c draws GL ES 2 on the CPU into a premultiplied
 *            buffer. A swap marks the damage; the widget's paint copies it,
 *            unpremultiplied, into a frameless alpha window.
 *
 *   Input    WindowServer has no input region; it hit-tests a window by pixel
 *            alpha against a threshold. Global mouse tracking follows the
 *            cursor, and the threshold switches with it:
 *
 *                cursor inside the region, or a button held  ->  0.0: take all
 *                cursor elsewhere                            ->  1.0: only
 *                                                                opaque pixels
 *
 *   Pacing   No vsync reaches clients. WindowServer's display link is the
 *            nearest thing: a tick every 16 ms from the loop that composes.
 *            A frame is ready on the tick after it was requested, and a swap
 *            paints at once instead of waiting for WindowServer to ask:
 *
 *                tick -> draw -> swap: paint, flip -> WindowServer composes
 *
 *            Frames paced this way reach the compositor one per tick, so two
 *            rarely land in one compose and overwrite each other.
 */
extern "C" {
#include "gles2_soft.h"
#include "platform.h"
}

#include <AK/FixedArray.h>
#include <AK/HashTable.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/DisplayLink.h>
#include <LibGUI/Event.h>
#include <LibGUI/MouseTracker.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Services/Taskbar/TaskbarWindow.h: taskbar_height(). Not a client API. */
static constexpr int SER_TASKBAR_HEIGHT = 27;
/* Services/WindowServer/Compositor.cpp: display links tick every 1000 / 60 ms,
   which the integer division makes 16 ms. */
static constexpr int SER_REFRESH_MHZ = 62500;
/* Without a tick for this long (no display link), frames fall back to a timer. */
static constexpr u64 SER_TICK_TIMEOUT_NS = 50'000'000;
static constexpr u64 SER_NS_PER_MS = 1'000'000;
static constexpr u64 SER_NS_PER_SEC = 1'000'000'000;
/* One wheel notch, in the axis units Wayland reports for it. */
static constexpr double SER_WHEEL_UNITS = 10.0;
static constexpr float SER_HIT_TAKE = 0.0f;
static constexpr float SER_HIT_PASS = 1.0f;

enum class SerHit {
    Pass,
    Take,
};

struct SerCursor {
    Vector<NonnullRefPtr<Gfx::Bitmap const>> frames;
    int size { 0 };
    int hot_x { 0 };
    int hot_y { 0 };
};

class SerSurface;
class SerTracker;

struct Plat {
    PlatHandlers handlers {};
    void *userdata { nullptr };
    PlatLog log { PLAT_LOG_QUIET };
    ByteString title;

    RefPtr<GUI::Application> app;
    RefPtr<GUI::Window> window;
    RefPtr<SerSurface> surface;
    OwnPtr<SerTracker> tracker;
    Gfx::IntRect work_area;
    bool closed { false };

    Vector<u32> pixels;
    int width { 0 };
    int height { 0 };

    PlatFrame frame {};
    i32 display_link { 0 };
    u64 last_tick_ns { 0 };
    u64 tick_count { 0 };

    Vector<PlatRect> region;
    SerHit hit { SerHit::Pass };
    Gfx::IntPoint cursor_screen;
    bool pointer_inside { false };
    int buttons_held { 0 };
    HashTable<PlatKey> keys_held;

    Vector<SerCursor> cursors;
    int cursor_in_use { 0 };
    int cursor_frame { 0 };

    /* PLAT_LOG_DEBUG only: frame timeline, see ser_trace(). */
    FILE *trace { nullptr };
    u64 frame_seq { 0 };
};

static u64 ser_now_ns()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * SER_NS_PER_SEC + (u64)now.tv_nsec;
}

/* One line per frame event, CLOCK_MONOTONIC ns, for matching against
   WindowServer's own trace:
     T <t> <tick>       display link tick
     R <t> <tick>       frame requested after this tick
     Y <t> <tick>       frame became ready on this tick
     D <t> <seq>        frame seq drawn and swapped
     P <t> <seq>        paint copied seq into the backing store */
static void ser_trace(Plat &p, char kind, u64 value)
{
    if(!p.trace) {
        return;
    }
    fprintf(p.trace, "%c %llu %llu\n", kind, (unsigned long long)ser_now_ns(), (unsigned long long)value);
}


static void ser_display_tick(Plat &p);

static bool ser_region_has(Plat const &p, Gfx::IntPoint point)
{
    for(auto const &rect : p.region) {
        if(point.x() >= rect.x && point.x() < rect.x + rect.w && point.y() >= rect.y && point.y() < rect.y + rect.h) {
            return true;
        }
    }
    return false;
}

/* Let input through unless the cursor is over the region or a press is in
   progress. See the header comment. */
static void ser_update_hit(Plat &p)
{
    if(!p.window) {
        return;
    }

    auto local = p.cursor_screen - p.window->position();
    bool take = p.buttons_held > 0 || ser_region_has(p, local);
    SerHit hit = take ? SerHit::Take : SerHit::Pass;
    if(hit == p.hit) {
        return;
    }

    p.hit = hit;
    p.window->set_alpha_hit_threshold(take ? SER_HIT_TAKE : SER_HIT_PASS);
}

static void ser_cursor_show(Plat &p)
{
    /* Before show() the window has no server id, and WindowServer drops a
       client that sets a cursor on it. plat_window_show() applies it. */
    if(!p.window || !p.window->is_visible() || p.cursors.is_empty()) {
        return;
    }
    auto const &cursor = p.cursors[p.cursor_in_use];
    p.window->set_cursor(cursor.frames[p.cursor_frame % cursor.frames.size()]);
}

static PlatKey ser_key(KeyCode key)
{
    if(key >= Key_1 && key <= Key_9) {
        return (PlatKey)(PLAT_KEY_1 + (key - Key_1));
    }

    switch(key) {
    case Key_E:
        return PLAT_KEY_E;
    case Key_T:
        return PLAT_KEY_T;
    case Key_Q:
        return PLAT_KEY_Q;
    case Key_Escape:
        return PLAT_KEY_ESC;
    case Key_Return:
        return PLAT_KEY_ENTER;
    case Key_Backspace:
        return PLAT_KEY_BACKSPACE;
    case Key_Delete:
        return PLAT_KEY_DELETE;
    case Key_M:
        return PLAT_KEY_M;
    case Key_A:
        return PLAT_KEY_A;
    case Key_P:
        return PLAT_KEY_P;
    case Key_C:
        return PLAT_KEY_C;
    case Key_Space:
        return PLAT_KEY_SPACE;
    case Key_LeftBracket:
        return PLAT_KEY_LEFTBRACE;
    case Key_RightBracket:
        return PLAT_KEY_RIGHTBRACE;
    case Key_Up:
        return PLAT_KEY_UP;
    case Key_Down:
        return PLAT_KEY_DOWN;
    case Key_Left:
        return PLAT_KEY_LEFT;
    case Key_Right:
        return PLAT_KEY_RIGHT;
    default:
        return PLAT_KEY_OTHER;
    }
}

static PlatButton ser_button(GUI::MouseButton button)
{
    switch(button) {
    case GUI::MouseButton::Primary:
        return PLAT_BTN_LEFT;
    case GUI::MouseButton::Secondary:
        return PLAT_BTN_RIGHT;
    case GUI::MouseButton::Middle:
        return PLAT_BTN_MIDDLE;
    default:
        return PLAT_BTN_OTHER;
    }
}

/* Every screen move, wherever the cursor is: how a click-through window
   learns the cursor has entered its region. */
class SerTracker final : public GUI::MouseTracker
{
public:
    explicit SerTracker(Plat &p)
        : m_plat(p)
    {
    }

private:
    virtual void track_mouse_move(Gfx::IntPoint point) override
    {
        m_plat.cursor_screen = point;
        ser_update_hit(m_plat);
    }

    Plat &m_plat;
};

class SerSurface final : public GUI::Widget
{
    C_OBJECT(SerSurface);

public:
    Plat *plat { nullptr };

private:
    SerSurface()
    {
        set_focus_policy(GUI::FocusPolicy::StrongFocus);
    }

    /* Straight alpha from premultiplied: channel * 255 / alpha, by table. */
    static u32 unpremultiply(u32 pixel)
    {
        static u32 const *reciprocals = [] {
            static u32 table[256] {};
            for(u32 alpha = 1; alpha < 256; alpha++)
            {
                table[alpha] = ((255u << 16) + alpha / 2) / alpha;
            }
            return table;
        }();

        u32 alpha = pixel >> 24;
        if(alpha == 0xFF) {
            return pixel;
        }
        if(alpha == 0) {
            return 0;
        }

        u32 scale = reciprocals[alpha];
        u32 r = min(255u, (((pixel >> 16) & 0xFF) * scale + 0x8000) >> 16);
        u32 g = min(255u, (((pixel >> 8) & 0xFF) * scale + 0x8000) >> 16);
        u32 b = min(255u, ((pixel & 0xFF) * scale + 0x8000) >> 16);
        return alpha << 24 | r << 16 | g << 8 | b;
    }

    virtual void paint_event(GUI::PaintEvent &event) override
    {
        GUI::Painter painter(*this);
        auto &target = painter.target();
        auto area = event.rect().intersected(rect()).intersected({ 0, 0, plat->width, plat->height });
        auto offset = painter.translation();

        for(int y = area.top(); y < area.bottom(); y++) {
            int target_y = y + offset.y();
            if(target_y < 0 || target_y >= target.height()) {
                continue;
            }
            u32 const *src = plat->pixels.data() + (size_t)y * (size_t)plat->width;
            u32 *dst = target.scanline(target_y);
            for(int x = area.left(); x < area.right(); x++) {
                int target_x = x + offset.x();
                if(target_x < 0 || target_x >= target.width()) {
                    continue;
                }
                dst[target_x] = unpremultiply(src[x]);
            }
        }
        ser_trace(*plat, 'P', plat->frame_seq);
    }

    void pointer_at(Gfx::IntPoint position)
    {
        auto &h = plat->handlers;
        if(!plat->pointer_inside) {
            plat->pointer_inside = true;
            if(h.pointer_enter) {
                h.pointer_enter(plat->userdata, position.x(), position.y());
            }
            return;
        }
        if(h.pointer_motion) {
            h.pointer_motion(plat->userdata, position.x(), position.y());
        }
    }

    virtual void mousemove_event(GUI::MouseEvent &event) override
    {
        pointer_at(event.position());
    }

    virtual void leave_event(Core::Event &) override
    {
        if(!plat->pointer_inside) {
            return;
        }
        plat->pointer_inside = false;
        if(plat->handlers.pointer_leave) {
            plat->handlers.pointer_leave(plat->userdata);
        }
    }

    virtual void mousedown_event(GUI::MouseEvent &event) override
    {
        pointer_at(event.position());
        plat->buttons_held++;
        if(plat->handlers.pointer_button) {
            plat->handlers.pointer_button(plat->userdata, ser_button(event.button()), PLAT_PRESSED);
        }
        ser_update_hit(*plat);
    }

    virtual void mouseup_event(GUI::MouseEvent &event) override
    {
        plat->buttons_held = max(0, plat->buttons_held - 1);
        if(plat->handlers.pointer_button) {
            plat->handlers.pointer_button(plat->userdata, ser_button(event.button()), PLAT_RELEASED);
        }
        ser_update_hit(*plat);
    }

    virtual void mousewheel_event(GUI::MouseEvent &event) override
    {
        if(plat->handlers.pointer_scroll && event.wheel_delta_y() != 0) {
            plat->handlers.pointer_scroll(plat->userdata, event.wheel_delta_y() * SER_WHEEL_UNITS);
        }
    }

    virtual void keydown_event(GUI::KeyEvent &event) override
    {
        auto &h = plat->handlers;
        PlatKey key = ser_key(event.key());
        /* WindowServer repeats held keys; Wayland does not. */
        if(plat->keys_held.set(key) == HashSetResult::KeptExistingEntry) {
            return;
        }
        if(h.key) {
            h.key(plat->userdata, key, PLAT_PRESSED);
        }

        u32 code_point = event.code_point();
        if(!h.text || code_point < 0x20 || code_point == 0x7F) {
            return;
        }
        auto text = event.text();
        h.text(plat->userdata, text.characters());
    }

    virtual void keyup_event(GUI::KeyEvent &event) override
    {
        PlatKey key = ser_key(event.key());
        plat->keys_held.remove(key);
        if(plat->handlers.key) {
            plat->handlers.key(plat->userdata, key, PLAT_RELEASED);
        }
    }

    virtual void focusout_event(GUI::FocusEvent &) override
    {
        plat->keys_held.clear();
        if(plat->handlers.keyboard_lost) {
            plat->handlers.keyboard_lost(plat->userdata);
        }
    }
};

Plat *plat_open(PlatConfig const *config, PlatHandlers const *handlers, void *userdata)
{
    static char program_name[] = "toy";
    static char *argv[] = { program_name, nullptr };
    static StringView strings[] = { "toy"sv };
    Main::Arguments arguments { 1, argv, strings };

    auto app_or_error = GUI::Application::create(arguments);
    if(app_or_error.is_error()) {
        warnln("Failed to connect to WindowServer: {}", app_or_error.error());
        return nullptr;
    }

    auto *p = new(nothrow) Plat;
    if(!p) {
        return nullptr;
    }
    p->app = app_or_error.release_value();
    p->handlers = *handlers;
    p->userdata = userdata;
    p->log = config->log;
    p->title = config->title ? config->title : "";

    p->work_area = GUI::Desktop::the().rect();
    p->work_area.set_height(max(0, p->work_area.height() - SER_TASKBAR_HEIGHT));
    p->frame.ready = true;

    if(p->log == PLAT_LOG_DEBUG) {
        auto path = ByteString::formatted("/tmp/poingo_trace_{}.log", getpid());
        p->trace = fopen(path.characters(), "w");
        if(p->trace) {
            warnln("[debug] frame trace:  {}", path);
        }
    }
    return p;
}

void plat_close(Plat *p)
{
    if(!p) {
        return;
    }
    soft_gl_target(nullptr, 0, 0);
    if(p->display_link) {
        GUI::DisplayLink::unregister_callback(p->display_link);
    }
    if(p->trace) {
        fclose(p->trace);
    }
    p->tracker = nullptr;
    if(p->window) {
        p->window->close();
    }
    p->surface = nullptr;
    p->window = nullptr;
    p->app = nullptr;
    delete p;
}

void plat_output_size(Plat const *p, int *width, int *height)
{
    *width = p->work_area.width();
    *height = p->work_area.height();
}

int plat_refresh_mhz(Plat const *)
{
    return SER_REFRESH_MHZ;
}

void plat_surface_resize(Plat *p, int width, int height)
{
    if(width <= 0 || height <= 0) {
        return;
    }
    size_t count = (size_t)width * (size_t)height;
    if(p->pixels.try_resize(count).is_error()) {
        warnln("Failed to allocate a {}x{} surface", width, height);
        return;
    }
    memset(p->pixels.data(), 0, count * sizeof(u32));
    p->width = width;
    p->height = height;
    soft_gl_target(p->pixels.data(), width, height);
}

bool plat_window_create(Plat *p, int width, int height)
{
    /* The work area decides the size, as maximizing does under Wayland. */
    (void)width;
    (void)height;

    auto window = GUI::Window::construct();
    window->set_title(p->title);
    window->set_frameless(true);
    window->set_resizable(false);
    window->set_has_alpha_channel(true);
    window->set_alpha_hit_threshold(SER_HIT_PASS);
    window->set_rect(p->work_area);

    auto surface = window->set_main_widget<SerSurface>();
    surface->plat = p;
    window->on_close_request = [p] {
        if(p->handlers.close)
        {
            p->handlers.close(p->userdata);
        }
        return GUI::Window::CloseRequestDecision::StayOpen;
    };

    p->window = window;
    p->surface = surface;
    p->tracker = make<SerTracker>(*p);
    plat_surface_resize(p, p->work_area.width(), p->work_area.height());
    return p->width > 0;
}

bool plat_window_show(Plat *p)
{
    p->window->show();
    p->window->set_always_on_top();
    p->last_tick_ns = ser_now_ns();
    p->display_link = GUI::DisplayLink::register_callback([p](i32) {
        ser_display_tick(*p);
    });
    p->surface->set_focus(true);
    ser_cursor_show(*p);
    if(p->handlers.resize) {
        p->handlers.resize(p->userdata, p->width, p->height);
    }
    return true;
}

/* Serenity puts a custom cursor's hotspot at the bitmap's centre, so pad the
   frame until its centre is the hotspot. */
static RefPtr<Gfx::Bitmap> ser_cursor_make(u32 const *argb, int size, int hot_x, int hot_y)
{
    int reach = max(max(hot_x, size - hot_x), max(hot_y, size - hot_y));
    int side = reach * 2;
    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, { side, side });
    if(bitmap_or_error.is_error()) {
        return nullptr;
    }
    auto bitmap = bitmap_or_error.release_value();
    bitmap->fill(Gfx::Color::Transparent);

    int left = reach - hot_x;
    int top = reach - hot_y;
    for(int y = 0; y < size; y++) {
        for(int x = 0; x < size; x++) {
            u32 pixel = argb[y * size + x];
            u32 alpha = pixel >> 24;
            if(alpha == 0) {
                continue;
            }
            u32 r = min(255u, (((pixel >> 16) & 0xFF) * 255 + alpha / 2) / alpha);
            u32 g = min(255u, (((pixel >> 8) & 0xFF) * 255 + alpha / 2) / alpha);
            u32 b = min(255u, ((pixel & 0xFF) * 255 + alpha / 2) / alpha);
            bitmap->scanline(top + y)[left + x] = alpha << 24 | r << 16 | g << 8 | b;
        }
    }
    return bitmap;
}

static bool ser_cursor_fill(SerCursor &cursor, u32 const *argb, int frame_count)
{
    Vector<NonnullRefPtr<Gfx::Bitmap const>> frames;
    size_t frame_pixels = (size_t)cursor.size * (size_t)cursor.size;
    for(int f = 0; f < frame_count; f++) {
        auto bitmap = ser_cursor_make(argb + (size_t)f * frame_pixels, cursor.size, cursor.hot_x, cursor.hot_y);
        if(!bitmap) {
            return false;
        }
        frames.append(bitmap.release_nonnull());
    }
    cursor.frames = move(frames);
    return true;
}

int plat_cursor_create(Plat *p, uint32_t const *argb, int size, int frame_count, int hot_x, int hot_y)
{
    if(size <= 0 || frame_count <= 0) {
        return -1;
    }

    SerCursor cursor;
    cursor.size = size;
    cursor.hot_x = hot_x;
    cursor.hot_y = hot_y;
    if(!ser_cursor_fill(cursor, argb, frame_count)) {
        return -1;
    }

    p->cursors.append(move(cursor));
    if(p->cursors.size() == 1) {
        ser_cursor_show(*p);
    }
    return (int)p->cursors.size() - 1;
}

void plat_cursor_update(Plat *p, int cursor_id, uint32_t const *argb)
{
    if(cursor_id < 0 || cursor_id >= (int)p->cursors.size()) {
        return;
    }
    auto &cursor = p->cursors[cursor_id];
    if(!ser_cursor_fill(cursor, argb, (int)cursor.frames.size())) {
        return;
    }
    if(cursor_id == p->cursor_in_use) {
        ser_cursor_show(*p);
    }
}

void plat_cursor_use(Plat *p, int cursor)
{
    if(cursor < 0 || cursor >= (int)p->cursors.size() || cursor == p->cursor_in_use) {
        return;
    }
    p->cursor_in_use = cursor;
    p->cursor_frame = 0;
    ser_cursor_show(*p);
}

void plat_cursor_frame(Plat *p, int frame)
{
    if(p->cursors.is_empty() || frame < 0 || frame >= (int)p->cursors[p->cursor_in_use].frames.size()
            || frame == p->cursor_frame) {
        return;
    }
    p->cursor_frame = frame;
    ser_cursor_show(*p);
}

void plat_input_region(Plat *p, PlatRect const *rects, int count)
{
    p->region.clear_with_capacity();
    if(count > 0 && p->region.try_append(rects, (size_t)count).is_error()) {
        return;
    }
    ser_update_hit(*p);
}

void plat_apply(Plat *)
{
    /* Window state takes effect as it is set. */
}

static void ser_frame_ready(Plat &p, u64 now)
{
    p.frame.ready = true;
    p.frame.presented_ms = (uint32_t)(now / SER_NS_PER_MS);
    p.frame.delivered_ns = now;
    ser_trace(p, 'Y', p.tick_count);
}

static void ser_display_tick(Plat &p)
{
    u64 now = ser_now_ns();
    p.last_tick_ns = now;
    p.tick_count++;
    ser_trace(p, 'T', p.tick_count);
    if(!p.frame.ready) {
        ser_frame_ready(p, now);
    }
}

/* Ticks can stop, e.g. while WindowServer is busy or the link is gone. */
static void ser_frame_fallback(Plat &p)
{
    u64 now = ser_now_ns();
    if(!p.frame.ready && now - p.last_tick_ns >= SER_TICK_TIMEOUT_NS) {
        p.last_tick_ns = now;
        ser_frame_ready(p, now);
    }
}

bool plat_pump(Plat *p, int timeout_ms)
{
    auto &loop = Core::EventLoop::current();
    ser_frame_fallback(*p);

    /* Wait for input, a tick, or the timeout, whichever comes first. */
    int wait_ms = p->frame.ready ? 0 : timeout_ms;
    if(!p->frame.ready) {
        int fallback_ms = (int)(SER_TICK_TIMEOUT_NS / SER_NS_PER_MS);
        wait_ms = wait_ms < 0 ? fallback_ms : min(wait_ms, fallback_ms);
    }

    if(wait_ms == 0) {
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    } else {
        RefPtr<Core::Timer> wake;
        if(wait_ms > 0) {
            wake = Core::Timer::create_single_shot(wait_ms, [] {});
            wake->start();
        }
        loop.pump(Core::EventLoop::WaitMode::WaitForEvents);
        if(wake) {
            wake->stop();
        }
    }

    ser_frame_fallback(*p);
    return !p->closed;
}

PlatFrame const *plat_frame(Plat const *p)
{
    return &p->frame;
}

bool plat_frame_request(Plat *p)
{
    /* Ready again on the next display link tick. */
    p->frame.ready = false;
    ser_trace(*p, 'R', p->tick_count);
    return true;
}

void plat_frame_poke(Plat *p)
{
    (void)plat_frame_request(p);
}

int plat_buffer_age(Plat const *)
{
    /* The toy draws into one persistent buffer. */
    return 1;
}

bool plat_has_damage(Plat const *)
{
    return true;
}

void plat_swap(Plat *p, PlatRect const *damage, int count)
{
    if(!p->surface) {
        return;
    }
    p->frame_seq++;
    ser_trace(*p, 'D', p->frame_seq);
    if(count <= 0) {
        p->surface->update();
        p->window->flush_pending_paints_immediately();
        return;
    }
    for(int i = 0; i < count; i++) {
        p->surface->update({ damage[i].x, damage[i].y, damage[i].w, damage[i].h });
    }
    /* Paint and flip now. Left to update(), the paint waits for a round trip
       through WindowServer, and a newer frame can replace it meanwhile. */
    p->window->flush_pending_paints_immediately();
}

int plat_cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}
