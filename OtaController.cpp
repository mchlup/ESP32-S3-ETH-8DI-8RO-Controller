// IMPORTANT: include Features.h first so FEATURE_OTA is visible before
// including the header (which provides stubs when the feature is disabled).
#include "Features.h"
#include "OtaController.h"

#if defined(FEATURE_OTA)

#include <Arduino.h>

#if __has_include(<ArduinoOTA.h>)
  #include <ArduinoOTA.h>
#endif

namespace OTA {
  void init() {
#if __has_include(<ArduinoOTA.h>)
    ArduinoOTA.setHostname("esp32-heat");
    ArduinoOTA.begin();
#endif
  }

  void loop() {
#if __has_include(<ArduinoOTA.h>)
    ArduinoOTA.handle();
#endif
  }
}

#endif // FEATURE_OTA
