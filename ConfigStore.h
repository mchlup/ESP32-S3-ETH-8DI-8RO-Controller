#pragma once

#include <Arduino.h>

// Very small config store (Preferences).
// Used for:
//  - input polarities (active LOW/HIGH)
//  - minimal persistent switches for OpenTherm/BLE/DS + a few basic parameters

namespace ConfigStore {
  void begin();
  void beginBatch();
  void endBatch();

  class BatchGuard {
   public:
    BatchGuard() { beginBatch(); }
    ~BatchGuard() { endBatch(); }
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
  };

  // Inputs
  uint8_t getInputActiveLevel(uint8_t inputIndex); // 0=active LOW, 1=active HIGH
  void setInputActiveLevels(const uint8_t* levels, uint8_t count);

  // OpenTherm (subset)
  bool getOtEnabled();
  void setOtEnabled(bool v);
  bool getOtAutoStart();
  void setOtAutoStart(bool v);
  uint32_t getOtPollMs();
  void setOtPollMs(uint32_t v);
  uint32_t getOtBootDelayMs();
  void setOtBootDelayMs(uint32_t v);

  // OpenTherm mode persistence ("readOnly" / "control")
  String getOtMode();
  void setOtMode(const String& v);

  // Advanced OT: allow raw Data-ID writes from web UI/API (dangerous).
  // Default false.
  bool getOtAllowRawWrite();
  void setOtAllowRawWrite(bool v);

  // BLE (subset)
  bool getBleEnabled();
  void setBleEnabled(bool v);
  String getBleNamePrefix();
  void setBleNamePrefix(const String& v);
  uint32_t getBleScanIntervalMs();
  void setBleScanIntervalMs(uint32_t v);

  // DS18B20 (GPIO3 tank)
  bool getDallasEnabled();
  void setDallasEnabled(bool v);

  // DS18B20 role mapping (ROM=0 => AUTO)
  uint64_t getDallasTankTopRom();
  void setDallasTankTopRom(uint64_t rom);
  uint64_t getDallasTankMidRom();
  void setDallasTankMidRom(uint64_t rom);
  uint64_t getDallasTankBottomRom();
  void setDallasTankBottomRom(uint64_t rom);

  uint64_t getDallasReturnRom();
  void setDallasReturnRom(uint64_t rom);
  uint64_t getDallasDhwReturnRom();
  void setDallasDhwReturnRom(uint64_t rom);
  uint64_t getDallasDhwTankRom();
  void setDallasDhwTankRom(uint64_t rom);

  // Optional DS18B20 outside temperature (GPIO0 / DALLAS_IO0_PIN)
  uint64_t getDallasOutsideRom();
  void setDallasOutsideRom(uint64_t rom);

  // Time (SNTP)
  bool getTimeEnabled();
  void setTimeEnabled(bool v);
  String getTimeTz();
  void setTimeTz(const String& v);
  String getTimeNtp1();
  String getTimeNtp2();
  String getTimeNtp3();
  void setTimeNtp1(const String& v);
  void setTimeNtp2(const String& v);
  void setTimeNtp3(const String& v);

  // Ekviterm (equitherm)
  bool getEqEnabled();
  void setEqEnabled(bool v);
  String getEqMode(); // "auto" / "day" / "night"
  void setEqMode(const String& v);
  bool getEqUseIn1NightOverride();
  void setEqUseIn1NightOverride(bool v);
  bool getEqSummerModeEnabled();
  void setEqSummerModeEnabled(bool v);
  float getEqSummerOffAboveC();
  void setEqSummerOffAboveC(float v);
  float getEqSummerOnBelowC();
  void setEqSummerOnBelowC(float v);

  // Week schedule legacy API: one day interval per day (Mon..Sun).
  void getEqSchedule(uint16_t dayStartMin[7], uint16_t nightStartMin[7]);
  void setEqSchedule(const uint16_t dayStartMin[7], const uint16_t nightStartMin[7]);

  // Extended week schedule: up to 6 heating intervals per day.
  void getEqScheduleIntervals(uint8_t counts[7], uint16_t starts[7][6], uint16_t ends[7][6]);
  void setEqScheduleIntervals(const uint8_t counts[7], const uint16_t starts[7][6], const uint16_t ends[7][6]);
  bool getEqScheduleEnabled();
  void setEqScheduleEnabled(bool v);

  // Curves: two-point linear mapping Tout -> Tflow
  float getEqDayOutColdC();
  float getEqDayFlowColdC();
  float getEqDayOutWarmC();
  float getEqDayFlowWarmC();
  void setEqDayCurve(float outColdC, float flowColdC, float outWarmC, float flowWarmC);

  float getEqNightOutColdC();
  float getEqNightFlowColdC();
  float getEqNightOutWarmC();
  float getEqNightFlowWarmC();
  void setEqNightCurve(float outColdC, float flowColdC, float outWarmC, float flowWarmC);

  // Limits + sending behavior
  float getEqMinFlowC();
  float getEqMaxFlowC();
  void setEqFlowLimits(float minFlowC, float maxFlowC);

  float getEqMinChSetpointC();
  float getEqMaxChSetpointC();
  void setEqChSetpointLimits(float minC, float maxC);

  uint32_t getEqTempMaxAgeMs();
  void setEqTempMaxAgeMs(uint32_t v);
  uint32_t getEqMinSendIntervalMs();
  void setEqMinSendIntervalMs(uint32_t v);
  float getEqMinSendDeltaC();
  void setEqMinSendDeltaC(float v);

  // Output mapping
  bool getEqUseOpenTherm();
  void setEqUseOpenTherm(bool v);
  bool getEqApplyBoilerMaxCh();
  void setEqApplyBoilerMaxCh(bool v);
  float getEqBoilerMaxChC();
  void setEqBoilerMaxChC(float v);

  bool getEqDriveNightRelay();
  void setEqDriveNightRelay(bool v);
  uint8_t getEqNightRelayIndex(); // 0..7 (R1..R8)
  void setEqNightRelayIndex(uint8_t idx);
  bool getEqNightRelayOnWhenNight();
  void setEqNightRelayOnWhenNight(bool v);

  // Mixing valve mapping (R1/R2)
  bool getEqMixingEnabled();
  void setEqMixingEnabled(bool v);
  uint8_t getEqMixOpenRelayIndex(); // 0..7
  void setEqMixOpenRelayIndex(uint8_t idx);
  uint8_t getEqMixCloseRelayIndex(); // 0..7
  void setEqMixCloseRelayIndex(uint8_t idx);
  float getEqMixDeadbandC();
  void setEqMixDeadbandC(float v);
  float getEqMixTargetOffsetC();
  void setEqMixTargetOffsetC(float v);
  // Behaviour after the accumulator-support target is reached:
  // "return_a" = move to end position A, "hold" = keep current position.
  String getEqMixTargetReachedAction();
  void setEqMixTargetReachedAction(const String& v);
  uint32_t getEqMixPulseMs();
  void setEqMixPulseMs(uint32_t v);
  uint32_t getEqMixMinIntervalMs();
  void setEqMixMinIntervalMs(uint32_t v);

  uint32_t getEqMixTravelMs();
  void setEqMixTravelMs(uint32_t v);
  uint32_t getEqMixCalibrationSeatMs();
  void setEqMixCalibrationSeatMs(uint32_t v);
  uint32_t getEqMixAutoRecalibrationMs();
  void setEqMixAutoRecalibrationMs(uint32_t v);

  // Temperature sources assigned to the hydraulic ports of the mixing valve.
  // Valid persisted values are port-specific:
  // A=tank_mid, B=return_dallas, AB=opentherm_ch (or none).
  String getEqMixTempSourceA();
  void setEqMixTempSourceA(const String& v);
  String getEqMixTempSourceB();
  void setEqMixTempSourceB(const String& v);
  String getEqMixTempSourceAB();
  void setEqMixTempSourceAB(const String& v);

  // Accumulator support for boiler heating. The legacy deltaC value is kept
  // for configuration compatibility but is no longer added to the OT setpoint.
  bool getEqBoilerAssistEnabled();
  void setEqBoilerAssistEnabled(bool v);
  float getEqBoilerAssistDeltaC();
  void setEqBoilerAssistDeltaC(float v);
  bool getEqBoilerAssistForceChEnable();
  void setEqBoilerAssistForceChEnable(bool v);


  // DHW / TUV
  bool getDhwEnabled();
  void setDhwEnabled(bool v);
  bool getDhwDisableEquithermDuringHeat();
  void setDhwDisableEquithermDuringHeat(bool v);
  uint32_t getDhwTempMaxAgeMs();
  void setDhwTempMaxAgeMs(uint32_t v);

  bool getDhwHeatUseInput();
  void setDhwHeatUseInput(bool v);
  bool getDhwHeatUseSchedule();
  void setDhwHeatUseSchedule(bool v);
  bool getDhwHeatScheduleEnabled();
  void setDhwHeatScheduleEnabled(bool v);
  float getDhwHeatTargetTempC();
  void setDhwHeatTargetTempC(float v);
  float getDhwHeatHysteresisC();
  void setDhwHeatHysteresisC(float v);
  String getDhwHeatRequestMode();
  void setDhwHeatRequestMode(const String& v);
  bool getDhwHeatOtEnableDhw();
  void setDhwHeatOtEnableDhw(bool v);
  float getDhwHeatOtDhwSetpointC();
  void setDhwHeatOtDhwSetpointC(float v);
  bool getDhwHeatRelayRequest();
  void setDhwHeatRelayRequest(bool v);
  bool getDhwHeatDriveValveRelay();
  void setDhwHeatDriveValveRelay(bool v);
  uint8_t getDhwHeatValveRelayIndex();
  void setDhwHeatValveRelayIndex(uint8_t idx);
  uint8_t getDhwHeatBoilerRelayIndex();
  void setDhwHeatBoilerRelayIndex(uint8_t idx);
  uint32_t getDhwHeatValveLeadMs();
  void setDhwHeatValveLeadMs(uint32_t v);
  uint32_t getDhwHeatValveSwitchBackMs();
  void setDhwHeatValveSwitchBackMs(uint32_t v);
  uint32_t getDhwHeatBoilerOffHoldMs();
  void setDhwHeatBoilerOffHoldMs(uint32_t v);
  String getDhwHeatScheduleJson();
  void setDhwHeatScheduleJson(const String& v);

  bool getDhwCircUseInput();
  void setDhwCircUseInput(bool v);
  bool getDhwCircUseSchedule();
  void setDhwCircUseSchedule(bool v);
  bool getDhwCircScheduleEnabled();
  void setDhwCircScheduleEnabled(bool v);
  bool getDhwCircPulseEnabled();
  void setDhwCircPulseEnabled(bool v);
  uint16_t getDhwCircPulseOnMin();
  void setDhwCircPulseOnMin(uint16_t v);
  uint16_t getDhwCircPulseOffMin();
  void setDhwCircPulseOffMin(uint16_t v);
  uint8_t getDhwCircRelayIndex();
  void setDhwCircRelayIndex(uint8_t idx);
  String getDhwCircScheduleJson();
  void setDhwCircScheduleJson(const String& v);
  bool getDhwAntiLegionellaEnabled();
  void setDhwAntiLegionellaEnabled(bool v);
  uint8_t getDhwAntiLegionellaWeekday();
  void setDhwAntiLegionellaWeekday(uint8_t v);
  uint16_t getDhwAntiLegionellaStartMin();
  void setDhwAntiLegionellaStartMin(uint16_t v);
  float getDhwAntiLegionellaTargetTempC();
  void setDhwAntiLegionellaTargetTempC(float v);
  uint16_t getDhwAntiLegionellaHoldMin();
  void setDhwAntiLegionellaHoldMin(uint16_t v);
  uint32_t getDhwAntiLegionellaLastDayKey();
  void setDhwAntiLegionellaLastDayKey(uint32_t v);

  // Arduino IDE OTA (network upload)
  bool getOtaEnabled();
  void setOtaEnabled(bool v);
  String getOtaHostname();
  void setOtaHostname(const String& v);
  uint16_t getOtaPort();
  void setOtaPort(uint16_t v);
  String getOtaPassword();
  void setOtaPassword(const String& v); // empty clears

  // MQTT + Home Assistant integration (config persistence + UI preview)
  bool getMqttEnabled();
  void setMqttEnabled(bool v);
  String getMqttHost();
  void setMqttHost(const String& v);
  uint16_t getMqttPort();
  void setMqttPort(uint16_t v);
  String getMqttUsername();
  void setMqttUsername(const String& v);
  String getMqttPassword();
  void setMqttPassword(const String& v); // empty clears
  String getMqttClientId();
  void setMqttClientId(const String& v);
  String getMqttBaseTopic();
  void setMqttBaseTopic(const String& v);
  uint32_t getMqttPublishIntervalMs();
  void setMqttPublishIntervalMs(uint32_t v);
  bool getMqttHaEnabled();
  void setMqttHaEnabled(bool v);
  bool getMqttHaDiscovery();
  void setMqttHaDiscovery(bool v);
  String getMqttDiscoveryPrefix();
  void setMqttDiscoveryPrefix(const String& v);
  String getMqttNodeId();
  void setMqttNodeId(const String& v);

  bool getPressureAlarmEnabled();
  void setPressureAlarmEnabled(bool v);
  float getPressureAlarmMinBar();
  void setPressureAlarmMinBar(float v);
  float getPressureAlarmMaxBar();
  void setPressureAlarmMaxBar(float v);
  float getPressureAlarmHysteresisBar();
  void setPressureAlarmHysteresisBar(float v);
}
