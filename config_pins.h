#pragma once
#include <Arduino.h>


// Waveshare ESP32-S3-ETH-8DI-8RO pin map (Waveshare Wiki)
//
// Relays are driven by TCA9554 (EXIO1..EXIO8) 
// Digital inputs are on GPIO4..GPIO11 
// WS2812 RGB LED is on GPIO38 
//
// RTC chip PCF85063 exists on board; I2C is on GPIO41/42 on this board family.

#define INPUT1_PIN 4
#define INPUT2_PIN 5
#define INPUT3_PIN 6
#define INPUT4_PIN 7
#define INPUT5_PIN 8
#define INPUT6_PIN 9
#define INPUT7_PIN 10
#define INPUT8_PIN 11

// I2C (shared bus for RTC + IO expander)
#define I2C_SCL_PIN 41
#define I2C_SDA_PIN 42
#define I2C_FREQ_HZ 100000

// RGB + buzzer
#define RGB_LED_PIN 38
#define BUZZER_PIN 46

// DS18B20 (OneWire) sběrnice – GPIO0..GPIO3
// Na každé sběrnici mohou být až 3 teploměry (dle návrhu projektu).
#define DALLAS_IO0_PIN 0
#define DALLAS_IO1_PIN 1
#define DALLAS_IO2_PIN 2
#define DALLAS_IO3_PIN 3

// TCA9554 default I2C address on many boards
#ifndef TCA9554_ADDR
#define TCA9554_ADDR 0x20
#endif
