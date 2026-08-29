#pragma once

// ---------------------------------------------------------------------------
// Optional module flag
//
// Comment out `#include "FeatureNetwork.h"` in Features.h to completely
// remove NetworkController (WiFi/Ethernet + time sync) from the build.
// ---------------------------------------------------------------------------
#define FEATURE_NETWORK 1

#include "NetworkController.h"
