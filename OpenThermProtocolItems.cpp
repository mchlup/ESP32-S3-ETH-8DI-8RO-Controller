#include "OpenThermProtocolItems.h"
#include <string.h>

namespace OpenThermProtocol {

const DataIdInfo DATA_IDS[] PROGMEM = {
  {0, "Master status", "flag8", "", "R"},
  {1, "Control setpoint", "f8.8", "\xC2\xB0C", "W"},
  {2, "Master configuration", "flag8", "", "W"},
  {3, "Slave configuration", "flag8", "", "R"},
  {4, "Command-Code", "u8", "", "W"},
  {5, "Application-specific fault flags", "flag8", "", "R"},
  {6, "Remote-parameter transfer-enable flags", "flag8", "", "R"},
  {7, "Cooling control signal", "f8.8", "%", "W"},
  {8, "Control setpoint 2 (TsetCH2)", "f8.8", "\xC2\xB0C", "W"},
  {9, "Remote override room setpoint", "f8.8", "\xC2\xB0C", "R"},
  {10, "Number of TSPs", "u8", "", "R"},
  {11, "TSP index no.", "u8", "", "R/W"},
  {12, "Size of Fault Buffer", "u8", "", "R"},
  {13, "FHB-entry index no.", "u8", "", "R"},
  {14, "Maximum relative modulation level setting", "f8.8", "%", "W"},
  {15, "Maximum boiler capacity & Minimum modulation level (HB=max capacity, LB=min modulation)", "u8/u8", "", "R"},
  {16, "Room setpoint", "f8.8", "\xC2\xB0C", "W"},
  {17, "Relative modulation level", "f8.8", "%", "R"},
  {18, "CH water pressure", "f8.8", "bar", "R"},
  {19, "DHW flow rate", "f8.8", "l/min", "R"},
  {20, "Day of week & time of day", "u8/u8", "", "R/W"},
  {21, "Date (month/day)", "u8/u8", "", "R/W"},
  {22, "Year", "u16", "", "R/W"},
  {23, "Room setpoint CH2", "f8.8", "\xC2\xB0C", "W"},
  {24, "Room temperature", "f8.8", "\xC2\xB0C", "R"},
  {25, "Boiler water temperature", "f8.8", "\xC2\xB0C", "R"},
  {26, "DHW temperature", "f8.8", "\xC2\xB0C", "R"},
  {27, "Outside temperature", "f8.8", "\xC2\xB0C", "R"},
  {28, "Return water temperature", "f8.8", "\xC2\xB0C", "R"},
  {29, "Solar storage temperature", "f8.8", "\xC2\xB0C", "R"},
  {30, "Solar collector temperature", "s16", "\xC2\xB0C", "R"},
  {31, "Flow temperature CH2", "f8.8", "\xC2\xB0C", "R"},
  {32, "DHW2 temperature", "f8.8", "\xC2\xB0C", "R"},
  {33, "Exhaust temperature", "s16", "\xC2\xB0C", "R"},

  {56, "DHW setpoint", "f8.8", "\xC2\xB0C", "R/W"},
  {57, "Max CH water setpoint", "f8.8", "\xC2\xB0C", "R/W"},

  {115, "OEM diagnostic code", "u16", "", "R"},
  {116, "Burner starts", "u16", "", "R/W"},
  {117, "CH pump starts", "u16", "", "R/W"},
  {118, "DHW pump/valve starts", "u16", "", "R/W"},
  {119, "DHW burner starts", "u16", "", "R/W"},
  {120, "Burner operation hours", "u16", "", "R/W"},
  {121, "CH pump operation hours", "u16", "", "R/W"},
  {122, "DHW pump/valve operation hours", "u16", "", "R/W"},
  {123, "DHW burner operation hours", "u16", "", "R/W"},
  {124, "OpenTherm version Master", "f8.8", "", "W"},
  {125, "OpenTherm version Slave", "f8.8", "", "R"},
  {126, "Master product version/type", "u8/u8", "", "W"},
  {127, "Slave product version/type", "u8/u8", "", "R"},
};

const uint8_t DATA_ID_COUNT = (uint8_t)(sizeof(DATA_IDS)/sizeof(DATA_IDS[0]));

const DataIdInfo* find(uint8_t id) {
  // Linear scan: small table.
  for (uint8_t i = 0; i < DATA_ID_COUNT; i++) {
    DataIdInfo tmp;
    memcpy_P(&tmp, &DATA_IDS[i], sizeof(DataIdInfo));
    if (tmp.id == id) return &DATA_IDS[i];
  }
  return nullptr;
}

} // namespace
