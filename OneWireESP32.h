/*
https://github.com/junkfix/esp32-ds18b20
*/

#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define MAX_BLOCKS 64
#else
#define MAX_BLOCKS 48
#endif

class OneWire32;

// NOTE: udata bude ukazatel na OneWire32 (ne QueueHandle_t)
IRAM_ATTR bool owrxdone(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata);

class OneWire32 {
  friend IRAM_ATTR bool owrxdone(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata);
  private:
    gpio_num_t owpin;

    // IMPORTANT: must be initialized to nullptr (constructor can early-return).
    rmt_channel_handle_t owtx = nullptr;
    rmt_channel_handle_t owrx = nullptr;
    rmt_encoder_handle_t owcenc = nullptr;
    rmt_encoder_handle_t owbenc = nullptr;
    rmt_symbol_word_t owbuf[MAX_BLOCKS];
    QueueHandle_t owqueue = nullptr;

    // Guard against ISR callback firing after cleanup/free
    volatile bool alive = false;

    uint8_t drv = 0;

    void cleanup();

  public:
    OneWire32(uint8_t pin);
    ~OneWire32();

    bool ready() const { return drv != 0; }

    bool reset();
    void request();
    uint8_t getTemp(uint64_t &addr, float &temp);
    uint8_t search(uint64_t *addresses, uint8_t total);
    bool read(uint8_t &data, uint8_t len = 8);
    bool write(const uint8_t data, uint8_t len = 8);

    // for ISR guard
    inline bool isAlive() const { return alive; }
};
