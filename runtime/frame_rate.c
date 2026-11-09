// Frame rate: the game's 60 fps patch (recomp/patches-v101.txt, set fps60) is on by default, and the menu's frame
// rate row switches it off for the game's shipping 30 fps. The set replaces the game's frame timer with a real-time step
// (rt_fps_timer) and asks the frame call for present interval 1; this file switches the set while the game runs, keeps
// the step and the whole 30 fps ticks it adds up to, caps the host's present rate at 60 or 30, and keeps the world clock
// for scripted input. The whole ticks and the step make the game's per-frame counters and blends frame-rate independent
// (the input/action timers, hitstop, rumble, the camera smoothing, the dialogue text reveal, the cue scheduler).
//
// The 60 fps patch leaves the game's cutscenes at 30 (cutscene_rate below): the game steps them by a fixed frame, and
// without the hold they play twice as fast as their animation data.
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

// ---- dynamic frame rate: the game's frame step (30 fps units) and the whole 30 fps ticks it adds up to.
// Both come from the game's own frame timer (main+0x91DBB0, rt_fps_timer below), once per game frame on the thread that
// steps the game, so every counter site of a frame sees the same ticks and the ticks add up to exactly the step the game
// moved by. The guest counter sites read them through the helpers below.
static _Atomic uint32_t g_fps_ticks;      // whole ticks of this game frame
static _Atomic uint32_t g_fps_step_bits;  // the game's step this frame (30 fps units, a float)
static double g_fps_rem;                  // what the whole ticks left of the steps (game thread only)
static double g_fps_debt_ms;              // frame time not yet handed out as whole present periods (game thread only)
static volatile int in_cutscene;          // the game's fixed-step flag at the last timer call

static int fps60_on = -1, fps_hz;
static unsigned fps_seen;
static int fps_read;

static void frame_limit_live(void);

void frame_step_init(void) {
    frame_limit_live();
}

int frame_step_realtime(void) { return 0; }

// The game's frame timer, main+0x91DBB0 (one call per game frame, from the main loop at main+0x3E39CC; x19 = the timer
// object, x21 = 0 while the flag of main+0x42FE60 asks for a fixed step, which the game sets while a cutscene plays).
// Fields: +0xC8 the previous tick, +0xD0 the step in 30 fps frames (what about 120 sites read), +0xD4 the carry,
// +0xD8 / +0xE4 the frame-time sum and frame count it averages, +0xDC 30/1000, +0xE0 this frame's time in ms.
// The original clamps the step to [1, 2] and turns a shorter frame into carry, so at 60 fps every frame steps a full
// 1/30 s; it also takes the audio-sync override (main+0x8A9570, -1 when unset; written by the cutscene player at
// main+0x8B9F0C and main+0x8C2F5C as a 1 +- correction, clamped to [0.5, 2]) as the whole step.
//
// rt_fps_timer replaces everything after the frame time store (site main+0x91DC10; the site branches to the tick store
// at main+0x91DCB4 next):
//   - fixed step (a cutscene): 1.0, as the original. The present pacer holds cutscenes to 30 presents per second
//     (nvn_api_obj.c present_period, frame_step_cutscene), so 1.0 is their real time.
//   - otherwise the frame time, on the present grid (frame_paced_ms), times 30/1000, times the override when it is set:
//     the override is a speed relative to one 30 fps frame, so it scales the measured step instead of replacing it.
//   - clamped to [1/16, 2]: 2 is the original's high clamp (a slower frame slows the game); 1/16 keeps the step away
//     from 0 for the few readers that divide by it. No carry: the step is the frame's real time.
// The averaging fields and the carry are cleared, so the original takes over cleanly when the set is switched off.
static double frame_paced_ms(double dt_ms);

static void fps_publish(float step) {
    uint32_t bits; memcpy(&bits, &step, 4);
    atomic_store_explicit(&g_fps_step_bits, bits, memory_order_relaxed);
    g_fps_rem += step;
    uint32_t ticks = (uint32_t)g_fps_rem;
    g_fps_rem -= (double)ticks;
    atomic_store_explicit(&g_fps_ticks, ticks, memory_order_relaxed);
}

void rt_fps_timer(Cpu *c) {
    uint8_t *t = (uint8_t *)(uintptr_t)c->x[19];
    int fixed = (uint32_t)c->x[21] == 0;
    float step;
    if (fixed) {
        step = 1.0f;
        g_fps_debt_ms = 0;
    } else {
        double dt_ms = *(float volatile *)(t + 0xE0);
        double s = frame_paced_ms(dt_ms) * *(float volatile *)(t + 0xDC);
        float *ov = *(float *volatile *)((uintptr_t)MOD_MAIN_BASE + 0xDC0F10ull);   // main+0x8A9570's float
        if (ov && *ov > 0.0f) s *= *ov;
        step = s > 2.0 ? 2.0f : s < 1.0 / 16.0 ? 1.0f / 16.0f : (float)s;
    }
    *(float volatile *)(t + 0xD0) = step;
    *(uint32_t volatile *)(t + 0xD4) = 0;
    *(uint32_t volatile *)(t + 0xD8) = 0;
    *(uint32_t volatile *)(t + 0xE4) = 0;
    if (in_cutscene != fixed) {
        in_cutscene = fixed;
        rt_log("frame rate: %s (world %.2f s): presents held to %d per second", fixed ? "cutscene" : "gameplay",
               (double)frame_step_world_us() / 1e6, fixed ? 30 : 60);
    }
    fps_publish(step);
}

// The timer's first call (no previous tick, site main+0x91DBDC): the game keeps the step it has, which is one whole
// frame for the counters.
void rt_fps_timer_first(Cpu *c) {
    if (c->x[8]) return;
    g_fps_rem = 0;
    g_fps_debt_ms = 0;
    uint8_t *t = (uint8_t *)(uintptr_t)c->x[19];
    float step = *(float volatile *)(t + 0xD0);
    fps_publish(step > 0.0f ? step : 1.0f);
}

// The frame time on the present grid. The game loop waits for its swapchain image (nvn_api_obj.c), so on average a
// frame lasts whole present periods, but the time the timer measures jitters by a millisecond or two around them. A
// step that follows the jitter moves objects by uneven amounts between evenly shown frames (judder). So the time is
// handed out in whole periods, and what rounding leaves is carried to the next frame: the sum stays the wall time.
// A frame outside a quarter to four and a half periods (a load, a debugger, a menu that does not present) is used as
// measured and drops the carry.
static double frame_paced_ms(double dt_ms) {
    double period = 1000.0 / (fps_hz ? fps_hz : 60);
    if (!(dt_ms >= 0.25 * period && dt_ms <= 4.5 * period)) {
        g_fps_debt_ms = 0;
        return dt_ms;
    }
    double d = g_fps_debt_ms + dt_ms;
    int n = (int)floor(d / period + 0.5);
    if (n < 1) n = 1;
    g_fps_debt_ms = d - n * period;
    if (g_fps_debt_ms < -period || g_fps_debt_ms > period) g_fps_debt_ms = 0;
    return n * period;
}

// ---- dynamic frame-rate guest helpers (called by @fn/@fnz patch sites in the lifted game code)

uint32_t rt_fps_ticks(void) { return atomic_load_explicit(&g_fps_ticks, memory_order_relaxed); }

float rt_fps_step(void) {
    uint32_t b = atomic_load_explicit(&g_fps_step_bits, memory_order_relaxed);
    float f; memcpy(&f, &b, 4); return f;
}

// The fcvtzs/fcvtzu step->int reader sites: x8 = the whole-tick count.
void rt_fps_ticks_w8(Cpu *c) { c->x[8] = rt_fps_ticks(); }

// The scaled decrement that replaces `subs wN, wN, #1` (and `subs w9, w10, #1`): decrement by the whole-tick count,
// clamped at 0, with NZCV matching what the 30 fps original produced. Flags: N = the counter was already 0 or below, Z =
// it reaches 0 this frame (counter <= ticks), C = it was not 0, V = 0. The stored value underflows when the counter is
// already 0 or below, as `subs wN,wN,#1` does, so a following `b.lt`/`b.ge` sees the same "expired" condition.
static uint64_t fps_hs(uint32_t w, uint32_t ticks, uint32_t *out) {
    if ((int32_t)w <= 0) {
        *out = (uint32_t)(w - ticks);
        return MKFLAGS(1, 0, w != 0, 0);
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

// `subs w9, w8, #1`: the scaled decrement of w8, written to w9 (the stick gesture parser's press windows).
uint64_t rt_fps_hs_w8w9(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[8], rt_fps_ticks(), &out);
    c->x[9] = out; return f;
}

// The stick gesture parser's sector stamp (`str w9, [x8, #0x1508]`, w9 = 12): the sector the stick points at stays live
// for 12 frames, and 6 live sectors make a rotation, which clears the stick reversal and turns the attack into the
// rotation attack. At 60 fps the stick is sampled twice as often, so a back-to-forward sweep along the rim stamped more
// sectors than at 30 and read as a rotation more often. Sectors are stamped only on frames with a whole tick: 30
// samples a second, as the game was tuned for.
void rt_fps_sector_str(Cpu *c) {
    if (rt_fps_ticks()) *(uint32_t volatile *)(uintptr_t)(c->x[8] + 0x1508) = (uint32_t)c->x[9];
}

// A float countdown of frames (`fmov s1, #-1.0` before `fadd s0, s0, s1`): s1 = minus the game's step, so it counts
// 30 fps frames of real time.
void rt_fps_neg_step_s1(Cpu *c) { FP_SETS(1, -rt_fps_step()); }

// `sub w8, w8, #1` (no flags), the r2 bank of tap/buffer counters: max(w8 - ticks, 0).
void rt_fps_sub_w8(Cpu *c) {
    uint32_t w = (uint32_t)c->x[8], t = rt_fps_ticks(), r = w - t;
    c->x[8] = (w > t) ? r : 0;
}

// `add w9, w9, #1` (no flags), the pad auto-repeat count: w9 + ticks. A repeat takes the interval off the count, which
// leaves it at or above the delay, and nothing fires at or below the delay. So a frame without a tick cannot fire twice,
// unless the interval is 0.
void rt_fps_up_w9(Cpu *c) { c->x[9] = (uint32_t)((uint32_t)c->x[9] + rt_fps_ticks()); }

// The hitstop manager's freeze phase: `sub w9,w9,#1` runs only at 2 or more and stays at 1 or above, and `add w8,w8,#1`
// counts its length. Neither writes flags, as the instructions they replace do not. Its ramp adds a constant a frame:
// `ldr s1,<constant>` is followed by the same constant times the ticks.
void rt_fps_floor1_w9(Cpu *c) {
    int32_t r = (int32_t)(uint32_t)c->x[9] - (int32_t)rt_fps_ticks();
    c->x[9] = (uint32_t)(r < 1 ? 1 : r);
}
void rt_fps_up_w8(Cpu *c) { c->x[8] = (uint32_t)((uint32_t)c->x[8] + rt_fps_ticks()); }
void rt_fps_ramp_s1(Cpu *c) { FP_SETS(1, VSF32(1) * (float)rt_fps_ticks()); }

// The tap/hold count, `add w9,w9,#1` while a button is held: on release 1 to 9 is a tap and 10 or more a hold, and 0 is
// neither, so the first held frame always counts even without a tick. The lockout count, `add w10,w9,#1`, only tests its
// old value against 15. Neither writes flags, as the instructions they replace do not.
void rt_fps_up_min1_w9(Cpu *c) {
    uint32_t w = (uint32_t)c->x[9], t = rt_fps_ticks();
    c->x[9] = (uint32_t)(w + (w == 0 && t == 0 ? 1 : t));
}
void rt_fps_up_w9w10(Cpu *c) { c->x[10] = (uint32_t)((uint32_t)c->x[9] + rt_fps_ticks()); }

// The rumble counters (`subs w9,w8,#1` and `subs w10,w9,#1`): as fps_hs, but a counter at 1 always reaches 0, so a tick
// of 0 cannot keep a channel at its last count on the frame that would stop it. A frame without a tick repeats the same
// strength, which the game blends from the old count.
static uint64_t fps_rumble(uint32_t w, uint32_t *out) { return fps_hs(w, w == 1 ? 1 : rt_fps_ticks(), out); }
uint64_t rt_fps_rumble_w8w9(Cpu *c) {
    uint32_t out; uint64_t f = fps_rumble((uint32_t)c->x[8], &out);
    c->x[9] = out; return f;
}
uint64_t rt_fps_rumble_w9w10(Cpu *c) {
    uint32_t out; uint64_t f = fps_rumble((uint32_t)c->x[9], &out);
    c->x[10] = out; return f;
}

// The per-object clock scale's counters in x16 (the player's deferred hitstop arms them through main+0x91D6A0). The delay
// is the usual scaled decrement. The duration (`subs w16,w16,#1; str w16; b.gt`) is a plain subtraction of the ticks with
// its real flags: the game ends it at or below 0.
uint64_t rt_fps_hs_w16(Cpu *c) {
    uint32_t out; uint64_t f = fps_hs((uint32_t)c->x[16], rt_fps_ticks(), &out);
    c->x[16] = out; return f;
}
uint64_t rt_fps_duration_w16(Cpu *c) {
    uint32_t w = (uint32_t)c->x[16], t = rt_fps_ticks(), r = w - t;
    c->x[16] = r;
    return MKFLAGS(r >> 31, r == 0, w >= t, ((w ^ t) & (w ^ r)) >> 31);
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

// The dialogue text reveal (main+0x615FFC): [x19+0xA8] += [x19+0xAC] a frame, and a character shows each time it
// passes 1. A talk line ends 0.5 s after both its text and its voice are done, so at 60 fps the text ran twice as fast
// and lines whose text outlasts the voice ended early, in gameplay as well as cutscenes. s2 = the rate times the game's
// step (1.0 in a cutscene, where the pacer holds 30 presents per second).
void rt_fps_text_rate_s2(Cpu *c) { FP_SETS(2, VSF32(2) * rt_fps_step()); }

// The frame-counted cue scheduler (main+0x6A3EC0, run while the mode word at GOT main+0xDBF4A0 is 1; mode 2 schedules
// by the music time instead). Each call adds 1 to a frame counter and fires every table entry whose frame equals it,
// so at 60 calls per second the cues come twice as fast. The counter now adds the whole ticks, and an entry fires when
// its frame is in (old counter, new counter]: a frame without a tick fires nothing (an equal compare would fire the
// same cues again) and a frame of 2 ticks fires the frame it skipped.
static int32_t g_cue_prev;   // the counter before this call's ticks (game thread only)

void rt_fps_cue_up_w8(Cpu *c) {
    g_cue_prev = (int32_t)(uint32_t)c->x[8];
    c->x[8] = (uint32_t)((uint32_t)c->x[8] + rt_fps_ticks());
}
// `cmp w9, w8` (w9 = the entry's frame, w8 = the counter), read by b.ne: Z when the entry is due this call.
uint64_t rt_fps_cue_cmp(Cpu *c) {
    int32_t f = (int32_t)(uint32_t)c->x[9], now = (int32_t)(uint32_t)c->x[8];
    return MKFLAGS(0, f > g_cue_prev && f <= now, 1, 0);
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

// Cutscenes stay at 30: the game steps a cutscene by a fixed 1.0 a frame (rt_fps_timer), and the patch clears the
// frame call's second-vsync wait, so the game keeps presenting at interval 1 while it steps 1/30 s a frame. The present
// pacer (nvn_api_obj.c present_period) holds those frames to 30 per second while frame_step_cutscene says so, or the
// cutscene plays at twice its speed. The flag is the game's own, taken by the frame timer each game frame.
//
// The community cutscene fix also writes the game's frame-rate selector: 2 (30 fps) in a cutscene and 1 (60 fps) in
// gameplay at +0xF14 of the object the pointer chain below reaches, each frame, as the cheat engine's code does.
int frame_step_cutscene(void) { return fps60_on && in_cutscene; }

static void cutscene_rate(void) {
    if (!fps60_on) return;
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    uint32_t want = in_cutscene ? 2u : 1u;
    void *volatile *slot = (void *volatile *)(base + 0xCCB7B08ull);
    void *obj = *slot;
    if (!obj) return;
    obj = *(void *volatile *)((uintptr_t)obj + 0x70);
    if (!obj) return;
    volatile uint32_t *selector = (volatile uint32_t *)((uintptr_t)obj + 0xF14);
    if (*selector != want) *selector = want;
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
    // The speed line adds the game's own step under the 60 fps patch (the last game frame's, 30 fps units), so its
    // speed is game time against wall time: 1.000 when the frame-rate fix holds.
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, fps60_on ? rt_fps_step() / 30.0 : game, interval);
}
