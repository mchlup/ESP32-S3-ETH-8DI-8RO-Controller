/*
https://github.com/junkfix/esp32-ds18b20
*/

#include <Arduino.h>
#include "OneWireESP32.h"

#define OWR_OK 0
#define OWR_CRC 1
#define OWR_BAD_DATA 2
#define OWR_TIMEOUT 3
#define OWR_DRIVER 4

#define OW_RESET_PULSE 500
#define OW_RESET_WAIT 200
#define OW_RESET_PRESENCE_WAIT_MIN 15
#define OW_RESET_PRESENCE_MIN 60
#define OW_SLOT_BIT_SAMPLE_TIME 15
#define OW_SLOT_START 2
#define OW_SLOT_BIT 60
#define OW_SLOT_RECOVERY 5
#define OW_TIMEOUT 50

static rmt_symbol_word_t ow_bit0 = {
  .duration0 = OW_SLOT_START + OW_SLOT_BIT,
  .level0 = 0,
  .duration1 = OW_SLOT_RECOVERY,
  .level1 = 1
};

static rmt_symbol_word_t ow_bit1 = {
  .duration0 = OW_SLOT_START,
  .level0 = 0,
  .duration1 = OW_SLOT_BIT + OW_SLOT_RECOVERY,
  .level1 = 1
};

const rmt_transmit_config_t owtxconf = {
  .loop_count = 0,
  .flags = {
    .eot_level = 1
  }
};

const rmt_receive_config_t owrxconf = {
  .signal_range_min_ns = 1000,
  .signal_range_max_ns = (OW_RESET_PULSE + OW_RESET_WAIT) * 1000,
};

IRAM_ATTR bool owrxdone(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata) {
  (void)ch;
  OneWire32* self = (OneWire32*)udata;
  if (!self) return false;
  if (!self->isAlive()) return false;
  // queue can be deleted during cleanup; alive=false protects us
  BaseType_t h = pdFALSE;
  // NOTE: send to the instance queue
  xQueueSendFromISR(self->owqueue, edata, &h);
  return (h == pdTRUE);
}

void OneWire32::cleanup() {
  alive = false;
  // order matters: disable -> delete
  if (owbenc) {
    rmt_del_encoder(owbenc);
    owbenc = nullptr;
  }
  if (owcenc) {
    rmt_del_encoder(owcenc);
    owcenc = nullptr;
  }
  if (owrx) {
    rmt_disable(owrx);
    rmt_del_channel(owrx);
    owrx = nullptr;
  }
  if (owtx) {
    rmt_disable(owtx);
    rmt_del_channel(owtx);
    owtx = nullptr;
  }
  if (owqueue) {
    vQueueDelete(owqueue);
    owqueue = nullptr;
  }
}

OneWire32::OneWire32(uint8_t pin) {
  owpin = static_cast<gpio_num_t>(pin);

  rmt_bytes_encoder_config_t bnc = {
    .bit0 = ow_bit0,
    .bit1 = ow_bit1,
    .flags = {
      .msb_first = 0
    }
  };

  if (rmt_new_bytes_encoder(&bnc, &(owbenc)) != ESP_OK) {
    cleanup();
    return;
  }

  rmt_copy_encoder_config_t cnc = {};
  if (rmt_new_copy_encoder(&cnc, &(owcenc)) != ESP_OK) {
    cleanup();
    return;
  }

  const rmt_rx_channel_config_t rxconf = {
    .gpio_num = owpin,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 1000000,
    .mem_block_symbols = MAX_BLOCKS
  };

  if (rmt_new_rx_channel(&rxconf, &(owrx)) != ESP_OK) {
    cleanup();
    return;
  }

  const rmt_tx_channel_config_t txconf = {
    .gpio_num = owpin,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 1000000,
    .mem_block_symbols = MAX_BLOCKS,
    .trans_queue_depth = 4,
    .flags = {
      .io_loop_back = 1,
      .io_od_mode = 1
    }
  };

  if (rmt_new_tx_channel(&txconf, &owtx) != ESP_OK) {
    cleanup();
    return;
  }

  owqueue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
  if (owqueue == NULL) {
    cleanup();
    return;
  }

  alive = true;

  rmt_rx_event_callbacks_t rx_callbacks = {
    .on_recv_done = owrxdone
  };

  if (rmt_rx_register_event_callbacks(owrx, &rx_callbacks, this) != ESP_OK) {
    cleanup();
    return;
  }

  if (rmt_enable(owrx) != ESP_OK) {
    cleanup();
    return;
  }

  if (rmt_enable(owtx) != ESP_OK) {
    cleanup();
    return;
  }

  // release line high
  static rmt_symbol_word_t release_symbol = {
    .duration0 = 1,
    .level0 = 1,
    .duration1 = 0,
    .level1 = 1,
  };
  rmt_transmit(owtx, owcenc, &release_symbol, sizeof(rmt_symbol_word_t), &owtxconf);

  drv = 1;
}

OneWire32::~OneWire32() {
  cleanup();
  drv = 0;
}

bool OneWire32::reset() {
  if (!drv) return false;

  rmt_symbol_word_t symbol_reset = {
    .duration0 = OW_RESET_PULSE,
    .level0 = 0,
    .duration1 = OW_RESET_WAIT,
    .level1 = 1
  };

  rmt_rx_done_event_data_t evt;
  rmt_receive(owrx, owbuf, sizeof(owbuf), &owrxconf);
  rmt_transmit(owtx, owcenc, &symbol_reset, sizeof(rmt_symbol_word_t), &owtxconf);

  bool found = false;
  if (xQueueReceive(owqueue, &evt, pdMS_TO_TICKS(OW_TIMEOUT)) == pdTRUE) {
    size_t symbol_num = evt.num_symbols;
    rmt_symbol_word_t *symbols = evt.received_symbols;

    if (symbol_num > 1) {
      if (symbols[0].level1 == 1) {
        if (symbols[0].duration1 > OW_RESET_PRESENCE_WAIT_MIN && symbols[1].duration0 > OW_RESET_PRESENCE_MIN) {
          found = true;
        }
      } else {
        if (symbols[0].duration0 > OW_RESET_PRESENCE_WAIT_MIN && symbols[1].duration1 > OW_RESET_PRESENCE_MIN) {
          found = true;
        }
      }
    }
    if (rmt_tx_wait_all_done(owtx, OW_TIMEOUT) != ESP_OK) {
      found = false;
    }
    } else {
    // Timeout: ensure TX is finished before destructor can free RMT objects
    rmt_tx_wait_all_done(owtx, OW_TIMEOUT);
  }
  return found;
}

bool OneWire32::read(uint8_t &data, uint8_t len) {
  if (!drv) return false;

  rmt_rx_done_event_data_t evt;
  rmt_receive(owrx, owbuf, sizeof(owbuf), &owrxconf);

  if (!write((len > 1) ? 0xff : 1, len) || xQueueReceive(owqueue, &evt, pdMS_TO_TICKS(OW_TIMEOUT)) != pdTRUE) {
    return false;
  }

  size_t symbol_num = evt.num_symbols;
  rmt_symbol_word_t *symbol = evt.received_symbols;
  data = 0;
  for (uint8_t i = 0; i < symbol_num && i < 8; i++) {
    if (!(symbol[i].duration0 > OW_SLOT_BIT_SAMPLE_TIME)) {
      data |= 1 << i;
    }
  }

  if (len != 8) { data = data & 0x01; }
  return true;
}

bool OneWire32::write(const uint8_t data, uint8_t len) {
  if (!drv) return false;

  if (len < 8) {
    const rmt_symbol_word_t *sb;
    for (uint8_t i = 0; i < len; i++) {
      sb = &ow_bit0;
      if ((data & (1 << i)) != 0) {
        sb = &ow_bit1;
      }
      if (rmt_transmit(owtx, owcenc, sb, sizeof(rmt_symbol_word_t), &owtxconf) != ESP_OK) {
        return false;
      }
    }
  } else {
    if (rmt_transmit(owtx, owbenc, &data, 1, &owtxconf) != ESP_OK) {
      return false;
    }
  }

  return (rmt_tx_wait_all_done(owtx, OW_TIMEOUT) == ESP_OK);
}

void OneWire32::request() {
  if (drv && reset()) {
    write(0xCC);
    write(0x44);
  }
}

const uint8_t crc_table[] = {
  0, 94, 188, 226, 97, 63, 221, 131, 194, 156, 126, 32, 163, 253, 31, 65, 157, 195, 33, 127, 252, 162, 64, 30, 95, 1, 227, 189, 62, 96, 130, 220, 35, 125, 159, 193, 66, 28, 254, 160, 225, 191, 93, 3, 128, 222, 60, 98, 190, 224, 2, 92, 223, 129, 99, 61, 124, 34, 192, 158, 29, 67, 161, 255, 70, 24, 250, 164, 39, 121, 155, 197, 132, 218, 56, 102, 229, 187, 89, 7, 219, 133, 103, 57, 186, 228, 6, 88, 25, 71, 165, 251, 120, 38, 196, 154, 101, 59, 217, 135, 4, 90, 184, 230, 167, 249, 27, 69, 198, 152, 122, 36, 248, 166, 68, 26, 153, 199, 37, 123, 58, 100, 134, 216, 91, 5, 231, 185, 140, 210, 48, 110, 237, 179, 81, 15, 78, 16, 242, 172, 47, 113, 147, 205, 17, 79, 173, 243, 112, 46, 204, 146, 211, 141, 111, 49, 178, 236, 14, 80, 175, 241, 19, 77, 206, 144, 114, 44, 109, 51, 209, 143, 12, 82, 176, 238, 50, 108, 142, 208, 83, 13, 239, 177, 240, 174, 76, 18, 145, 207, 45, 115, 202, 148, 118, 40, 171, 245, 23, 73, 8, 86, 180, 234, 105, 55, 213, 139, 87, 9, 235, 181, 54, 104, 138, 212, 149, 203, 41, 119, 244, 170, 72, 22, 233, 183, 85, 11, 136, 214, 52, 106, 43, 117, 151, 201, 74, 20, 246, 168, 116, 42, 200, 150, 21, 75, 169, 247, 182, 232, 10, 84, 215, 137, 107, 53
};

uint8_t OneWire32::getTemp(uint64_t &addr, float &temp) {
  if (!drv) return OWR_DRIVER;

  if (reset()) {
    write(0x55);
    uint8_t *a = (uint8_t *)&addr;

    uint8_t crc = 0;
    for (uint8_t i = 0; i < 8; i++) {
      write(*a);
      crc = crc_table[crc ^ *a];
      a++;
    }
    if (crc) { return OWR_CRC; }

    write(0xBE);
    uint8_t data[9] = {0};
    for (uint8_t i = 0; i < 9; i++) {
      if (!read(data[i])) { return OWR_TIMEOUT; }
    }

    crc = 0;
    for (uint8_t i = 0; i < 8; i++) {
      crc = crc_table[crc ^ data[i]];
    }
    if (crc != data[8]) { return OWR_CRC; }

    int16_t raw = (data[1] << 8) | data[0];
    temp = (float)raw / 16.0f;
    return OWR_OK;
  }
  return OWR_TIMEOUT;
}

uint8_t OneWire32::search(uint64_t *addresses, uint8_t total) {
  if (!drv) return 0;

  uint8_t found = 0;
  uint8_t last_discrepancy = 0;
  bool last_device_flag = false;
  uint8_t last_rom[8] = {0};

  while (!last_device_flag && found < total) {
    if (!reset()) break;
    write(0xF0);

    uint8_t rom[8] = {0};
    uint8_t rom_byte_number = 0;
    uint8_t rom_byte_mask = 1;
    uint8_t id_bit_number = 1;
    uint8_t discrepancy = 0;

    while (rom_byte_number < 8) {
      uint8_t b1 = 0, b2 = 0;
      if (!read(b1, 1) || !read(b2, 1)) return found;

      uint8_t id_bit = b1 & 1;
      uint8_t cmp_id_bit = b2 & 1;

      // No devices on the bus
      if (id_bit == 1 && cmp_id_bit == 1) {
        id_bit_number = 0;
        break;
      }

      uint8_t search_direction = 0;

      if (id_bit != cmp_id_bit) {
        // Only one possible direction
        search_direction = id_bit;
      } else {
        // Discrepancy: decide based on previous ROM and last discrepancy position
        if (id_bit_number < last_discrepancy) {
          search_direction = (last_rom[rom_byte_number] & rom_byte_mask) ? 1 : 0;
        } else {
          search_direction = (id_bit_number == last_discrepancy) ? 1 : 0;
        }

        if (search_direction == 0) {
          discrepancy = id_bit_number;
        }
      }

      if (search_direction == 1) {
        rom[rom_byte_number] |= rom_byte_mask;
      } // else keep 0

      write(search_direction, 1);

      id_bit_number++;
      rom_byte_mask <<= 1;
      if (rom_byte_mask == 0) {
        rom_byte_number++;
        rom_byte_mask = 1;
      }
    }

    if (id_bit_number == 0) break;

    uint64_t addr = 0;
    memcpy(&addr, rom, 8);
    addresses[found++] = addr;

    memcpy(last_rom, rom, 8);
    last_discrepancy = discrepancy;
    if (last_discrepancy == 0) last_device_flag = true;
  }

  return found;
}
