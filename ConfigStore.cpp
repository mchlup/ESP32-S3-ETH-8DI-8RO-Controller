#include "ConfigStore.h"

#include <Preferences.h>

namespace {
  Preferences g_prefs;
  bool g_inited = false;
  uint16_t g_batchDepth = 0;
  bool g_writeSessionOpen = false;

  // Defaults
  uint8_t  g_inLevels[8] = {0,0,0,0,0,0,0,0}; // default active-low

  bool     g_otEnabled = false;
  bool     g_otAutoStart = false;
  uint32_t g_otPollMs = 2000;
  uint32_t g_otBootDelayMs = 15000;
  String   g_otMode = "readOnly";
  bool     g_otAllowRawWrite = false;

  bool     g_bleEnabled = false;
  String   g_bleNamePrefix = "ESP-Meteostanice";
  uint32_t g_bleScanMs = 10000;

  bool     g_dallasEnabled = true;

  // DHW / TUV
  bool     g_dhwEnabled = true;
  bool     g_dhwDisableEq = true;
  uint32_t g_dhwTempMaxAgeMs = 900000;

  bool     g_dhwHeatUseInput = true;
  bool     g_dhwHeatUseSchedule = true;
  bool     g_dhwHeatScheduleEnabled = true;
  float    g_dhwHeatTargetTempC = 50.0f;
  float    g_dhwHeatHysteresisC = 2.0f;
  String   g_dhwHeatRequestMode = "relay";
  bool     g_dhwHeatOtEnableDhw = true;
  float    g_dhwHeatOtDhwSetpointC = 50.0f;
  bool     g_dhwHeatRelayRequest = true;
  bool     g_dhwHeatDriveValveRelay = true;
  uint8_t  g_dhwHeatValveRelayIndex = 2;
  uint8_t  g_dhwHeatBoilerRelayIndex = 4;
  uint32_t g_dhwHeatValveLeadMs = 3000;
  uint32_t g_dhwHeatValveSwitchBackMs = 1500;
  uint32_t g_dhwHeatBoilerOffHoldMs = 2000;
  String   g_dhwHeatScheduleJson = "[[],[],[],[],[],[],[]]";

  bool     g_dhwCircUseInput = true;
  bool     g_dhwCircUseSchedule = true;
  bool     g_dhwCircScheduleEnabled = true;
  bool     g_dhwCircPulseEnabled = true;
  uint32_t g_dhwCircPulseOnMin = 5;
  uint32_t g_dhwCircPulseOffMin = 15;
  uint8_t  g_dhwCircRelayIndex = 3;
  String   g_dhwCircScheduleJson = "[[],[],[],[],[],[],[]]";
  bool     g_dhwAlEnabled = false;
  uint32_t g_dhwAlWeekday = 0;
  uint32_t g_dhwAlStartMin = 120;
  float    g_dhwAlTargetTempC = 60.0f;
  uint32_t g_dhwAlHoldMin = 30;
  uint32_t g_dhwAlLastDayKey = 0;

  // Arduino IDE OTA
  bool     g_otaEnabled = true;
  String   g_otaHostname = ""; // empty => auto (from MAC)
  uint32_t g_otaPort = 3232;
  String   g_otaPassword = ""; // optional

  // MQTT + Home Assistant integration (config only in this build)
  bool     g_mqttEnabled = false;
  String   g_mqttHost = "";
  uint32_t g_mqttPort = 1883;
  String   g_mqttUsername = "";
  String   g_mqttPassword = "";
  String   g_mqttClientId = "esp32-controller";
  String   g_mqttBaseTopic = "esp32-controller";
  uint32_t g_mqttPublishIntervalMs = 10000;
  bool     g_mqttHaEnabled = true;
  bool     g_mqttHaDiscovery = true;
  String   g_mqttDiscoveryPrefix = "homeassistant";
  String   g_mqttNodeId = "esp32_controller";
  bool     g_pressureAlarmEnabled = true;
  float    g_pressureAlarmMinBar = 0.8f;
  float    g_pressureAlarmMaxBar = 2.8f;
  float    g_pressureAlarmHysteresisBar = 0.05f;

  // Dallas role ROM mapping (ROM=0 => AUTO)
  uint64_t g_dsTankTopRom = 0;
  uint64_t g_dsTankMidRom = 0;
  uint64_t g_dsTankBottomRom = 0;
  uint64_t g_dsReturnRom = 0;
  uint64_t g_dsDhwReturnRom = 0;
  uint64_t g_dsDhwTankRom = 0;
  uint64_t g_dsOutsideRom = 0; // GPIO0

  // Time (SNTP)
  bool   g_timeEnabled = true;
  String g_timeTz = "CET-1CEST,M3.5.0,M10.5.0/3";
  String g_timeNtp1 = "pool.ntp.org";
  String g_timeNtp2 = "europe.pool.ntp.org";
  String g_timeNtp3 = "time.nist.gov";

  // Ekviterm
  bool   g_eqEnabled = false;
  String g_eqMode = "auto"; // auto/day/night
  bool   g_eqUseIn1NightOverride = true;
  bool   g_eqSummerModeEnabled = false;
  float  g_eqSummerOffAboveC = 18.0f;
  float  g_eqSummerOnBelowC = 16.0f;
  bool   g_eqScheduleEnabled = false;
  // Week schedule: Mon..Sun
  uint16_t g_eqDayStartMin[7]   = { 360,360,360,360,360,480,480 }; // legacy first interval start
  uint16_t g_eqNightStartMin[7] = { 1320,1320,1320,1320,1320,1320,1320 }; // legacy first interval end
  uint8_t  g_eqIntervalCount[7] = { 1,1,1,1,1,1,1 };
  uint16_t g_eqIntervalsStart[7][6] = {
    {360,0,0,0,0,0},{360,0,0,0,0,0},{360,0,0,0,0,0},{360,0,0,0,0,0},{360,0,0,0,0,0},{480,0,0,0,0,0},{480,0,0,0,0,0}
  };
  uint16_t g_eqIntervalsEnd[7][6] = {
    {1320,0,0,0,0,0},{1320,0,0,0,0,0},{1320,0,0,0,0,0},{1320,0,0,0,0,0},{1320,0,0,0,0,0},{1320,0,0,0,0,0},{1320,0,0,0,0,0}
  };

  // Curves (two-point)
  float g_eqDayOutColdC  = -12.0f;
  float g_eqDayFlowColdC = 55.0f;
  float g_eqDayOutWarmC  = 20.0f;
  float g_eqDayFlowWarmC = 25.0f;

  float g_eqNightOutColdC  = -12.0f;
  float g_eqNightFlowColdC = 50.0f;
  float g_eqNightOutWarmC  = 20.0f;
  float g_eqNightFlowWarmC = 22.0f;

  float g_eqMinFlowC = 22.0f;
  float g_eqMaxFlowC = 60.0f;
  float g_eqMinChSetpointC = 22.0f;
  float g_eqMaxChSetpointC = 60.0f;

  uint32_t g_eqTempMaxAgeMs = 600000;
  uint32_t g_eqMinSendIntervalMs = 60000;
  float    g_eqMinSendDeltaC = 0.5f;

  bool   g_eqUseOpenTherm = true;
  bool   g_eqApplyBoilerMaxCh = false;
  float  g_eqBoilerMaxChC = 60.0f;
  bool   g_eqDriveNightRelay = true;
  uint8_t g_eqNightRelayIndex = 5; // R6 (0-based)
  bool   g_eqNightRelayOnWhenNight = true;

  // Mixing valve
  bool   g_eqMixingEnabled = true;
  uint8_t g_eqMixOpenRelayIndex = 0; // R1
  uint8_t g_eqMixCloseRelayIndex = 1; // R2
  float  g_eqMixDeadbandC = 0.5f;
  float  g_eqMixTargetOffsetC = 0.0f;
  String g_eqMixTargetReachedAction = "return_a";
  uint32_t g_eqMixPulseMs = 300;
  uint32_t g_eqMixMinIntervalMs = 30000;
  uint32_t g_eqMixTravelMs = 6000;
  uint32_t g_eqMixCalibrationSeatMs = 1500;
  uint32_t g_eqMixAutoRecalibrationMs = 21600000UL;
  // Hydraulic temperature mapping for the three-way mixing valve.
  // A  = hot supply from the middle of the accumulation tank (DS18B20 role tank_mid)
  // B  = return/cold branch from the dedicated DS18B20 Return role on GPIO2
  // AB = mixed output measured by the boiler over OpenTherm (CH measured temperature)
  String g_eqMixTempSourceA = "tank_mid";
  String g_eqMixTempSourceB = "return_dallas";
  String g_eqMixTempSourceAB = "opentherm_ch";

  // Boiler assist headroom
  bool   g_eqBoilerAssistEnabled = true;
  float  g_eqBoilerAssistDeltaC = 5.0f;
  bool   g_eqBoilerAssistForceChEnable = true;

  static constexpr const char* NS = "cfg";
  static constexpr const char* K_INLVL = "in_lvl";

  static constexpr const char* K_OT_EN = "ot_en";
  static constexpr const char* K_OT_AS = "ot_as";
  static constexpr const char* K_OT_POLL = "ot_poll";
  static constexpr const char* K_OT_BOOT = "ot_boot";
  static constexpr const char* K_OT_MODE = "ot_mode";
  static constexpr const char* K_OT_RAWW = "ot_raww";

  static constexpr const char* K_BLE_EN = "ble_en";
  static constexpr const char* K_BLE_NAME = "ble_name";
  static constexpr const char* K_BLE_SCAN = "ble_scan";

  static constexpr const char* K_DS_EN = "ds_en";

  // Dallas role ROM mapping (split to hi/lo 32-bit for compatibility)
  static constexpr const char* K_DS_TTOP_H = "ds_tt_h";
  static constexpr const char* K_DS_TTOP_L = "ds_tt_l";
  static constexpr const char* K_DS_TMID_H = "ds_tm_h";
  static constexpr const char* K_DS_TMID_L = "ds_tm_l";
  static constexpr const char* K_DS_TBOT_H = "ds_tb_h";
  static constexpr const char* K_DS_TBOT_L = "ds_tb_l";

  static constexpr const char* K_DS_RET_H  = "ds_r_h";
  static constexpr const char* K_DS_RET_L  = "ds_r_l";
  static constexpr const char* K_DS_DHWR_H = "ds_dr_h";
  static constexpr const char* K_DS_DHWR_L = "ds_dr_l";
  static constexpr const char* K_DS_DHWT_H = "ds_dt_h";
  static constexpr const char* K_DS_DHWT_L = "ds_dt_l";

  static constexpr const char* K_DS_OUT_H = "ds_o_h";
  static constexpr const char* K_DS_OUT_L = "ds_o_l";

  // Time
  static constexpr const char* K_TIME_EN = "t_en";
  static constexpr const char* K_TIME_TZ = "t_tz";
  static constexpr const char* K_TIME_N1 = "t_n1";
  static constexpr const char* K_TIME_N2 = "t_n2";
  static constexpr const char* K_TIME_N3 = "t_n3";

  // Ekviterm
  static constexpr const char* K_EQ_EN = "eq_en";
  static constexpr const char* K_EQ_MODE = "eq_mode";
  static constexpr const char* K_EQ_IN1 = "eq_in1";
  static constexpr const char* K_EQ_SUM_EN = "eq_s_en";
  static constexpr const char* K_EQ_SUM_OFF = "eq_s_off";
  static constexpr const char* K_EQ_SUM_ON = "eq_s_on";
  static constexpr const char* K_EQ_SCHED_EN = "eq_sc_en";
  static constexpr const char* K_EQ_SCHED = "eq_sched"; // bytes: 7x u16 day + 7x u16 night
  static constexpr const char* K_EQ_SCHED_V2 = "eq_sc_v2"; // bytes: count + 6*(start,end) for each of 7 days

  static constexpr const char* K_EQ_D_OC = "eq_d_oc";
  static constexpr const char* K_EQ_D_FC = "eq_d_fc";
  static constexpr const char* K_EQ_D_OW = "eq_d_ow";
  static constexpr const char* K_EQ_D_FW = "eq_d_fw";

  static constexpr const char* K_EQ_N_OC = "eq_n_oc";
  static constexpr const char* K_EQ_N_FC = "eq_n_fc";
  static constexpr const char* K_EQ_N_OW = "eq_n_ow";
  static constexpr const char* K_EQ_N_FW = "eq_n_fw";

  static constexpr const char* K_EQ_MINF = "eq_minf";
  static constexpr const char* K_EQ_MAXF = "eq_maxf";
  static constexpr const char* K_EQ_MINC = "eq_minc";
  static constexpr const char* K_EQ_MAXC = "eq_maxc";

  static constexpr const char* K_EQ_TAGE = "eq_tage";
  static constexpr const char* K_EQ_MININT = "eq_minint";
  static constexpr const char* K_EQ_MINDEL = "eq_mindel";

  static constexpr const char* K_EQ_USEOT = "eq_useot";
  static constexpr const char* K_EQ_APPMAX = "eq_appmx";
  static constexpr const char* K_EQ_BMAX = "eq_bmax";
  static constexpr const char* K_EQ_NRE = "eq_nre";
  static constexpr const char* K_EQ_NRIDX = "eq_nidx";
  static constexpr const char* K_EQ_NRON = "eq_nron";
static constexpr const char* K_EQ_MIX_EN = "eq_mx_en";
static constexpr const char* K_EQ_MIX_O = "eq_mx_o";
static constexpr const char* K_EQ_MIX_C = "eq_mx_c";
static constexpr const char* K_EQ_MIX_DB = "eq_mx_db";
static constexpr const char* K_EQ_MIX_TO = "eq_mx_to";
static constexpr const char* K_EQ_MIX_DONE = "eq_mx_dn";
static constexpr const char* K_EQ_MIX_P = "eq_mx_p";
static constexpr const char* K_EQ_MIX_MI = "eq_mx_mi";
static constexpr const char* K_EQ_MIX_T = "eq_mx_t";
static constexpr const char* K_EQ_MIX_SEAT = "eq_mx_se";
static constexpr const char* K_EQ_MIX_RECAL = "eq_mx_rc";
static constexpr const char* K_EQ_MIX_SRC_A = "eq_mx_sa";
static constexpr const char* K_EQ_MIX_SRC_B = "eq_mx_sb";
static constexpr const char* K_EQ_MIX_SRC_AB = "eq_mx_sab";

static constexpr const char* K_EQ_BA_EN = "eq_ba_en";
static constexpr const char* K_EQ_BA_D = "eq_ba_d";
static constexpr const char* K_EQ_BA_CH = "eq_ba_ch";

  // DHW / TUV
  static constexpr const char* K_DHW_EN = "dhw_en";
  static constexpr const char* K_DHW_DEQ = "dhw_deq";
  static constexpr const char* K_DHW_TAGE = "dhw_tage";
  static constexpr const char* K_DHW_H_IN = "dhw_h_in";
  static constexpr const char* K_DHW_H_SC = "dhw_h_sc";
  static constexpr const char* K_DHW_H_SEN = "dhw_h_se";
  static constexpr const char* K_DHW_H_TG = "dhw_h_tg";
  static constexpr const char* K_DHW_H_HY = "dhw_h_hy";
  static constexpr const char* K_DHW_H_RM = "dhw_h_rm";
  static constexpr const char* K_DHW_H_ODE = "dhw_h_ode";
  static constexpr const char* K_DHW_H_ODS = "dhw_h_ods";
  static constexpr const char* K_DHW_H_RRQ = "dhw_h_rrq";
  static constexpr const char* K_DHW_H_DVR = "dhw_h_dvr";
  static constexpr const char* K_DHW_H_VR = "dhw_h_vr";
  static constexpr const char* K_DHW_H_BR = "dhw_h_br";
  static constexpr const char* K_DHW_H_VLD = "dhw_h_vld";
  static constexpr const char* K_DHW_H_VBK = "dhw_h_vbk";
  static constexpr const char* K_DHW_H_BOH = "dhw_h_boh";
  static constexpr const char* K_DHW_H_SJ = "dhw_h_sj";
  static constexpr const char* K_DHW_C_IN = "dhw_c_in";
  static constexpr const char* K_DHW_C_SC = "dhw_c_sc";
  static constexpr const char* K_DHW_C_SEN = "dhw_c_se";
  static constexpr const char* K_DHW_C_PE = "dhw_c_pe";
  static constexpr const char* K_DHW_C_PON = "dhw_c_po";
  static constexpr const char* K_DHW_C_POF = "dhw_c_pf";
  static constexpr const char* K_DHW_C_RI = "dhw_c_ri";
  static constexpr const char* K_DHW_C_SJ = "dhw_c_sj";
  static constexpr const char* K_DHW_AL_EN = "dhw_al_en";
  static constexpr const char* K_DHW_AL_WD = "dhw_al_wd";
  static constexpr const char* K_DHW_AL_SM = "dhw_al_sm";
  static constexpr const char* K_DHW_AL_TG = "dhw_al_tg";
  static constexpr const char* K_DHW_AL_HM = "dhw_al_hm";
  static constexpr const char* K_DHW_AL_DK = "dhw_al_dk";

  // OTA
  static constexpr const char* K_OTA_EN   = "ota_en";
  static constexpr const char* K_OTA_HOST = "ota_host";
  static constexpr const char* K_OTA_PORT = "ota_port";
  static constexpr const char* K_OTA_PASS = "ota_pass";

  // MQTT
  static constexpr const char* K_MQ_EN   = "mq_en";
  static constexpr const char* K_MQ_HOST = "mq_host";
  static constexpr const char* K_MQ_PORT = "mq_port";
  static constexpr const char* K_MQ_USER = "mq_user";
  static constexpr const char* K_MQ_PASS = "mq_pass";
  static constexpr const char* K_MQ_CID  = "mq_cid";
  static constexpr const char* K_MQ_BASE = "mq_base";
  static constexpr const char* K_MQ_PMS  = "mq_pms";
  static constexpr const char* K_MQ_HAEN = "mq_ha_en";
  static constexpr const char* K_MQ_DISC = "mq_disc";
  static constexpr const char* K_MQ_DPRE = "mq_dpre";
  static constexpr const char* K_MQ_NODE = "mq_node";

  // Pressure alarm
  static constexpr const char* K_PAL_EN = "pal_en";
  static constexpr const char* K_PAL_MIN = "pal_min";
  static constexpr const char* K_PAL_MAX = "pal_max";
  static constexpr const char* K_PAL_HYS = "pal_hys";

  static inline uint64_t pack64(uint32_t hi, uint32_t lo) {
    return ((uint64_t)hi << 32) | (uint64_t)lo;
  }
  static inline void unpack64(uint64_t v, uint32_t& hi, uint32_t& lo) {
    hi = (uint32_t)(v >> 32);
    lo = (uint32_t)(v & 0xFFFFFFFFu);
  }

  String normalizeMixTempSourceA(String value);
  String normalizeMixTempSourceB(String value);
  String normalizeMixTempSourceAB(String value);

  void load() {
    if (!g_prefs.begin(NS, true)) return;

    size_t len = g_prefs.getBytesLength(K_INLVL);
    if (len == sizeof(g_inLevels)) {
      g_prefs.getBytes(K_INLVL, g_inLevels, sizeof(g_inLevels));
    }

    g_otEnabled     = g_prefs.getBool(K_OT_EN, g_otEnabled);
    g_otAutoStart   = g_prefs.getBool(K_OT_AS, g_otAutoStart);
    g_otPollMs      = g_prefs.getUInt(K_OT_POLL, g_otPollMs);
    g_otBootDelayMs = g_prefs.getUInt(K_OT_BOOT, g_otBootDelayMs);
    g_otMode        = g_prefs.getString(K_OT_MODE, g_otMode);
    g_otAllowRawWrite = g_prefs.getBool(K_OT_RAWW, g_otAllowRawWrite);

    g_bleEnabled    = g_prefs.getBool(K_BLE_EN, g_bleEnabled);
    g_bleNamePrefix = g_prefs.getString(K_BLE_NAME, g_bleNamePrefix);
    g_bleScanMs     = g_prefs.getUInt(K_BLE_SCAN, g_bleScanMs);

    g_dallasEnabled = g_prefs.getBool(K_DS_EN, g_dallasEnabled);

    // Dallas roles
    uint32_t hi=0, lo=0;
    hi = g_prefs.getUInt(K_DS_TTOP_H, 0); lo = g_prefs.getUInt(K_DS_TTOP_L, 0); g_dsTankTopRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_TMID_H, 0); lo = g_prefs.getUInt(K_DS_TMID_L, 0); g_dsTankMidRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_TBOT_H, 0); lo = g_prefs.getUInt(K_DS_TBOT_L, 0); g_dsTankBottomRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_RET_H, 0);  lo = g_prefs.getUInt(K_DS_RET_L, 0);  g_dsReturnRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_DHWR_H, 0); lo = g_prefs.getUInt(K_DS_DHWR_L, 0); g_dsDhwReturnRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_DHWT_H, 0); lo = g_prefs.getUInt(K_DS_DHWT_L, 0); g_dsDhwTankRom = pack64(hi, lo);
    hi = g_prefs.getUInt(K_DS_OUT_H, 0);  lo = g_prefs.getUInt(K_DS_OUT_L, 0);  g_dsOutsideRom = pack64(hi, lo);

    // Time
    g_timeEnabled = g_prefs.getBool(K_TIME_EN, g_timeEnabled);
    g_timeTz = g_prefs.getString(K_TIME_TZ, g_timeTz);
    g_timeNtp1 = g_prefs.getString(K_TIME_N1, g_timeNtp1);
    g_timeNtp2 = g_prefs.getString(K_TIME_N2, g_timeNtp2);
    g_timeNtp3 = g_prefs.getString(K_TIME_N3, g_timeNtp3);

    // Ekviterm
    g_eqEnabled = g_prefs.getBool(K_EQ_EN, g_eqEnabled);
    g_eqMode = g_prefs.getString(K_EQ_MODE, g_eqMode);
    g_eqUseIn1NightOverride = g_prefs.getBool(K_EQ_IN1, g_eqUseIn1NightOverride);
    g_eqSummerModeEnabled = g_prefs.getBool(K_EQ_SUM_EN, g_eqSummerModeEnabled);
    g_eqSummerOffAboveC = g_prefs.getFloat(K_EQ_SUM_OFF, g_eqSummerOffAboveC);
    g_eqSummerOnBelowC = g_prefs.getFloat(K_EQ_SUM_ON, g_eqSummerOnBelowC);
    g_eqScheduleEnabled = g_prefs.getBool(K_EQ_SCHED_EN, g_eqScheduleEnabled);

    // schedule bytes
    {
      const size_t legacyLen = 28;
      const size_t v2Len = 7 * (1 + 6 * 4);
      size_t sl2 = g_prefs.getBytesLength(K_EQ_SCHED_V2);
      if (sl2 == v2Len) {
        uint8_t buf[v2Len];
        g_prefs.getBytes(K_EQ_SCHED_V2, buf, sizeof(buf));
        size_t o = 0;
        for (int d = 0; d < 7; d++) {
          uint8_t cnt = buf[o++];
          if (cnt > 6) cnt = 6;
          g_eqIntervalCount[d] = cnt;
          for (int i = 0; i < 6; i++) {
            uint16_t s = (uint16_t)(buf[o] | ((uint16_t)buf[o+1] << 8)); o += 2;
            uint16_t e = (uint16_t)(buf[o] | ((uint16_t)buf[o+1] << 8)); o += 2;
            g_eqIntervalsStart[d][i] = s;
            g_eqIntervalsEnd[d][i] = e;
          }
          if (cnt > 0) {
            g_eqDayStartMin[d] = g_eqIntervalsStart[d][0];
            g_eqNightStartMin[d] = g_eqIntervalsEnd[d][0];
          }
        }
      } else {
        uint8_t buf[legacyLen];
        size_t sl = g_prefs.getBytesLength(K_EQ_SCHED);
        if (sl == sizeof(buf)) {
          g_prefs.getBytes(K_EQ_SCHED, buf, sizeof(buf));
          for (int i = 0; i < 7; i++) {
            g_eqDayStartMin[i] = (uint16_t)(buf[i*2] | ((uint16_t)buf[i*2+1] << 8));
          }
          for (int i = 0; i < 7; i++) {
            int oo = 14 + i*2;
            g_eqNightStartMin[i] = (uint16_t)(buf[oo] | ((uint16_t)buf[oo+1] << 8));
            g_eqIntervalCount[i] = 1;
            g_eqIntervalsStart[i][0] = g_eqDayStartMin[i];
            g_eqIntervalsEnd[i][0] = g_eqNightStartMin[i];
            for (int j = 1; j < 6; j++) { g_eqIntervalsStart[i][j] = 0; g_eqIntervalsEnd[i][j] = 0; }
          }
        }
      }
    }

    // curves + limits
    g_eqDayOutColdC  = g_prefs.getFloat(K_EQ_D_OC, g_eqDayOutColdC);
    g_eqDayFlowColdC = g_prefs.getFloat(K_EQ_D_FC, g_eqDayFlowColdC);
    g_eqDayOutWarmC  = g_prefs.getFloat(K_EQ_D_OW, g_eqDayOutWarmC);
    g_eqDayFlowWarmC = g_prefs.getFloat(K_EQ_D_FW, g_eqDayFlowWarmC);

    g_eqNightOutColdC  = g_prefs.getFloat(K_EQ_N_OC, g_eqNightOutColdC);
    g_eqNightFlowColdC = g_prefs.getFloat(K_EQ_N_FC, g_eqNightFlowColdC);
    g_eqNightOutWarmC  = g_prefs.getFloat(K_EQ_N_OW, g_eqNightOutWarmC);
    g_eqNightFlowWarmC = g_prefs.getFloat(K_EQ_N_FW, g_eqNightFlowWarmC);

    g_eqMinFlowC = g_prefs.getFloat(K_EQ_MINF, g_eqMinFlowC);
    g_eqMaxFlowC = g_prefs.getFloat(K_EQ_MAXF, g_eqMaxFlowC);
    g_eqMinChSetpointC = g_prefs.getFloat(K_EQ_MINC, g_eqMinChSetpointC);
    g_eqMaxChSetpointC = g_prefs.getFloat(K_EQ_MAXC, g_eqMaxChSetpointC);

    g_eqTempMaxAgeMs = g_prefs.getUInt(K_EQ_TAGE, g_eqTempMaxAgeMs);
    g_eqMinSendIntervalMs = g_prefs.getUInt(K_EQ_MININT, g_eqMinSendIntervalMs);
    g_eqMinSendDeltaC = g_prefs.getFloat(K_EQ_MINDEL, g_eqMinSendDeltaC);

    g_eqUseOpenTherm = g_prefs.getBool(K_EQ_USEOT, g_eqUseOpenTherm);
    g_eqApplyBoilerMaxCh = g_prefs.getBool(K_EQ_APPMAX, g_eqApplyBoilerMaxCh);
    g_eqBoilerMaxChC = g_prefs.getFloat(K_EQ_BMAX, g_eqBoilerMaxChC);
    g_eqDriveNightRelay = g_prefs.getBool(K_EQ_NRE, g_eqDriveNightRelay);
    g_eqNightRelayIndex = (uint8_t)g_prefs.getUInt(K_EQ_NRIDX, g_eqNightRelayIndex);
    g_eqNightRelayOnWhenNight = g_prefs.getBool(K_EQ_NRON, g_eqNightRelayOnWhenNight);
      g_eqMixingEnabled = g_prefs.getBool(K_EQ_MIX_EN, g_eqMixingEnabled);
      g_eqMixOpenRelayIndex = (uint8_t)g_prefs.getUInt(K_EQ_MIX_O, g_eqMixOpenRelayIndex);
      g_eqMixCloseRelayIndex = (uint8_t)g_prefs.getUInt(K_EQ_MIX_C, g_eqMixCloseRelayIndex);
      g_eqMixDeadbandC = g_prefs.getFloat(K_EQ_MIX_DB, g_eqMixDeadbandC);
      g_eqMixTargetOffsetC = g_prefs.getFloat(K_EQ_MIX_TO, g_eqMixTargetOffsetC);
      g_eqMixTargetReachedAction = g_prefs.getString(K_EQ_MIX_DONE, g_eqMixTargetReachedAction);
      g_eqMixPulseMs = g_prefs.getUInt(K_EQ_MIX_P, g_eqMixPulseMs);
      g_eqMixMinIntervalMs = g_prefs.getUInt(K_EQ_MIX_MI, g_eqMixMinIntervalMs);
      g_eqMixTravelMs = g_prefs.getUInt(K_EQ_MIX_T, g_eqMixTravelMs);
    g_eqMixCalibrationSeatMs = g_prefs.getUInt(K_EQ_MIX_SEAT, g_eqMixCalibrationSeatMs);
    g_eqMixAutoRecalibrationMs = g_prefs.getUInt(K_EQ_MIX_RECAL, g_eqMixAutoRecalibrationMs);
    g_eqMixTempSourceA = normalizeMixTempSourceA(g_prefs.getString(K_EQ_MIX_SRC_A, g_eqMixTempSourceA));
    g_eqMixTempSourceB = normalizeMixTempSourceB(g_prefs.getString(K_EQ_MIX_SRC_B, g_eqMixTempSourceB));
    g_eqMixTempSourceAB = normalizeMixTempSourceAB(g_prefs.getString(K_EQ_MIX_SRC_AB, g_eqMixTempSourceAB));

      g_eqBoilerAssistEnabled = g_prefs.getBool(K_EQ_BA_EN, g_eqBoilerAssistEnabled);
      g_eqBoilerAssistDeltaC = g_prefs.getFloat(K_EQ_BA_D, g_eqBoilerAssistDeltaC);
      g_eqBoilerAssistForceChEnable = g_prefs.getBool(K_EQ_BA_CH, g_eqBoilerAssistForceChEnable);

    // DHW / TUV
    g_dhwEnabled = g_prefs.getBool(K_DHW_EN, g_dhwEnabled);
    g_dhwDisableEq = g_prefs.getBool(K_DHW_DEQ, g_dhwDisableEq);
    g_dhwTempMaxAgeMs = g_prefs.getUInt(K_DHW_TAGE, g_dhwTempMaxAgeMs);
    g_dhwHeatUseInput = g_prefs.getBool(K_DHW_H_IN, g_dhwHeatUseInput);
    g_dhwHeatUseSchedule = g_prefs.getBool(K_DHW_H_SC, g_dhwHeatUseSchedule);
    g_dhwHeatScheduleEnabled = g_prefs.getBool(K_DHW_H_SEN, g_dhwHeatScheduleEnabled);
    g_dhwHeatTargetTempC = g_prefs.getFloat(K_DHW_H_TG, g_dhwHeatTargetTempC);
    g_dhwHeatHysteresisC = g_prefs.getFloat(K_DHW_H_HY, g_dhwHeatHysteresisC);
    g_dhwHeatRequestMode = g_prefs.getString(K_DHW_H_RM, g_dhwHeatRequestMode);
    g_dhwHeatOtEnableDhw = g_prefs.getBool(K_DHW_H_ODE, g_dhwHeatOtEnableDhw);
    g_dhwHeatOtDhwSetpointC = g_prefs.getFloat(K_DHW_H_ODS, g_dhwHeatOtDhwSetpointC);
    g_dhwHeatRelayRequest = g_prefs.getBool(K_DHW_H_RRQ, g_dhwHeatRelayRequest);
    g_dhwHeatDriveValveRelay = g_prefs.getBool(K_DHW_H_DVR, g_dhwHeatDriveValveRelay);
    g_dhwHeatValveRelayIndex = (uint8_t)g_prefs.getUInt(K_DHW_H_VR, g_dhwHeatValveRelayIndex);
    g_dhwHeatBoilerRelayIndex = (uint8_t)g_prefs.getUInt(K_DHW_H_BR, g_dhwHeatBoilerRelayIndex);
    g_dhwHeatValveLeadMs = g_prefs.getUInt(K_DHW_H_VLD, g_dhwHeatValveLeadMs);
    g_dhwHeatValveSwitchBackMs = g_prefs.getUInt(K_DHW_H_VBK, g_dhwHeatValveSwitchBackMs);
    g_dhwHeatBoilerOffHoldMs = g_prefs.getUInt(K_DHW_H_BOH, g_dhwHeatBoilerOffHoldMs);
    g_dhwHeatScheduleJson = g_prefs.getString(K_DHW_H_SJ, g_dhwHeatScheduleJson);
    g_dhwCircUseInput = g_prefs.getBool(K_DHW_C_IN, g_dhwCircUseInput);
    g_dhwCircUseSchedule = g_prefs.getBool(K_DHW_C_SC, g_dhwCircUseSchedule);
    g_dhwCircScheduleEnabled = g_prefs.getBool(K_DHW_C_SEN, g_dhwCircScheduleEnabled);
    g_dhwCircPulseEnabled = g_prefs.getBool(K_DHW_C_PE, g_dhwCircPulseEnabled);
    g_dhwCircPulseOnMin = g_prefs.getUInt(K_DHW_C_PON, g_dhwCircPulseOnMin);
    g_dhwCircPulseOffMin = g_prefs.getUInt(K_DHW_C_POF, g_dhwCircPulseOffMin);
    g_dhwCircRelayIndex = (uint8_t)g_prefs.getUInt(K_DHW_C_RI, g_dhwCircRelayIndex);
    g_dhwCircScheduleJson = g_prefs.getString(K_DHW_C_SJ, g_dhwCircScheduleJson);
    g_dhwAlEnabled = g_prefs.getBool(K_DHW_AL_EN, g_dhwAlEnabled);
    g_dhwAlWeekday = g_prefs.getUInt(K_DHW_AL_WD, g_dhwAlWeekday);
    g_dhwAlStartMin = g_prefs.getUInt(K_DHW_AL_SM, g_dhwAlStartMin);
    g_dhwAlTargetTempC = g_prefs.getFloat(K_DHW_AL_TG, g_dhwAlTargetTempC);
    g_dhwAlHoldMin = g_prefs.getUInt(K_DHW_AL_HM, g_dhwAlHoldMin);
    g_dhwAlLastDayKey = g_prefs.getUInt(K_DHW_AL_DK, g_dhwAlLastDayKey);

    // OTA
    g_otaEnabled  = g_prefs.getBool(K_OTA_EN, g_otaEnabled);
    g_otaHostname = g_prefs.getString(K_OTA_HOST, g_otaHostname);
    g_otaPort     = g_prefs.getUInt(K_OTA_PORT, g_otaPort);
    g_otaPassword = g_prefs.getString(K_OTA_PASS, g_otaPassword);

    // MQTT
    g_mqttEnabled = g_prefs.getBool(K_MQ_EN, g_mqttEnabled);
    g_mqttHost = g_prefs.getString(K_MQ_HOST, g_mqttHost);
    g_mqttPort = g_prefs.getUInt(K_MQ_PORT, g_mqttPort);
    g_mqttUsername = g_prefs.getString(K_MQ_USER, g_mqttUsername);
    g_mqttPassword = g_prefs.getString(K_MQ_PASS, g_mqttPassword);
    g_mqttClientId = g_prefs.getString(K_MQ_CID, g_mqttClientId);
    g_mqttBaseTopic = g_prefs.getString(K_MQ_BASE, g_mqttBaseTopic);
    g_mqttPublishIntervalMs = g_prefs.getUInt(K_MQ_PMS, g_mqttPublishIntervalMs);
    g_mqttHaEnabled = g_prefs.getBool(K_MQ_HAEN, g_mqttHaEnabled);
    g_mqttHaDiscovery = g_prefs.getBool(K_MQ_DISC, g_mqttHaDiscovery);
    g_mqttDiscoveryPrefix = g_prefs.getString(K_MQ_DPRE, g_mqttDiscoveryPrefix);
    g_mqttNodeId = g_prefs.getString(K_MQ_NODE, g_mqttNodeId);
    g_pressureAlarmEnabled = g_prefs.getBool(K_PAL_EN, g_pressureAlarmEnabled);
    g_pressureAlarmMinBar = g_prefs.getFloat(K_PAL_MIN, g_pressureAlarmMinBar);
    g_pressureAlarmMaxBar = g_prefs.getFloat(K_PAL_MAX, g_pressureAlarmMaxBar);
    g_pressureAlarmHysteresisBar = g_prefs.getFloat(K_PAL_HYS, g_pressureAlarmHysteresisBar);

    g_prefs.end();

    // Clamp
    if (g_otPollMs < 250) g_otPollMs = 250;
    if (g_otPollMs > 30000) g_otPollMs = 30000;
    if (g_otBootDelayMs > 120000) g_otBootDelayMs = 120000;

    if (g_bleScanMs < 2000) g_bleScanMs = 2000;
    if (g_bleScanMs > 60000) g_bleScanMs = 60000;

    if (g_otMode != "readOnly" && g_otMode != "control") g_otMode = "readOnly";

    // Clamp DHW
    if (g_dhwTempMaxAgeMs < 10000) g_dhwTempMaxAgeMs = 10000;
    if (g_dhwTempMaxAgeMs > 3600000) g_dhwTempMaxAgeMs = 3600000;
    if (g_dhwHeatRequestMode != "relay" && g_dhwHeatRequestMode != "opentherm") g_dhwHeatRequestMode = "relay";
    if (g_dhwHeatTargetTempC < 20.0f) g_dhwHeatTargetTempC = 20.0f;
    if (g_dhwHeatTargetTempC > 80.0f) g_dhwHeatTargetTempC = 80.0f;
    if (g_dhwHeatHysteresisC < 0.5f) g_dhwHeatHysteresisC = 0.5f;
    if (g_dhwHeatHysteresisC > 15.0f) g_dhwHeatHysteresisC = 15.0f;
    if (g_dhwHeatOtDhwSetpointC < 20.0f) g_dhwHeatOtDhwSetpointC = 20.0f;
    if (g_dhwHeatOtDhwSetpointC > 80.0f) g_dhwHeatOtDhwSetpointC = 80.0f;
    if (g_dhwHeatValveRelayIndex > 7) g_dhwHeatValveRelayIndex = 2;
    if (g_dhwHeatBoilerRelayIndex > 7) g_dhwHeatBoilerRelayIndex = 4;
    if (g_dhwCircRelayIndex > 7) g_dhwCircRelayIndex = 3;
    if (g_dhwCircPulseOnMin > 1440) g_dhwCircPulseOnMin = 1440;
    if (g_dhwCircPulseOffMin > 1440) g_dhwCircPulseOffMin = 1440;
    if (g_dhwAlWeekday > 6) g_dhwAlWeekday = 0;
    if (g_dhwAlStartMin > 1439) g_dhwAlStartMin = 120;
    if (g_dhwAlTargetTempC < 45.0f) g_dhwAlTargetTempC = 45.0f;
    if (g_dhwAlTargetTempC > 75.0f) g_dhwAlTargetTempC = 75.0f;
    if (g_dhwAlHoldMin > 240) g_dhwAlHoldMin = 240;
    if (!g_dhwHeatScheduleJson.length()) g_dhwHeatScheduleJson = "[[],[],[],[],[],[],[]]";
    if (!g_dhwCircScheduleJson.length()) g_dhwCircScheduleJson = "[[],[],[],[],[],[],[]]";
    if (g_dhwHeatValveLeadMs > 60000) g_dhwHeatValveLeadMs = 60000;
    if (g_dhwHeatValveSwitchBackMs > 60000) g_dhwHeatValveSwitchBackMs = 60000;
    if (g_dhwHeatBoilerOffHoldMs > 60000) g_dhwHeatBoilerOffHoldMs = 60000;

    if (g_otaPort < 1024) g_otaPort = 3232;
    if (g_otaPort > 65535) g_otaPort = 65535;

    // Clamp MQTT
    auto trimTopic = [](String& s) {
      s.trim();
      while (s.startsWith("/")) s.remove(0, 1);
      while (s.endsWith("/")) s.remove(s.length() - 1);
    };
    g_mqttHost.trim();
    g_mqttUsername.trim();
    g_mqttClientId.trim();
    g_mqttBaseTopic.trim();
    g_mqttDiscoveryPrefix.trim();
    g_mqttNodeId.trim();
    trimTopic(g_mqttBaseTopic);
    trimTopic(g_mqttDiscoveryPrefix);
    trimTopic(g_mqttNodeId);
    if (!g_mqttPort || g_mqttPort > 65535) g_mqttPort = 1883;
    if (g_mqttPublishIntervalMs < 1000) g_mqttPublishIntervalMs = 1000;
    if (g_mqttPublishIntervalMs > 600000) g_mqttPublishIntervalMs = 600000;
    if (!g_mqttClientId.length()) g_mqttClientId = "esp32-controller";
    if (!g_mqttBaseTopic.length()) g_mqttBaseTopic = "esp32-controller";
    if (!g_mqttDiscoveryPrefix.length()) g_mqttDiscoveryPrefix = "homeassistant";
    if (!g_mqttNodeId.length()) g_mqttNodeId = "esp32_controller";

    // Clamp time
    g_timeTz.trim();
    g_timeNtp1.trim(); g_timeNtp2.trim(); g_timeNtp3.trim();
    if (!g_timeNtp1.length()) g_timeNtp1 = "pool.ntp.org";

    // Clamp equitherm
    if (g_eqMode != "auto" && g_eqMode != "day" && g_eqMode != "night") g_eqMode = "auto";
    if (g_eqSummerOnBelowC > g_eqSummerOffAboveC) { float t=g_eqSummerOnBelowC; g_eqSummerOnBelowC=g_eqSummerOffAboveC; g_eqSummerOffAboveC=t; }
    auto clampMin = [](uint16_t& m) { if (m > 1439) m = 1439; };
    for (int i = 0; i < 7; i++) {
      clampMin(g_eqDayStartMin[i]); clampMin(g_eqNightStartMin[i]);
      if (g_eqIntervalCount[i] > 6) g_eqIntervalCount[i] = 6;
      for (int j = 0; j < 6; j++) {
        clampMin(g_eqIntervalsStart[i][j]);
        clampMin(g_eqIntervalsEnd[i][j]);
      }
    }
    if (g_eqTempMaxAgeMs < 10000) g_eqTempMaxAgeMs = 10000;
    if (g_eqTempMaxAgeMs > 3600000) g_eqTempMaxAgeMs = 3600000;
    if (g_eqMinSendIntervalMs < 5000) g_eqMinSendIntervalMs = 5000;
    if (g_eqMinSendIntervalMs > 3600000) g_eqMinSendIntervalMs = 3600000;
    if (g_eqMinSendDeltaC < 0.1f) g_eqMinSendDeltaC = 0.1f;
    if (g_eqMinSendDeltaC > 10.0f) g_eqMinSendDeltaC = 10.0f;
    if (g_eqMinFlowC < 10.0f) g_eqMinFlowC = 10.0f;
    if (g_eqMaxFlowC > 90.0f) g_eqMaxFlowC = 90.0f;
    if (g_eqMinFlowC > g_eqMaxFlowC) { float t=g_eqMinFlowC; g_eqMinFlowC=g_eqMaxFlowC; g_eqMaxFlowC=t; }
    if (g_eqMinChSetpointC < 10.0f) g_eqMinChSetpointC = 10.0f;
    if (g_eqMaxChSetpointC > 90.0f) g_eqMaxChSetpointC = 90.0f;
    if (g_eqMinChSetpointC > g_eqMaxChSetpointC) { float t=g_eqMinChSetpointC; g_eqMinChSetpointC=g_eqMaxChSetpointC; g_eqMaxChSetpointC=t; }
    if (g_eqNightRelayIndex > 7) g_eqNightRelayIndex = 5;
    // Mixing valve relay mapping is fixed by project wiring: R1 = direction A, R2 = direction B.
    g_eqMixOpenRelayIndex = 0;
    g_eqMixCloseRelayIndex = 1;
    if (g_eqMixDeadbandC < 0.1f) g_eqMixDeadbandC = 0.1f;
    if (g_eqMixDeadbandC > 10.0f) g_eqMixDeadbandC = 10.0f;
    if (g_eqMixTargetOffsetC < 0.0f) g_eqMixTargetOffsetC = 0.0f;
    if (g_eqMixTargetOffsetC > 20.0f) g_eqMixTargetOffsetC = 20.0f;
    g_eqMixTargetReachedAction.trim();
    g_eqMixTargetReachedAction.toLowerCase();
    if (g_eqMixTargetReachedAction != "return_a" && g_eqMixTargetReachedAction != "hold") g_eqMixTargetReachedAction = "return_a";
    if (g_eqMixPulseMs < 100) g_eqMixPulseMs = 100;
    if (g_eqMixPulseMs > 10000) g_eqMixPulseMs = 10000;
    if (g_eqMixMinIntervalMs < 500) g_eqMixMinIntervalMs = 500;
    if (g_eqMixMinIntervalMs > 60000) g_eqMixMinIntervalMs = 60000;
    if (g_eqMixTravelMs < 1000) g_eqMixTravelMs = 1000;
    if (g_eqMixTravelMs > 900000) g_eqMixTravelMs = 900000;
    g_eqMixTempSourceA = normalizeMixTempSourceA(g_eqMixTempSourceA);
    g_eqMixTempSourceB = normalizeMixTempSourceB(g_eqMixTempSourceB);
    g_eqMixTempSourceAB = normalizeMixTempSourceAB(g_eqMixTempSourceAB);
    if (g_eqBoilerAssistDeltaC < 0.0f) g_eqBoilerAssistDeltaC = 0.0f;
    if (g_eqBoilerAssistDeltaC > 30.0f) g_eqBoilerAssistDeltaC = 30.0f;
  }

  bool beginWriteSession() {
    if (g_writeSessionOpen) return true;
    if (!g_prefs.begin(NS, false)) return false;
    g_writeSessionOpen = true;
    return true;
  }

  void endWriteSessionIfNeeded() {
    if (!g_writeSessionOpen) return;
    if (g_batchDepth > 0) return;
    g_prefs.end();
    g_writeSessionOpen = false;
  }

  void saveBytes(const char* key, const void* data, size_t len) {
    if (!beginWriteSession()) return;
    g_prefs.putBytes(key, data, len);
    endWriteSessionIfNeeded();
  }

  void saveBool(const char* key, bool v) {
    if (!beginWriteSession()) return;
    g_prefs.putBool(key, v);
    endWriteSessionIfNeeded();
  }

  void saveUInt(const char* key, uint32_t v) {
    if (!beginWriteSession()) return;
    g_prefs.putUInt(key, v);
    endWriteSessionIfNeeded();
  }

  void saveString(const char* key, const String& v) {
    if (!beginWriteSession()) return;
    g_prefs.putString(key, v);
    endWriteSessionIfNeeded();
  }

  void saveU64(const char* keyHi, const char* keyLo, uint64_t v) {
    uint32_t hi=0, lo=0;
    unpack64(v, hi, lo);
    if (!beginWriteSession()) return;
    g_prefs.putUInt(keyHi, hi);
    g_prefs.putUInt(keyLo, lo);
    endWriteSessionIfNeeded();
  }

  void saveFloat(const char* key, float v) {
    if (!beginWriteSession()) return;
    g_prefs.putFloat(key, v);
    endWriteSessionIfNeeded();
  }

  String normalizeMixTempSourceToken(String value) {
    value.trim();
    value.toLowerCase();
    value.replace(" ", "_");
    value.replace("-", "_");
    value.replace(".", "_");
    while (value.indexOf("__") >= 0) value.replace("__", "_");
    return value;
  }

  String normalizeMixTempSourceA(String value) {
    value = normalizeMixTempSourceToken(value);
    if (value == "none" || value == "disabled" || value == "off") return "none";
    if (value == "tank_mid" || value == "tankmid" || value == "tank_middle" ||
        value == "aku_mid" || value == "aku_uprostred" || value == "accu_mid" ||
        value == "flow") { // migrate the interim generic-source build safely
      return "tank_mid";
    }
    return "tank_mid";
  }

  String normalizeMixTempSourceB(String value) {
    value = normalizeMixTempSourceToken(value);
    if (value == "none" || value == "disabled" || value == "off") return "none";
    if (value == "return_dallas" || value == "dallas_return" || value == "return_ds" ||
        value == "return" || value == "mix_feedback" || value == "after_mix" ||
        value == "aftermix" || value == "feedback") {
      return "return_dallas";
    }
    return "return_dallas";
  }

  String normalizeMixTempSourceAB(String value) {
    value = normalizeMixTempSourceToken(value);
    if (value == "none" || value == "disabled" || value == "off") return "none";
    if (value == "opentherm_ch" || value == "ot_ch" || value == "ch_measured" ||
        value == "boiler_temp" || value == "boiler_temperature" || value == "flow" ||
        value == "opentherm_flow") {
      return "opentherm_ch";
    }
    return "opentherm_ch";
  }
}

namespace ConfigStore {
  void begin() {
    if (g_inited) return;
    g_inited = true;
    load();
  }

  void beginBatch() {
    begin();
    g_batchDepth++;
    if (g_batchDepth == 1) beginWriteSession();
  }

  void endBatch() {
    if (g_batchDepth == 0) return;
    g_batchDepth--;
    if (g_batchDepth == 0 && g_writeSessionOpen) {
      g_prefs.end();
      g_writeSessionOpen = false;
    }
  }

  // Inputs
  uint8_t getInputActiveLevel(uint8_t inputIndex) {
    begin();
    if (inputIndex >= 8) return 0;
    return g_inLevels[inputIndex] ? 1 : 0;
  }

  void setInputActiveLevels(const uint8_t* levels, uint8_t count) {
    begin();
    const uint8_t n = (count > 8) ? 8 : count;
    for (uint8_t i = 0; i < n; i++) {
      g_inLevels[i] = levels[i] ? 1 : 0;
    }
    saveBytes(K_INLVL, g_inLevels, sizeof(g_inLevels));
  }

  // OpenTherm
  bool getOtEnabled() { begin(); return g_otEnabled; }
  void setOtEnabled(bool v) { begin(); g_otEnabled = v; saveBool(K_OT_EN, v); }
  bool getOtAutoStart() { begin(); return g_otAutoStart; }
  void setOtAutoStart(bool v) { begin(); g_otAutoStart = v; saveBool(K_OT_AS, v); }
  uint32_t getOtPollMs() { begin(); return g_otPollMs; }
  void setOtPollMs(uint32_t v) {
    begin();
    if (v < 250) v = 250;
    if (v > 30000) v = 30000;
    g_otPollMs = v;
    saveUInt(K_OT_POLL, v);
  }
  uint32_t getOtBootDelayMs() { begin(); return g_otBootDelayMs; }
  void setOtBootDelayMs(uint32_t v) {
    begin();
    if (v > 120000) v = 120000;
    g_otBootDelayMs = v;
    saveUInt(K_OT_BOOT, v);
  }

  String getOtMode() { begin(); return g_otMode; }
  void setOtMode(const String& v) {
    begin();
    if (v != "readOnly" && v != "control") return;
    g_otMode = v;
    saveString(K_OT_MODE, v);
  }

  bool getOtAllowRawWrite() { begin(); return g_otAllowRawWrite; }
  void setOtAllowRawWrite(bool v) { begin(); g_otAllowRawWrite = v; saveBool(K_OT_RAWW, v); }

  // BLE
  bool getBleEnabled() { begin(); return g_bleEnabled; }
  void setBleEnabled(bool v) { begin(); g_bleEnabled = v; saveBool(K_BLE_EN, v); }
  String getBleNamePrefix() { begin(); return g_bleNamePrefix; }
  void setBleNamePrefix(const String& v) { begin(); g_bleNamePrefix = v; saveString(K_BLE_NAME, v); }
  uint32_t getBleScanIntervalMs() { begin(); return g_bleScanMs; }
  void setBleScanIntervalMs(uint32_t v) {
    begin();
    if (v < 2000) v = 2000;
    if (v > 60000) v = 60000;
    g_bleScanMs = v;
    saveUInt(K_BLE_SCAN, v);
  }

  // Dallas
  bool getDallasEnabled() { begin(); return g_dallasEnabled; }
  void setDallasEnabled(bool v) { begin(); g_dallasEnabled = v; saveBool(K_DS_EN, v); }

  uint64_t getDallasTankTopRom() { begin(); return g_dsTankTopRom; }
  void setDallasTankTopRom(uint64_t rom) { begin(); g_dsTankTopRom = rom; saveU64(K_DS_TTOP_H, K_DS_TTOP_L, rom); }
  uint64_t getDallasTankMidRom() { begin(); return g_dsTankMidRom; }
  void setDallasTankMidRom(uint64_t rom) { begin(); g_dsTankMidRom = rom; saveU64(K_DS_TMID_H, K_DS_TMID_L, rom); }
  uint64_t getDallasTankBottomRom() { begin(); return g_dsTankBottomRom; }
  void setDallasTankBottomRom(uint64_t rom) { begin(); g_dsTankBottomRom = rom; saveU64(K_DS_TBOT_H, K_DS_TBOT_L, rom); }

  uint64_t getDallasReturnRom() { begin(); return g_dsReturnRom; }
  void setDallasReturnRom(uint64_t rom) { begin(); g_dsReturnRom = rom; saveU64(K_DS_RET_H, K_DS_RET_L, rom); }
  uint64_t getDallasDhwReturnRom() { begin(); return g_dsDhwReturnRom; }
  void setDallasDhwReturnRom(uint64_t rom) { begin(); g_dsDhwReturnRom = rom; saveU64(K_DS_DHWR_H, K_DS_DHWR_L, rom); }
  uint64_t getDallasDhwTankRom() { begin(); return g_dsDhwTankRom; }
  void setDallasDhwTankRom(uint64_t rom) { begin(); g_dsDhwTankRom = rom; saveU64(K_DS_DHWT_H, K_DS_DHWT_L, rom); }

  uint64_t getDallasOutsideRom() { begin(); return g_dsOutsideRom; }
  void setDallasOutsideRom(uint64_t rom) { begin(); g_dsOutsideRom = rom; saveU64(K_DS_OUT_H, K_DS_OUT_L, rom); }

  // Time
  bool getTimeEnabled() { begin(); return g_timeEnabled; }
  void setTimeEnabled(bool v) { begin(); g_timeEnabled = v; saveBool(K_TIME_EN, v); }
  String getTimeTz() { begin(); return g_timeTz; }
  void setTimeTz(const String& v) { begin(); g_timeTz = v; saveString(K_TIME_TZ, g_timeTz); }
  String getTimeNtp1() { begin(); return g_timeNtp1; }
  String getTimeNtp2() { begin(); return g_timeNtp2; }
  String getTimeNtp3() { begin(); return g_timeNtp3; }
  void setTimeNtp1(const String& v) { begin(); g_timeNtp1 = v; saveString(K_TIME_N1, g_timeNtp1); }
  void setTimeNtp2(const String& v) { begin(); g_timeNtp2 = v; saveString(K_TIME_N2, g_timeNtp2); }
  void setTimeNtp3(const String& v) { begin(); g_timeNtp3 = v; saveString(K_TIME_N3, g_timeNtp3); }

  // Ekviterm
  bool getEqEnabled() { begin(); return g_eqEnabled; }
  void setEqEnabled(bool v) { begin(); g_eqEnabled = v; saveBool(K_EQ_EN, v); }

  String getEqMode() { begin(); return g_eqMode; }
  void setEqMode(const String& v) {
    begin();
    if (v != "auto" && v != "day" && v != "night") return;
    g_eqMode = v;
    saveString(K_EQ_MODE, g_eqMode);
  }

  bool getEqUseIn1NightOverride() { begin(); return g_eqUseIn1NightOverride; }
  void setEqUseIn1NightOverride(bool v) { begin(); g_eqUseIn1NightOverride = v; saveBool(K_EQ_IN1, v); }

  bool getEqSummerModeEnabled() { begin(); return g_eqSummerModeEnabled; }
  void setEqSummerModeEnabled(bool v) { begin(); g_eqSummerModeEnabled = v; saveBool(K_EQ_SUM_EN, v); }
  float getEqSummerOffAboveC() { begin(); return g_eqSummerOffAboveC; }
  void setEqSummerOffAboveC(float v) { begin(); g_eqSummerOffAboveC = v; saveFloat(K_EQ_SUM_OFF, g_eqSummerOffAboveC); }
  float getEqSummerOnBelowC() { begin(); return g_eqSummerOnBelowC; }
  void setEqSummerOnBelowC(float v) { begin(); g_eqSummerOnBelowC = v; saveFloat(K_EQ_SUM_ON, g_eqSummerOnBelowC); }

  void getEqSchedule(uint16_t dayStartMin[7], uint16_t nightStartMin[7]) {
    begin();
    for (int i = 0; i < 7; i++) { dayStartMin[i] = g_eqDayStartMin[i]; nightStartMin[i] = g_eqNightStartMin[i]; }
  }

  void setEqSchedule(const uint16_t dayStartMin[7], const uint16_t nightStartMin[7]) {
    begin();
    uint8_t counts[7];
    uint16_t starts[7][6] = {};
    uint16_t ends[7][6] = {};
    for (int i = 0; i < 7; i++) {
      counts[i] = 1;
      starts[i][0] = dayStartMin[i];
      ends[i][0] = nightStartMin[i];
    }
    setEqScheduleIntervals(counts, starts, ends);
  }

  void getEqScheduleIntervals(uint8_t counts[7], uint16_t starts[7][6], uint16_t ends[7][6]) {
    begin();
    for (int d = 0; d < 7; d++) {
      counts[d] = g_eqIntervalCount[d];
      for (int i = 0; i < 6; i++) {
        starts[d][i] = g_eqIntervalsStart[d][i];
        ends[d][i] = g_eqIntervalsEnd[d][i];
      }
    }
  }

  void setEqScheduleIntervals(const uint8_t counts[7], const uint16_t starts[7][6], const uint16_t ends[7][6]) {
    begin();
    uint8_t legacy[28];
    uint8_t buf[7 * (1 + 6 * 4)];
    size_t o = 0;
    for (int d = 0; d < 7; d++) {
      uint8_t cnt = counts[d] > 6 ? 6 : counts[d];
      g_eqIntervalCount[d] = cnt;
      buf[o++] = cnt;
      for (int i = 0; i < 6; i++) {
        uint16_t s = starts[d][i];
        uint16_t e = ends[d][i];
        if (s > 1439) s = 1439;
        if (e > 1439) e = 1439;
        if (i >= cnt) { s = 0; e = 0; }
        g_eqIntervalsStart[d][i] = s;
        g_eqIntervalsEnd[d][i] = e;
        buf[o++] = (uint8_t)(s & 0xFF);
        buf[o++] = (uint8_t)(s >> 8);
        buf[o++] = (uint8_t)(e & 0xFF);
        buf[o++] = (uint8_t)(e >> 8);
      }
      uint16_t ls = (cnt > 0) ? g_eqIntervalsStart[d][0] : 360;
      uint16_t le = (cnt > 0) ? g_eqIntervalsEnd[d][0] : 1320;
      g_eqDayStartMin[d] = ls;
      g_eqNightStartMin[d] = le;
      legacy[d*2] = (uint8_t)(ls & 0xFF);
      legacy[d*2+1] = (uint8_t)(ls >> 8);
      int lo = 14 + d*2;
      legacy[lo] = (uint8_t)(le & 0xFF);
      legacy[lo+1] = (uint8_t)(le >> 8);
    }
    saveBytes(K_EQ_SCHED, legacy, sizeof(legacy));
    saveBytes(K_EQ_SCHED_V2, buf, sizeof(buf));
  }

  bool getEqScheduleEnabled() { begin(); return g_eqScheduleEnabled; }
  void setEqScheduleEnabled(bool v) { begin(); g_eqScheduleEnabled = v; saveBool(K_EQ_SCHED_EN, v); }

  float getEqDayOutColdC() { begin(); return g_eqDayOutColdC; }
  float getEqDayFlowColdC() { begin(); return g_eqDayFlowColdC; }
  float getEqDayOutWarmC() { begin(); return g_eqDayOutWarmC; }
  float getEqDayFlowWarmC() { begin(); return g_eqDayFlowWarmC; }
  void setEqDayCurve(float outColdC, float flowColdC, float outWarmC, float flowWarmC) {
    begin();
    g_eqDayOutColdC = outColdC; g_eqDayFlowColdC = flowColdC; g_eqDayOutWarmC = outWarmC; g_eqDayFlowWarmC = flowWarmC;
    saveFloat(K_EQ_D_OC, g_eqDayOutColdC);
    saveFloat(K_EQ_D_FC, g_eqDayFlowColdC);
    saveFloat(K_EQ_D_OW, g_eqDayOutWarmC);
    saveFloat(K_EQ_D_FW, g_eqDayFlowWarmC);
  }

  float getEqNightOutColdC() { begin(); return g_eqNightOutColdC; }
  float getEqNightFlowColdC() { begin(); return g_eqNightFlowColdC; }
  float getEqNightOutWarmC() { begin(); return g_eqNightOutWarmC; }
  float getEqNightFlowWarmC() { begin(); return g_eqNightFlowWarmC; }
  void setEqNightCurve(float outColdC, float flowColdC, float outWarmC, float flowWarmC) {
    begin();
    g_eqNightOutColdC = outColdC; g_eqNightFlowColdC = flowColdC; g_eqNightOutWarmC = outWarmC; g_eqNightFlowWarmC = flowWarmC;
    saveFloat(K_EQ_N_OC, g_eqNightOutColdC);
    saveFloat(K_EQ_N_FC, g_eqNightFlowColdC);
    saveFloat(K_EQ_N_OW, g_eqNightOutWarmC);
    saveFloat(K_EQ_N_FW, g_eqNightFlowWarmC);
  }

  float getEqMinFlowC() { begin(); return g_eqMinFlowC; }
  float getEqMaxFlowC() { begin(); return g_eqMaxFlowC; }
  void setEqFlowLimits(float minFlowC, float maxFlowC) {
    begin();
    g_eqMinFlowC = minFlowC; g_eqMaxFlowC = maxFlowC;
    saveFloat(K_EQ_MINF, g_eqMinFlowC);
    saveFloat(K_EQ_MAXF, g_eqMaxFlowC);
  }

  float getEqMinChSetpointC() { begin(); return g_eqMinChSetpointC; }
  float getEqMaxChSetpointC() { begin(); return g_eqMaxChSetpointC; }
  void setEqChSetpointLimits(float minC, float maxC) {
    begin();
    g_eqMinChSetpointC = minC; g_eqMaxChSetpointC = maxC;
    saveFloat(K_EQ_MINC, g_eqMinChSetpointC);
    saveFloat(K_EQ_MAXC, g_eqMaxChSetpointC);
  }

  uint32_t getEqTempMaxAgeMs() { begin(); return g_eqTempMaxAgeMs; }
  void setEqTempMaxAgeMs(uint32_t v) {
    begin();
    if (v < 10000) v = 10000;
    if (v > 3600000) v = 3600000;
    g_eqTempMaxAgeMs = v;
    saveUInt(K_EQ_TAGE, v);
  }

  uint32_t getEqMinSendIntervalMs() { begin(); return g_eqMinSendIntervalMs; }
  void setEqMinSendIntervalMs(uint32_t v) {
    begin();
    if (v < 5000) v = 5000;
    if (v > 3600000) v = 3600000;
    g_eqMinSendIntervalMs = v;
    saveUInt(K_EQ_MININT, v);
  }

  float getEqMinSendDeltaC() { begin(); return g_eqMinSendDeltaC; }
  void setEqMinSendDeltaC(float v) { begin(); g_eqMinSendDeltaC = v; saveFloat(K_EQ_MINDEL, g_eqMinSendDeltaC); }

  bool getEqUseOpenTherm() { begin(); return g_eqUseOpenTherm; }
  void setEqUseOpenTherm(bool v) { begin(); g_eqUseOpenTherm = v; saveBool(K_EQ_USEOT, v); }
  bool getEqApplyBoilerMaxCh() { begin(); return g_eqApplyBoilerMaxCh; }
  void setEqApplyBoilerMaxCh(bool v) { begin(); g_eqApplyBoilerMaxCh = v; saveBool(K_EQ_APPMAX, v); }
  float getEqBoilerMaxChC() { begin(); return g_eqBoilerMaxChC; }
  void setEqBoilerMaxChC(float v) { begin(); g_eqBoilerMaxChC = v; saveFloat(K_EQ_BMAX, g_eqBoilerMaxChC); }

  bool getEqDriveNightRelay() { begin(); return g_eqDriveNightRelay; }
  void setEqDriveNightRelay(bool v) { begin(); g_eqDriveNightRelay = v; saveBool(K_EQ_NRE, v); }
  uint8_t getEqNightRelayIndex() { begin(); return g_eqNightRelayIndex; }
  void setEqNightRelayIndex(uint8_t idx) { begin(); if (idx > 7) idx = 5; g_eqNightRelayIndex = idx; saveUInt(K_EQ_NRIDX, idx); }
  bool getEqNightRelayOnWhenNight() { begin(); return g_eqNightRelayOnWhenNight; }
  void setEqNightRelayOnWhenNight(bool v) { begin(); g_eqNightRelayOnWhenNight = v; saveBool(K_EQ_NRON, v); }


  // Mixing valve mapping (R1/R2)
  bool getEqMixingEnabled() { begin(); return g_eqMixingEnabled; }
  void setEqMixingEnabled(bool v) { begin(); g_eqMixingEnabled = v; saveBool(K_EQ_MIX_EN, v); }

  uint8_t getEqMixOpenRelayIndex() { begin(); return 0; }
  void setEqMixOpenRelayIndex(uint8_t idx) {
    begin();
    (void)idx;
    g_eqMixOpenRelayIndex = 0;
    g_eqMixCloseRelayIndex = 1;
    saveUInt(K_EQ_MIX_O, 0);
    saveUInt(K_EQ_MIX_C, 1);
  }

  uint8_t getEqMixCloseRelayIndex() { begin(); return 1; }
  void setEqMixCloseRelayIndex(uint8_t idx) {
    begin();
    (void)idx;
    g_eqMixOpenRelayIndex = 0;
    g_eqMixCloseRelayIndex = 1;
    saveUInt(K_EQ_MIX_O, 0);
    saveUInt(K_EQ_MIX_C, 1);
  }

  float getEqMixDeadbandC() { begin(); return g_eqMixDeadbandC; }
  void setEqMixDeadbandC(float v) {
    begin();
    if (v < 0.1f) v = 0.1f;
    if (v > 10.0f) v = 10.0f;
    g_eqMixDeadbandC = v;
    saveFloat(K_EQ_MIX_DB, v);
  }

  float getEqMixTargetOffsetC() { begin(); return g_eqMixTargetOffsetC; }
  void setEqMixTargetOffsetC(float v) {
    begin();
    // Offset is used only as a positive accumulator-support target increase.
    if (v < 0.0f) v = 0.0f;
    if (v > 20.0f) v = 20.0f;
    g_eqMixTargetOffsetC = v;
    saveFloat(K_EQ_MIX_TO, v);
  }

  String getEqMixTargetReachedAction() { begin(); return g_eqMixTargetReachedAction; }
  void setEqMixTargetReachedAction(const String& v) {
    begin();
    String normalized = v;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized != "return_a" && normalized != "hold") normalized = "return_a";
    g_eqMixTargetReachedAction = normalized;
    saveString(K_EQ_MIX_DONE, normalized);
  }

  uint32_t getEqMixPulseMs() { begin(); return g_eqMixPulseMs; }
  void setEqMixPulseMs(uint32_t v) {
    begin();
    if (v < 100) v = 100;
    if (v > 10000) v = 10000;
    g_eqMixPulseMs = v;
    saveUInt(K_EQ_MIX_P, v);
  }

  uint32_t getEqMixMinIntervalMs() { begin(); return g_eqMixMinIntervalMs; }
  void setEqMixMinIntervalMs(uint32_t v) {
    begin();
    if (v < 500) v = 500;
    if (v > 60000) v = 60000;
    g_eqMixMinIntervalMs = v;
    saveUInt(K_EQ_MIX_MI, v);
  }

  uint32_t getEqMixTravelMs() { begin(); return g_eqMixTravelMs; }
  void setEqMixTravelMs(uint32_t v) {
    begin();
    if (v < 1000) v = 1000;
    if (v > 900000) v = 900000;
    g_eqMixTravelMs = v;
    saveUInt(K_EQ_MIX_T, v);
  }
  uint32_t getEqMixCalibrationSeatMs() { begin(); return g_eqMixCalibrationSeatMs; }
  void setEqMixCalibrationSeatMs(uint32_t v) {
    begin();
    if (v < 250) v = 250;
    if (v > 10000) v = 10000;
    g_eqMixCalibrationSeatMs = v;
    saveUInt(K_EQ_MIX_SEAT, v);
  }
  uint32_t getEqMixAutoRecalibrationMs() { begin(); return g_eqMixAutoRecalibrationMs; }
  void setEqMixAutoRecalibrationMs(uint32_t v) {
    begin();
    if (v > 604800000UL) v = 604800000UL;
    g_eqMixAutoRecalibrationMs = v;
    saveUInt(K_EQ_MIX_RECAL, v);
  }

  String getEqMixTempSourceA() { begin(); return g_eqMixTempSourceA; }
  void setEqMixTempSourceA(const String& v) {
    begin();
    g_eqMixTempSourceA = normalizeMixTempSourceA(v);
    saveString(K_EQ_MIX_SRC_A, g_eqMixTempSourceA);
  }

  String getEqMixTempSourceB() { begin(); return g_eqMixTempSourceB; }
  void setEqMixTempSourceB(const String& v) {
    begin();
    g_eqMixTempSourceB = normalizeMixTempSourceB(v);
    saveString(K_EQ_MIX_SRC_B, g_eqMixTempSourceB);
  }

  String getEqMixTempSourceAB() { begin(); return g_eqMixTempSourceAB; }
  void setEqMixTempSourceAB(const String& v) {
    begin();
    g_eqMixTempSourceAB = normalizeMixTempSourceAB(v);
    saveString(K_EQ_MIX_SRC_AB, g_eqMixTempSourceAB);
  }

  // Boiler assist headroom
  bool getEqBoilerAssistEnabled() { begin(); return g_eqBoilerAssistEnabled; }
  void setEqBoilerAssistEnabled(bool v) { begin(); g_eqBoilerAssistEnabled = v; saveBool(K_EQ_BA_EN, v); }

  float getEqBoilerAssistDeltaC() { begin(); return g_eqBoilerAssistDeltaC; }
  void setEqBoilerAssistDeltaC(float v) {
    begin();
    if (v < 0.0f) v = 0.0f;
    if (v > 30.0f) v = 30.0f;
    g_eqBoilerAssistDeltaC = v;
    saveFloat(K_EQ_BA_D, v);
  }

  bool getEqBoilerAssistForceChEnable() { begin(); return g_eqBoilerAssistForceChEnable; }
  void setEqBoilerAssistForceChEnable(bool v) { begin(); g_eqBoilerAssistForceChEnable = v; saveBool(K_EQ_BA_CH, v); }
  // DHW / TUV
  bool getDhwEnabled() { begin(); return g_dhwEnabled; }
  void setDhwEnabled(bool v) { begin(); g_dhwEnabled = v; saveBool(K_DHW_EN, v); }
  bool getDhwDisableEquithermDuringHeat() { begin(); return g_dhwDisableEq; }
  void setDhwDisableEquithermDuringHeat(bool v) { begin(); g_dhwDisableEq = v; saveBool(K_DHW_DEQ, v); }
  uint32_t getDhwTempMaxAgeMs() { begin(); return g_dhwTempMaxAgeMs; }
  void setDhwTempMaxAgeMs(uint32_t v) { begin(); if (v < 10000) v = 10000; if (v > 3600000) v = 3600000; g_dhwTempMaxAgeMs = v; saveUInt(K_DHW_TAGE, v); }
  bool getDhwHeatUseInput() { begin(); return g_dhwHeatUseInput; }
  void setDhwHeatUseInput(bool v) { begin(); g_dhwHeatUseInput = v; saveBool(K_DHW_H_IN, v); }
  bool getDhwHeatUseSchedule() { begin(); return g_dhwHeatUseSchedule; }
  void setDhwHeatUseSchedule(bool v) { begin(); g_dhwHeatUseSchedule = v; saveBool(K_DHW_H_SC, v); }
  bool getDhwHeatScheduleEnabled() { begin(); return g_dhwHeatScheduleEnabled; }
  void setDhwHeatScheduleEnabled(bool v) { begin(); g_dhwHeatScheduleEnabled = v; saveBool(K_DHW_H_SEN, v); }
  float getDhwHeatTargetTempC() { begin(); return g_dhwHeatTargetTempC; }
  void setDhwHeatTargetTempC(float v) { begin(); if (v < 20.0f) v = 20.0f; if (v > 80.0f) v = 80.0f; g_dhwHeatTargetTempC = v; saveFloat(K_DHW_H_TG, v); }
  float getDhwHeatHysteresisC() { begin(); return g_dhwHeatHysteresisC; }
  void setDhwHeatHysteresisC(float v) { begin(); if (v < 0.5f) v = 0.5f; if (v > 15.0f) v = 15.0f; g_dhwHeatHysteresisC = v; saveFloat(K_DHW_H_HY, v); }
  String getDhwHeatRequestMode() { begin(); return g_dhwHeatRequestMode; }
  void setDhwHeatRequestMode(const String& v) { begin(); if (v != "relay" && v != "opentherm") return; g_dhwHeatRequestMode = v; saveString(K_DHW_H_RM, v); }
  bool getDhwHeatOtEnableDhw() { begin(); return g_dhwHeatOtEnableDhw; }
  void setDhwHeatOtEnableDhw(bool v) { begin(); g_dhwHeatOtEnableDhw = v; saveBool(K_DHW_H_ODE, v); }
  float getDhwHeatOtDhwSetpointC() { begin(); return g_dhwHeatOtDhwSetpointC; }
  void setDhwHeatOtDhwSetpointC(float v) { begin(); if (v < 20.0f) v = 20.0f; if (v > 80.0f) v = 80.0f; g_dhwHeatOtDhwSetpointC = v; saveFloat(K_DHW_H_ODS, v); }
  bool getDhwHeatRelayRequest() { begin(); return g_dhwHeatRelayRequest; }
  void setDhwHeatRelayRequest(bool v) { begin(); g_dhwHeatRelayRequest = v; saveBool(K_DHW_H_RRQ, v); }
  bool getDhwHeatDriveValveRelay() { begin(); return g_dhwHeatDriveValveRelay; }
  void setDhwHeatDriveValveRelay(bool v) { begin(); g_dhwHeatDriveValveRelay = v; saveBool(K_DHW_H_DVR, v); }
  uint8_t getDhwHeatValveRelayIndex() { begin(); return g_dhwHeatValveRelayIndex; }
  void setDhwHeatValveRelayIndex(uint8_t idx) { begin(); if (idx > 7) idx = 2; g_dhwHeatValveRelayIndex = idx; saveUInt(K_DHW_H_VR, idx); }
  uint8_t getDhwHeatBoilerRelayIndex() { begin(); return g_dhwHeatBoilerRelayIndex; }
  void setDhwHeatBoilerRelayIndex(uint8_t idx) { begin(); if (idx > 7) idx = 4; g_dhwHeatBoilerRelayIndex = idx; saveUInt(K_DHW_H_BR, idx); }
  uint32_t getDhwHeatValveLeadMs() { begin(); return g_dhwHeatValveLeadMs; }
  void setDhwHeatValveLeadMs(uint32_t v) { begin(); if (v > 60000) v = 60000; g_dhwHeatValveLeadMs = v; saveUInt(K_DHW_H_VLD, v); }
  uint32_t getDhwHeatValveSwitchBackMs() { begin(); return g_dhwHeatValveSwitchBackMs; }
  void setDhwHeatValveSwitchBackMs(uint32_t v) { begin(); if (v > 60000) v = 60000; g_dhwHeatValveSwitchBackMs = v; saveUInt(K_DHW_H_VBK, v); }
  uint32_t getDhwHeatBoilerOffHoldMs() { begin(); return g_dhwHeatBoilerOffHoldMs; }
  void setDhwHeatBoilerOffHoldMs(uint32_t v) { begin(); if (v > 60000) v = 60000; g_dhwHeatBoilerOffHoldMs = v; saveUInt(K_DHW_H_BOH, v); }
  String getDhwHeatScheduleJson() { begin(); return g_dhwHeatScheduleJson; }
  void setDhwHeatScheduleJson(const String& v) { begin(); g_dhwHeatScheduleJson = v; saveString(K_DHW_H_SJ, v); }
  bool getDhwCircUseInput() { begin(); return g_dhwCircUseInput; }
  void setDhwCircUseInput(bool v) { begin(); g_dhwCircUseInput = v; saveBool(K_DHW_C_IN, v); }
  bool getDhwCircUseSchedule() { begin(); return g_dhwCircUseSchedule; }
  void setDhwCircUseSchedule(bool v) { begin(); g_dhwCircUseSchedule = v; saveBool(K_DHW_C_SC, v); }
  bool getDhwCircScheduleEnabled() { begin(); return g_dhwCircScheduleEnabled; }
  void setDhwCircScheduleEnabled(bool v) { begin(); g_dhwCircScheduleEnabled = v; saveBool(K_DHW_C_SEN, v); }
  bool getDhwCircPulseEnabled() { begin(); return g_dhwCircPulseEnabled; }
  void setDhwCircPulseEnabled(bool v) { begin(); g_dhwCircPulseEnabled = v; saveBool(K_DHW_C_PE, v); }
  uint16_t getDhwCircPulseOnMin() { begin(); return (uint16_t)g_dhwCircPulseOnMin; }
  void setDhwCircPulseOnMin(uint16_t v) { begin(); if (v > 1440) v = 1440; g_dhwCircPulseOnMin = v; saveUInt(K_DHW_C_PON, v); }
  uint16_t getDhwCircPulseOffMin() { begin(); return (uint16_t)g_dhwCircPulseOffMin; }
  void setDhwCircPulseOffMin(uint16_t v) { begin(); if (v > 1440) v = 1440; g_dhwCircPulseOffMin = v; saveUInt(K_DHW_C_POF, v); }
  uint8_t getDhwCircRelayIndex() { begin(); return g_dhwCircRelayIndex; }
  void setDhwCircRelayIndex(uint8_t idx) { begin(); if (idx > 7) idx = 3; g_dhwCircRelayIndex = idx; saveUInt(K_DHW_C_RI, idx); }
  String getDhwCircScheduleJson() { begin(); return g_dhwCircScheduleJson; }
  void setDhwCircScheduleJson(const String& v) { begin(); g_dhwCircScheduleJson = v; saveString(K_DHW_C_SJ, v); }

  bool getDhwAntiLegionellaEnabled() { begin(); return g_dhwAlEnabled; }
  void setDhwAntiLegionellaEnabled(bool v) { begin(); g_dhwAlEnabled = v; saveBool(K_DHW_AL_EN, v); }
  uint8_t getDhwAntiLegionellaWeekday() { begin(); return (uint8_t)g_dhwAlWeekday; }
  void setDhwAntiLegionellaWeekday(uint8_t v) { begin(); if (v > 6) v = 0; g_dhwAlWeekday = v; saveUInt(K_DHW_AL_WD, v); }
  uint16_t getDhwAntiLegionellaStartMin() { begin(); return (uint16_t)g_dhwAlStartMin; }
  void setDhwAntiLegionellaStartMin(uint16_t v) { begin(); if (v > 1439) v = 1439; g_dhwAlStartMin = v; saveUInt(K_DHW_AL_SM, v); }
  float getDhwAntiLegionellaTargetTempC() { begin(); return g_dhwAlTargetTempC; }
  void setDhwAntiLegionellaTargetTempC(float v) { begin(); g_dhwAlTargetTempC = v; saveFloat(K_DHW_AL_TG, v); }
  uint16_t getDhwAntiLegionellaHoldMin() { begin(); return (uint16_t)g_dhwAlHoldMin; }
  void setDhwAntiLegionellaHoldMin(uint16_t v) { begin(); g_dhwAlHoldMin = v; saveUInt(K_DHW_AL_HM, v); }
  uint32_t getDhwAntiLegionellaLastDayKey() { begin(); return g_dhwAlLastDayKey; }
  void setDhwAntiLegionellaLastDayKey(uint32_t v) { begin(); g_dhwAlLastDayKey = v; saveUInt(K_DHW_AL_DK, v); }

  // OTA
  bool getOtaEnabled() { begin(); return g_otaEnabled; }
  void setOtaEnabled(bool v) { begin(); g_otaEnabled = v; saveBool(K_OTA_EN, v); }

  String getOtaHostname() { begin(); return g_otaHostname; }
  void setOtaHostname(const String& v) { begin(); g_otaHostname = v; saveString(K_OTA_HOST, v); }

  uint16_t getOtaPort() { begin(); return (uint16_t)g_otaPort; }
  void setOtaPort(uint16_t v) {
    begin();
    uint32_t p = (uint32_t)v;
    if (p < 1024) p = 3232;
    if (p > 65535) p = 65535;
    g_otaPort = p;
    saveUInt(K_OTA_PORT, p);
  }

  String getOtaPassword() { begin(); return g_otaPassword; }
  void setOtaPassword(const String& v) { begin(); g_otaPassword = v; saveString(K_OTA_PASS, v); }

  // MQTT
  bool getMqttEnabled() { begin(); return g_mqttEnabled; }
  void setMqttEnabled(bool v) { begin(); g_mqttEnabled = v; saveBool(K_MQ_EN, v); }

  String getMqttHost() { begin(); return g_mqttHost; }
  void setMqttHost(const String& v) { begin(); g_mqttHost = v; g_mqttHost.trim(); saveString(K_MQ_HOST, g_mqttHost); }

  uint16_t getMqttPort() { begin(); return (uint16_t)g_mqttPort; }
  void setMqttPort(uint16_t v) {
    begin();
    uint32_t p = (uint32_t)v;
    if (!p || p > 65535) p = 1883;
    g_mqttPort = p;
    saveUInt(K_MQ_PORT, p);
  }

  String getMqttUsername() { begin(); return g_mqttUsername; }
  void setMqttUsername(const String& v) { begin(); g_mqttUsername = v; g_mqttUsername.trim(); saveString(K_MQ_USER, g_mqttUsername); }

  String getMqttPassword() { begin(); return g_mqttPassword; }
  void setMqttPassword(const String& v) { begin(); g_mqttPassword = v; saveString(K_MQ_PASS, g_mqttPassword); }

  String getMqttClientId() { begin(); return g_mqttClientId; }
  void setMqttClientId(const String& v) {
    begin();
    g_mqttClientId = v;
    g_mqttClientId.trim();
    if (!g_mqttClientId.length()) g_mqttClientId = "esp32-controller";
    saveString(K_MQ_CID, g_mqttClientId);
  }

  String getMqttBaseTopic() { begin(); return g_mqttBaseTopic; }
  void setMqttBaseTopic(const String& v) {
    begin();
    g_mqttBaseTopic = v;
    g_mqttBaseTopic.trim();
    while (g_mqttBaseTopic.startsWith("/")) g_mqttBaseTopic.remove(0, 1);
    while (g_mqttBaseTopic.endsWith("/")) g_mqttBaseTopic.remove(g_mqttBaseTopic.length() - 1);
    if (!g_mqttBaseTopic.length()) g_mqttBaseTopic = "esp32-controller";
    saveString(K_MQ_BASE, g_mqttBaseTopic);
  }

  uint32_t getMqttPublishIntervalMs() { begin(); return g_mqttPublishIntervalMs; }
  void setMqttPublishIntervalMs(uint32_t v) {
    begin();
    if (v < 1000) v = 1000;
    if (v > 600000) v = 600000;
    g_mqttPublishIntervalMs = v;
    saveUInt(K_MQ_PMS, v);
  }

  bool getMqttHaEnabled() { begin(); return g_mqttHaEnabled; }
  void setMqttHaEnabled(bool v) { begin(); g_mqttHaEnabled = v; saveBool(K_MQ_HAEN, v); }

  bool getMqttHaDiscovery() { begin(); return g_mqttHaDiscovery; }
  void setMqttHaDiscovery(bool v) { begin(); g_mqttHaDiscovery = v; saveBool(K_MQ_DISC, v); }

  String getMqttDiscoveryPrefix() { begin(); return g_mqttDiscoveryPrefix; }
  void setMqttDiscoveryPrefix(const String& v) {
    begin();
    g_mqttDiscoveryPrefix = v;
    g_mqttDiscoveryPrefix.trim();
    while (g_mqttDiscoveryPrefix.startsWith("/")) g_mqttDiscoveryPrefix.remove(0, 1);
    while (g_mqttDiscoveryPrefix.endsWith("/")) g_mqttDiscoveryPrefix.remove(g_mqttDiscoveryPrefix.length() - 1);
    if (!g_mqttDiscoveryPrefix.length()) g_mqttDiscoveryPrefix = "homeassistant";
    saveString(K_MQ_DPRE, g_mqttDiscoveryPrefix);
  }

  String getMqttNodeId() { begin(); return g_mqttNodeId; }
  void setMqttNodeId(const String& v) {
    begin();
    g_mqttNodeId = v;
    g_mqttNodeId.trim();
    while (g_mqttNodeId.startsWith("/")) g_mqttNodeId.remove(0, 1);
    while (g_mqttNodeId.endsWith("/")) g_mqttNodeId.remove(g_mqttNodeId.length() - 1);
    if (!g_mqttNodeId.length()) g_mqttNodeId = "esp32_controller";
    saveString(K_MQ_NODE, g_mqttNodeId);
  }
  bool getPressureAlarmEnabled() { begin(); return g_pressureAlarmEnabled; }
  void setPressureAlarmEnabled(bool v) { begin(); g_pressureAlarmEnabled = v; saveBool(K_PAL_EN, v); }
  float getPressureAlarmMinBar() { begin(); return g_pressureAlarmMinBar; }
  void setPressureAlarmMinBar(float v) { begin(); if (v < 0.1f) v = 0.1f; if (v > 6.0f) v = 6.0f; g_pressureAlarmMinBar = v; saveFloat(K_PAL_MIN, v); }
  float getPressureAlarmMaxBar() { begin(); return g_pressureAlarmMaxBar; }
  void setPressureAlarmMaxBar(float v) { begin(); if (v < 0.1f) v = 0.1f; if (v > 6.0f) v = 6.0f; g_pressureAlarmMaxBar = v; saveFloat(K_PAL_MAX, v); }
  float getPressureAlarmHysteresisBar() { begin(); return g_pressureAlarmHysteresisBar; }
  void setPressureAlarmHysteresisBar(float v) { begin(); if (v < 0.01f) v = 0.01f; if (v > 1.0f) v = 1.0f; g_pressureAlarmHysteresisBar = v; saveFloat(K_PAL_HYS, v); }
}
