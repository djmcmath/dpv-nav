// Host-side test for the log idle gate. Compiles the REAL src/util/log_idle.h
// and the REAL thresholds from src/config.h, not copies.
//   see tools/logidle_test/run.sh
#include "../../src/config.h"
#include "../../src/util/log_idle.h"

#include <cstdio>
#include <vector>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static const log_idle::Params P{LOG_IDLE_MAX_DEPTH_M, LOG_IDLE_MAX_SPEED_MS,
                                LOG_IDLE_MAX_DRIFT_M, LOG_IDLE_ENTER_MS};

// A unit parked on the beach: surfaced, GPS, stopped, at the origin.
static log_idle::Sample beach(uint32_t ms) { return {ms, 0.0f, 0.0f, true, 0.0f, 0.0f}; }

// Feed `beach` samples at 10 Hz from t0 for `ms` and return the last verdict.
static bool sit(log_idle::Gate& g, uint32_t t0, uint32_t ms) {
    bool idle = false;
    for (uint32_t t = t0; t <= t0 + ms; t += 100) idle = g.update(beach(t), P);
    return idle;
}

// What logging::log() does with the gate and the pacer, recording the rows.
struct Session {
    log_idle::Gate  gate;
    log_idle::Pacer pacer;
    log_idle::Sample held{};
    std::vector<log_idle::Sample> rows;
    void feed(const log_idle::Sample& s, bool gated) {
        bool idle = gated && gate.update(s, P);
        auto plan = pacer.step(s.nowMs, idle, LOG_LOW_INTERVAL_MS, LOG_IDLE_HEARTBEAT_MS);
        if (plan.flushHeld) rows.push_back(held);
        if (plan.hold) held = s;
        if (plan.write) rows.push_back(s);
    }
};

int main() {
    printf("entering idle\n");
    {
        log_idle::Gate g;
        check(!g.update(beach(1000), P), "not idle on the first quiet sample");
        check(!sit(g, 1000, LOG_IDLE_ENTER_MS - 100), "not idle just short of the enter delay");
        check(sit(g, 1000, LOG_IDLE_ENTER_MS), "idle once the enter delay has passed");
    }

    printf("each condition alone prevents idle\n");
    {
        struct Case { const char* what; log_idle::Sample s; };
        const Case cases[] = {
            {"no GPS never goes idle (indoor bench, dive mode)", {0, 0.0f, 0.0f, false, 0, 0}},
            {"at depth never goes idle",                        {0, 5.0f, 0.0f, true, 0, 0}},
            {"exactly at the depth limit is not surfaced",      {0, LOG_IDLE_MAX_DEPTH_M, 0.0f, true, 0, 0}},
            {"any speed never goes idle",                       {0, 0.0f, 0.2f, true, 0, 0}},
            {"NaN depth never goes idle",                       {0, NAN, 0.0f, true, 0, 0}},
            {"NaN speed never goes idle",                       {0, 0.0f, NAN, true, 0, 0}},
        };
        for (const auto& c : cases) {
            log_idle::Gate g;
            bool everIdle = false;
            for (uint32_t t = 0; t <= 10 * LOG_IDLE_ENTER_MS; t += 100) {
                log_idle::Sample s = c.s;
                s.nowMs = t;
                everIdle |= g.update(s, P);
            }
            check(!everIdle, c.what);
        }
        // A surface gauge that reads slightly negative is still surfaced.
        log_idle::Gate g;
        bool idle = false;
        for (uint32_t t = 0; t <= LOG_IDLE_ENTER_MS; t += 100)
            idle = g.update({t, -0.77f, 0.0f, true, 0, 0}, P);
        check(idle, "negative surface depth still goes idle");
    }

    printf("leaving idle is immediate\n");
    {
        const log_idle::Sample breakers[] = {
            {0, 1.5f, 0.0f, true, 0, 0},     // descends
            {0, 0.0f, 0.5f, true, 0, 0},     // moves
            {0, 0.0f, 0.0f, false, 0, 0},    // loses GPS
            {0, 0.0f, 0.0f, true, LOG_IDLE_MAX_DRIFT_M + 1.0f, 0},   // carried off
        };
        const char* names[] = {"descending ends idle at once", "moving ends idle at once",
                               "losing GPS ends idle at once", "drifting off ends idle at once"};
        for (int i = 0; i < 4; i++) {
            log_idle::Gate g;
            check(sit(g, 0, LOG_IDLE_ENTER_MS), "(setup) idle");
            log_idle::Sample s = breakers[i];
            s.nowMs = LOG_IDLE_ENTER_MS + 100;
            check(!g.update(s, P), names[i]);
            // ...and idle has to be earned again from scratch.
            check(!g.update(beach(LOG_IDLE_ENTER_MS + 200), P), "one quiet sample does not restore idle");
        }
    }

    printf("drift\n");
    {
        // GPS scatter inside the limit does not break idle.
        log_idle::Gate g;
        bool idle = false;
        for (uint32_t t = 0; t <= 2 * LOG_IDLE_ENTER_MS; t += 100) {
            float wobble = ((t / 100) % 2) ? 4.0f : -4.0f;
            idle = g.update({t, 0.0f, 0.0f, true, wobble, -wobble}, P);
        }
        check(idle, "scatter within the drift limit stays idle");

        // A slow carry (speed reads 0 the whole way) never goes idle while it
        // lasts, and the spell restarts at the new spot once it stops.
        log_idle::Gate c;
        bool everIdle = false;
        uint32_t t = 0;
        float x = 0;
        for (; t <= 5 * LOG_IDLE_ENTER_MS; t += 100, x += 0.1f)   // 1 m/s
            everIdle |= c.update({t, 0.0f, 0.0f, true, x, 0}, P);
        check(!everIdle, "a walk with zero indicated speed never goes idle");
        bool settled = false;
        for (uint32_t end = t + LOG_IDLE_ENTER_MS + 200; t <= end; t += 100)
            settled = c.update({t, 0.0f, 0.0f, true, x, 0}, P);
        check(settled, "idle resumes at the new resting place");
    }

    printf("millis rollover\n");
    {
        log_idle::Gate g;
        uint32_t t0 = 0xFFFFFFFFu - LOG_IDLE_ENTER_MS / 2;
        bool idle = false;
        for (uint32_t i = 0; i <= LOG_IDLE_ENTER_MS; i += 100) idle = g.update(beach(t0 + i), P);
        check(idle, "enter delay survives millis() wrapping");
    }

    printf("rows written over a typical session\n");
    {
        // 30 min on the beach, 60 min dive, surfaced with GPS for 30 min.
        auto session = [](bool gated, int& surfacingFullRate) {
            Session c;
            uint32_t t = 0;
            for (; t < 30u * 60000; t += 100) c.feed(beach(t), gated);
            for (; t < 90u * 60000; t += 100) c.feed({t, 20.0f, 0.8f, false, 0, 0}, gated);
            int before = (int)c.rows.size();
            for (uint32_t end = t + LOG_IDLE_ENTER_MS; t < end; t += 100) c.feed(beach(t), gated);
            surfacingFullRate = (int)c.rows.size() - before;
            for (; t < 120u * 60000; t += 100) c.feed(beach(t), gated);
            return (int)c.rows.size();
        };
        int sfGated = 0, sfOpen = 0;
        int gated = session(true, sfGated), open = session(false, sfOpen);
        printf("  ungated %d rows, gated %d rows\n", open, gated);
        check(open == 7200, "ungated two hours is 7200 rows");
        check(gated > 3600 && gated < 3800, "gated keeps the dive plus heartbeats");
        check(sfGated == sfOpen, "the first minute after surfacing is logged at full rate");
    }

    printf("the row that ends idle is not a heartbeat late\n");
    {
        // Idle, then the diver pulls the trigger 37.3 s after the last
        // heartbeat. The track processor integrates each row's speed over the
        // time since the row before it, so that first moving row must sit right
        // behind a still one -- not 37 s behind.
        Session c;
        uint32_t t = 0;
        for (; t < 5u * 60000 + 37300; t += 100) c.feed(beach(t), true);
        size_t idleRows = c.rows.size();
        uint32_t lastHeartbeat = c.rows.back().nowMs;
        check(t - lastHeartbeat > 30000, "(setup) the last heartbeat is well in the past");
        for (uint32_t end = t + 5000; t < end; t += 100) c.feed({t, 0.0f, 0.8f, true, 0, 0}, true);

        const auto& still = c.rows[idleRows];
        const auto& moving = c.rows[idleRows + 1];
        check(still.speedMs == 0.0f, "a still row is written first");
        check(moving.speedMs > 0.0f, "then the first moving row");
        check(moving.nowMs - still.nowMs == 100, "the moving row spans one tick, not the idle gap");
        check(still.nowMs - lastHeartbeat <= LOG_IDLE_HEARTBEAT_MS, "the still row stays within a heartbeat");

        // No step in the whole log carries speed across more than the normal
        // interval, and none exceeds the heartbeat at all.
        uint32_t worstMoving = 0, worstAny = 0;
        for (size_t i = 1; i < c.rows.size(); i++) {
            uint32_t dt = c.rows[i].nowMs - c.rows[i - 1].nowMs;
            if (dt > worstAny) worstAny = dt;
            if (c.rows[i].speedMs > 0.0f && dt > worstMoving) worstMoving = dt;
        }
        check(worstMoving <= LOG_LOW_INTERVAL_MS, "no moving row follows a long step");
        check(worstAny <= LOG_IDLE_HEARTBEAT_MS, "no step is longer than the heartbeat");

        // Idle ending on the very tick a heartbeat was written has nothing held to flush.
        Session d;
        t = 0;
        for (; t <= LOG_IDLE_ENTER_MS + LOG_IDLE_HEARTBEAT_MS + LOG_LOW_INTERVAL_MS; t += 100) {
            d.feed(beach(t), true);
            if (d.gate.idle() && d.rows.back().nowMs == t && t > LOG_IDLE_ENTER_MS + 1000) break;
        }
        size_t n = d.rows.size();
        uint32_t heartbeatAt = d.rows.back().nowMs;
        check(heartbeatAt == t, "(setup) a heartbeat was written on this tick");
        for (uint32_t end = t + LOG_LOW_INTERVAL_MS; t < end; ) { t += 100; d.feed({t, 0.0f, 0.8f, true, 0, 0}, true); }
        check(d.rows.size() == n + 1 && d.rows.back().speedMs > 0.0f,
              "leaving idle right after a heartbeat adds no extra still row");
        check(d.rows.back().nowMs - heartbeatAt <= LOG_LOW_INTERVAL_MS,
              "and the moving row follows within the normal interval");
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nAll log_idle tests passed.\n");
    return 0;
}
