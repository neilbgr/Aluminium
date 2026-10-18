#pragma once
#include "plugin.hpp"
#include <functional>

using namespace rack;

// PadX-style per-control label, shared by Fader/Faders2/Faders4 and Buttons:
// a single-line LED text field, purely cosmetic, saved with the patch, and
// auto-filled from the source of the cable patched into the control's input
// while it's empty (cleared again when that cable goes, unless the user typed
// over it). PadX keeps its own copy of the same logic (src/PadX.cpp).

// Max characters an auto-filled label keeps — what a 3HP-wide field shows.
static const size_t AL_LABEL_AUTO_LENGTH = 6;

struct AlLabel {
    std::string text;
    // Set whenever `text` changes from outside the field (patch load,
    // auto-fill), so the field re-syncs its displayed text next frame.
    bool dirty = true;
    // True while `text` was auto-filled from the input cable's source, so
    // unpatching it can clear a name we chose ourselves without wiping out
    // one the user typed.
    bool fromCable = false;

    void toJson(json_t* rootJ) const {
        json_object_set_new(rootJ, "label", json_string(text.c_str()));
        json_object_set_new(rootJ, "labelFromCable", json_boolean(fromCable));
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
        if (json_t* j = json_object_get(rootJ, "labelFromCable")) {
            fromCable = json_boolean_value(j);
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
    // See PadXLabelField: only a real edit (not step() mirroring an external
    // change) lowers fromCable.
    bool syncingFromModule = false;

    AlLabelField() {
        multiline = false;
        placeholder = "Label";
        textOffset = Vec(0.f, -1.f);
    }

    void step() override {
        LedDisplayTextField::step();
        if (label && label->dirty) {
            syncingFromModule = true;
            setText(label->text);
            syncingFromModule = false;
            label->dirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        LedDisplayTextField::onChange(e);
        if (label) {
            label->text = getText();
            if (!syncingFromModule) {
                label->fromCable = false;
            }
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

// Label auto-fill from the cable patched into `input`. Call from the
// ModuleWidget's step(): cable topology only exists at the UI layer, and
// polling it there (UI thread, no engine lock) is the standard way plugins do
// it — see PadXWidget::step() for the full reasoning.
struct AlCableLabelWatcher {
    bool wasConnected = false;

    void step(PortWidget* input, AlLabel& label, const std::function<void()>& onEdited) {
        if (!input) {
            return;
        }
        std::vector<CableWidget*> cables = APP->scene->rack->getCompleteCablesOnPort(input);
        bool connected = !cables.empty();

        if (connected && !wasConnected && label.text.empty()) {
            engine::Cable* cable = cables[0]->cable;
            if (cable && cable->outputModule) {
                engine::PortInfo* info = cable->outputModule->getOutputInfo(cable->outputId);
                if (info) {
                    label.text = info->getName().substr(0, AL_LABEL_AUTO_LENGTH);
                    label.fromCable = true;
                    label.dirty = true;
                    onEdited();
                }
            }
        }
        else if (!connected && wasConnected && label.fromCable) {
            label.text.clear();
            label.fromCable = false;
            label.dirty = true;
            onEdited();
        }
        wasConnected = connected;
    }
};
#endif
