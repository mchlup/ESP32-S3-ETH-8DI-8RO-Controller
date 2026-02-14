#include "EquithermController.h"

// Compatibility wrapper.
// The real Equitherm implementation lives in LogicController (logicUpdate()).

void equithermInit() {
    // no-op (logicInit() performs all initialization)
}

void equithermApplyConfig(const String& json) {
    // Delegate to the main config applier.
    // This keeps older code paths working if they still call equithermApplyConfig().
    logicApplyConfig(json);
}

void equithermLoop() {
    // no-op (logicUpdate() runs from the main loop)
}

EquithermStatus equithermGetStatus() {
    return logicGetEquithermStatus();
}
