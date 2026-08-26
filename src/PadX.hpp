#pragma once
#include <rack.hpp>
#include "plugin.hpp"

using namespace rack;

// Shared message contract between Pads and its PadX expander(s). Pads
// already knows, for each of its 16 gate cells, which incoming poly
// channel (if any) currently satisfies that cell's learned note — that's
// the only thing an expander needs: it reads its own local poly input at
// that same channel index to produce its own cell i output. No note numbers
// or pitch data need to cross the boundary at all.
static const int PAD_NUM_CELLS = 16;

struct PadXMessage {
    int8_t activeChannel[PAD_NUM_CELLS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // Pads' own current V/OCT+GATE channel count, so an expander can
    // tell whether its own (separately patched) lane cable has a matching
    // channel count.
    int8_t channels = 0;
};

inline bool padIsExpanderModel(Model* model) {
    return model == modelPadX;
}

// Pushes `msg` into `self`'s right neighbor's own leftExpander producer
// buffer — not self's own rightExpander buffer, per Module::Expander's own
// doc comment: the *consumer* owns the buffer it's read from, the *producer*
// writes across the boundary into it — and requests the flip, but only if
// that neighbor is a known expander model. Lets Pads, and each expander in
// turn, relay the same mapping to further PadX instances stacked to
// the right (mirrors ImpromptuModular's ChordKey/ChordKeyExpander
// chained-forward pattern).
inline void padForwardMessage(Module* self, const PadXMessage& msg) {
    Module* neighbor = self->rightExpander.module;
    if (!neighbor || !padIsExpanderModel(neighbor->model)) {
        return;
    }
    PadXMessage* out = reinterpret_cast<PadXMessage*>(neighbor->leftExpander.producerMessage);
    if (!out) {
        return;
    }
    *out = msg;
    neighbor->leftExpander.messageFlipRequested = true;
}
