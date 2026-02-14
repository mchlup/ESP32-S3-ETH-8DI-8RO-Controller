#pragma once


// ---------------------------------------------------------------------------
// Feature switches
//
// Comment out a single #include below to completely remove the feature from
// the build (including all related calls guarded by FEATURE_* macros).
// ---------------------------------------------------------------------------

// --- UI indicators ---

// RGB status LED (GPIO38)
#if !defined(DISABLE_RGB_LED)
  #include "FeatureRgbLed.h"
#endif

// Buzzer (GPIO46)
#if !defined(DISABLE_BUZZER)
  #include "FeatureBuzzer.h"
#endif

// --- Services / protocols ---

// Network (WiFi/Ethernet + time)
#if !defined(DISABLE_NETWORK)
  #include "FeatureNetwork.h"
#endif

// MQTT client
#if !defined(DISABLE_MQTT)
  #include "FeatureMqtt.h"
#endif

// OTA updates (HTTP)
#if !defined(DISABLE_OTA)
  #include "FeatureOTA.h"
#endif

// OpenTherm boiler communication
#if !defined(DISABLE_OPENTHERM)
  #include "FeatureOpenTherm.h"
#include "FeatureHeatLoss.h"
#endif

// BLE (scanner + outdoor sensor + optional mesh relay)
#if !defined(DISABLE_BLE)
  #include "FeatureBle.h"
#endif

// WebServer (UI + REST API)
#if !defined(DISABLE_WEBSERVER)
  #include "FeatureWebServer.h"
#endif
