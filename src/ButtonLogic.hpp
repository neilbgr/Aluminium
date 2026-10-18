#pragma once

// One Buttons button: a lit panel button and a gate/trigger input acting as a
// remote press of that same button (rising edge = press, falling edge =
// release), kept as plain C++ (no Rack engine) so it can be unit-tested on
// its own — see tests/ButtonLogicTest.cpp.
//
//   - Momentary: a press sets the state high, a release sets it low; the
//     last event wins, whichever source it came from.
//   - Latch: every press flips the state, releases are ignored (so a
//     trigger flips it too).
//   - Toggle trigger: every press flips the light only; the output is a
//     straight copy of the input (so a trigger reaches a downstream
//     trigger-driven switch untouched), plus a 1 ms trigger when the button
//     itself is clicked — the light then tracks the downstream switch's own
//     state, whichever of the two toggled it.
// In Momentary and Latch, the output is a 10V gate following the state.
//
// The input starts "uninitialized" (like Rack's own dsp::SchmittTrigger), so
// a gate already high on patch load isn't taken for a press.

enum ButtonMode {
    BUTTON_MOMENTARY,
    BUTTON_LATCH,
    BUTTON_TOGGLE_TRIGGER,
    NUM_BUTTON_MODES
};

struct ButtonLogic {
    int mode = BUTTON_MOMENTARY;
    bool state = false;

    // Schmitt state of the input: -1 uninitialized, 0 low, 1 high.
    int inputLevel = -1;
    bool buttonWasHeld = false;
    // Remaining time of the output trigger a click emits in Toggle trigger.
    float pulseRemaining = 0.f;

    static constexpr float LOW_THRESHOLD = 0.1f;
    static constexpr float HIGH_THRESHOLD = 1.f;
    static constexpr float PULSE_DURATION = 1e-3f;
    static constexpr float GATE_VOLTAGE = 10.f;

    void setMode(int newMode) {
        // Latch <-> Toggle trigger keep the light (both are a remembered
        // on/off state); anything to or from Momentary starts low.
        bool keep = mode != BUTTON_MOMENTARY && newMode != BUTTON_MOMENTARY;
        if (!keep) {
            state = false;
        }
        mode = newMode;
        pulseRemaining = 0.f;
    }

    // Flips the remembered state without any output (Toggle trigger's
    // resync, when the downstream switch was toggled from elsewhere).
    void flip() {
        state = !state;
    }

    // Patch load / reset: forget the input's level so it isn't seen as an
    // edge, and drop any pending trigger.
    void resetInput() {
        inputLevel = -1;
        pulseRemaining = 0.f;
    }

    void press(bool fromButton) {
        switch (mode) {
            case BUTTON_MOMENTARY:
                state = true;
                break;
            case BUTTON_LATCH:
                state = !state;
                break;
            default:
                state = !state;
                if (fromButton) {
                    pulseRemaining = PULSE_DURATION;
                }
                break;
        }
    }

    void release() {
        if (mode == BUTTON_MOMENTARY) {
            state = false;
        }
    }

    // `buttonHeld`: the panel button's momentary param. `inputVoltage`: 0V
    // when nothing is patched (an unpatched held gate then reads as a
    // release). Returns the output voltage; the light is `state`.
    float step(bool buttonHeld, float inputVoltage, float sampleTime) {
        if (buttonHeld && !buttonWasHeld) {
            press(true);
        }
        else if (!buttonHeld && buttonWasHeld) {
            release();
        }
        buttonWasHeld = buttonHeld;

        if (inputLevel != 1 && inputVoltage >= HIGH_THRESHOLD) {
            if (inputLevel == 0) {
                press(false);
            }
            inputLevel = 1;
        }
        else if (inputLevel != 0 && inputVoltage <= LOW_THRESHOLD) {
            if (inputLevel == 1) {
                release();
            }
            inputLevel = 0;
        }

        if (mode != BUTTON_TOGGLE_TRIGGER) {
            return state ? GATE_VOLTAGE : 0.f;
        }
        if (pulseRemaining > 0.f) {
            pulseRemaining -= sampleTime;
            return GATE_VOLTAGE;
        }
        return inputVoltage;
    }
};
