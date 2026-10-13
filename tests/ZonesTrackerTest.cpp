// Headless unit test for Zones' note tracking (src/ZonesTracker.hpp) — no
// Rack engine involved. Build and run with `make test` (see Makefile).
#include "ZonesTracker.hpp"
#include <cstdio>

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

// One input sample: per-channel pitch/gate/rtrg, plus the split state the
// module would otherwise read from its params.
struct Frame {
    float pitch[16] = {};
    bool gate[16] = {};
    bool rtrg[16] = {};
    float threshold = 0.f;
    bool learnArmed = false;
    bool retrig[2] = {};

    bool run(ZonesTracker& t, int channels, float splitMin = -4.f, float splitMax = 4.f) {
        bool learned = t.processChannels(channels, pitch, gate, rtrg, threshold, learnArmed, splitMin, splitMax, retrig);
        // RTRG is a 1-sample edge here; gate stays as set.
        for (int c = 0; c < 16; c++) {
            rtrg[c] = false;
        }
        return learned;
    }

    void press(int c, float p) {
        pitch[c] = p;
        gate[c] = true;
        rtrg[c] = true;
    }
};

static void testPriorities() {
    ZonesTracker t;
    Frame f;
    f.press(0, -1.f);
    f.press(1, -0.5f);
    f.run(t, 3);
    f.press(2, -2.f);
    f.run(t, 3);
    CHECK(t.zones[0].held.count == 3);
    CHECK(t.zones[1].held.empty());

    t.zones[0].priority = ZonesTracker::PRIORITY_LAST;
    CHECK(t.topOf(0, f.pitch) == 2);
    t.zones[0].priority = ZonesTracker::PRIORITY_HIGH;
    CHECK(t.topOf(0, f.pitch) == 1);
    t.zones[0].priority = ZonesTracker::PRIORITY_LOW;
    CHECK(t.topOf(0, f.pitch) == 2);

    // Release the last note: LAST falls back to the previous one.
    t.zones[0].priority = ZonesTracker::PRIORITY_LAST;
    f.gate[2] = false;
    f.run(t, 3);
    CHECK(t.topOf(0, f.pitch) == 1);
    CHECK(!f.retrig[0]);
}

static void testSplitAndRetrig() {
    ZonesTracker t;
    Frame f;
    f.threshold = 0.5f;
    f.press(0, 0.f);
    f.press(1, 1.f);
    f.run(t, 2);
    CHECK(t.channelZone[0] == 0);
    CHECK(t.channelZone[1] == 1);
    CHECK(f.retrig[0] && f.retrig[1]);

    // Mono CV source: gated channel drifts across the split, no RTRG.
    f.pitch[0] = 2.f;
    f.run(t, 2);
    CHECK(t.channelZone[0] == 1);
    CHECK(t.zones[0].held.empty());
    CHECK(t.zones[1].held.count == 2);
}

// Finding 1: a held note on a channel beyond a shrunken channel count must
// be released, not stuck in its zone forever.
static void testChannelCountShrink() {
    ZonesTracker t;
    Frame f;
    f.press(0, -1.f);
    f.press(3, 1.f);
    f.run(t, 4);
    CHECK(t.channelZone[3] == 1);
    CHECK(t.topOf(1, f.pitch) == 3);

    f.run(t, 2);
    CHECK(t.channelZone[3] == -1);
    CHECK(t.zones[1].held.empty());
    CHECK(t.topOf(1, f.pitch) == -1);
    CHECK(!t.prevGateHigh[3]);
    // Channel 0 (still in range) is untouched.
    CHECK(t.channelZone[0] == 0);

    // Channel count grows back with channel 3's gate low: nothing happens.
    f.gate[3] = false;
    f.run(t, 4);
    CHECK(t.channelZone[3] == -1);

    // Count to 0 releases everything too.
    f.run(t, 0);
    CHECK(!t.anyHeld());
}

// A LEARN-suppressed channel that disappears must not stay suppressed for
// the next note to land on that channel index.
static void testShrinkClearsLearnSuppressed() {
    ZonesTracker t;
    Frame f;
    f.learnArmed = true;
    f.press(2, 0.25f);
    CHECK(f.run(t, 3));
    CHECK(t.learnSuppressed[2]);

    f.run(t, 1);
    CHECK(!t.learnSuppressed[2]);

    f.press(2, 1.f);
    f.run(t, 3);
    CHECK(t.topOf(1, f.pitch) == 2);
}

// Finding 2: LEARN clamps the split point to the knob's own range.
static void testLearnClamp() {
    {
        ZonesTracker t;
        Frame f;
        f.learnArmed = true;
        f.press(0, 1.25f);
        CHECK(f.run(t, 1));
        CHECK(f.threshold == 1.25f);
        CHECK(!f.learnArmed);
        CHECK(t.learnSuppressed[0]);
        CHECK(t.topOf(1, f.pitch) == -1);
    }
    {
        ZonesTracker t;
        Frame f;
        f.learnArmed = true;
        f.press(0, 5.5f);
        CHECK(f.run(t, 1));
        CHECK(f.threshold == 4.f);
        CHECK(t.channelZone[0] == 1);
    }
    {
        ZonesTracker t;
        Frame f;
        f.learnArmed = true;
        f.press(0, -6.f);
        CHECK(f.run(t, 1));
        CHECK(f.threshold == -4.f);
        CHECK(t.channelZone[0] == 0);
        // Not armed any more: the next note doesn't move the split.
        f.press(1, 3.f);
        CHECK(!f.run(t, 2));
        CHECK(f.threshold == -4.f);
    }
}

static void testDuo() {
    ZonesTracker t;
    Frame f;
    f.press(0, 0.f);      // C
    f.run(t, 3);
    f.press(1, 7 / 12.f); // G
    f.run(t, 3);
    int count;
    int highest = t.duoHighest(f.pitch, &count);
    CHECK(count == 2);
    CHECK(highest == 1);
    CHECK(t.duoPickA(highest, f.pitch) == 0);

    // Third key (E) takes voice A under Last-note priority.
    f.press(2, 4 / 12.f);
    f.run(t, 3);
    highest = t.duoHighest(f.pitch, &count);
    CHECK(count == 3);
    CHECK(highest == 1);
    CHECK(t.duoPickA(highest, f.pitch) == 2);

    t.zones[0].priority = ZonesTracker::PRIORITY_LOW;
    CHECK(t.duoPickA(highest, f.pitch) == 0);
}

static void testHeldListNoDuplicates() {
    ZonesTracker t;
    Frame f;
    // Same-channel "Reuse" retrigger without a note-off in between.
    f.press(0, -1.f);
    f.run(t, 1);
    f.press(0, -1.f);
    f.run(t, 1);
    CHECK(t.zones[0].held.count == 1);
}

int main() {
    testPriorities();
    testSplitAndRetrig();
    testChannelCountShrink();
    testShrinkClearsLearnSuppressed();
    testLearnClamp();
    testDuo();
    testHeldListNoDuplicates();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ZonesTracker: all checks passed\n");
    return 0;
}
