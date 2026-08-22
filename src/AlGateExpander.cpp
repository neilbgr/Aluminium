#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include "AlGateExpander.hpp"

// Expander for AlGate: mirrors AlGate's current note->channel mapping onto
// whatever polyphonic cable is patched into its single input (from Core
// MIDI-CV / AlSplitter), one output per of AlGate's 16 cells. Only produces
// real output while chained (directly, or through another AlGate expander)
// to an AlGate instance to its left. The "label" field is purely cosmetic —
// process() never depends on it, it just lets the user note what they
// patched in (Velocity, Aftertouch, Retrigger, or anything else).

struct AlGateExpander : Module {
    enum ParamIds {
        NUM_PARAMS
    };
    enum InputIds {
        LANE_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENUMS(LANE_OUTPUTS, 16),
        NUM_OUTPUTS
    };
    enum LightIds {
        CONNECTED_LIGHT,
        MISMATCH_LIGHT,
        OK_LIGHT,
        NUM_LIGHTS
    };

    std::string label;
    bool labelDirty = true;
    // True while `label` was auto-filled from the connected cable's source
    // (rather than typed by hand) — lets the label track a disconnected
    // cable back to empty without also wiping out a name the user chose.
    bool labelFromCable = false;

    AlGateExpander() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(LANE_INPUT, "Lane (poly)");
        for (int id = 0; id < 16; id++)
            configOutput(LANE_OUTPUTS + id, string::f("Lane %d", id + 1));
        // All three lights sit stacked at the same spot on the panel (only
        // one is ever lit at a time), so whichever of the three widgets
        // happens to catch the mouse hover should show the same tooltip —
        // give them identical name/description text covering all 3 colors,
        // rather than each describing only its own color.
        static const char* laneStatusName = "Lane status";
        static const char* laneStatusDescription =
            " - Red: patched, but its channel count doesn't match Al Gate's V/OCT + Gate cable — outputs beyond its channel count read 0V.\n"
            " - Yellow: chained to Al Gate, but nothing patched into this lane's input yet.\n"
            " - Green: patched and channel counts match.";
        configLight(CONNECTED_LIGHT, laneStatusName)->description = laneStatusDescription;
        configLight(MISMATCH_LIGHT, laneStatusName)->description = laneStatusDescription;
        configLight(OK_LIGHT, laneStatusName)->description = laneStatusDescription;
        updatePortNames();

        leftExpander.producerMessage = new AlGateExpanderMessage;
        leftExpander.consumerMessage = new AlGateExpanderMessage;
    }

    // Ports aren't re-configured per instance (configInput/configOutput only
    // run once, in the constructor, before `label` has its final value), so
    // their live tooltip text is kept in sync by hand instead, any time
    // `label` changes (construction, patch load, or the on-panel field).
    void updatePortNames() {
        std::string base = label.empty() ? "Lane" : label;
        if (PortInfo* info = getInputInfo(LANE_INPUT))
            info->name = base + " (poly)";
        for (int id = 0; id < 16; id++)
            if (PortInfo* info = getOutputInfo(LANE_OUTPUTS + id))
                info->name = string::f("%s %d", base.c_str(), id + 1);
    }

    void setLabel(const std::string& newLabel) {
        label = newLabel;
        updatePortNames();
    }

    ~AlGateExpander() {
        delete (AlGateExpanderMessage*) leftExpander.producerMessage;
        delete (AlGateExpanderMessage*) leftExpander.consumerMessage;
    }

    static bool motherAt(Module* neighbor) {
        return neighbor && (neighbor->model == modelAlGate || alGateIsExpanderModel(neighbor->model));
    }

    void process(const ProcessArgs&) override {
        AlGateExpanderMessage activeChannel;
        bool present = motherAt(leftExpander.module);

        if (present)
            activeChannel = *(AlGateExpanderMessage*) leftExpander.consumerMessage;
        else
            for (int id = 0; id < 16; id++)
                activeChannel.activeChannel[id] = -1;

        // Three mutually exclusive states, checked every block (not just in
        // onExpanderChange, since both lane-connection and channel count
        // can change independently of chain topology): chained but nothing
        // patched into the lane yet (yellow), chained and patched but a
        // channel-count mismatch (red), or fully working (green).
        bool laneConnected = inputs[LANE_INPUT].isConnected();
        bool mismatch = present && laneConnected && inputs[LANE_INPUT].getChannels() != activeChannel.channels;
        bool ok = present && laneConnected && !mismatch;
        bool waiting = present && !laneConnected;
        lights[CONNECTED_LIGHT].setBrightness(waiting ? 1.f : 0.f);
        lights[MISMATCH_LIGHT].setBrightness(mismatch ? 1.f : 0.f);
        lights[OK_LIGHT].setBrightness(ok ? 1.f : 0.f);

        // `c` indexes AlGate's own V/OCT+GATE cable, which may have a
        // different channel count than our own LANE_INPUT cable — don't
        // rely on the far end having zeroed its unused channels, check
        // ourselves so a channel-count mismatch reads as silence (0V)
        // instead of whatever was last left in that channel's slot.
        for (int id = 0; id < 16; id++) {
            int c = activeChannel.activeChannel[id];
            bool inRange = c >= 0 && c < inputs[LANE_INPUT].getChannels();
            outputs[LANE_OUTPUTS + id].setChannels(1);
            outputs[LANE_OUTPUTS + id].setVoltage(inRange ? inputs[LANE_INPUT].getVoltage(c) : 0.f);
        }

        if (rightExpander.module && alGateIsExpanderModel(rightExpander.module->model))
            alGateForwardMessage(this, activeChannel);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "label", json_string(label.c_str()));
        json_object_set_new(rootJ, "labelFromCable", json_boolean(labelFromCable));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "label")) {
            setLabel(json_string_value(j));
            labelDirty = true;
        }
        if (json_t* j = json_object_get(rootJ, "labelFromCable"))
            labelFromCable = json_boolean_value(j);
    }
};

#ifndef HEADLESS
struct AlGateExpanderLabelField : LedDisplayTextField {
    AlGateExpander* module = nullptr;
    // Suppresses onChange's "user edited it" handling while step() is the
    // one calling setText() to mirror an external change (auto-fill from a
    // cable, or patch load) — only a real keystroke/paste/etc. should ever
    // lower labelFromCable.
    bool syncingFromModule = false;

    AlGateExpanderLabelField() {
        multiline = false;
        placeholder = "Label";
        // LedDisplayTextField's own default (5, 5) is tuned for a much
        // bigger display than this narrow panel can spare — tighten it so
        // less of the already-scarce width/height goes to padding instead
        // of the text itself.
        textOffset = Vec(0.f, -1.f);
    }

    void step() override {
        LedDisplayTextField::step();
        if (module && module->labelDirty) {
            syncingFromModule = true;
            setText(module->label);
            syncingFromModule = false;
            module->labelDirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        LedDisplayTextField::onChange(e);
        if (module) {
            module->setLabel(getText());
            if (!syncingFromModule)
                module->labelFromCable = false;
        }
    }
};

struct AlGateExpanderWidget : ModuleWidget {
    PortWidget* laneInputWidget = nullptr;
    bool laneInputWasConnected = false;

    AlGateExpanderWidget(AlGateExpander* module) {
        setModule(module);

        const float panelWidth = 25.4f;

        setPanel(new AlPanel(mm2px(Vec(panelWidth, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/AlGateExpander_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/AlGateExpander_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(0, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Height picked to comfortably fit LedDisplayTextField's fixed
        // 12px font + 5px top/bottom padding (its own defaults, in raw
        // pixels regardless of panel scale) — too short a box clips the
        // text against its own clip rect instead of just looking cramped.
        const float labelCenterY = 23.8f, labelWidth = 19.4f, labelHeight = 6.f;
        LedDisplay* labelDisplay = createWidget<LedDisplay>(mm2px(Vec(panelWidth / 2.f - labelWidth / 2.f, labelCenterY - labelHeight / 2.f)));
        labelDisplay->box.size = mm2px(Vec(labelWidth, labelHeight));
        AlGateExpanderLabelField* labelField = createWidget<AlGateExpanderLabelField>(Vec(0, 0));
        labelField->box.size = labelDisplay->box.size;
        labelField->module = module;
        labelDisplay->addChild(labelField);
        addChild(labelDisplay);

        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(1.8f, 11.f)), module, AlGateExpander::CONNECTED_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(1.8f, 11.f)), module, AlGateExpander::MISMATCH_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(1.8f, 11.f)), module, AlGateExpander::OK_LIGHT));

        const float colLeft = 8.f, colRight = 17.4f;
        const float displayCenterX = panelWidth / 2.f;
        const float inputsZoneOffsetY = 16.f;

        laneInputWidget = createInputCentered<AlPortComponentIn>(mm2px(Vec(displayCenterX, inputsZoneOffsetY)), module, AlGateExpander::LANE_INPUT);
        addInput(laneInputWidget);

        const float outputsZoneOffsetY = 28.f;
        const float outputsZoneHeight = 88.f;

        for (int row = 0; row < 8; row++) {
            float rowY = outputsZoneOffsetY + outputsZoneHeight * (row + 0.5f) / 8.f;
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colLeft, rowY)), module, AlGateExpander::LANE_OUTPUTS + row));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colRight, rowY)), module, AlGateExpander::LANE_OUTPUTS + 8 + row));
        }
    }

    // Cable topology (which output feeds our input) only exists at the UI
    // layer (RackWidget's CableWidgets), not on the engine-side Module —
    // Module::onPortChange fires while Engine::addCable still holds its
    // mutex, so querying cables from there would deadlock. Polling here in
    // step() (UI thread, no engine lock involved) is the standard way
    // plugins solve this (e.g. MindMeldModular's Meld.cpp/Unmeld.cpp).
    void step() override {
        ModuleWidget::step();

        AlGateExpander* mod = dynamic_cast<AlGateExpander*>(module);
        if (!mod || !laneInputWidget)
            return;

        std::vector<CableWidget*> cables = APP->scene->rack->getCompleteCablesOnPort(laneInputWidget);
        bool connected = !cables.empty();

        if (connected && !laneInputWasConnected && mod->label.empty()) {
            engine::Cable* cable = cables[0]->cable;
            if (cable && cable->outputModule) {
                engine::PortInfo* info = cable->outputModule->getOutputInfo(cable->outputId);
                if (info) {
                    mod->setLabel(info->getName().substr(0, 8));
                    mod->labelFromCable = true;
                    // Unlike the label field's own onChange (which already
                    // reflects what the user just typed), this change comes
                    // from outside the field — flag it so step() re-syncs
                    // the displayed text next frame.
                    mod->labelDirty = true;
                }
            }
        }
        else if (!connected && laneInputWasConnected && mod->labelFromCable) {
            // The cable that provided this label is gone — clear it back to
            // empty rather than leave a stale name, but only because we set
            // it ourselves; a name the user typed by hand is left alone.
            mod->setLabel("");
            mod->labelFromCable = false;
            mod->labelDirty = true;
        }
        laneInputWasConnected = connected;
    }

    void appendContextMenu(Menu* menu) override {
        appendAluminiumThemeMenu(menu);
    }
};
#else
struct AlGateExpanderWidget : ModuleWidget {
    AlGateExpanderWidget(AlGateExpander* module) {
        setModule(module);
        addInput(createInput<PJ301MPort>({}, module, AlGateExpander::LANE_INPUT));
        for (int id = 0; id < 16; id++)
            addOutput(createOutput<PJ301MPort>({}, module, AlGateExpander::LANE_OUTPUTS + id));
    }
};
#endif

Model* modelAlGateExpander = createModel<AlGateExpander, AlGateExpanderWidget>("AlGateExpander");
