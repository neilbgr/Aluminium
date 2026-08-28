#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include "PadX.hpp"
#include <algorithm>
#include <cmath>

// Like Cardinal/Rack core's "MIDI-Gate" (18 gate outputs, one per learned
// note, driven by MIDI note-on/off) but driven instead by a polyphonic
// V/OCT + GATE cable (typically from Core MIDI-CV or Zones) — 16 gate
// outputs, one per learned note. Each output is high whenever any incoming
// poly channel currently carries that exact pitch with its gate high.
//
// A note is (re)learned either by clicking its display cell and then
// playing the note on the incoming poly cable, or by clicking the cell and
// typing a note name directly (letter A-G, optional #, octave digit, Enter
// to commit) — mirrors HostMIDI-Gate.cpp's CardinalNoteChoice exactly, just
// adapted to one shared 2x8 grid widget with manual mouse hit-testing
// instead of 16 separate LedDisplayChoice widgets. Backspace/Delete clears
// a cell (shows "--", that output stays low).
//
// PadX (a separate expander module, one instance per poly lane you need)
// reads which poly channel is currently satisfying each of these 16 cells
// via PadXMessage, so it can mirror the same note->channel mapping onto
// whatever lane is patched into it without duplicating the note-learning UI.

static const char* AL_GATE_NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static std::string padNoteName(int8_t note) {
    if (note < 0) {
        return "--";
    }
    int oct = note / 12 - 1;
    int semi = note % 12;
    return std::string(AL_GATE_NOTE_NAMES[semi]) + std::to_string(oct);
}

struct Pads : Module {
    enum ParamIds {
        ENUMS(LATCH_PARAMS, 16),
        NUM_PARAMS
    };
    enum InputIds {
        PITCH_INPUT,
        GATE_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENUMS(GATE_OUTPUTS, 16),
        NUM_OUTPUTS
    };
    enum LightIds {
        ENUMS(LATCH_LIGHTS, 16),
        NUM_LIGHTS
    };

    // -1 = unassigned ("--", cell always outputs 0V).
    int8_t learnedNotes[16];
    // -1 = not learning; id of the cell awaiting its next played/typed note otherwise.
    int learningId = -1;
    // Ctrl+click-armed learn sessions advance learningId to the next cell
    // after each learned note instead of leaving learn mode — set per-arm in
    // PadsNoteGridDisplay::onButton(), consumed in process()'s Learn block.
    bool learnSequential = false;
    bool prevGateHigh[16] = {};
    // Per-cell toggled gate value while that cell is in Latch mode (see
    // LATCH_PARAMS) — flips on each rising edge of the cell's own `matched`
    // state, independently of prevGateHigh above (which tracks per-channel
    // input gate state for the Learn feature, a different concept).
    bool latchedState[16] = {};
    bool prevMatched[16] = {};
    // Transient display state, not serialized — mirrored every tick from
    // process()'s own `matched`/`gateHigh` locals so PadsNoteGridDisplay can
    // show source-vs-output activity even when nothing is patched into the
    // corresponding GATE_OUTPUTS jack.
    bool sourceGateActive[16] = {};
    bool outputGateActive[16] = {};

    PadXMessage expMsg = {};

    Pads() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(PITCH_INPUT, "Pitch (poly)");
        configInput(GATE_INPUT, "Gate (poly)");
        for (int id = 0; id < 16; id++) {
            configOutput(GATE_OUTPUTS + id, string::f("Gate %d", id + 1));
            configSwitch(LATCH_PARAMS + id, 0.f, 1.f, 0.f, string::f("Latch cell %d", id + 1), {"Normal", "Latch"});
        }

        onReset();
    }

    void onReset() override {
        for (int id = 0; id < 16; id++) {
            learnedNotes[id] = 36 + id;
            latchedState[id] = false;
            prevMatched[id] = false;
            sourceGateActive[id] = false;
            outputGateActive[id] = false;
        }
        learningId = -1;
        learnSequential = false;
        for (int c = 0; c < 16; c++) {
            prevGateHigh[c] = false;
        }
    }

    static int noteOf(float voltage) {
        return 60 + (int)std::round(voltage * 12.f);
    }

    // Mirrors HostMIDI-Gate.cpp's own setLearnedNote(): unset any other cell
    // currently holding `note` before assigning it to `id`, so a note is
    // never mapped to two cells at once.
    void setLearnedNote(int id, int8_t note) {
        if (note >= 0) {
            for (int i = 0; i < 16; i++) {
                if (learnedNotes[i] == note) {
                    learnedNotes[i] = -1;
                }
            }
        }
        learnedNotes[id] = note;
        // A cell's note identity just changed — don't let a stale toggled
        // state from whatever was learned before bleed into the new note.
        latchedState[id] = false;
        prevMatched[id] = false;
        sourceGateActive[id] = false;
        outputGateActive[id] = false;
    }

    void process(const ProcessArgs&) override {
        int channels = std::min(inputs[PITCH_INPUT].getChannels(), 16);
        expMsg.channels = (int8_t) channels;

        for (int id = 0; id < 16; id++) {
            bool matched = false;
            int matchedChannel = -1;

            if (learnedNotes[id] >= 0) {
                for (int c = 0; c < channels; c++) {
                    bool gateHigh = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
                    if (gateHigh && noteOf(inputs[PITCH_INPUT].getVoltage(c)) == learnedNotes[id]) {
                        matched = true;
                        matchedChannel = c;   // last matching channel wins if 2+ channels share a pitch
                    }
                }
            }

            // Latch: on each rising edge of `matched`, toggle latchedState
            // instead of following it directly. expMsg.activeChannel still
            // reflects the raw physical state below, unaffected by latch —
            // an expander chained on Velocity/Aftertouch/etc. still tracks
            // the real note, not the latched gate.
            bool latchOn = params[LATCH_PARAMS + id].getValue() > 0.5f;
            bool gateHigh;
            if (latchOn) {
                if (matched && !prevMatched[id]) {
                    latchedState[id] = !latchedState[id];
                }
                gateHigh = latchedState[id];
            }
            else {
                gateHigh = matched;
            }
            prevMatched[id] = matched;

            outputs[GATE_OUTPUTS + id].setChannels(1);
            outputs[GATE_OUTPUTS + id].setVoltage(gateHigh ? 10.f : 0.f);
            lights[LATCH_LIGHTS + id].setBrightness(latchOn ? 1.f : 0.f);
            expMsg.activeChannel[id] = (int8_t) matchedChannel;
            sourceGateActive[id] = matched;
            outputGateActive[id] = gateHigh;
        }

        // Learn: on any channel's gate rising edge, if a cell is armed, assign it that note.
        for (int c = 0; c < channels; c++) {
            bool gateHigh = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
            if (gateHigh && !prevGateHigh[c] && learningId >= 0) {
                setLearnedNote(learningId, (int8_t) noteOf(inputs[PITCH_INPUT].getVoltage(c)));
                if (learnSequential && learningId < 15) {
                    learningId += 1;
                }
                else {
                    learningId = -1;
                }
            }
            prevGateHigh[c] = gateHigh;
        }
        // Channels beyond the current poly width can't have a tracked previous
        // state — clear it so a later channel-count increase isn't seen as a
        // spurious rising edge.
        for (int c = channels; c < 16; c++) {
            prevGateHigh[c] = false;
        }

        if (rightExpander.module && padIsExpanderModel(rightExpander.module->model)) {
            padForwardMessage(this, expMsg);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* notesJ = json_array();
        json_t* latchedJ = json_array();
        for (int id = 0; id < 16; id++) {
            json_array_append_new(notesJ, json_integer(learnedNotes[id]));
            json_array_append_new(latchedJ, json_boolean(latchedState[id]));
        }
        json_object_set_new(rootJ, "notes", notesJ);
        json_object_set_new(rootJ, "latched", latchedJ);
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* notesJ = json_object_get(rootJ, "notes")) {
            for (int id = 0; id < 16; id++) {
                if (json_t* noteJ = json_array_get(notesJ, id)) {
                    learnedNotes[id] = (int8_t) json_integer_value(noteJ);
                }
                else {
                    learnedNotes[id] = -1;
                }
            }
        }
        if (json_t* latchedJ = json_object_get(rootJ, "latched")) {
            for (int id = 0; id < 16; id++) {
                if (json_t* latchJ = json_array_get(latchedJ, id)) {
                    latchedState[id] = json_boolean_value(latchJ);
                }
                else {
                    latchedState[id] = false;
                }
            }
        }
    }
};

#ifndef HEADLESS
// One shared widget for the whole 2x8 grid (column-major id = col*8+row,
// same indexing convention as HostMIDI-Gate.cpp's 3x6 grid, just 2x8), with
// the cell hit-tested directly from mouse position inside onButton() rather
// than delegating to 16 separate LedDisplayChoice children (confirmed
// pattern: voxglitch's CellularAutomatonDisplay::getRowAndColumnFromVec).
// Also supports HostMIDI-Gate.cpp's typed-note-name / clear-cell editing
// (CardinalNoteChoice::onSelectText/onSelectKey/onDeselect), adapted to one
// widget tracking a single "selected" cell instead of 16 widgets each
// tracking their own.
struct PadsNoteGridDisplay : OpaqueWidget {
    Pads* module;
    int selectedId = -1;
    int8_t focusNote = -1;
    ui::Tooltip* tooltip = nullptr;

    PadsNoteGridDisplay(Pads* module, Vec size) : module(module) {
        box.size = size;
    }

    // Static text (no per-frame Quantity to read, unlike ParamWidget's own
    // ParamTooltip), so a plain ui::Tooltip is enough — same createTooltip/
    // destroyTooltip + onEnter/onLeave recipe as ParamWidget.cpp.
    void createTooltip() {
        if (!settings::tooltips || tooltip) {
            return;
        }
        tooltip = new ui::Tooltip;
        tooltip->text = string::f(
            "Click a cell, then play a note to learn it.\n"
            "%s+click instead to learn a whole row: the next cell arms automatically after each note.\n"
            "Or click and type a note name (A-G, #, octave).\n"
            "Enter confirms (advances to the next cell if %s-armed) — Esc cancels.\n"
            "Click elsewhere also cancels, leaving the cell unchanged.",
            RACK_MOD_CTRL_NAME, RACK_MOD_CTRL_NAME);
        APP->scene->addChild(tooltip);
    }

    void destroyTooltip() {
        if (!tooltip) {
            return;
        }
        APP->scene->removeChild(tooltip);
        delete tooltip;
        tooltip = nullptr;
    }

    void onEnter(const EnterEvent& e) override {
        createTooltip();
    }

    void onLeave(const LeaveEvent& e) override {
        destroyTooltip();
    }

    int idFromPos(Vec pos) {
        int col = (int) (pos.x / (box.size.x / 2.f));
        int row = (int) (pos.y / (box.size.y / 8.f));
        col = math::clamp(col, 0, 1);
        row = math::clamp(row, 0, 7);
        return col * 8 + row;
    }

    void onButton(const ButtonEvent& e) override {
        if (module == nullptr || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) {
            OpaqueWidget::onButton(e);
            return;
        }

        int id = idFromPos(e.pos);
        if (module->learningId == id) {
            // Click the already-armed cell again to cancel learn. Also drop
            // selectedId/focusNote — this widget stays Rack's selected
            // widget (no onDeselect fires here), so leaving them stale would
            // let a stray keypress right after this click silently commit a
            // note onto the now-cancelled cell.
            module->learningId = -1;
            if (selectedId == id) {
                selectedId = -1;
                focusNote = -1;
            }
        }
        else {
            selectedId = id;
            focusNote = -1;
            module->learningId = id;
            // Ctrl+click arms sequential learn: advance to the next cell
            // after each learned note instead of leaving learn mode.
            module->learnSequential = (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL;
            APP->event->setSelectedWidget(this);
        }
        e.consume(this);
    }

    void onSelectText(const SelectTextEvent& e) override {
        if (selectedId < 0) {
            return;
        }

        const int c = e.codepoint;
        if ('a' <= c && c <= 'g') {
            static const int majorNotes[7] = {9, 11, 0, 2, 4, 5, 7};
            focusNote = majorNotes[c - 'a'];
        }
        else if (c == '#') {
            if (focusNote >= 0) {
                focusNote += 1;
            }
        }
        else if ('0' <= c && c <= '9') {
            if (focusNote >= 0) {
                focusNote = focusNote % 12;
                focusNote += 12 * (c - '0' + 1);
            }
        }

        if (focusNote < 0) {
            focusNote = -1;
        }

        e.consume(this);
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        if (selectedId < 0 || e.action != GLFW_PRESS) {
            OpaqueWidget::onSelectKey(e);
            return;
        }

        if (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) {
            if (e.mods & RACK_MOD_MASK) {
                OpaqueWidget::onSelectKey(e);
                return;
            }
            if (focusNote >= 0) {
                module->setLearnedNote(selectedId, focusNote);
                // Mirrors process()'s own Learn block: a cell armed via
                // Ctrl+click stays in sequential mode after a typed-and-
                // confirmed note too, not just a played one — advance to
                // the next cell and stay selected/armed for more typing.
                if (module->learnSequential && selectedId < 15) {
                    int nextId = selectedId + 1;
                    selectedId = nextId;
                    focusNote = -1;
                    module->learningId = nextId;
                    e.consume(this);
                    return;
                }
            }
            APP->event->setSelectedWidget(NULL);
            e.consume(this);
        }
        else if (e.key == GLFW_KEY_BACKSPACE || e.key == GLFW_KEY_DELETE) {
            module->setLearnedNote(selectedId, -1);
            focusNote = -1;
            e.consume(this);
        }
        else if (e.key == GLFW_KEY_ESCAPE) {
            // Cancel: deselect — onDeselect() discards any in-progress
            // typed entry and clears learningId/selectedId for us.
            APP->event->setSelectedWidget(NULL);
            e.consume(this);
        }
        else {
            OpaqueWidget::onSelectKey(e);
        }
    }

    void onDeselect(const DeselectEvent&) override {
        // Losing selection always cancels — a typed-in-progress entry is
        // discarded, not committed (Enter is the only way to commit one;
        // see onSelectKey). Unconditional on module too: this widget covers
        // all 16 cells, so losing selection (e.g. clicking elsewhere in
        // Rack) means no cell should stay armed — including mid-sequence
        // during a Ctrl+click learn run, where learningId has already moved
        // on from selectedId.
        if (module) {
            module->learningId = -1;
        }
        selectedId = -1;
        focusNote = -1;
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGBA(0x10, 0x10, 0x10, 0xff));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0x50, 0x50, 0x50, 0xff));
        nvgStroke(args.vg);

        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font) {
            return;
        }

        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 13.f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        float cellW = box.size.x / 2.f;
        float cellH = box.size.y / 8.f;

        for (int id = 0; id < 16; id++) {
            int col = id / 8;
            int row = id % 8;
            float cx = cellW * col + cellW / 2.f;
            float cy = cellH * row + cellH / 2.f;

            // `armed` (module->learningId) alone drives the highlight — it's
            // the single source of truth for "this cell is being learned".
            // `selected` only tracks keyboard focus for typed note entry
            // below; the two are kept in sync everywhere they're set, so
            // this never needs to fall back to `selected` for the highlight
            // (doing so previously let a stale selectedId show a second
            // cell as if still armed, e.g. after a cancel-click or after
            // sequential learn advanced learningId elsewhere).
            bool armed = module && (id == module->learningId);
            // Sequential (Ctrl/Cmd-armed) learn gets its own color so a whole-
            // row learn run is visually distinct from a plain single-cell one.
            bool sequential = armed && module->learnSequential;
            bool selected = (id == selectedId);
            if (armed) {
                nvgBeginPath(args.vg);
                nvgRoundedRect(args.vg, cellW * col + 1.f, cellH * row + 1.f, cellW - 2.f, cellH - 2.f, 1.f);
                nvgFillColor(args.vg, sequential ? nvgRGBA(0x40, 0x28, 0x10, 0xff) : nvgRGBA(0x40, 0x40, 0x10, 0xff));
                nvgFill(args.vg);
            }

            int8_t note = module ? module->learnedNotes[id] : (int8_t) (36 + id);
            std::string label = (selected && focusNote >= 0) ? padNoteName(focusNote) : padNoteName(note);

            NVGcolor noteColor = sequential ? nvgRGBA(0xff, 0xa0, 0x40, 0xee)
                : armed ? nvgRGBA(0xff, 0xff, 0x40, 0xee) : nvgRGBA(0x40, 0xff, 0x80, 0xee);
            nvgFillColor(args.vg, noteColor);
            nvgText(args.vg, cx, cy, label.c_str(), NULL);

            // Two independent activity traits around the note name: top
            // reflects the source gate (`matched`, i.e. the note is
            // currently held on the incoming poly cable), bottom reflects
            // the actual output gate (`gateHigh`, post-Latch) — they only
            // diverge while a cell's Latch is on and the source note has
            // been released but the output is still toggled high.
            bool sourceActive = module && module->sourceGateActive[id];
            bool outputActive = module && module->outputGateActive[id];
            if (sourceActive || outputActive) {
                float lineHalfW = cellW / 2.f - 4.f;
                nvgStrokeColor(args.vg, noteColor);
                nvgStrokeWidth(args.vg, 1.f);
                if (sourceActive) {
                    float lineY = cy - 9.f;
                    nvgBeginPath(args.vg);
                    nvgMoveTo(args.vg, cx - lineHalfW, lineY);
                    nvgLineTo(args.vg, cx + lineHalfW, lineY);
                    nvgStroke(args.vg);
                }
                if (outputActive) {
                    float lineY = cy + 9.f;
                    nvgBeginPath(args.vg);
                    nvgMoveTo(args.vg, cx - lineHalfW, lineY);
                    nvgLineTo(args.vg, cx + lineHalfW, lineY);
                    nvgStroke(args.vg);
                }
            }
        }
    }
};

// Smaller lit latch button than the stock VCVLightLatch (which is built on
// VCVButton, ~18px/side) — same LightButton+latch recipe, just wrapping the
// smaller bundled TL1105 switch (~15.36px/side) instead. TL1105 already
// ships both on/off SVG frames, so no new artwork is needed.
template <typename TLight>
struct SmallLightLatch : LightButton<TL1105, TLight> {
    SmallLightLatch() {
        this->momentary = false;
        this->latch = true;
    }
};

struct PadsWidget : ModuleWidget {
    PadsWidget(Pads* module) {
        setModule(module);
        const float panelWidth = 60.96f;
        setPanel(new AlPanel(mm2px(Vec(panelWidth, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/Pads_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/Pads_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder coordinates — panel layout WIP in Inkscape, same
        // approach as Zones' own widget constructor. Row 1: the two poly
        // inputs. Below that, left to right: colLatchLeft (8 Latch buttons)
        // | colGateLeft (8 gate outputs) | col2 (2x8 note display) |
        // colGateRight (8 gate outputs) | colLatchRight (8 Latch buttons),
        // rows aligned with the display's own 8 internal rows.
        const float displayCenterX = panelWidth / 2.f;
        const float inputsZoneOffsetY = 22.f;

        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(panelWidth / 3.f, inputsZoneOffsetY)), module, Pads::PITCH_INPUT));
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(panelWidth * 2.f / 3.f, inputsZoneOffsetY)), module, Pads::GATE_INPUT));

        const float colLatchLeft = 5.46f, colGateLeft = 15.46f, colGateRight = 45.5f, colLatchRight = 55.5f;
        const float outputsZoneOffsetY = 31.5f;
        const float outputsZoneHeight = 88.f;

        Vec displaySize = mm2px(Vec(18.f, outputsZoneHeight));
        Vec displayPos = mm2px(Vec(displayCenterX, outputsZoneOffsetY + outputsZoneHeight / 2.f)).minus(displaySize.div(2));
        PadsNoteGridDisplay* display = new PadsNoteGridDisplay(module, displaySize);
        display->box.pos = displayPos;
        addChild(display);

        for (int row = 0; row < 8; row++) {
            float rowY = outputsZoneOffsetY + outputsZoneHeight * (row + 0.5f) / 8.f;
            addParam(createLightParamCentered<SmallLightLatch<MediumSimpleLight<WhiteLight>>>(
                mm2px(Vec(colLatchLeft, rowY)), module, Pads::LATCH_PARAMS + row, Pads::LATCH_LIGHTS + row));
            addParam(createLightParamCentered<SmallLightLatch<MediumSimpleLight<WhiteLight>>>(
                mm2px(Vec(colLatchRight, rowY)), module, Pads::LATCH_PARAMS + 8 + row, Pads::LATCH_LIGHTS + 8 + row));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colGateLeft, rowY)), module, Pads::GATE_OUTPUTS + row));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(colGateRight, rowY)), module, Pads::GATE_OUTPUTS + 8 + row));
        }
    }

    // Appends `model` after the end of the existing PadX chain to this
    // Pads' right (any number of instances can be stacked at once,
    // forwarded right to left — see PadX.hpp), instead of always
    // placing it immediately next to Pads itself where it would overlap
    // an expander already there. Mirrors Venom's own VenomWidget::
    // addExpander (Venom.hpp), adapted to walk the chain first.
    void addPadXModel(Model* model) {
        Module* last = module;
        while (last->rightExpander.module && padIsExpanderModel(last->rightExpander.module->model)) {
            last = last->rightExpander.module;
        }
        ModuleWidget* lastWidget = (last == module) ? this : APP->scene->rack->getModule(last->id);

        Module* newModule = model->createModule();
        APP->engine->addModule(newModule);
        ModuleWidget* newWidget = model->createModuleWidget(newModule);
        APP->scene->rack->setModulePosForce(newWidget,
            Vec(lastWidget->box.pos.x + lastWidget->box.size.x, lastWidget->box.pos.y));
        APP->scene->rack->addModule(newWidget);

        history::ModuleAdd* h = new history::ModuleAdd;
        h->name = "create " + model->name;
        h->setModule(newWidget);
        APP->history->push(h);
    }

    // Clears (learnedNotes[id] = -1) every cell whose GATE_OUTPUTS jack has
    // no cable plugged in — a quick way to drop cells that were only ever
    // placeholders (e.g. a factory preset's unused rows) without hand-
    // clearing each one via Backspace.
    void clearUnpatchedCells() {
        Pads* pads = static_cast<Pads*>(module);
        for (int id = 0; id < 16; id++) {
            PortWidget* port = getOutput(Pads::GATE_OUTPUTS + id);
            if (port && APP->scene->rack->getCablesOnPort(port).empty()) {
                pads->setLearnedNote(id, -1);
            }
        }
    }

    // Flips every cell's LATCH_PARAMS switch (Normal <-> Latch), letting a
    // whole layout be swapped in one click instead of clicking all 16
    // Latch buttons by hand.
    void invertLatches() {
        Pads* pads = static_cast<Pads*>(module);
        for (int id = 0; id < 16; id++) {
            bool latchOn = pads->params[Pads::LATCH_PARAMS + id].getValue() > 0.5f;
            pads->params[Pads::LATCH_PARAMS + id].setValue(latchOn ? 0.f : 1.f);
        }
    }

    void appendContextMenu(Menu* menu) override {
        appendAluminiumThemeMenu(menu);

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Add an expander", "",
            [this]() { addPadXModel(modelPadX); }));
        menu->addChild(createMenuItem("Clear unpatched cells", "",
            [this]() { clearUnpatchedCells(); }));
        menu->addChild(createMenuItem("Invert latches", "",
            [this]() { invertLatches(); }));
    }
};
#else
struct PadsWidget : ModuleWidget {
    PadsWidget(Pads* module) {
        setModule(module);
        addInput(createInput<PJ301MPort>({}, module, Pads::PITCH_INPUT));
        addInput(createInput<PJ301MPort>({}, module, Pads::GATE_INPUT));
        for (int id = 0; id < 16; id++) {
            addOutput(createOutput<PJ301MPort>({}, module, Pads::GATE_OUTPUTS + id));
        }
    }
};
#endif

Model* modelPads = createModel<Pads, PadsWidget>("Pads");
