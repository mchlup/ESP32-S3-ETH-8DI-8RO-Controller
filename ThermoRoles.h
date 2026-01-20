#pragma once

#include <Arduino.h>

// Canonical thermometer role keys used across the project (UI + config + backend).
// Keep these stable: they are stored in config.json.
//
// Known keys:
//  - outdoor
//  - flow
//  - return
//  - dhw
//  - aku_top
//  - aku_mid
//  - aku_bottom

static inline String thermoNormalizeRole(const String& role) {
    String v = role;
    v.trim();
    if (!v.length()) return "";

    String s = v;
    s.toLowerCase();

    // Backward compatibility
    if (s == "heating_flow" || s == "heatingflow" || s == "heating" || s == "flow") return "flow";
    if (s == "heating_return" || s == "heatingreturn" || s == "return" || s == "ret") return "return";
    if (s == "dhw_tank" || s == "dhw" || s == "tuv" || s == "hotwater") return "dhw";
    if (s == "aku_top" || s == "akutop" || s == "aku-horni" || s == "aku_horni") return "aku_top";
    if (s == "aku_mid" || s == "akumid" || s == "aku-uprostred" || s == "aku_uprostred" || s == "aku_middle") return "aku_mid";
    if (s == "aku_bottom" || s == "akubottom" || s == "aku-dolni" || s == "aku_dolni" || s == "aku_low") return "aku_bottom";
    if (s == "outdoor" || s == "venek" || s == "outside") return "outdoor";

    // If it's already one of the canonical keys, keep it.
    if (s == "outdoor" || s == "flow" || s == "return" || s == "dhw" || s == "aku_top" || s == "aku_mid" || s == "aku_bottom") {
        return s;
    }

    // Unknown/custom role -> keep trimmed original value (same behavior as UI).
    return v;
}
