#pragma once

// Board-based build profiles.
//
// Goal: allow compiling the same codebase on "small" dev boards (e.g. ESP32
// DevKitV1) by automatically disabling IRAM-heavy modules.

// ---------------- Board detection ----------------

// Classic ESP32 (DevKitV1, WROOM/WROVER)
// Note: Arduino-ESP32 defines CONFIG_IDF_TARGET_ESP32 for the classic chip.
#if defined(ARDUINO_ESP32_DEV) || defined(CONFIG_IDF_TARGET_ESP32)
  #define BOARD_CLASSIC_ESP32 1
#endif

// Convenience alias for the user's "ESP32 DevKitV1" test target.
#if defined(ARDUINO_ESP32_DEV)
  #define BOARD_ESP32_DEVKIT 1
#endif

// ESP32-S3
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define BOARD_ESP32_S3 1
#endif

// ---------------- Profile selection ----------------

// Uncomment to force the minimal profile regardless of board.
// #define FEATURE_DEVKIT_MINIMAL 1

#if defined(BOARD_CLASSIC_ESP32) && !defined(FEATURE_DEVKIT_MINIMAL)
  #define FEATURE_DEVKIT_MINIMAL 1
#endif

// ---------------- Profile effects ----------------

#if defined(FEATURE_DEVKIT_MINIMAL)
  // Keep the build tiny on classic ESP32.
  #define DISABLE_BLE 0
  #define DISABLE_WEBSERVER 0
  #define DISABLE_MQTT 1
  #define DISABLE_OTA 1
  #define DISABLE_OPENTHERM 1
  #define DISABLE_NETWORK 0
  #define DISABLE_RGB_LED 1
  #define DISABLE_BUZZER 1
#endif

// ---------------- DS18B20 pin map ----------------
// The project uses 4x OneWire buses (up to 3 sensors each).
// On the Waveshare ESP32-S3-ETH-8DI-8RO board these are GPIO0..GPIO3.
// For ESP32 DevKitV1 testing, remap to GPIO5/18/19/21.

#if defined(BOARD_ESP32_DEVKIT)
  #define DS18B20_PIN_1 5   // D5
  #define DS18B20_PIN_2 18  // D18
  #define DS18B20_PIN_3 19  // D19
  #define DS18B20_PIN_4 21  // D21
#else
  #define DS18B20_PIN_1 0
  #define DS18B20_PIN_2 1
  #define DS18B20_PIN_3 2
  #define DS18B20_PIN_4 3
#endif
