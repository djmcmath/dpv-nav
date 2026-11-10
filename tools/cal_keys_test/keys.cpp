// Host tests for the cal screens' softkeys (src/util/cal_keys.h). Compiles the
// real header, not a copy -- see run.sh.
//
// What these pin down: a hold of LONG_PRESS_MS on BTN1 alone is the only way to
// throw a cal away, it fires once, and nothing involving both buttons at once
// ever fires anything here (that gesture belongs to the display reinit).
#include <cstdio>

#include "../../src/util/cal_keys.h"

using cal_keys::Button;
using cal_keys::Result;
using cal_keys::Tracker;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

// Two buttons and a clock, driven the way display_main drives them: the
// debounced state changes, then update() runs, then the caller marks fired.
struct Rig {
    Tracker  t;
    Button   b1{false, 0, false}, b2{false, 0, false};
    uint32_t now = 10000;
    int discards = 0, shorts = 0, advances = 0;
    uint8_t lastPct = 0;

    Rig() { t.reset(); }

    Result pass() {
        const Result r = t.update(b1, b2, now);
        lastPct = r.holdPct;
        if (r.discard)    { discards++; b1.fired = true; }
        if (r.shortPress) { shorts++;   b1.fired = true; }
        if (r.advance)    { advances++; b2.fired = true; }
        return r;
    }
    // Advance the clock in 10 ms passes.
    void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i += 10) { now += 10; pass(); } }

    void press1()   { b1 = {true, now, false}; pass(); }
    void release1() { b1.pressed = false; pass(); }
    void press2()   { b2 = {true, now, false}; pass(); }
    void release2() { b2.pressed = false; pass(); }
};

int main() {
    printf("cal softkeys\n");

    // --- BTN1 hold ----------------------------------------------------------
    {
        Rig r;
        r.press1();
        check(r.lastPct == 0, "a fresh press has no fill");
        r.run(LONG_PRESS_MS / 2);
        check(r.lastPct == 50, "half the hold reads 50%");
        check(r.discards == 0, "and has not fired");
        r.run(LONG_PRESS_MS / 2 - 10);
        check(r.discards == 0 && r.lastPct == 99, "10 ms short still has not fired");
        r.run(10);
        check(r.discards == 1, "the hold fires at LONG_PRESS_MS");
        r.run(3000);
        check(r.discards == 1, "and only once, however long it is held");
        check(r.lastPct == 0, "the fill clears once it has fired");
        r.release1();
        check(r.shorts == 0, "releasing after it fired is not a short press");
    }
    {
        Rig r;
        r.press1();
        r.run(1500);
        check(r.lastPct == 75, "1.5 s reads 75%");
        r.release1();
        check(r.discards == 0, "releasing at 1.5 s throws nothing away");
        check(r.shorts == 1, "and shows the hint");
        check(r.lastPct == 0, "the fill resets on release");
        r.run(1000);
        check(r.shorts == 1 && r.discards == 0, "the release is acted on once");
    }
    {
        Rig r;
        r.press1();
        r.run(40);
        r.release1();
        check(r.shorts == 1 && r.discards == 0, "a tap is a short press");
    }
    {
        Rig r;
        r.press1();
        r.run(1500);
        r.release1();
        r.press1();
        r.run(1500);
        check(r.discards == 0 && r.lastPct == 75, "a second hold starts again from zero");
        r.run(500);
        check(r.discards == 1, "and fires on its own full length");
    }

    // --- BTN2 ---------------------------------------------------------------
    {
        Rig r;
        r.press2();
        r.run(300);
        check(r.advances == 0, "BTN2 acts on release, not press");
        r.release2();
        check(r.advances == 1, "a BTN2 press advances");
        r.run(500);
        check(r.advances == 1, "once");
        r.press2();
        r.run(3000);
        r.release2();
        check(r.advances == 2, "a long BTN2 press is still just an advance");
        check(r.discards == 0 && r.shorts == 0, "and BTN2 never discards");
    }

    // --- Both buttons -------------------------------------------------------
    {
        Rig r;                      // BTN1 first, then BTN2 joins
        r.press1();
        r.run(800);
        check(r.lastPct == 40, "BTN1 alone is filling");
        r.press2();
        check(r.lastPct == 0, "BTN2 joining resets the fill");
        r.run(3000);
        check(r.discards == 0, "and the hold never fires while both are down");
        r.release2();
        r.run(3000);
        check(r.discards == 0 && r.lastPct == 0, "or after BTN2 lets go, for the rest of that BTN1 press");
        r.release1();
        check(r.advances == 0 && r.shorts == 0, "neither release acts after a chord");
    }
    {
        Rig r;                      // a quick two-button tap
        r.press1();
        r.press2();
        r.run(100);
        r.release1();
        r.release2();
        check(r.advances == 0 && r.shorts == 0 && r.discards == 0, "a two-button tap does nothing");
    }
    {
        Rig r;                      // BTN2 first, BTN1 joins, BTN2 lets go first
        r.press2();
        r.run(200);
        r.press1();
        r.run(200);
        r.release2();
        r.run(2500);
        check(r.discards == 0, "a BTN1 press that began with BTN2 down never discards");
        check(r.advances == 0, "and BTN2's release was not an advance");
        r.release1();
        r.press1();
        r.run(LONG_PRESS_MS);
        check(r.discards == 1, "the next clean BTN1 hold works");
    }
    {
        Rig r;                      // a BTN1 press that ended before BTN2's began is no chord
        r.press1();
        r.run(100);
        r.release1();
        r.press2();
        r.run(100);
        r.release2();
        check(r.shorts == 1 && r.advances == 1, "separate presses each act");
    }

    // --- Pre-existing state -------------------------------------------------
    {
        Rig r;                      // a release the menu already consumed
        r.b2 = {false, 9000, true};
        r.b1 = {false, 9500, true};
        r.run(100);
        check(r.advances == 0 && r.shorts == 0, "fired presses are ignored");
    }
    {
        Rig r;                      // never pressed since boot
        r.run(100);
        check(r.advances == 0 && r.shorts == 0 && r.discards == 0, "no press, no action");
    }

    if (failures == 0) printf("  all cal softkey checks passed\n");
    return failures ? 1 : 0;
}
