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
        NUM_LIGHTS
    };

    std::string label = "Velocity";
    bool labelDirty = true;

    AlGateExpander() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(LANE_INPUT, "Lane (poly, from MIDI-CV)");
        for (int id = 0; id < 16; id++)
            configOutput(LANE_OUTPUTS + id, string::f("Lane %d", id + 1));
        configLight(CONNECTED_LIGHT, "Left connection indicator");
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
            info->name = base + " (poly, from MIDI-CV)";
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

    void onExpanderChange(const ExpanderChangeEvent& e) override {
        if (e.side == 0)
            lights[CONNECTED_LIGHT].setBrightness(motherAt(leftExpander.module) ? 1.f : 0.f);
    }

    void process(const ProcessArgs&) override {
        AlGateExpanderMessage activeChannel;
        bool present = motherAt(leftExpander.module);

        if (present)
            activeChannel = *(AlGateExpanderMessage*) leftExpander.consumerMessage;
        else
            for (int id = 0; id < 16; id++)
                activeChannel.activeChannel[id] = -1;

        for (int id = 0; id < 16; id++) {
            int c = activeChannel.activeChannel[id];
            outputs[LANE_OUTPUTS + id].setChannels(1);
            outputs[LANE_OUTPUTS + id].setVoltage(c >= 0 ? inputs[LANE_INPUT].getVoltage(c) : 0.f);
        }

        if (rightExpander.module && alGateIsExpanderModel(rightExpander.module->model))
            alGateForwardMessage(this, activeChannel);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "label", json_string(label.c_str()));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "label")) {
            setLabel(json_string_value(j));
            labelDirty = true;
        }
    }
};

#ifndef HEADLESS
struct AlGateExpanderLabelField : LedDisplayTextField {
    AlGateExpander* module = nullptr;

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
            setText(module->label);
            module->labelDirty = false;
        }
    }

    void onChange(const ChangeEvent& e) override {
        LedDisplayTextField::onChange(e);
        if (module)
            module->setLabel(getText());
    }
};

struct AlGateExpanderWidget : ModuleWidget {
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

        const float colLeft = 8.f, colRight = 17.4f;
        const float displayCenterX = panelWidth / 2.f;
        const float inputsZoneOffsetY = 16.f;

        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(displayCenterX, inputsZoneOffsetY)), module, AlGateExpander::LANE_INPUT));

        const float outputsZoneOffsetY = 28.f;
        const float outputsZoneHeight = 88.f;

        for (int row = 0; row < 8; row++) {
            float rowY = outputsZoneOffsetY + outputsZoneHeight * (row + 0.5f) / 8.f;
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colLeft, rowY)), module, AlGateExpander::LANE_OUTPUTS + row));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colRight, rowY)), module, AlGateExpander::LANE_OUTPUTS + 8 + row));
        }
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
