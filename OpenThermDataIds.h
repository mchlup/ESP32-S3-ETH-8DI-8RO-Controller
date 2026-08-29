#pragma once

#include <Arduino.h>

// Human-friendly metadata for OpenTherm Data-IDs.
//
// Used by:
//  - scan UI tooltips
//  - /api/opentherm/dataid/read JSON metadata
//  - marking temperature-related IDs for quick decoding
//
// NOTE: Not every boiler implements every Data-ID. Unknown/unimplemented IDs are
// expected and handled.

struct OpenThermDataIdMeta {
  uint8_t id;
  const char* name;
  const char* desc;
  bool isTemperature;
};

// Keep strings short (flash friendly). Unknown IDs fall back to "IDxx".
static const OpenThermDataIdMeta kOpenThermDataIdMeta[] PROGMEM = {
  // 0..39: common boiler/room controller set
  {  0, "Status", "Master/Slave status flags (fault, CH/DHW active, flame, ...)", false },
  {  1, "CH control setpoint", "Requested CH water temperature setpoint (TSet)", true },
  {  2, "Master config + MemberID", "Master configuration flags + MemberID code", false },
  {  3, "Slave config + MemberID", "Slave configuration flags + MemberID code", false },
  {  4, "Remote request", "Remote request (reset, service, water filling, ...)", false },
  {  5, "ASF + OEM fault", "Application-specific fault flags + OEM fault code", false },
  {  6, "RBP flags", "Remote boiler parameter transfer-enable & read/write flags", false },
  {  7, "Cooling control", "Cooling control signal (%)", false },
  {  8, "CH2 control setpoint", "Control setpoint for 2nd CH circuit (TSetCH2)", true },
  {  9, "Room setpoint override", "Remote override room setpoint (TrOverride)", true },
  { 10, "TSP count", "Number of Transparent-Slave-Parameters supported by slave", false },
  { 11, "TSP entry", "Index / value of referred-to transparent slave parameter", false },
  { 12, "FHB size", "Size of Fault-History-Buffer supported by slave", false },
  { 13, "FHB entry", "Index / value of referred-to fault-history buffer entry", false },
  { 14, "Max modulation setting", "Maximum relative modulation level setting (%)", false },
  { 15, "Max capacity / min modulation", "Maximum boiler capacity (kW) / min modulation (%)", false },
  { 16, "Room setpoint", "Room temperature setpoint (TrSet)", true },
  { 17, "Relative modulation", "Relative modulation level (%)", false },
  { 18, "CH pressure", "Water pressure in CH circuit (bar)", false },
  { 19, "DHW flow rate", "Water flow rate in DHW circuit (L/min)", false },
  { 20, "Day/time", "Day of week and time of day", false },
  { 21, "Date", "Calendar date (month/day)", false },
  { 22, "Year", "Calendar year", false },
  { 23, "Room setpoint CH2", "Room setpoint for 2nd CH circuit", true },
  { 24, "Room temperature", "Measured room temperature (Tr)", true },
  { 25, "Boiler flow temperature", "Boiler flow water temperature (Tboiler)", true },
  { 26, "DHW temperature", "Domestic hot water temperature (Tdhw)", true },
  { 27, "Outside temperature", "Outside temperature (Toutside)", true },
  { 28, "Return temperature", "Return water temperature (Tret)", true },
  { 29, "Solar storage temp", "Solar storage temperature (Tstorage)", true },
  { 30, "Solar collector temp", "Solar collector temperature (Tcollector)", true },
  { 31, "Flow temp CH2", "Flow water temperature in CH2 circuit", true },
  { 32, "DHW temperature 2", "Domestic hot water temperature 2 (Tdhw2)", true },
  { 33, "Exhaust temperature", "Boiler exhaust temperature (Texhaust)", true },
  { 34, "Heat exchanger temp", "Boiler heat exchanger temperature", true },
  { 35, "Fan speed set/actual", "Boiler fan speed setpoint and actual value", false },
  { 36, "Flame current", "Electrical current through burner flame (uA)", false },
  { 37, "Room temperature CH2", "Measured room temperature for 2nd CH circuit", true },
  { 38, "Relative humidity", "Actual relative humidity (%)", false },
  { 39, "Room setpoint override 2", "Remote override room setpoint 2", true },

  // 48..58: bounds + remote parameters + OTC (Outside Temperature Compensation)
  { 48, "DHW setpoint bounds", "Upper/lower bounds for DHW setpoint adjustment (s8)", false },
  { 49, "Max CH setpoint bounds", "Upper/lower bounds for max CH setpoint adjustment (s8)", false },
  { 50, "OTC ratio bounds", "Upper/lower bounds for OTC heatcurve ratio (s8)", false },
  { 56, "DHW setpoint", "DHW setpoint (remote parameter 1)", true },
  { 57, "Max CH setpoint", "Maximum CH water setpoint (remote parameter 2)", true },
  { 58, "OTC heatcurve ratio", "Outside temperature compensation heatcurve ratio", false },

  // 70..91: ventilation/heat-recovery (rare)
  { 70, "Status V/H", "Ventilation/heat-recovery status flags", false },
  { 71, "V/H setpoint", "Relative ventilation position (0..100)", false },
  { 72, "Fault V/H", "Ventilation/heat-recovery fault flags + OEM code", false },
  { 73, "Diagnostic V/H", "Ventilation/heat-recovery OEM diagnostic code", false },
  { 74, "Config+MemberID V/H", "Ventilation/heat-recovery config flags + MemberID", false },
  { 75, "OT version V/H", "OpenTherm protocol version (ventilation)", false },
  { 76, "Version+type V/H", "Ventilation product version number and type", false },
  { 77, "Relative ventilation", "Relative ventilation level (0..100)", false },
  { 78, "Humidity exhaust", "Relative humidity exhaust air (0..100)", false },
  { 79, "CO2 exhaust", "CO2 level exhaust air (ppm)", false },
  { 80, "Supply inlet temp", "Supply inlet temperature (Tsi)", true },
  { 81, "Supply outlet temp", "Supply outlet temperature (Tso)", true },
  { 82, "Exhaust inlet temp", "Exhaust inlet temperature (Tei)", true },
  { 83, "Exhaust outlet temp", "Exhaust outlet temperature (Teo)", true },
  { 84, "RPM exhaust", "Exhaust fan speed (rpm)", false },
  { 85, "RPM supply", "Supply fan speed (rpm)", false },
  { 86, "RBP flags V/H", "Remote ventilation parameter transfer-enable & read/write flags", false },
  { 87, "Nominal ventilation", "Nominal relative ventilation value", false },
  { 88, "TSP count V/H", "Number of TSP parameters (ventilation)", false },
  { 89, "TSP entry V/H", "Index/value of ventilation TSP parameter", false },
  { 90, "FHB size V/H", "Fault history buffer size (ventilation)", false },
  { 91, "FHB entry V/H", "Fault history buffer entry (ventilation)", false },

  // 93..95: brand strings (rare)
  { 93, "Brand char", "Index/character in brand string", false },
  { 94, "Brand version char", "Index/character in brand version string", false },
  { 95, "Serial char", "Index/character in brand serial number string", false },

  // 96..127: counters + protocol/product info (often supported on modern boilers)
  { 96, "Cooling hours", "Hours in cooling mode", false },
  { 97, "Power cycles", "Number of power cycles (wake-up after reset)", false },
  { 98, "RF sensor status", "RF signal strength and battery level (special)", false },
  { 99, "Operating mode", "Operating mode HC1/HC2 and DHW (special)", false },
  {100, "Remote override function", "Function of manual/program changes in master/remote room setpoint", false },

  {109, "Elec prod starts", "Electricity producer starts", false },
  {110, "Elec prod hours", "Electricity producer hours", false },
  {111, "Elec production", "Current electricity production (W)", false },
  {112, "Elec production cum", "Cumulative electricity production (kWh)", false },
  {113, "Unsuccessful starts", "Number of unsuccessful burner starts", false },
  {114, "Flame low count", "Number of times flame signal was too low", false },
  {115, "OEM diagnostic code", "OEM-specific diagnostic/service code", false },
  {116, "Burner starts", "Number of successful burner starts", false },
  {117, "CH pump starts", "Number of starts CH pump", false },
  {118, "DHW pump/valve starts", "Number of starts DHW pump/valve", false },
  {119, "DHW burner starts", "Number of starts burner during DHW mode", false },
  {120, "Burner hours", "Hours burner is in operation (flame on)", false },
  {121, "CH pump hours", "Hours CH pump has been running", false },
  {122, "DHW pump/valve hours", "Hours DHW pump has been running / DHW valve opened", false },
  {123, "DHW burner hours", "Hours burner is in operation during DHW mode", false },
  {124, "OT version (master)", "OpenTherm protocol version implemented in master", false },
  {125, "OT version (slave)", "OpenTherm protocol version implemented in slave", false },
  {126, "Master product version", "Master product version number and type", false },
  {127, "Slave product version", "Slave product version number and type", false },
};

inline bool openthermLookupDataIdMeta(uint8_t id, OpenThermDataIdMeta& out) {
  // Known IDs
  for (size_t i = 0; i < (sizeof(kOpenThermDataIdMeta) / sizeof(kOpenThermDataIdMeta[0])); i++) {
    OpenThermDataIdMeta m;
    memcpy_P(&m, &kOpenThermDataIdMeta[i], sizeof(OpenThermDataIdMeta));
    if (m.id == id) { out = m; return true; }
  }
  // Fallback for unknown IDs (caller can format "IDxx")
  out.id = id;
  out.name = nullptr;
  out.desc = "Unknown/undocumented Data-ID (boiler dependent)";
  out.isTemperature = false;
  return true;
}

