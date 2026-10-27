// Headless unit test for Fader's slider/input arbitration
// (src/FaderTakeover.hpp) — no Rack engine involved. Build and run with
// `make test` (see Makefile).
#include "FaderTakeover.hpp"
#include <cstdio>

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

// Plays the module's role: holds the slider param, writes back whatever
// step() returns.
struct Rig {
    FaderTakeover t;
    float slider = 0.f;
    float min = 0.f, max = 10.f, threshold = 0.f;

    void run(bool connected, float v, bool dragging = false) {
        slider = t.step(slider, dragging, connected, v, min, max, threshold);
    }
};

static bool approx(float a, float b) {
    return a - b < 1e-6f && b - a < 1e-6f;
}

static void connectingDoesNotMove() {
    Rig r;
    r.slider = 0.3f;
    r.run(true, 7.f);
    r.run(true, 7.f);
    CHECK(approx(r.slider, 0.3f));
    CHECK(!r.t.inputOwns);
}

static void inputChangeTakesOverAndFollows() {
    Rig r;
    r.slider = 0.3f;
    r.run(true, 7.f);
    r.run(true, 8.f);
    CHECK(r.t.inputOwns);
    CHECK(approx(r.slider, 0.8f));
    // Followed continuously once in control, even by tiny steps.
    r.threshold = 1.f;
    r.run(true, 8.01f);
    CHECK(approx(r.slider, 0.801f));
}

static void sliderMoveTakesBack() {
    Rig r;
    r.run(true, 2.f);
    r.run(true, 5.f);
    CHECK(r.t.inputOwns);
    // Someone else (mouse, MIDI-MAP, preset) moves the slider.
    r.slider = 0.9f;
    r.run(true, 5.f);
    CHECK(!r.t.inputOwns);
    CHECK(approx(r.slider, 0.9f));
    // A constant input doesn't steal it back.
    r.run(true, 5.f);
    CHECK(approx(r.slider, 0.9f));
    // Its next change does.
    r.run(true, 4.f);
    CHECK(r.t.inputOwns);
    CHECK(approx(r.slider, 0.4f));
}

static void draggingBlocksInput() {
    Rig r;
    r.run(true, 2.f);
    r.slider = 0.5f;
    r.run(true, 6.f, true);
    r.run(true, 9.f, true);
    CHECK(approx(r.slider, 0.5f));
    CHECK(!r.t.inputOwns);
    // Released: the input's value during the drag is the new reference.
    r.run(true, 9.f);
    CHECK(approx(r.slider, 0.5f));
    r.run(true, 1.f);
    CHECK(approx(r.slider, 0.1f));
}

static void thresholdFiltersNoise() {
    Rig r;
    r.threshold = 0.05f;
    r.slider = 0.2f;
    r.run(true, 5.f);
    r.run(true, 5.04f);
    r.run(true, 4.96f);
    CHECK(approx(r.slider, 0.2f));
    // Measured from the reference, not sample to sample: a slow drift adds up.
    r.run(true, 5.06f);
    CHECK(r.t.inputOwns);
    CHECK(approx(r.slider, 0.506f));
}

static void bipolarRangeAndClamp() {
    Rig r;
    r.min = -5.f;
    r.max = 5.f;
    r.slider = 0.5f;
    r.run(true, 0.f);
    r.run(true, 2.5f);
    CHECK(approx(r.slider, 0.75f));
    r.run(true, 12.f);
    CHECK(approx(r.slider, 1.f));
    r.run(true, -12.f);
    CHECK(approx(r.slider, 0.f));
}

// A 0..10 V MIDI CC driving a -10..10 V fader: the input range alone sets
// the position, the caller maps it onto the output range.
static void unipolarInputFullTravel() {
    Rig r;
    r.slider = 0.5f;
    r.run(true, 0.f);
    r.run(true, 10.f);
    CHECK(approx(r.slider, 1.f));
    r.run(true, 0.f);
    CHECK(approx(r.slider, 0.f));
    r.run(true, 5.f);
    CHECK(approx(-10.f + r.slider * 20.f, 0.f));
}

static void disconnectForgetsReference() {
    Rig r;
    r.run(true, 2.f);
    r.run(true, 5.f);
    CHECK(r.t.inputOwns);
    r.run(false, 0.f);
    CHECK(!r.t.inputOwns);
    r.slider = 0.1f;
    r.run(false, 0.f);
    // Reconnected at a different value: still not a change by itself.
    r.run(true, 8.f);
    CHECK(approx(r.slider, 0.1f));
}

int main() {
    connectingDoesNotMove();
    inputChangeTakesOverAndFollows();
    sliderMoveTakesBack();
    draggingBlocksInput();
    thresholdFiltersNoise();
    bipolarRangeAndClamp();
    unipolarInputFullTravel();
    disconnectForgetsReference();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("FaderTakeover: all tests passed\n");
    return 0;
}
