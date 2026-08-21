#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include "AlGateExpander.hpp"

// Expander for AlGate: mirrors AlGate's current note->channel mapping onto
// the poly RETRIGGER lane (from Core MIDI-CV / AlSplitter), one output per
// of AlGate's 16 cells. Only produces real output while chained (directly,
// or through another AlGate expander) to an AlGate instance to its left.

struct AlRetrigger : Module {
    enum ParamIds {
        NUM_PARAMS
    };
    enum InputIds {
        RTRG_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENUMS(RTRG_OUTPUTS, 16),
        NUM_OUTPUTS
    };
    enum LightIds {
        CONNECTED_LIGHT,
        NUM_LIGHTS
    };

    AlRetrigger() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(RTRG_INPUT, "Retrigger (poly, from MIDI-CV)");
        for (int id = 0; id < 16; id++)
            configOutput(RTRG_OUTPUTS + id, string::f("Retrigger %d", id + 1));

        leftExpander.producerMessage = new AlGateExpanderMessage;
        leftExpander.consumerMessage = new AlGateExpanderMessage;
    }

    ~AlRetrigger() {
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
            outputs[RTRG_OUTPUTS + id].setChannels(1);
            outputs[RTRG_OUTPUTS + id].setVoltage(c >= 0 ? inputs[RTRG_INPUT].getVoltage(c) : 0.f);
        }

        if (rightExpander.module && alGateIsExpanderModel(rightExpander.module->model))
            alGateForwardMessage(this, activeChannel);
    }
};

#ifndef HEADLESS
struct AlRetriggerWidget : ModuleWidget {
    AlRetriggerWidget(AlRetrigger* module) {
        setModule(module);

        const float panelWidth = 25.4f;

        setPanel(new AlPanel(mm2px(Vec(panelWidth, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/AlRetrigger_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/AlRetrigger_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(0, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(1.8f, 11.f)), module, AlRetrigger::CONNECTED_LIGHT));

        const float colLeft = 8.f, colRight = 17.4f;
        const float displayCenterX = panelWidth / 2.f;
        const float inputsZoneOffsetY = 16.f;

        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(displayCenterX, inputsZoneOffsetY)), module, AlRetrigger::RTRG_INPUT));

        const float outputsZoneOffsetY = 28.f;
        const float outputsZoneHeight = 88.f;

        for (int row = 0; row < 8; row++) {
            float rowY = outputsZoneOffsetY + outputsZoneHeight * (row + 0.5f) / 8.f;
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colLeft, rowY)), module, AlRetrigger::RTRG_OUTPUTS + row));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colRight, rowY)), module, AlRetrigger::RTRG_OUTPUTS + 8 + row));
        }
    }

    void appendContextMenu(Menu* menu) override {
        appendAluminiumThemeMenu(menu);
    }
};
#else
struct AlRetriggerWidget : ModuleWidget {
    AlRetriggerWidget(AlRetrigger* module) {
        setModule(module);
        addInput(createInput<PJ301MPort>({}, module, AlRetrigger::RTRG_INPUT));
        for (int id = 0; id < 16; id++)
            addOutput(createOutput<PJ301MPort>({}, module, AlRetrigger::RTRG_OUTPUTS + id));
    }
};
#endif

Model* modelAlRetrigger = createModel<AlRetrigger, AlRetriggerWidget>("AlRetrigger");
