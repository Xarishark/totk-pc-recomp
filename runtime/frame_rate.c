// Frame rate: the game's 60 fps patch (recomp/patches-v101.txt, set fps60) is on by default, and the menu's frame
// rate row switches it off for the game's shipping 30 fps. The set halves the frame-time constants of the game's frame
// accumulator and clears the frame call's wait argument; this file switches the set while the game runs, caps the
// host's present rate at the same 60 or 30, and keeps the world clock for scripted input.
//
// The 60 fps patch leaves the game's cutscenes at 30: its cutscene speed variable (main+0xCCA6764, 1/30 during a
// cutscene) selects 2 in the frame-rate object instead of 1, as the community cutscene fix does. Without that, the
// cutscenes play twice as fast as their animation data.
//
// What remains:
//   - the frame limit (AC_FPS60): 60 fps by default, 30 fps when the patch is off. The pacer (nvn_api_obj.c) holds
//     frames to that rate where the game presents faster than it.
//   - the world clock (frame_step_world_us): wall time from the first present. Recorded and scripted input
//     (input.c) runs on it, so a replay presses at the same game time at any frame rate.
//
// AC_TRACE=perf adds a line per 120 presents: fps and the game seconds stepped against wall seconds.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_RESET_S 0.25          // a gap this long (loading, a debugger) restarts the average

static double step_now;   // the step the game used this frame (whole vsyncs: interval / 60)
static uint64_t last_ns;

static int fps60_on = -1, fps_hz;
static unsigned fps_seen;
static int fps_read;

static void frame_limit_live(void);

void frame_step_init(void) {
    frame_limit_live();
}

int frame_step_realtime(void) { return 0; }

// The frame rate (menu, AC_FPS60): 60 fps runs the game's 60 fps patch, 30 fps switches it off. Read live: the patch
// set is switched in frame_limit_live, so the generated C picks the patched or the original instructions from the next
// site it runs.
static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (fps_read && v == fps_seen) return;
    fps_read = 1;
    fps_seen = v;
    int hz = cfg_int("AC_FPS");
    int on = hz >= 60;
    if (on == fps60_on && fps_hz) return;
    fps60_on = on;
    fps_hz = on ? 60 : 30;
    rt_patch_switch(rt_patch_set("fps60"), on);
    rt_log("frame rate: %d fps%s", fps_hz, on ? " (60 fps patch)" : " (30 fps)");
}

uint64_t frame_step_cap_ns(void) {
    frame_limit_live();
    return (uint64_t)(1e9 / fps_hz + 0.5);
}

// The frame rate the limit gives, for the performance overlay: 60 or 30; 0 before the first frame read the setting.
double frame_step_limit_hz(void) { return fps_hz; }

// The community cutscene fix for the 60 fps patch: the game's frame-rate object holds 1 (60 fps) in gameplay and 2
// (30 fps) in a cutscene. The game's cutscene speed variable is 1/30 in a cutscene and 1/60 in gameplay; write the
// selector each frame when the patch is on, as the cheat engine's code does.
static void cutscene_rate(void) {
    if (!fps60_on) return;
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    void *volatile *slot = (void *volatile *)(base + 0xCCB7B08ull);
    void *obj = *slot;
    if (!obj) return;
    obj = *(void *volatile *)((uintptr_t)obj + 0x70);
    if (!obj) return;
    uint32_t speed = *(uint32_t volatile *)(base + 0xCCA6764ull);
    uint32_t want = speed == 0x3D088889u ? 2u : speed < 0x3D088889u ? 1u : 0u;
    if (!want) return;
    volatile uint32_t *selector = (volatile uint32_t *)((uintptr_t)obj + 0xF14);
    if (*selector != want) {
        *selector = want;
        rt_log("frame rate: cutscene selector %u (speed 0x%08x)", want, speed);
    }
}

// ---- AC_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static double win_game;

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    double wall = (double)(now - win_t0) * 1e-9;
    rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu",
           (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
           (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3]);
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;   // game time stepped since the first present (frame_step_ticks)
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started",
               (unsigned long long)presents,
               rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    if (!frame_step_realtime()) step = dt > STEP_MAX_S ? STEP_MAX_S : dt;   // whole-vsync steps: wall time
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

void frame_step_present(uint64_t display_ns, int interval) {
    frame_limit_live();
    cutscene_rate();
    double dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    if (interval > 0) step_now = interval / 60.0;
    last_ns = display_ns;
    // The game clock of untimed script lines: what the game stepped this frame, whole vsyncs.
    double game = step_now;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, game);
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, game, interval);
}
