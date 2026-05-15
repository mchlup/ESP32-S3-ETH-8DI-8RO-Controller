#pragma once

// Minimal build feature list.
// Only the modules needed for base functionality are enabled.

// Network (WiFiManager)
#include "FeatureNetwork.h"

// OpenTherm boiler communication (read-only polling + discovery)
#include "FeatureOpenTherm.h"

// BLE client (ESP-Meteostanice-Outdoor)
#include "FeatureBle.h"

// Web portal (UI + API)
#include "FeatureWebPortal.h"

// Arduino IDE OTA (network upload)
#include "FeatureOta.h"

// Ekviterm (day/night curves + weekly schedule)
#include "FeatureEquitherm.h"
