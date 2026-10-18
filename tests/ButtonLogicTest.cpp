// Headless unit test for one Buttons button (src/ButtonLogic.hpp) — no Rack
// engine involved. Build and run with `make test` (see Makefile).
#include "ButtonLogic.hpp"
#include <cstdio>

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static const float DT = 1.f / 48000.f;

// Plays the module's role: holds the button and the input, keeps the last
// output.
struct Rig {
    ButtonLogic b;
    bool held = false;
    float in = 0.f;
    float out = 0.f;

    explicit Rig(int mode) {
        b.setMode(mode);
    }

    void run(int samples = 1) {
        for (int i = 0; i < samples; i++) {
            out = b.step(held, in, DT);
        }
    }

    void click() {
        held = true;
        run();
        held = false;
        run();
    }

    void trigger() {
        in = 10.f;
        run();
        in = 0.f;
        run();
    }
};

static void momentaryFollowsBothSources() {
    Rig r(BUTTON_MOMENTARY);
    r.run();
    r.held = true;
    r.run();
    CHECK(r.b.state && r.out == 10.f);
    r.held = false;
    r.run();
    CHECK(!r.b.state && r.out == 0.f);
    r.in = 5.f;
    r.run();
    CHECK(r.b.state && r.out == 10.f);
    r.in = 0.f;
    r.run();
    CHECK(!r.b.state);
}

static void momentaryLastEventWins() {
    Rig r(BUTTON_MOMENTARY);
    r.run();
    r.held = true;
    r.run();
    r.in = 10.f;
    r.run();
    // The gate falls while the button is still held: the gate was last.
    r.in = 0.f;
    r.run();
    CHECK(!r.b.state);
    r.held = false;
    r.run();
    CHECK(!r.b.state);
}

static void latchFlipsOnPressOnly() {
    Rig r(BUTTON_LATCH);
    r.run();
    r.click();
    CHECK(r.b.state && r.out == 10.f);
    r.trigger();
    CHECK(!r.b.state && r.out == 0.f);
    r.trigger();
    CHECK(r.b.state);
    // A long gate: one flip on its rise, nothing on its fall.
    r.in = 10.f;
    r.run(100);
    CHECK(!r.b.state);
    r.in = 0.f;
    r.run();
    CHECK(!r.b.state);
}

static void toggleTriggerCopiesInput() {
    Rig r(BUTTON_TOGGLE_TRIGGER);
    r.run();
    r.in = 7.5f;
    r.run();
    CHECK(r.out == 7.5f);
    CHECK(r.b.state);
    r.in = 0.f;
    r.run();
    CHECK(r.out == 0.f);
    CHECK(r.b.state);
    r.trigger();
    CHECK(!r.b.state);
}

static void toggleTriggerClickEmitsPulse() {
    Rig r(BUTTON_TOGGLE_TRIGGER);
    r.run();
    r.held = true;
    r.run();
    CHECK(r.b.state && r.out == 10.f);
    r.held = false;
    // Still high until 1 ms has elapsed (48 samples at 48 kHz)...
    r.run(40);
    CHECK(r.out == 10.f);
    // ...then back to the (unpatched) input.
    r.run(20);
    CHECK(r.out == 0.f);
    CHECK(r.b.state);
}

static void flipWithoutTrigger() {
    Rig r(BUTTON_TOGGLE_TRIGGER);
    r.run();
    r.b.flip();
    r.run();
    CHECK(r.b.state && r.out == 0.f);
}

static void noPressOnLoadWithGateHigh() {
    Rig r(BUTTON_LATCH);
    r.in = 10.f;
    r.run();
    CHECK(!r.b.state);
    r.in = 0.f;
    r.run();
    r.in = 10.f;
    r.run();
    CHECK(r.b.state);
}

static void modeChanges() {
    Rig r(BUTTON_LATCH);
    r.run();
    r.click();
    CHECK(r.b.state);
    r.b.setMode(BUTTON_TOGGLE_TRIGGER);
    CHECK(r.b.state);
    r.b.setMode(BUTTON_MOMENTARY);
    CHECK(!r.b.state);
    r.b.setMode(BUTTON_LATCH);
    CHECK(!r.b.state);
}

int main() {
    momentaryFollowsBothSources();
    momentaryLastEventWins();
    latchFlipsOnPressOnly();
    toggleTriggerCopiesInput();
    toggleTriggerClickEmitsPulse();
    flipWithoutTrigger();
    noPressOnLoadWithGateHigh();
    modeChanges();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ButtonLogic: all tests passed\n");
    return 0;
}
