#pragma once
#include "plugin.hpp"
#include <cmath>
#include <string>

using namespace rack;

// Shared bits of Aluminium's LED-style readouts (Zones' split display, Pads'
// note grid): note-name formatting, the dark rounded background, and Rack's
// own bundled ShareTechMono font (asset::system, rather than shipping our
// own copy).

// MIDI note number (60 = C4) -> "C4", or "--" for a negative (unassigned)
// note.
inline std::string alNoteName(int note) {
    static const char* NOTE_NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (note < 0) {
        return "--";
    }
    return std::string(NOTE_NAMES[note % 12]) + std::to_string(note / 12 - 1);
}

// V/OCT voltage (0V = C4) -> nearest note name.
inline std::string alVoltageToNoteName(float voltage) {
    return alNoteName(60 + (int) std::round(voltage * 12.f));
}

inline void alDrawLedBackground(NVGcontext* vg, Vec size) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, size.x, size.y, 2.f);
    nvgFillColor(vg, nvgRGBA(0x10, 0x10, 0x10, 0xff));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(0x50, 0x50, 0x50, 0xff));
    nvgStroke(vg);
}

inline std::shared_ptr<window::Font> alLoadLedFont() {
    return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}
