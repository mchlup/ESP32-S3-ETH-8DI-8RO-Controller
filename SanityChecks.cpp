#include "SanityChecks.h"
#include "config_pins.h"

namespace {
  template <size_t N>
  bool allUnique(const int (&pins)[N]) {
    for (size_t i = 0; i < N; i++) {
      for (size_t j = i + 1; j < N; j++) {
        if (pins[i] == pins[j]) return false;
      }
    }
    return true;
  }

  void warn(const __FlashStringHelper* msg) {
    Serial.print(F("[SANITY] "));
    Serial.println(msg);
  }

  void warnPinDup() { warn(F("Duplicate GPIO assignment detected (check config_pins.h).")); }
}

namespace SanityChecks {
  void runOnce() {
    // Basic pin sanity – catches accidental pin reuse when adding features.
    const int dallasPins[] = {DS18B20_PIN_1, DS18B20_PIN_2, DS18B20_PIN_3, DS18B20_PIN_4};
    if (!allUnique(dallasPins)) warnPinDup();

    const int i2cPins[] = {I2C_SCL_PIN, I2C_SDA_PIN};
    if (!allUnique(i2cPins)) warnPinDup();

    const int otPins[] = {OT_TX_PIN, OT_RX_PIN};
    if (!allUnique(otPins)) warn(F("OpenTherm TX/RX pins must be different."));

    // Cross-check obvious conflicts
    for (int p : dallasPins) {
      if (p == I2C_SCL_PIN || p == I2C_SDA_PIN) warn(F("Dallas pin conflicts with I2C pin."));
      if (p == OT_TX_PIN || p == OT_RX_PIN) warn(F("Dallas pin conflicts with OpenTherm pin."));
      if (p == RGB_LED_PIN) warn(F("Dallas pin conflicts with RGB LED pin."));
      if (p == BUZZER_PIN) warn(F("Dallas pin conflicts with Buzzer pin."));
    }

    if (RGB_LED_PIN == BUZZER_PIN) warn(F("RGB LED pin conflicts with Buzzer pin."));

    // Helpful info
    Serial.print(F("[SANITY] Pins: Dallas="));
    Serial.print(dallasPins[0]); Serial.print(',');
    Serial.print(dallasPins[1]); Serial.print(',');
    Serial.print(dallasPins[2]); Serial.print(',');
    Serial.print(dallasPins[3]);
    Serial.print(F(" I2C="));
    Serial.print(I2C_SDA_PIN); Serial.print('/'); Serial.print(I2C_SCL_PIN);
    Serial.print(F(" OT="));
    Serial.print(OT_TX_PIN); Serial.print('/'); Serial.print(OT_RX_PIN);
    Serial.print(F(" RGB="));
    Serial.print(RGB_LED_PIN);
    Serial.print(F(" BUZZ="));
    Serial.println(BUZZER_PIN);
  }
}
