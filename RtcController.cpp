#include "RtcController.h"
#include "config_pins.h"

#include <Wire.h>
#include <time.h>

static constexpr uint8_t RTC_ADDR = 0x51;

// PCF85063 registers
static constexpr uint8_t REG_CTRL1  = 0x00;
static constexpr uint8_t REG_CTRL2  = 0x01;
static constexpr uint8_t REG_OFFSET = 0x02;
static constexpr uint8_t REG_RAM    = 0x03;
static constexpr uint8_t REG_SEC    = 0x04; // 0..59 (bit7 OS)
static constexpr uint8_t REG_MIN    = 0x05;
static constexpr uint8_t REG_HOUR   = 0x06;
static constexpr uint8_t REG_DAY    = 0x07;
static constexpr uint8_t REG_WDAY   = 0x08;
static constexpr uint8_t REG_MONTH  = 0x09;
static constexpr uint8_t REG_YEAR   = 0x0A; // 0..99

static bool s_present = false;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + 10 * ((v >> 4) & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool i2cRead(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_ADDR, (int)len) != (int)len) return false;
  for (size_t i=0;i<len;i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool i2cWrite(uint8_t reg, const uint8_t* buf, size_t len) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  for (size_t i=0;i<len;i++) Wire.write(buf[i]);
  return (Wire.endTransmission() == 0);
}

void rtcInit() {
  // Ensure Wire is up (RelayController also calls Wire.begin with same pins)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Wire.beginTransmission(RTC_ADDR);
  s_present = (Wire.endTransmission() == 0);

  if (s_present) {
    Serial.println(F("[RTC] Detected at 0x51"));
  } else {
    Serial.println(F("[RTC] Not present"));
  }
}

bool rtcIsPresent() { return s_present; }

bool rtcGetEpoch(time_t &outEpoch) {
  if (!s_present) return false;

  uint8_t buf[7] = {0};
  if (!i2cRead(REG_SEC, buf, sizeof(buf))) return false;

  const uint8_t secRaw = buf[0];
  const bool osFlag = (secRaw & 0x80) != 0; // oscillator stop
  const uint8_t sec = bcd2bin(secRaw & 0x7F);
  const uint8_t min = bcd2bin(buf[1] & 0x7F);
  const uint8_t hour = bcd2bin(buf[2] & 0x3F);
  const uint8_t day = bcd2bin(buf[3] & 0x3F);
  const uint8_t month = bcd2bin(buf[5] & 0x1F);
  const uint16_t year = 2000 + bcd2bin(buf[6]);

  if (osFlag) {
    // RTC reports oscillator stop => time may be invalid
    Serial.println(F("[RTC] OS flag set, time may be invalid"));
    // still allow reading; caller can decide based on epoch validity
  }

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_sec = sec;
  t.tm_min = min;
  t.tm_hour = hour;
  t.tm_mday = day;
  t.tm_mon = (int)month - 1;
  t.tm_year = (int)year - 1900;

  // Interpret as local time? RTC typically stores local time.
  // We treat RTC as local time and convert via mktime (uses TZ already set).
  time_t local = mktime(&t);
  if (local < 0) return false;
  outEpoch = local;
  return true;
}

bool rtcSetEpoch(time_t epoch) {
  if (!s_present) return false;

  struct tm t;
  localtime_r(&epoch, &t);

  uint8_t buf[7];
  buf[0] = bin2bcd((uint8_t)t.tm_sec) & 0x7F; // clear OS bit
  buf[1] = bin2bcd((uint8_t)t.tm_min) & 0x7F;
  buf[2] = bin2bcd((uint8_t)t.tm_hour) & 0x3F;
  buf[3] = bin2bcd((uint8_t)t.tm_mday) & 0x3F;
  buf[4] = bin2bcd((uint8_t)((t.tm_wday + 6) % 7 + 1)); // convert to 1..7 if needed (not critical)
  buf[5] = bin2bcd((uint8_t)(t.tm_mon + 1)) & 0x1F;
  buf[6] = bin2bcd((uint8_t)(t.tm_year - 100)) & 0xFF; // years since 2000

  if (!i2cWrite(REG_SEC, buf, sizeof(buf))) return false;
  Serial.println(F("[RTC] Time written"));
  return true;
}
