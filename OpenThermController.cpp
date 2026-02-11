// IMPORTANT: include Features.h first so FEATURE_OPENTHERM is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "OpenThermController.h"

#if defined(FEATURE_OPENTHERM)

// Stub: keeps firmware buildable.

namespace {
  OpenThermStatus g_st;
}

void openthermInit() {
  g_st.present = false;
  g_st.ready = false;
  g_st.fault = false;
  g_st.boilerTempC = NAN;
  g_st.returnTempC = NAN;
  g_st.lastUpdateMs = 0;
  g_st.reason = "stub";
}

void openthermLoop() {
  // No-op.
}

OpenThermStatus openthermGetStatus() {
  return g_st;
}

#endif // FEATURE_OPENTHERM
