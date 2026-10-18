#pragma once
#include "plugin.hpp"
#include <functional>

using namespace rack;

// PadX-style per-control label, shared by Fader/Faders2/Faders4 and Buttons:
// a single-line LED text field, purely cosmetic, saved with the patch, typed
// by hand only (unlike PadX's, which also auto-fills from its input cable —
// a fader's or button's input is usually a MIDI CC/gate, whose name says
// nothing useful).

struct AlLabel {
    std::string text;
    // Set whenever `text` changes from outside the field (patch load), so
    // the field re-syncs its displayed text next frame.
    bool dirty = true;

    void toJson(json_t* rootJ) const {
        json_object_set_new(rootJ, "label", json_string(text.c_str()));
    }

    // Returns true if a label was read (caller then refreshes port names).
    bool fromJson(json_t* rootJ) {
        bool changed = false;
        json_t* labelJ = json_object_get(rootJ, "label");
        if (labelJ && json_is_string(labelJ)) {
            text = json_string_value(labelJ);
            dirty = true;
            changed = true;
        }
        return changed;
    }
};

#ifndef HEADLESS
// `label` is null in the module browser's preview. `onEdited` runs after
// every change of the text (so the module can rename its ports).
struct AlLabelField : LedDisplayTextField {
    AlLabel* label = nullptr;
    std::function<void()> onEdited;

    AlLabelField() {
        multiline = false;
        placeholder = "Label";
        textOffset = Vec(0.f, -1.f);
    }

    void step() override {
        LedDisplayTextField::step();
        if (label && label->dirty) {
            setText(label->text);
            label->dirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        LedDisplayTextField::onChange(e);
        if (label) {
            label->text = getText();
            if (onEdited) {
                onEdited();
            }
        }
    }
};

// Adds a label field centered at `centerMm` (same 6 mm height as PadX's,
// which fits LedDisplayTextField's fixed 12px font).
inline AlLabelField* alAddLabelField(ModuleWidget* mw, Vec centerMm, float widthMm, AlLabel* label, std::function<void()> onEdited) {
    const float heightMm = 6.f;
    LedDisplay* display = createWidget<LedDisplay>(mm2px(Vec(centerMm.x - widthMm / 2.f, centerMm.y - heightMm / 2.f)));
    display->box.size = mm2px(Vec(widthMm, heightMm));
    AlLabelField* field = createWidget<AlLabelField>(Vec(0, 0));
    field->box.size = display->box.size;
    field->label = label;
    field->onEdited = onEdited;
    display->addChild(field);
    mw->addChild(display);
    return field;
}

#endif
