#pragma once

// Local copy of https://github.com/junkfix/esp32-ds18b20 (RMT-based OneWire)
// This header matches the implementation in OneWireESP32.cpp.

#include <Arduino.h>

#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>

// RMT memory blocks (symbols). Tune if needed.
static constexpr uint32_t MAX_BLOCKS = 64;

class OneWire32 {
 public:
  explicit OneWire32(uint8_t pin);
  ~OneWire32();

  bool ready() const { return alive; }
  bool isAlive() const { return alive; }

  bool reset();
  // NOTE: default is a full byte (8 bits). Pass len=1 for single-bit operations.
  bool read(uint8_t& data, uint8_t len = 8);
  bool write(const uint8_t data, uint8_t len = 8);

  // Returns 0 on OK, non-zero on error.
  uint8_t getTemp(uint64_t& addr, float& temp);

  // Search ROMs; returns number of found addresses (<= total).
  uint8_t search(uint64_t* addresses, uint8_t total);

  // Used by ISR callback in OneWireESP32.cpp
  QueueHandle_t owqueue = nullptr;

 private:
  void cleanup();
  void request();

  bool alive = false;
  // Some legacy call sites in OneWireESP32.cpp use 'drv' as a shorthand flag.
  // Keep it as an alias to 'alive' for compatibility.
  bool drv = false;
  gpio_num_t owpin = GPIO_NUM_NC;

  // RX buffer for RMT receive. Size is in symbols.
  rmt_symbol_word_t owbuf[MAX_BLOCKS] = {};

  rmt_channel_handle_t owtx = nullptr;
  rmt_channel_handle_t owrx = nullptr;
  rmt_encoder_handle_t owbenc = nullptr;
  rmt_encoder_handle_t owcenc = nullptr;
};
