/* Headless tests for the Wayland layer's bookkeeping.

   Nothing here connects to a compositor: the listeners are called directly,
   as the dispatcher would call them. Built by `make test` on Linux. */

#include "platform_wayland.c"

static int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

static int g_keyboard_lost = 0;
static int g_pointer_lost = 0;
static char g_text[16];

static void on_enter(void *userdata, int x, int y)
{
    (void)userdata;
    (void)x;
    (void)y;
}

static void on_leave(void *userdata)
{
    (void)userdata;
}

static void on_button(void *userdata, PlatButton button, PlatPress press)
{
    (void)userdata;
    (void)button;
    (void)press;
}

static void on_scroll(void *userdata, double delta)
{
    (void)userdata;
    (void)delta;
}

static void on_pointer_lost(void *userdata)
{
    (void)userdata;
    g_pointer_lost++;
}

static void on_key(void *userdata, PlatKey key, PlatPress press)
{
    (void)userdata;
    (void)key;
    (void)press;
}

static void on_text(void *userdata, const char *utf8)
{
    (void)userdata;
    snprintf(g_text, sizeof(g_text), "%s", utf8);
}

static void on_keyboard_lost(void *userdata)
{
    (void)userdata;
    g_keyboard_lost++;
}

static void on_resize(void *userdata, int width, int height)
{
    (void)userdata;
    (void)width;
    (void)height;
}

static const PlatHandlers test_handlers = {
    .pointer_enter = on_enter,
    .pointer_leave = on_leave,
    .pointer_motion = on_enter,
    .pointer_button = on_button,
    .pointer_scroll = on_scroll,
    .pointer_lost = on_pointer_lost,
    .key = on_key,
    .text = on_text,
    .keyboard_lost = on_keyboard_lost,
    .resize = on_resize,
    .close = on_leave,
};

static Plat test_plat(void)
{
    Plat p = {
        .handlers = test_handlers,
        .frame = { .ready = true },
    };
    g_keyboard_lost = 0;
    g_pointer_lost = 0;
    g_text[0] = '\0';
    return p;
}

static void test_keyboard_text(void)
{
    Plat p = test_plat();
    p.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    p.xkb_keymap = xkb_keymap_new_from_names(p.xkb_context, NULL,
                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    p.xkb_state = xkb_state_new(p.xkb_keymap);

    way_keyboard_key(&p, NULL, 0, 0, KEY_A,
                     WL_KEYBOARD_KEY_STATE_PRESSED);
    CHECK(strcmp(g_text, "a") == 0, "key text was '%s'", g_text);
    CHECK(way_key(KEY_ENTER) == PLAT_KEY_ENTER, "enter was not mapped");
    CHECK(way_key(KEY_BACKSPACE) == PLAT_KEY_BACKSPACE,
          "backspace was not mapped");

    xkb_state_unref(p.xkb_state);
    xkb_keymap_unref(p.xkb_keymap);
    xkb_context_unref(p.xkb_context);
}

/* A hidden surface must keep at most one outstanding frame callback. */
static void test_frame_callback_single_flight(void)
{
    Plat p = test_plat();
    CHECK(way_frame_can_request(&p), "first callback was blocked");

    p.frame_callback = (struct wl_callback *)&p;
    CHECK(!way_frame_can_request(&p), "duplicate callback was allowed");
}

/* A vanished pointer or keyboard cannot deliver the release that ends a grab
   or a held key, so the game has to be told. */
static void test_capability_loss_reported(void)
{
    Plat p = test_plat();

    way_seat_capabilities(&p, NULL, 0);

    CHECK(g_pointer_lost == 1, "pointer capability loss not reported");
    CHECK(g_keyboard_lost == 1, "keyboard capability loss not reported");
}

/* Removed globals must release their slot for replacement devices. */
static void test_registry_remove_clears_slots(void)
{
    enum {
        TEST_SEAT_NAME = 17,
        TEST_OUTPUT_NAME = 23,
        TEST_COMPOSITOR_NAME = 29,
        TEST_WM_BASE_NAME = 31,
    };
    Plat p = test_plat();
    p.seat_name = TEST_SEAT_NAME;
    p.output_name = TEST_OUTPUT_NAME;
    p.compositor_name = TEST_COMPOSITOR_NAME;
    p.wm_base_name = TEST_WM_BASE_NAME;

    way_registry_remove(&p, NULL, TEST_OUTPUT_NAME);
    CHECK(p.output_name == WAY_GLOBAL_NONE, "removed output kept its registry slot");

    way_registry_remove(&p, NULL, TEST_SEAT_NAME);
    CHECK(p.seat_name == WAY_GLOBAL_NONE, "removed seat kept its registry slot");
    CHECK(g_keyboard_lost == 1, "removed seat kept held keys");

    p.compositor = (struct wl_compositor *)&p;
    way_registry_remove(&p, NULL, TEST_COMPOSITOR_NAME);
    CHECK(p.compositor != NULL, "removed compositor destroyed before its surfaces");
    CHECK(p.lost, "removed compositor left the client running");

    p.lost = false;
    p.wm_base = (struct xdg_wm_base *)&p;
    way_registry_remove(&p, NULL, TEST_WM_BASE_NAME);
    CHECK(p.wm_base != NULL, "removed wm_base destroyed before its surfaces");
    CHECK(p.lost, "removed wm_base left the client running");
}

/* A failed cursor build must discard its mapped storage. */
static void test_cursor_failure_cleans_storage(void)
{
    WayCursor cursor = {0};
    const size_t map_size = 4096;
    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map != MAP_FAILED, "cursor test mmap failed");
    if (map == MAP_FAILED) {
        return;
    }

    cursor.map = map;
    cursor.map_size = map_size;

    CHECK(!way_cursor_fail(&cursor), "cursor failure reported success");
    CHECK(cursor.map == NULL, "cursor map survived failure");
}

int main(void)
{
    test_keyboard_text();
    test_frame_callback_single_flight();
    test_capability_loss_reported();
    test_registry_remove_clears_slots();
    test_cursor_failure_cleans_storage();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
