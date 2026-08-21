#pragma once
#include <rack.hpp>
#include "plugin.hpp"

using namespace rack;

// Shared message contract between AlGate and its AlVelocity/AlAftertouch/
// AlRetrigger expanders. AlGate already knows, for each of its 16 gate
// cells, which incoming poly channel (if any) currently satisfies that
// cell's learned note — that's the only thing an expander needs: it reads
// its own local VEL/AFT/RTRG poly input at that same channel index to
// produce its own cell i output. No note numbers or pitch data need to
// cross the boundary at all.
static const int AL_GATE_NUM_CELLS = 16;

struct AlGateExpanderMessage {
    int8_t activeChannel[AL_GATE_NUM_CELLS];
};

inline bool alGateIsExpanderModel(Model* model) {
    return model == modelAlVelocityExpander || model == modelAlAftertouchExpander || model == modelAlRetriggerExpander;
}

// Pushes `msg` into `self`'s right neighbor's own leftExpander producer
// buffer — not self's own rightExpander buffer, per Module::Expander's own
// doc comment: the *consumer* owns the buffer it's read from, the *producer*
// writes across the boundary into it — and requests the flip, but only if
// that neighbor is one of the three known expander models. Lets AlGate, and
// each expander in turn, relay the same mapping to further expanders
// stacked to the right regardless of order (mirrors ImpromptuModular's
// ChordKey/ChordKeyExpander chained-forward pattern).
inline void alGateForwardMessage(Module* self, const AlGateExpanderMessage& msg) {
    Module* neighbor = self->rightExpander.module;
    if (!neighbor || !alGateIsExpanderModel(neighbor->model))
        return;
    AlGateExpanderMessage* out = reinterpret_cast<AlGateExpanderMessage*>(neighbor->leftExpander.producerMessage);
    if (!out)
        return;
    *out = msg;
    neighbor->leftExpander.messageFlipRequested = true;
}
