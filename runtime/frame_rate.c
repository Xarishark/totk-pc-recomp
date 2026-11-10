// Frame rate: the game's 60 fps patch (recomp/patches-v101.txt, set fps60) is on by default, and the menu's frame
// rate row switches it off for the game's shipping 30 fps. The set replaces the game's frame timer with a real-time step
// (rt_fps_timer) and asks the frame call for present interval 1; this file switches the set while the game runs, keeps
// the step and the whole 30 fps ticks it adds up to, caps the host's present rate at 60 or 30, and keeps the world clock
// for scripted input. The whole ticks and the step make the game's per-frame counters and blends frame-rate independent
// (the input/action timers, hitstop, rumble, the camera smoothing, the dialogue text reveal, the cue scheduler).
//
// The game steps a cutscene by a fixed frame, so at 60 presents per second it would play twice as fast as its
// animation data. Under the 60 fps patch an in-engine cutscene steps by the real time instead and presents at 60
// (cutscene_step below); a cutscene with a full-screen movie keeps the fixed step and is held to 30 presents per second
// (cutscene_rate below).
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

#include "audren.h"
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

// ---- 60 fps cutscenes: part of the 60 fps patch (AC_FPS=60), without a setting of their own
// The game steps a cutscene by a fixed 1.0 (1/30 s) a frame and plays it frame-locked, so at 60 presents per second it
// would run at twice its speed. Under the 60 fps patch a cutscene frame steps by the real time instead, in
// whole 60 Hz vsyncs (0.5 each): the cutscene presents at 60 and keeps to the wall clock, which the voices and music
// play on. A frame that took longer than its vsyncs (a hitch, a shader compile) leaves its time as debt, which the next
// frames pay back at up to 4 vsyncs (step 2.0, the original timer's high clamp) each, so the picture catches up with
// the sound instead of staying behind. The debt is capped at 1 s: what a longer stall leaves beyond that is dropped
// (and counted in the log), so a load or a debugger pause does not end in seconds of fast-forward. A stall the sound
// stood still in as well (the process suspended, a debugger break) is not caught up at all: the debt is there to bring
// the picture back to the voices, and those did not move (cutscene_stall). The cut clock's end-of-cut test is made
// exact for steps of 0.5 (rt_cut_* below). A full-screen movie (a Bink file with its own clock) keeps the cutscene it
// plays in at the 30 fps hold (rt_movie_* below).
#define CUT_VSYNC_MS (1000.0 / 60.0)
#define CUT_VSYNCS_MAX 4             // a frame steps 2.0 at most, the original timer's high clamp
#define CUT_DEBT_MAX_MS 1000.0       // the frame time a cutscene catches up after a stall
#define CUT_STALL_MS 100.0           // a frame this long is checked against the audio device's clock
#define CUT_STALL_SLACK_MS 50.0      // what the device's clock may lag a frame's time by: it moves in device periods
#define MOVIE_HOLD_FRAMES 8          // a movie counts as playing for this many game frames after its last BinkWait

static volatile int cut60_now;            // this game frame steps a cutscene at 60 (rt_fps_timer; game thread only writes)
static _Atomic unsigned fps_switches;     // times frame_limit_live switched the 60 fps patch (rt_fps_timer restarts on it)
static _Atomic uint64_t g_timer_frames;   // rt_fps_timer calls so far: the game frame the movie hold counts in
static double g_cut_debt_ms;              // cutscene time not yet stepped (game thread only)
static uint64_t cut_audio_ns;             // audio_device_clock_ns at the last frame of the run (game thread only)
// one run of 60 fps cutscene frames, for the log line at its end (cutscene_report; game thread only)
static unsigned cut_catchup;
static double cut_drop_ms, cut_debt_peak;

static int movie_playing(void);
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
//   - fixed step (a cutscene): the whole vsyncs of the real time (cutscene_step), presented at 60. While a full-screen
//     movie plays in it: 1.0, as the original, and the present pacer holds the cutscene to 30 presents per second
//     (nvn_api_obj.c present_period, frame_step_cutscene), so 1.0 is its real time.
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

// The cutscene step of one frame at 60 (30 fps units): the frame's time plus the debt, in whole vsyncs, 1 to 4. The
// first frame of a run starts without debt and counts one to two vsyncs, whatever came before it (a load, a movie),
// and restarts the whole-tick remainder at 1/2, so the first frame and every other one after it have a tick. A 30 fps
// frame runs its logic at the start of its 1/30 s; of the two 60 fps frames in that 1/30 s, the first starts at the
// same time, so the tick-gated talk logic (rt_fps_talk_*, rt_fps_voiced_*) runs when it runs at 30 rather than on
// whatever phase the gameplay before the cutscene left.
//
// cutscene_stall: the part of a long frame's time the cutscene may catch up. A frame over CUT_STALL_MS counts only as
// long as the audio device played in it (audio_mixer.c audio_device_clock_ns) plus the slack: when the game thread
// alone stalled (a shader compile, streaming) the device kept asking for sound and the whole frame counts; when the
// device stood still too, the voices are where they were and the rest of the frame is dropped (counted in the log).
// Without a device the clock is 0 and the frame counts whole.
static double cutscene_stall(double dt_ms, int first) {
    uint64_t now = audio_device_clock_ns(), prev = cut_audio_ns;
    cut_audio_ns = now;
    if (first || !prev || !(dt_ms > CUT_STALL_MS)) return dt_ms;
    double played_ms = (double)(now - prev) / 1e6 + CUT_STALL_SLACK_MS;
    if (played_ms >= dt_ms) return dt_ms;
    cut_drop_ms += dt_ms - played_ms;
    return played_ms;
}

static float cutscene_step(double dt_ms, int first) {
    if (first) {
        g_cut_debt_ms = 0;
        g_fps_rem = 0.5;   // a whole tick on the first frame and every other one after it (see above)
        dt_ms = dt_ms < CUT_VSYNC_MS ? CUT_VSYNC_MS : dt_ms > 2 * CUT_VSYNC_MS ? 2 * CUT_VSYNC_MS : dt_ms;
    }
    if (!(dt_ms >= 0.0)) dt_ms = CUT_VSYNC_MS;
    dt_ms = cutscene_stall(dt_ms, first);
    double d = g_cut_debt_ms + dt_ms;
    int n = (int)floor(d / CUT_VSYNC_MS + 0.5);
    if (n < 1) n = 1;
    if (n > CUT_VSYNCS_MAX) n = CUT_VSYNCS_MAX;
    d -= n * CUT_VSYNC_MS;
    if (d > CUT_DEBT_MAX_MS) {
        cut_drop_ms += d - CUT_DEBT_MAX_MS;
        d = CUT_DEBT_MAX_MS;
    }
    if (d < -CUT_VSYNC_MS) d = -CUT_VSYNC_MS;   // one vsync at least, even for a frame shorter than one
    g_cut_debt_ms = d;
    if (n > 2 && dt_ms < (n - 0.5) * CUT_VSYNC_MS) cut_catchup++;   // more vsyncs than the frame itself took
    if (d > cut_debt_peak) cut_debt_peak = d;
    return (float)n * 0.5f;
}

// Seconds since the guest started, for the log lines that mark where things happen.
static double wall_s(void) { return rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0; }

// ---- the cutscene log line: one per run of cutscene frames in one mode (at 60, or held to 30), at its end. Measured
// is the frame time the game's timer took, stepped what the frames moved the cutscene by (1/30 s per 1.0 of step); the
// two differ by the debt still open, the stall time dropped and the first frame's clamp at 60, and by every late frame
// when held (the held mode is frame-locked, as the original).
static unsigned run_frames;
static double run_wall_ms, run_game_ms;

static void cutscene_report(int at60) {
    if (!run_frames) return;
    if (at60)
        rt_log("frame rate: cutscene run at 60: %u frames, %.3f s measured, %.3f s stepped, %u catch-up frames, peak "
               "debt %.1f ms, %.1f ms of stalls dropped",
               run_frames, run_wall_ms / 1000.0, run_game_ms / 1000.0, cut_catchup, cut_debt_peak, cut_drop_ms);
    else
        rt_log("frame rate: cutscene run held to 30: %u frames, %.3f s measured, %.3f s stepped", run_frames,
               run_wall_ms / 1000.0, run_game_ms / 1000.0);
    run_frames = cut_catchup = 0;
    run_wall_ms = run_game_ms = cut_drop_ms = cut_debt_peak = 0;
}

// The game's audio sync of an event with a voice track (main+0x8B9DC0 and its twin main+0x8C2E10): s0 = the track's
// position minus the picture's, in seconds (the cut frames are 1/59.94 s). The game turns more than 30 ms of it into
// the timer override, which only the step outside the cutscene flag takes, so the check may run with the flag off.
// Recorded either way (site: after the fadd that forms it) and logged once the checks stop for a second.
static unsigned sync_n, sync_flag_n;
static double sync_sum, sync_lo, sync_hi;
static uint64_t sync_last_ns;

void rt_cut_drift_s0(Cpu *c) {
    double ms = VSF32(0) * 1000.0;
    if (!sync_n || ms < sync_lo) sync_lo = ms;
    if (!sync_n || ms > sync_hi) sync_hi = ms;
    sync_sum += ms;
    sync_n++;
    sync_flag_n += in_cutscene != 0;
    sync_last_ns = kern_now_ns();
}

static void sync_report(void) {
    if (!sync_n || kern_now_ns() - sync_last_ns < 1000000000ull) return;
    rt_log("frame rate: voice sync (wall %.3f s): audio minus picture %+.1f ms mean, %+.1f..%+.1f ms over %u checks, "
           "%u of them with the cutscene flag set",
           wall_s(), sync_sum / sync_n, sync_lo, sync_hi, sync_n, sync_flag_n);
    sync_n = sync_flag_n = 0;
    sync_sum = 0;
}

// AC_TEST_HITCH=<ms>[:<every ms>] (testing): the game thread sleeps <ms> in a cutscene frame, once per <every ms> of
// wall time (every frame without it), so the cutscene's hitch handling can be measured. Read with the frame rate.
static volatile uint32_t hitch_ms, hitch_every_ms;   // frame_limit_live writes, the game thread reads
static uint64_t hitch_last_ns;

static void test_hitch(void) {
    if (!hitch_ms) return;
    uint64_t now = kern_now_ns();
    if (hitch_every_ms && hitch_last_ns && now - hitch_last_ns < hitch_every_ms * 1000000ull) return;
    hitch_last_ns = now;
    plat_sleep_ns(hitch_ms * 1000000ull);
}

static int movie_now;   // a full-screen movie held the last cutscene frame (game thread, for the log)
static unsigned timer_switches;   // fps_switches at the last timer call (game thread only)

void rt_fps_timer(Cpu *c) {
    uint8_t *t = (uint8_t *)(uintptr_t)c->x[19];
    int fixed = (uint32_t)c->x[21] == 0;
    double dt_ms = *(float volatile *)(t + 0xE0);
    atomic_fetch_add_explicit(&g_timer_frames, 1, memory_order_relaxed);
    // The 60 fps patch was switched since the last call (frame_limit_live, on the present path): the timer did not run
    // at 30, so what it knew of a cutscene is stale. The run it was in ends here, under the mode it ran in, and this
    // frame starts from gameplay. Done here so that only the game thread writes this state.
    unsigned switches = atomic_load_explicit(&fps_switches, memory_order_relaxed);
    if (switches != timer_switches) {
        timer_switches = switches;
        if (in_cutscene) cutscene_report(cut60_now);
        in_cutscene = cut60_now = movie_now = 0;
    }
    int movie = fixed && movie_playing();       // a full-screen movie keeps its cutscene at the 30 fps hold
    int cut60 = fixed && !movie;
    int ended = in_cutscene && (!fixed || cut60 != cut60_now);   // the run of cutscene frames before this one
    int first = fixed && (!in_cutscene || cut60 != cut60_now);   // a new run: its frame time is what came before it
    float step;
    if (fixed) {
        step = cut60 ? cutscene_step(dt_ms, first) : 1.0f;
        g_fps_debt_ms = 0;
    } else {
        double s = frame_paced_ms(dt_ms) * *(float volatile *)(t + 0xDC);
        float *ov = *(float *volatile *)((uintptr_t)MOD_MAIN_BASE + 0xDC0F10ull);   // main+0x8A9570's float
        if (ov && *ov > 0.0f) s *= *ov;
        step = s > 2.0 ? 2.0f : s < 1.0 / 16.0 ? 1.0f / 16.0f : (float)s;
    }
    *(float volatile *)(t + 0xD0) = step;
    *(uint32_t volatile *)(t + 0xD4) = 0;
    *(uint32_t volatile *)(t + 0xD8) = 0;
    *(uint32_t volatile *)(t + 0xE4) = 0;
    if (ended) cutscene_report(cut60_now);
    if (in_cutscene != fixed || cut60 != cut60_now || movie != movie_now)
        rt_log("frame rate: %s (world %.2f s, wall %.3f s): presents held to %d per second",
               !fixed ? "gameplay" : cut60 ? "cutscene at 60 fps" : movie ? "cutscene with a movie" : "cutscene",
               (double)frame_step_world_us() / 1e6, wall_s(), fixed && !cut60 ? 30 : 60);
    // frame_step_cutscene reads both on the present path: written in an order that never shows a cutscene at 60 as held
    if (fixed) cut60_now = cut60, in_cutscene = 1;
    else in_cutscene = 0, cut60_now = 0;
    movie_now = movie;
    if (fixed) {
        run_frames++;
        run_game_ms += step * (1000.0 / 30.0);
        run_wall_ms += first ? step * (1000.0 / 30.0) : dt_ms;
        test_hitch();
    }
    sync_report();
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
// step (1.0 in a cutscene held to 30 presents per second, the real time in one at 60).
// The talk line's state machine (main+0x6161D0, [x19+0xB0] = 0..3, anything above returns at once), run every frame by
// the line's update. It times nothing itself: it polls what is timed (the voice, the text box's layout animations,
// which follow the step) and moves on a frame after it sees a change: 33 ms at 30, 17 ms at 60. With the talk commands
// below it made each line of a cutscene at 60 come about 50 ms sooner, 1.3 s over a 124 s scene. In a cutscene at 60
// the machine now runs only on frames with a whole 30 fps tick: w8 = 4 (nothing to do) on the others, so it sees the
// changes 30 times a second, as the game was built for.
void rt_fps_talk_state_w8(Cpu *c) {
    c->x[8] = cut60_now && !rt_fps_ticks() ? 4u : *(uint32_t volatile *)(uintptr_t)(c->x[19] + 0xB0);
}

// The event script's talk command (main+0x443E50, an entry of the event command table at main+0xAB2720): the script
// calls it every frame until it returns 1. It polls the line (voice done, text box state) and moves on a frame after
// it sees a change, and its waits are countdowns: [x19+0x84], 0.5 s after a line, adds step * -1/30 a call (at 30 fps
// -1/30 rounds to -0.033333335 in single precision, so 0.5 is still 2e-8 after 15 frames and the wait lasts 16 frames,
// 0.533 s), and [x19+0x7C] counts down the same way (main+0x443F90). At 60 the command ran twice as often: each
// line's waits and reactions came sooner, and a 124 s scene ended 1.3 s early. In a cutscene at 60 it runs only on
// frames with a whole 30 fps tick, and its step-based countdowns take the whole ticks: the same calls, decrements and
// rounding as at 30. On the other frames it returns 0 (still running) at once and leaves the scene's per-frame talking
// flag ([[x19+8]+0x9F0], which it clears on entry and sets while a line plays) as the last run left it.
static int talk_skip(void) { return cut60_now && !rt_fps_ticks(); }

void rt_fps_talk_clear(Cpu *c) {   // str wzr, [x8, #0x9f0]: the talking flag, cleared on a run
    if (!talk_skip()) *(uint32_t volatile *)(uintptr_t)(c->x[8] + 0x9F0) = 0;
}
void rt_fps_talk_busy_w8(Cpu *c) {   // ldr w8, [x24, #0x28]: nonzero returns at once; 1 on a skipped frame
    c->x[8] = talk_skip() ? 1u : *(uint32_t volatile *)(uintptr_t)(c->x[24] + 0x28);
}
// mov w20, #1: that return's value, 0 (still running) on a skipped frame. A helper may only change caller-saved
// registers, so it sets w8 (dead on the way out) and the site's `mov w20, w8` copies it.
void rt_fps_talk_ret_w8(Cpu *c) { c->x[8] = talk_skip() ? 0u : 1u; }

// The voiced talk command (main+0x446C40, with its variant main+0x447600 behind [x19+0x4C]): while its voice
// ([x19+0x60]) is still in the sound engine it waits, and then counts [x19+0x68] / [x19+0x70] down from 0.5 s by
// step * -1/30 (main+0x4473DC, main+0x447414, main+0x4477FC), the same countdown as the talk command's. It is gated
// the same way: in a cutscene at 60, on frames without a whole tick, it reads its done flag ([x19+0x44], which it sets
// itself at main+0x4472BC and main+0x447578 and returns 1 on at its next call) and its variant switch as 0 and its
// state as 4, which together return 0 (still running) at once; its countdowns take the whole ticks. The scene measured
// does not use it, so this follows from the code, not from a measurement.
void rt_fps_voiced_done_w8(Cpu *c) {   // ldr w8, [x19, #0x44]
    c->x[8] = talk_skip() ? 0u : *(uint32_t volatile *)(uintptr_t)(c->x[19] + 0x44);
}
void rt_fps_voiced_alt_w8(Cpu *c) {   // ldr w8, [x19, #0x4c]
    c->x[8] = talk_skip() ? 0u : *(uint32_t volatile *)(uintptr_t)(c->x[19] + 0x4C);
}
void rt_fps_voiced_state_w8(Cpu *c) {   // ldr w8, [x19, #0x24]
    c->x[8] = talk_skip() ? 4u : *(uint32_t volatile *)(uintptr_t)(c->x[19] + 0x24);
}

// The other talk command (main+0x446300): [x19+0x24] = 0 starts the line (main+0x615000), 1 polls the text box and
// the voice until the line is over and returns 1; any other state returns 0 (still running) at once. In a cutscene at
// 60 it reads 2 on frames without a whole 30 fps tick, so it starts and ends lines on the frames it would at 30.
void rt_fps_msg_state_w8(Cpu *c) {
    c->x[8] = talk_skip() ? 2u : *(uint32_t volatile *)(uintptr_t)(c->x[19] + 0x24);
}

void rt_fps_talk_wait_s1(Cpu *c) {   // ldr s1, [x8|x9, #0xd0] (the step) at a countdown: the whole ticks at 60
    FP_SETS(1, cut60_now ? (float)rt_fps_ticks() : rt_fps_step());
}

// In a cutscene at 60 the rate is times the whole 30 fps ticks instead: the same additions as at 30, so a line's text
// finishes on the same 30 fps frame (rate * 0.5 rounds differently, and some lines finished a frame early).
void rt_fps_text_rate_s2(Cpu *c) {
    FP_SETS(2, VSF32(2) * (cut60_now ? (float)rt_fps_ticks() : rt_fps_step()));
    // AC_TRACE=perf: the start of each line's text in a cutscene ([x19+0xA0] the characters shown, +0xA4 its length)
    if (!(rt_trace & RT_TRACE_PERF)) return;
    static uintptr_t line_obj;
    static uint32_t line_shown, line_n;
    uintptr_t obj = (uintptr_t)c->x[19];
    uint32_t shown = *(uint32_t volatile *)(obj + 0xA0), len = *(uint32_t volatile *)(obj + 0xA4);
    if (in_cutscene && (obj != line_obj || shown < line_shown))
        rt_log("frame rate: line %u (%u characters) starts at %.3f s stepped, wall %.3f s", ++line_n, len,
               run_game_ms / 1000.0, wall_s());
    line_obj = obj, line_shown = shown;
}

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

// The cut clock (main+0x8C2410, one call per clock and frame): time in the current cut += rate * step * speed, where
// rate (s8) is 1 or 2 by the data's authored frame rate and the cut lasts len = w24 units (x19 = the clock it
// writes, [x19] the cut, [x19+4] the time; x20 = the clock it read). Past len - rate it shows len - rate once (the
// hold) and then starts the next cut at 0. At step 1.0 the time lands on len - rate exactly, so every frame of the cut
// shows once. At step 0.5 the hold would end each cut one 60 fps frame early (1/60 s per cut, the picture pulling ahead
// of the voices), and after a hitch's larger step the next cut would start at 0 and lose what the step went past the
// cut's end. While a cutscene runs at 60 (cut60_now) the end test becomes: the cut goes on while its time is below
// len, and the next cut starts at what the time went past len. At step 1.0 that is the original's sequence. Otherwise
// the game's own test runs.
void rt_cut_hold_s1(Cpu *c) {   // fsub s1, s1, s8 (s1 = len): the time the cut may reach and still go on
    float len = VSF32(1);
    FP_SETS(1, cut60_now ? nextafterf(len, -INFINITY) : len - VSF32(8));
}
void rt_cut_prev_s0(Cpu *c) {   // ldr s0, [x20, #4]: the time before this frame; the hold runs while it is below s1
    FP_SETS(0, cut60_now ? VSF32(1) : *(float volatile *)(uintptr_t)(c->x[20] + 4));
}
void rt_cut_next_s1(Cpu *c) {   // fmov s1, wzr: where the next cut starts
    float start = 0.0f;
    if (cut60_now) {
        float over = *(float volatile *)(uintptr_t)(c->x[19] + 4) - (float)(int32_t)(uint32_t)c->x[24];
        float most = 2.0f * VSF32(8);   // the step's high clamp, 2.0, times the rate
        start = over > 0.0f ? (over < most ? over : most) : 0.0f;
    }
    FP_SETS(1, start);
}

// AC_TRACE=perf, diagnostics: the scene clock after its update and the event's commit (main+0x8B7CB0, x19 = the event
// object, +0x20 the cut and time it kept, +0x28 the cut before). Logs each committed cut change and each stretch the
// clock stood still (a stop point waiting for a line), with where it stood. The event's commit (main+0x8C2590) applies
// the stop points, not the cut clock's end test: in the scene measured both modes stood at the same cut times.
void rt_cut_state(Cpu *c) {
    if (!(rt_trace & RT_TRACE_PERF) || !in_cutscene) return;
    static struct { uintptr_t obj; int32_t idx; float t; unsigned still; double since; } k[4];
    uintptr_t obj = (uintptr_t)c->x[19];
    int32_t idx = *(int32_t volatile *)(obj + 0x20);
    float t = *(float volatile *)(obj + 0x24);
    int i = 0;
    while (i < 4 && k[i].obj && k[i].obj != obj) i++;
    if (i == 4) return;
    if (!k[i].obj) {
        k[i].obj = obj, k[i].idx = idx, k[i].t = t;
        return;
    }
    if (idx == k[i].idx && t == k[i].t) {
        if (!k[i].still++) k[i].since = run_game_ms / 1000.0;
        return;
    }
    if (k[i].still)
        rt_log("frame rate: scene %06llx stood at cut %d, %.2f for %u frames (%.3f s) from %.3f s stepped",
               (unsigned long long)(obj & 0xFFFFFF), k[i].idx, k[i].t, k[i].still, run_game_ms / 1000.0 - k[i].since,
               k[i].since);
    if (idx != k[i].idx)
        rt_log("frame rate: scene %06llx cut %d (%.2f) -> cut %d (%.2f) at %.3f s stepped, wall %.3f s",
               (unsigned long long)(obj & 0xFFFFFF), k[i].idx, k[i].t, idx, t, run_game_ms / 1000.0, wall_s());
    k[i].idx = idx, k[i].t = t, k[i].still = 0;
}

// ---- full-screen movies: the game's Bink player (the class whose open is main+0x9BEFD0) plays both the event movies
// (movie/ev%04x.bk2: the opening, the chapter movies, the title screen's background) and the effect movies
// (mv/mv%04x.bk2, textures in the scene). The event movies are 1600x900 or 1280x720 Bink files at 30 fps on Bink's own
// clock (BinkWait, main+0x7003E0). The game opens most of them from a file handle, so there is no name to tell them
// by, but their size does: of the effect movies only four short ones (mv0065, mv0504, mv0510, mv0511) are as large,
// and the rest are 1280x460 or smaller. While one plays in a cutscene, the cutscene keeps the 30 fps hold
// (rt_fps_timer): Bink shows a new frame 30 times a second, and decoding 1600x900 on the game thread made a third of
// 60 fps frames late in the opening movie. Outside cutscenes movies are not held (frame_step_cutscene), so the title
// screen's menus stay at 60. The player's BinkOpen call returns the handle (main+0x9BF058, x0), BinkClose
// (main+0x9BF26C) and BinkWait (three sites) take it in x0. The sites are a set of their own (patches-v101.txt,
// movies), on at 30 fps too, so a movie opened before a switch to 60 is known.
#define MOVIE_SLOTS 4
#define MOVIE_MIN_PIXELS (1280u * 720u)
static _Atomic uintptr_t g_movies[MOVIE_SLOTS];   // the open full-screen movies' Bink handles
static _Atomic uint64_t g_movie_wait_frame;      // the game frame (g_timer_frames) of the last BinkWait on one; 0 none

// x0 = the handle BinkOpen returned. HBINK starts with the width, the height and the frame count.
void rt_movie_opened(Cpu *c) {
    uintptr_t h = (uintptr_t)c->x[0];
    if (!h) return;
    const uint32_t *hb = (const uint32_t *)h;
    if ((uint64_t)hb[0] * hb[1] < MOVIE_MIN_PIXELS) return;
    for (int i = 0; i < MOVIE_SLOTS; i++) {
        uintptr_t none = 0;
        if (atomic_compare_exchange_strong(&g_movies[i], &none, h)) {
            // It counts as playing from here on: the timer ran before the player in this frame, so without this the
            // frames up to the first BinkWait would step at 60.
            atomic_store_explicit(&g_movie_wait_frame, atomic_load_explicit(&g_timer_frames, memory_order_relaxed),
                                  memory_order_relaxed);
            rt_log("frame rate: movie opened, %ux%u, %u frames (wall %.3f s); a cutscene holds 30 fps while it plays",
                   hb[0], hb[1], hb[2], wall_s());
            return;
        }
    }
    // BinkClose has one caller in main (the site at main+0x9BF26C), so a slot is never left behind; this is more
    // full-screen movies open at once than the table holds, and this one plays without the hold.
    rt_log("frame rate: movie opened, %ux%u, but %d full-screen movies are open already: not tracked", hb[0], hb[1],
           MOVIE_SLOTS);
}

// The last full-screen movie closing ends the hold at once; closing one of two (the opening's logo, before the
// opening) keeps it.
void rt_movie_close(Cpu *c) {
    uintptr_t h = (uintptr_t)c->x[0];
    int closed = 0, open = 0;
    for (int i = 0; i < MOVIE_SLOTS; i++) {
        uintptr_t want = h;
        if (h && atomic_compare_exchange_strong(&g_movies[i], &want, 0)) closed = 1;
        else if (atomic_load(&g_movies[i])) open = 1;
    }
    if (!closed) return;
    if (!open) atomic_store(&g_movie_wait_frame, 0);
    rt_log("frame rate: movie closed (wall %.3f s)", wall_s());
}

void rt_movie_wait(Cpu *c) {
    uintptr_t h = (uintptr_t)c->x[0];
    for (int i = 0; i < MOVIE_SLOTS; i++)
        if (h && atomic_load_explicit(&g_movies[i], memory_order_relaxed) == h) {
            atomic_store_explicit(&g_movie_wait_frame, atomic_load_explicit(&g_timer_frames, memory_order_relaxed),
                                  memory_order_relaxed);
            return;
        }
}

// A movie is playing while one waited for its next frame within the last MOVIE_HOLD_FRAMES game frames. Counted in
// game frames, not wall time: a stall of the game thread (a hitch while the movie decodes) passes no frames, so it
// does not end the hold. rt_fps_timer counts its own call first, so the frame count is at least 1 at any BinkWait.
static int movie_playing(void) {
    uint64_t w = atomic_load_explicit(&g_movie_wait_frame, memory_order_relaxed);
    return w && atomic_load_explicit(&g_timer_frames, memory_order_relaxed) - w <= MOVIE_HOLD_FRAMES;
}


// The frame rate (menu, AC_FPS60): 60 fps runs the game's 60 fps patch, 30 fps switches it off. Read live: the patch
// set is switched in frame_limit_live, so the generated C picks the patched or the original instructions from the next
// site it runs.
static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (fps_read && v == fps_seen) return;
    fps_read = 1;
    fps_seen = v;
    unsigned hm = 0, he = 0;
    const char *hitch = cfg_get("AC_TEST_HITCH");
    if (hitch && sscanf(hitch, "%u:%u", &hm, &he) >= 1 && (hm != hitch_ms || he != hitch_every_ms)) {
        if (he) rt_log("frame rate: test hitch of %u ms in a cutscene frame every %u ms", hm, he);
        else rt_log("frame rate: test hitch of %u ms in every cutscene frame", hm);
    }
    hitch_ms = hm, hitch_every_ms = he;
    int hz = cfg_int("AC_FPS");
    int on = hz >= 60;
    if (on == fps60_on && fps_hz) return;
    fps60_on = on;
    // the timer does not run at 30: it ends the cutscene run it was in and starts anew at its next call (rt_fps_timer)
    atomic_fetch_add_explicit(&fps_switches, 1, memory_order_relaxed);
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
// A cutscene at 60 (cut60_now) steps by the real time and is not held; while an event movie plays in a cutscene, the
// cutscene is held. Outside a cutscene a movie is not held: the title screen's background is an
// event movie (movie/ev0001.bk2) and its prompt and menus run at 60 over it, each movie frame on two presents.
//
// The community cutscene fix also writes the game's frame-rate selector: 2 (30 fps) in a held cutscene and 1 (60 fps)
// otherwise at +0xF14 of the object the pointer chain below reaches, each frame, as the cheat engine's code does.
int frame_step_cutscene(void) { return fps60_on && in_cutscene && !cut60_now; }

static void cutscene_rate(void) {
    if (!fps60_on) return;
    uintptr_t base = (uintptr_t)MOD_MAIN_BASE;
    uint32_t want = frame_step_cutscene() ? 2u : 1u;
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
