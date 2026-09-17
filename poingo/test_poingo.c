/* Headless regression tests for the parts of Poingo that need no compositor.

   Built twice by `make test`: once plain, and once under ThreadSanitizer,
   which is what actually catches the regeneration races. */

#define main poingo_main
#include "poingo.c"
#undef main

#include <fcntl.h>
#include <unistd.h>

/* No audio device: the stream never starts. Replaces the platform stream, so
   nothing in the tests opens real audio. */
ToyAudioStream *toy_audio_stream_start(const ToyAudioStreamConfig *config) {
    (void)config;
    return NULL;
}

void toy_audio_stream_stop(ToyAudioStream *stream) {
    (void)stream;
}

bool toy_audio_stream_get_latency(const ToyAudioStream *stream, double *seconds) {
    (void)stream;
    (void)seconds;
    return false;
}

bool toy_audio_stream_is_ready(const ToyAudioStream *stream) {
    (void)stream;
    return false;
}

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

/* ------------------------------------------------------------------ */
/* Physics                                                             */
/* ------------------------------------------------------------------ */

#define TEST_WINDOW_W 1280
#define TEST_WINDOW_H 720

/* One integrator step with no audio and no event capture. */
static void step_physics(float *x, float *y, float *vx, float *vy, int *dir,
                         float diameter, double sim_delta) {
    float diameter_norm = diameter / (float)TEST_WINDOW_H;
    update_ball_physics(x, y, vx, vy, dir, TEST_WINDOW_W,
                        0.0f, 0.0f, diameter, diameter_norm,
                        sim_delta, false, false,
                        NULL, NULL, NULL, 0, 0.0f);
}

/* A hard flick at maximum speed advances the ball several window widths in
   one step. Reflecting once leaves it outside the opposite wall. */
static void test_fast_ball_stays_in_window(void) {
    g_speed_multiplier = SPEED_MAX;
    g_floor_y_normalized = 1.0f;

    const float diameter = 124.0f;
    const float diameter_norm = diameter / (float)TEST_WINDOW_H;
    float x = TEST_WINDOW_W * 0.5f;
    float y = 0.5f;
    float vx = (float)TEST_WINDOW_W * 0.4f;   /* the slingshot/flick cap */
    float vy = 0.0f;
    int dir = 1;

    for (int i = 0; i < 240; i++) {
        step_physics(&x, &y, &vx, &vy, &dir, diameter, 1.0 / 60.0);
        CHECK(x >= 0.0f && x + diameter <= (float)TEST_WINDOW_W,
              "step %d: ball_x %.1f outside [0, %d]", i, (double)x,
              TEST_WINDOW_W - (int)diameter);
        CHECK(y >= 0.0f && y + diameter_norm <= g_floor_y_normalized,
              "step %d: ball_y %.4f outside [0, %.4f]", i, (double)y,
              (double)(g_floor_y_normalized - diameter_norm));
        if (g_failures) {
            break;
        }
    }

    g_speed_multiplier = 1.0f;
}

/* Horizontal damping pulls ball_vx towards natural_vx. It must converge on
   it, never shoot past it: ball_vx is a magnitude, paired with a separate
   direction, so a negative value inverts the ball's travel. */
static void test_damping_does_not_overshoot(void) {
    g_speed_multiplier = SPEED_MAX;
    g_floor_y_normalized = 1.0f;

    const float natural_vx = get_natural_vx(TEST_WINDOW_W);
    const float diameter = 124.0f;
    /* A 20 Hz frame at maximum speed -- inside the 15..240 Hz range the
       frame pacer accepts. */
    const double slow_frame = 1.0 / 20.0;

    float x = TEST_WINDOW_W * 0.5f;
    float y = 0.5f;
    float vx = natural_vx * 8.0f;
    float vy = 0.0f;
    int dir = 1;

    for (int i = 0; i < 60; i++) {
        float before = vx;
        step_physics(&x, &y, &vx, &vy, &dir, diameter, slow_frame);
        CHECK(vx >= 0.0f, "step %d: ball_vx went negative (%.3f)", i, (double)vx);
        if (before > natural_vx) {
            CHECK(vx >= natural_vx - 0.001f,
                  "step %d: ball_vx %.3f undershot natural_vx %.3f from %.3f",
                  i, (double)vx, (double)natural_vx, (double)before);
        }
        if (g_failures) {
            break;
        }
    }

    g_speed_multiplier = 1.0f;
}

static PoingoBall test_ball(float x, float y, float diameter,
                            float vx, float vy) {
    PoingoBall ball = {
        .x = x,
        .y = y,
        .vx = fabsf(vx),
        .vy = vy / (float)TEST_WINDOW_H,
        .vx_direction = vx < 0.0f ? -1 : 1,
        .scale = 1.0f,
        .diameter = diameter,
        .diameter_norm = diameter / (float)TEST_WINDOW_H,
        .mode = BALL_MODE_POINGO,
    };
    return ball;
}

static float signed_vx(const PoingoBall *ball) {
    return ball->vx * (float)ball->vx_direction;
}

/* Equal hollow shells exchange direction; unequal shells use diameter squared
   as mass, proportional to spherical surface area. */
static void test_ball_collision_mass(void) {
    FreedomState st = {
        .width = TEST_WINDOW_W,
        .height = TEST_WINDOW_H,
        .grabbed_ball = -1,
        .ball_count = 2,
    };
    st.balls[0] = test_ball(100.0f, 0.3f, 100.0f, 5.0f, 0.0f);
    st.balls[1] = test_ball(190.0f, 0.3f, 100.0f, -5.0f, 0.0f);

    solve_ball_pair(&st, 0, 1, BALL_SOUND_SILENT);

    CHECK(fabsf(signed_vx(&st.balls[0]) + 4.7f) < 0.01f,
          "equal ball A velocity %.3f", (double)signed_vx(&st.balls[0]));
    CHECK(fabsf(signed_vx(&st.balls[1]) - 4.7f) < 0.01f,
          "equal ball B velocity %.3f", (double)signed_vx(&st.balls[1]));

    st.balls[0] = test_ball(100.0f, 0.3f, 100.0f, 10.0f, 0.0f);
    st.balls[1] = test_ball(190.0f, 166.0f / TEST_WINDOW_H,
                            200.0f, 0.0f, 0.0f);
    solve_ball_pair(&st, 0, 1, BALL_SOUND_SILENT);

    CHECK(fabsf(signed_vx(&st.balls[0]) + 5.52f) < 0.01f,
          "small ball velocity %.3f", (double)signed_vx(&st.balls[0]));
    CHECK(fabsf(signed_vx(&st.balls[1]) - 3.88f) < 0.01f,
          "large ball velocity %.3f", (double)signed_vx(&st.balls[1]));
}

/* A grabbed ball is kinematic: it keeps the pointer's velocity while still
   transferring energy to balls it strikes. */
static void test_grabbed_ball_strikes(void) {
    FreedomState st = {
        .width = TEST_WINDOW_W,
        .height = TEST_WINDOW_H,
        .grabbed_ball = 0,
        .ball_count = 2,
    };
    st.balls[0] = test_ball(100.0f, 0.3f, 100.0f, 10.0f, 0.0f);
    st.balls[1] = test_ball(190.0f, 0.3f, 100.0f, 0.0f, 0.0f);

    solve_ball_pair(&st, 0, 1, BALL_SOUND_SILENT);

    CHECK(fabsf(signed_vx(&st.balls[0]) - 10.0f) < 0.01f,
          "grabbed ball velocity %.3f", (double)signed_vx(&st.balls[0]));
    CHECK(signed_vx(&st.balls[1]) > 19.0f,
          "struck ball velocity %.3f", (double)signed_vx(&st.balls[1]));

    st.balls[0] = test_ball(100.0f, 0.3f, 100.0f, 300.0f, 0.0f);
    st.balls[1] = test_ball(400.0f, 0.3f, 100.0f, 0.0f, 0.0f);
    sweep_grabbed_ball(&st, 100.0f, 0.3f, 500.0f, 0.3f);
    CHECK(signed_vx(&st.balls[1]) > 0.0f,
          "grabbed ball tunneled through target");
}

/* Substeps keep a fast ball from crossing another between frames. */
static void test_fast_ball_collision(void) {
    FreedomState st = {
        .width = TEST_WINDOW_W,
        .height = TEST_WINDOW_H,
        .grabbed_ball = -1,
        .ball_count = 2,
    };
    st.balls[0] = test_ball(100.0f, 0.3f, 100.0f, 300.0f, 0.0f);
    st.balls[1] = test_ball(400.0f, 0.3f, 100.0f, 0.0f, 0.0f);
    g_speed_multiplier = 1.0f;

    step_balls(&st, 1.0 / 60.0, BALL_SOUND_SILENT);

    CHECK(signed_vx(&st.balls[1]) > 0.0f,
          "fast ball tunneled through target");
}

static void test_add_remove_balls(void) {
    FreedomState st = {
        .width = TEST_WINDOW_W,
        .height = TEST_WINDOW_H,
        .grabbed_ball = -1,
        .menu_ball = -1,
    };
    srandom(7);
    for (int i = 0; i < POINGO_MAX_BALLS; i++) {
        CHECK(add_ball(&st), "add %d failed", i);
    }
    CHECK(st.ball_count == POINGO_MAX_BALLS, "ball count %d", st.ball_count);
    CHECK(!add_ball(&st), "accepted a seventh ball");

    for (int a = 0; a < st.ball_count; a++) {
        CHECK(st.balls[a].x >= 0.0f &&
              st.balls[a].x + st.balls[a].diameter <= st.width,
              "ball %d outside horizontal bounds", a);
        CHECK(st.balls[a].y >= 0.0f &&
              st.balls[a].y + st.balls[a].diameter_norm <= 1.0f,
              "ball %d outside vertical bounds", a);
        for (int b = a + 1; b < st.ball_count; b++) {
            CHECK(ball_fits(&(FreedomState){
                      .width = st.width,
                      .height = st.height,
                      .balls = {st.balls[a]},
                      .ball_count = 1,
                  }, &st.balls[b]),
                  "balls %d and %d overlap", a, b);
        }
    }

    int removed = st.ball_count - 1;
    remove_ball(&st, removed);
    CHECK(st.ball_count == POINGO_MAX_BALLS - 1,
          "remove left %d balls", st.ball_count);
}

static void test_targeted_ball_actions(void) {
    FreedomState st = {
        .width = TEST_WINDOW_W,
        .height = TEST_WINDOW_H,
        .grabbed_ball = 1,
        .menu_ball = 1,
        .ball_count = 2,
    };
    st.balls[0] = test_ball(100.0f, 0.2f, 100.0f, 1.0f, 0.0f);
    st.balls[1] = test_ball(300.0f, 0.2f, 100.0f, 1.0f, 0.0f);
    st.balls[0].scale = 0.5f;
    st.balls[1].scale = 0.5f;
    st.pointer_x = 350;
    st.pointer_y = 194;
    st.grab_u = 0.5f;
    st.grab_v = 0.5f;

    freerange_adjust_ball_scale(&st, 1);

    CHECK(st.balls[0].scale == 0.5f, "wheel resized the wrong ball");
    CHECK(fabsf(st.balls[1].scale - 0.55f) < 0.001f,
          "wheel missed the grabbed ball");
    set_ball_mode(&st.balls[1], BALL_MODE_NOSTALGIA);
    CHECK(st.balls[0].mode == BALL_MODE_POINGO,
          "mode changed the wrong ball");
    CHECK(st.balls[1].mode == BALL_MODE_NOSTALGIA,
          "mode missed the target ball");

    st.grabbed_ball = -1;
    PoingoBall kept = st.balls[1];
    remove_ball(&st, 0);
    CHECK(st.ball_count == 1, "targeted remove left %d balls", st.ball_count);
    CHECK(memcmp(&st.balls[0], &kept, sizeof(kept)) == 0,
          "targeted remove kept the wrong ball");
    remove_ball(&st, 0);
    CHECK(st.shutdown_pending, "last-ball remove did not request shutdown");
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

static PoingoArgsResult parse(const char *a, const char *b) {
    char *argv[3];
    int argc = 1;
    argv[0] = (char *)"poingo";
    if (a) {
        argv[argc++] = (char *)a;
    }
    if (b) {
        argv[argc++] = (char *)b;
    }
    bool muted = false;
    return poingo_parse_args(argc, argv, &muted);
}

static void test_cli_rejects_bad_input(void) {
    float saved_scale = g_freerange_ball_scale;
    /* Usage text and diagnostics are expected here; keep them out of the
       test log so a real failure stands out. */
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    int devnull = open(POINGO_NULL_DEVICE, O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }

    CHECK(parse("--start-size", "1.0") == POINGO_ARGS_RUN, "valid --start-size rejected");
    CHECK(g_freerange_ball_scale == 1.0f, "valid --start-size not applied");

    CHECK(parse("--start-size", "nan") == POINGO_ARGS_ERROR, "--start-size nan accepted");
    CHECK(parse("--start-size", "inf") == POINGO_ARGS_ERROR, "--start-size inf accepted");
    CHECK(parse("--start-size", "2.0") == POINGO_ARGS_ERROR, "--start-size above max accepted");
    CHECK(parse("--start-size", NULL) == POINGO_ARGS_ERROR, "--start-size with no value accepted");
    CHECK(parse("--light-color", NULL) == POINGO_ARGS_ERROR, "--light-color with no value accepted");
    CHECK(parse("--dark-color", NULL) == POINGO_ARGS_ERROR, "--dark-color with no value accepted");
    CHECK(parse("--light-color", "1,2,3junk") == POINGO_ARGS_ERROR,
          "--light-color accepted a trailing suffix");
    CHECK(parse("--start-szie", "1.0") == POINGO_ARGS_ERROR, "unknown option accepted");
    CHECK(parse("garbage", NULL) == POINGO_ARGS_ERROR, "stray operand accepted");

    CHECK(parse("--mute", NULL) == POINGO_ARGS_RUN, "--mute rejected");
    CHECK(parse("--help", NULL) == POINGO_ARGS_DONE, "--help did not stop");

    fflush(stdout);
    fflush(stderr);
    if (devnull >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        dup2(saved_err, STDERR_FILENO);
        close(devnull);
    }
    close(saved_out);
    close(saved_err);

    g_freerange_ball_scale = saved_scale;
}

/* ------------------------------------------------------------------ */
/* Keyboard focus                                                      */
/* ------------------------------------------------------------------ */

/* Key repeat runs off key_*_pressed, which only a RELEASED event clears.
   Losing focus mid-hold means that release never arrives. */
static void test_keyboard_leave_clears_keys(void) {
    FreedomState st = {0};
    st.key_vol_up_pressed = true;
    st.key_vol_down_pressed = true;
    st.key_speed_up_pressed = true;
    st.key_speed_down_pressed = true;

    freerange_keyboard_leave(&st);

    CHECK(!st.key_vol_up_pressed, "vol up still held after focus loss");
    CHECK(!st.key_vol_down_pressed, "vol down still held after focus loss");
    CHECK(!st.key_speed_up_pressed, "speed up still held after focus loss");
    CHECK(!st.key_speed_down_pressed, "speed down still held after focus loss");
}

/* A vanished pointer cannot deliver the release that ends a grab. */
static void test_pointer_loss_releases_grab(void) {
    FreedomState st = {
        .pointer_down = true,
        .grabbed_ball = 0,
    };

    freerange_pointer_lost(&st);

    CHECK(!st.pointer_down, "pointer stayed down after capability loss");
    CHECK(st.grabbed_ball == -1, "ball stayed grabbed after capability loss");
}

/* A failed cursor build must discard its local storage. The Wayland layer's
   own cursor, frame callback and registry checks are in
   test_platform_wayland.c. */
static void test_cursor_failure_cleans_storage(void) {
    g_ball_cursor.pixels = malloc(16);
    g_ball_cursor.blade = malloc(16);
    CHECK(g_ball_cursor.pixels != NULL && g_ball_cursor.blade != NULL,
          "cursor test allocation failed");

    CHECK(!ball_cursor_fail(), "cursor failure reported success");
    CHECK(g_ball_cursor.pixels == NULL, "cursor frames survived failure");
    CHECK(g_ball_cursor.blade == NULL, "cursor bitmap survived failure");
}

/* Menu teardown owns every global workspace. */
static void test_menu_cleanup_releases_storage(void) {
    g_menu_scratch = malloc(16);
    g_menu_scratch_cap = 4;
    g_field_cache[0].px = malloc(16);
    g_field_cache[0].cap = 4;
    g_upload_staging = malloc(16);
    g_upload_cap = 4;

    poingo_menu_destroy();

    CHECK(g_menu_scratch == NULL, "menu workspace survived cleanup");
    CHECK(g_field_cache[0].px == NULL, "field cache survived cleanup");
    CHECK(g_upload_staging == NULL, "upload staging survived cleanup");
}

/* ------------------------------------------------------------------ */
/* Regeneration lifecycle                                              */
/* ------------------------------------------------------------------ */

#define TEST_FRAME_COUNT 24

static bool regen_state_init(FreedomState *st, FreedomFrameSet *frames) {
    memset(st, 0, sizeof(*st));
    memset(frames, 0, sizeof(*frames));
    if (!freerange_prepare_blank_frames(frames, TEST_FRAME_COUNT)) {
        return false;
    }
    st->frames_ref = frames;
    st->color_regen_angle_period = (4.0f * PI) / LON_TILES;
    return freerange_color_regen_prepare_assets(st, frames) &&
           freerange_regen_workspace_prepare(st, frames);
}

static void regen_state_free(FreedomState *st, FreedomFrameSet *frames) {
    freerange_color_regen_shutdown(st);
    freerange_destroy_frames(frames);
    free(st->regen_unit_done_storage);
    free(st->regen_order_storage);
    free(st->regen_thread_storage);
}

/* Quitting mid-regen frees the frame set the workers are still writing into.
   freerange_color_regen_shutdown() has to be the join point, so that no
   caller can free worker-visible memory behind a live thread. */
static void test_regen_shutdown_joins_workers(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    freerange_color_regen_start(&st, &frames);
    CHECK(st.color_regen_active, "regen did not start");

    freerange_color_regen_shutdown(&st);
    CHECK(st.regen_thread_count == 0, "%d worker threads outlived the shutdown",
          st.regen_thread_count);
    CHECK(st.regen_worker_ctx == NULL, "worker context outlived the shutdown");
    CHECK(!st.color_regen_active, "regen still marked active after shutdown");

    regen_state_free(&st, &frames);
}

/* A colour change rewrites the shared sphere cache. Doing that while the
   previous regen's workers are still reading it is a data race, so the
   transition has to join them before it touches the palette. TSan proves
   the race; this check proves the ordering even without it. */
static void test_color_change_joins_before_palette(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    const uint8_t light[3] = { 255, 255, 255 };
    const uint8_t dark[3] = { 255, 0, 0 };

    for (int i = 0; i < 8; i++) {
        freerange_color_regen_start(&st, &frames);
        CHECK(st.color_regen_active, "round %d: regen did not start", i);
        /* No upload step in between: the workers are still mid-flight. */
        freerange_regen_transition(&st, i & 1 ? POINGO_MODE_NOSTALGIA : POINGO_MODE_POINGO,
                                   i & 1 ? light : dark, i & 1 ? dark : light);
        CHECK(st.color_regen_active, "round %d: regen not restarted", i);
        if (g_failures) {
            break;
        }
    }

    regen_state_free(&st, &frames);
}

/* Every worker allocates its own scratch image. If they all fail to, nothing
   ever marks a unit done and the upload loop waits on it forever. */
static void test_regen_survives_worker_failure(void) {
    FreedomState st;
    FreedomFrameSet frames;
    if (!regen_state_init(&st, &frames)) {
        CHECK(false, "could not set up regen state");
        return;
    }

    freerange_color_regen_start(&st, &frames);
    CHECK(st.color_regen_active, "regen did not start");
    CHECK(st.regen_worker_ctx != NULL, "no worker context");

    if (st.regen_worker_ctx) {
        /* Stand in for every worker's scratch allocation failing. */
        poingo_atomic_set(&st.regen_worker_ctx->failed, 1);
        for (int i = 0; i < 10; i++) {
            freerange_regen_upload_step(&st, &frames, 0, 1000.0);
        }
        CHECK(!st.color_regen_active,
              "regen stayed active after every worker failed");
    }

    regen_state_free(&st, &frames);
}

/* A machine without audio output still runs the toy, silently: startup
   succeeds, sounds are dropped, and the volume controls keep working. */
static void test_runs_without_audio_device(void) {
    CHECK(init_audio(false), "startup failed without an audio device");
    CHECK(g_audio_stream == NULL, "a stream exists without a device");
    CHECK(!play_bounce_sound(32, 0.5f), "a bounce played without a device");

    set_master_mute(true);
    CHECK(g_volume_muted, "mute stopped working without a device");
    set_master_mute(false);
    adjust_master_volume(0.1f);

    shutdown_audio();
}

int main(void) {
    srandom(1);

    test_fast_ball_stays_in_window();
    test_damping_does_not_overshoot();
    test_ball_collision_mass();
    test_grabbed_ball_strikes();
    test_fast_ball_collision();
    test_add_remove_balls();
    test_targeted_ball_actions();
    test_cli_rejects_bad_input();
    test_keyboard_leave_clears_keys();
    test_pointer_loss_releases_grab();
    test_cursor_failure_cleans_storage();
    test_menu_cleanup_releases_storage();
    test_regen_shutdown_joins_workers();
    test_color_change_joins_before_palette();
    test_regen_survives_worker_failure();
    test_runs_without_audio_device();

    release_sphere_pixel_cache();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
