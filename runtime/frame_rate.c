// Frame rate: the game's 60 fps patch (recomp/patches-v101.txt, set fps60) is on by default, and the menu's frame
// rate row switches it off for the game's shipping 30 fps. The set turns the game's frame accumulator into a real-time
// step (low clamp 1/16) and clears the frame call's wait argument; this file switches the set while the game runs,
// computes the whole-tick count and the floating step each present, caps the host's present rate at 60 or 30, and
// keeps the world clock for scripted input. The whole ticks and the step are what make the input/action timers and
// the camera smoothing frame-rate independent (the Dynamic mod v1.1.1 fixes for the 60 fps patch's two bugs).
//
// Cutscenes render at 60 fps too. The game steps them by a fixed 1.0 (1/30 s) a frame, so the patch replaces that value
// with the whole vsyncs the frame took (rt_fps_cutscene_s8 below), which keeps the scene's clock at its authored speed.
// A talk line also waits for its text reveal, which counts frames (rt_fps_text_rate_s2 below). With that scaled, the
// measured scene runs 122.86 s at 60 fps against 124.30 s at 30 fps. The rest of that difference is not explained.
//
// What remains:
//   - the frame limit (AC_FPS): 60 fps by default, 30 fps when the patch is off. The pacer (nvn_api_obj.c) holds
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

// ---- dynamic frame rate: whole 30 fps ticks (for the input/action counters) and the floating step (for the camera).
// Computed from the real frame time at each present; the guest counter sites read them through the helpers below.
static _Atomic uint32_t g_fps_ticks;      // whole ticks for the guest counters
static _Atomic uint32_t g_fps_step_bits;  // the floating step (30 fps units), for the camera
static double g_fps_rem;                  // fractional whole-tick remainder (present thread only)
static _Atomic uint32_t g_cut_vsyncs;     // whole 60 Hz vsyncs of the last frame, for the cutscene step
static double g_cut_rem;                  // what rounding to whole vsyncs left over (present thread only)

static int fps60_on = -1, fps_hz;
static unsigned fps_seen;
static int fps_read;

static void frame_limit_live(void);

void frame_step_init(void) {
    frame_limit_live();
}

int frame_step_realtime(void) { return 0; }

// ---- dynamic frame-rate guest helpers (called by @fn/@fnz patch sites in the lifted game code)

uint32_t rt_fps_ticks(void) { return atomic_load_explicit(&g_fps_ticks, memory_order_relaxed); }

float rt_fps_step(void) {
    uint32_t b = atomic_load_explicit(&g_fps_step_bits, memory_order_relaxed);
    float f; memcpy(&f, &b, 4); return f;
}

// The fcvtzs/fcvtzu step->int reader sites: x8 = the whole-tick count.
void rt_fps_ticks_w8(Cpu *c) { c->x[8] = rt_fps_ticks(); }

// The scaled decrement that replaces `subs wN, wN, #1` (and `subs w9, w10, #1`): decrement by the whole-tick count,
// clamped at 0, with NZCV matching what the 30 fps original produced. Flags: N = the counter was already 0, Z = it
// reaches 0 this frame (counter <= ticks), C = it was not 0, V = 0. The stored value underflows when the counter is
// already 0, exactly as `subs wN,wN,#1` does, so a following `b.lt`/`b.ge` sees the same "expired" condition.
static uint64_t fps_hs(uint32_t w, uint32_t ticks, uint32_t *out) {
    if (w == 0) {
        *out = (uint32_t)(w - ticks);
        return MKFLAGS(1, 0, 0, 0);
    }
    if (ticks == 0) {
        *out = w;
        return MKFLAGS(0, 0, 1, 0);
    }
    uint32_t m = w < ticks ? w : ticks;
    *out = w - m;
    return MKFLAGS(0, w <= ticks, 1, 0);
}

uint64_t rt_fps_hs_w8(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[8], rt_fps_ticks(), &out);
    c->x[8] = out; return f;
}
uint64_t rt_fps_hs_w9(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[9], rt_fps_ticks(), &out);
    c->x[9] = out; return f;
}
uint64_t rt_fps_hs_w10(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[10], rt_fps_ticks(), &out);
    c->x[10] = out; return f;
}
uint64_t rt_fps_hs_w10w9(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[10], rt_fps_ticks(), &out);
    c->x[9] = out; return f;
}

// `sub w8, w8, #1` (no flags), the r2 bank of tap/buffer counters: max(w8 - ticks, 0).
void rt_fps_sub_w8(Cpu *c) {
    uint32_t w = (uint32_t)c->x[8], t = rt_fps_ticks(), r = w - t;
    c->x[8] = (w > t) ? r : 0;
}

// The cutscene step in 30 fps units: 0.5 per whole vsync.
static float cutscene_step(void) {
    uint32_t vsyncs = atomic_load_explicit(&g_cut_vsyncs, memory_order_relaxed);
    return (float)(vsyncs ? vsyncs : 1u) * 0.5f;
}

// The frame timer's fixed path (main+0x91DC48), taken while the game's cutscene flag is set: the shipping game stores
// the 1.0 its main loop passes in s8. s8 is the timer's own callee-saved register, so nothing outside it sees this.
// Whole vsyncs rather than the measured frame time, so the cut clock below lands on its hold exactly at 60 fps.
void rt_fps_cutscene_s8(Cpu *c) { FP_SETS(8, cutscene_step()); }

// The hold of the cut clock (main+0x8C2410). The clock adds rate * step to the time in the current cut, where rate is
// 1.0 or 2.0 by the data's authored frame rate. Past cut length - rate it holds the time there for a frame and then
// starts the next cut at 0, which at step 1.0 shows every frame of the cut once. At step 0.5 the same hold would end
// each cut one frame early, 1/60 s per cut, and the picture would pull ahead of the voices. So the hold is cut length -
// rate * step: the same at step 1.0, and the last increment before the cut's end at any whole-vsync step. Outside a
// cutscene the step floats and the game's own hold stays. s1 = the cut length, s8 = rate, as the replaced fsub has them.
void rt_fps_cut_hold_s1(Cpu *c) {
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    uint32_t volatile *flags = *(uint32_t volatile *volatile *)(base + 0xDB8398ull);
    float rate = VSF32(8);
    FP_SETS(1, VSF32(1) - (flags && (*flags >> 30 & 1) ? rate * cutscene_step() : rate));
}

// The dialogue text reveal adds a fixed amount per frame to its character accumulator. A talk line ends 0.5 s after both
// its text and its voice are done, so at 60 fps a line whose text outlasts its voice ended early. The step is the game's
// own, the value its scaled timers read from the frame timer, in 30 fps units. s2 = the rate, as the replaced load has it.
void rt_fps_text_rate_s2(Cpu *c) {
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    uintptr_t timer = *(uintptr_t volatile *)(base + 0xDB89A0ull);
    FP_SETS(2, VSF32(2) * *(float volatile *)(timer + 0xD0));
}

// The staff roll's cue scheduler (main+0x6A3EC0) adds 1 to a frame counter per call and fires a cue when the counter
// equals its frame, so 60 calls per second would bring the cues twice as fast. The test before the call, cmp w8,#1, now
// also needs a whole 30 fps tick this frame, which keeps the counter at 30 per second and never holds it on a frame whose
// cue already fired.
uint64_t rt_fps_cue_cmp_w8(Cpu *c) {
    return MKFLAGS(0, (uint32_t)c->x[8] == 1u && rt_fps_ticks() > 0, 1, 0);
}

// Whole vsyncs for this frame: at least one so the clock always moves, at most 4 (the timer's high clamp, step 2.0;
// a slower frame slows the cutscene, as it does gameplay). The remainder carries so late frames stay on the wall clock.
static void cutscene_vsyncs(double dt) {
    double vs = dt * 60.0 + g_cut_rem;
    int n = (int)floor(vs + 0.5);
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    g_cut_rem = dt >= STEP_RESET_S || vs - n > 1.0 ? 0.0 : vs - n;
    atomic_store_explicit(&g_cut_vsyncs, (uint32_t)n, memory_order_relaxed);
}

// Camera smoothing: s8 = 1 - powf(1 - a, clamp(step, 0, 4)), the per-frame blend a rescaled to per-unit-time. a <= 0,
// a >= 1, NaN and step == 1.0 all pass through unchanged.
static float fps_blend(float a) {
    float step = rt_fps_step();
    if (step == 1.0f) return a;
    if (!(a > 0.0f) || !(a < 1.0f)) return a;
    float s = step < 0.0f ? 0.0f : (step > 4.0f ? 4.0f : step);
    return 1.0f - powf(1.0f - a, s);
}

void rt_fps_cam_s0(Cpu *c) {
    float m = VSF32(0), s10 = VSF32(10);
    FP_SETS(8, fps_blend(m < s10 ? m : s10));
}
void rt_fps_cam_s1(Cpu *c) {
    float m = VSF32(1), s10 = VSF32(10);
    FP_SETS(8, fps_blend(m < s10 ? m : s10));
}
void rt_fps_cam_fcsel(Cpu *c) {
    float s10 = VSF32(10);
    FP_SETS(9, fps_blend(VSF32(9)));   // the smoothed s9, for the +0x100/+0x108 blend
    float t = *(float volatile *)(c->x[19] + 0x214ull);   // re-test [cam+0x214] > 0
    FP_SETS(8, t > 0.0f ? s10 : VSF32(9));
}


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

// The swap interval of the game's window, at +0xF14 of the object the pointer chain below reaches: 1 for as long as the
// 60 fps patch is on, cutscenes included, written each frame. The game's cutscene flag is bit 30 of the word the pointer
// at main+0xDB8398 names (main+0x42FE60 sets it, main+0x42FEA0 clears it); the log marks where cutscenes start and end.
static int in_cutscene;

static void cutscene_rate(void) {
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    uint32_t volatile *flags = *(uint32_t volatile *volatile *)(base + 0xDB8398ull);
    int now = flags && (*flags >> 30 & 1);
    if (in_cutscene != now) {
        in_cutscene = now;
        rt_log("frame rate: %s (world %.2f s)", now ? "cutscene" : "gameplay", (double)frame_step_world_us() / 1e6);
    }
    if (!fps60_on) return;
    void *volatile *slot = (void *volatile *)(base + 0xCCB7B08ull);
    void *obj = *slot;
    if (!obj) return;
    obj = *(void *volatile *)((uintptr_t)obj + 0x70);
    if (!obj) return;
    volatile uint32_t *selector = (volatile uint32_t *)((uintptr_t)obj + 0xF14);
    if (*selector != 1u) *selector = 1u;
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
    cutscene_vsyncs(dt);
    // Dynamic frame rate: whole 30 fps ticks from the real frame time, for the input/action counters and the camera.
    double step = dt * 30.0;
    if (step > 2.0) step = 2.0;
    else if (step < 1.0 / 16.0) step = 1.0 / 16.0;
    g_fps_rem += step;
    uint32_t ticks = (uint32_t)g_fps_rem;
    g_fps_rem -= (double)ticks;
    atomic_store_explicit(&g_fps_ticks, ticks, memory_order_relaxed);
    float fs = (float)step;
    uint32_t bits; memcpy(&bits, &fs, 4);
    atomic_store_explicit(&g_fps_step_bits, bits, memory_order_relaxed);
    // The game clock of untimed script lines: what the game stepped this frame, whole vsyncs.
    double game = step_now;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, game);
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, game, interval);
}
