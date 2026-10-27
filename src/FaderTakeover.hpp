#pragma once
#include <cmath>

// Fader's "last mover wins" arbitration between the on-panel slider and its
// CV input, kept as plain C++ (no Rack engine) so it can be unit-tested on
// its own — see tests/FaderTakeoverTest.cpp.
//
// Two owners, and whichever one moved last keeps the fader:
//   - the input takes over only once its voltage has moved away, by more
//     than `threshold` (in input volts), from the value it had when it last lost
//     the fader (or was first seen). A cable that's merely connected, or a
//     constant voltage, never moves the fader on its own;
//   - once in control, the input is followed continuously (so a slow ramp,
//     or a smoothed MIDI CC, drags the fader smoothly, not in threshold-sized
//     steps), until the slider is touched again: a drag, a double-click
//     reset, a MIDI-MAP, a preset, a randomize — any change to the slider
//     that this class didn't write itself.
// Not persisted: after a patch load the slider keeps its saved position until
// the input actually changes (a MIDI-CV CC output reads 0V until its first
// message arrives — restoring "input owns" would slam the fader to 0 instead).
struct FaderTakeover {
    bool inputOwns = false;
    // Input voltage when the input last lost (or was first seen without) the
    // fader — what it has to move away from to take over again.
    bool referenceValid = false;
    float reference = 0.f;
    // Slider value as of the end of the previous step(), to tell our own
    // writes apart from anyone else's.
    float lastSlider = NAN;

    void reset() {
        inputOwns = false;
        referenceValid = false;
        lastSlider = NAN;
    }

    // `slider`: current normalized slider position [0, 1]. `dragging`: the
    // slider is held by the mouse right now (the input never wins while it's
    // held). `inputConnected`/`inputVoltage`: the CV input. `rangeMin`/
    // `rangeMax`: the voltage range the input is read in (the output's own
    // range is applied by the caller, from the returned position).
    // Returns the slider position to apply (unchanged unless the input owns
    // the fader).
    float step(float slider, bool dragging, bool inputConnected, float inputVoltage,
               float rangeMin, float rangeMax, float threshold) {
        if (!inputConnected) {
            inputOwns = false;
            referenceValid = false;
            lastSlider = slider;
            return slider;
        }

        bool sliderMoved = !std::isnan(lastSlider) && slider != lastSlider;
        if (dragging || sliderMoved || !referenceValid) {
            inputOwns = false;
            referenceValid = true;
            reference = inputVoltage;
        }
        else if (!inputOwns && std::fabs(inputVoltage - reference) > threshold) {
            inputOwns = true;
        }

        if (inputOwns) {
            float n = (inputVoltage - rangeMin) / (rangeMax - rangeMin);
            slider = n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
        }
        lastSlider = slider;
        return slider;
    }
};
