#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <Adafruit_HTU21DF.h>
#include <BH1750.h>
#include <Adafruit_BMP085.h>
#include <esp_mac.h>

// -------------------- I2C pins (uprav podle své desky) --------------------
#ifndef I2C_SDA_PIN
  #define I2C_SDA_PIN 8
#endif
#ifndef I2C_SCL_PIN
  #define I2C_SCL_PIN 9
#endif

// -------------------- BLE UUIDs (musí sedět s ESP32-S3 projektem) --------------------
static NimBLEUUID UUID_SVC_METEO("7b7c1001-3a2b-4f2a-8bb0-8d2c2c1a1001");
static NimBLEUUID UUID_CH_METEO ("7b7c1002-3a2b-4f2a-8bb0-8d2c2c1a1001"); // notify

// DŮLEŽITÉ: prefix musí být "ESP-Meteostanice"
static const char* DEVICE_NAME = "ESP-Meteostanice-Outdoor";

// Periodiky (bez delay)
static const uint32_t SENSOR_PERIOD_MS = 2000;
static const uint32_t NOTIFY_PERIOD_MS = 5000;

// Po startu chceme rychlejší měření/broadcast
static const uint32_t FAST_START_MS = 10000;           // 10 s po boot
static const uint32_t FAST_SENSOR_PERIOD_MS = 300;     // častější čtení
static const uint32_t FAST_ADV_BCAST_PERIOD_MS = 1000; // rychlejší broadcast

// Broadcast do advertising (Manufacturer Data)
static const uint32_t ADV_BCAST_PERIOD_MS = 5000;
static const uint16_t METEO_MFG_ID = 0xFFFE;
static const uint8_t  METEO_BCAST_VER = 1;

// --- BLE Mesh config (origin node) ---
static const uint8_t  MESH_TTL_DEFAULT = 3;

// -------------------- Senzory --------------------
static Adafruit_HTU21DF g_htu;
static BH1750 g_bh1750;
static Adafruit_BMP085 g_bmp;

static bool g_hasHTU = false;
static bool g_hasBH  = false;
static bool g_hasBMP = false;

// -------------------- BLE server --------------------
static NimBLEServer* g_server = nullptr;
static NimBLEService* g_svc = nullptr;
static NimBLECharacteristic* g_chMeteo = nullptr;

static volatile bool g_connected = false;
static volatile bool g_advRestartPending = false;

static NimBLEAdvertising* g_adv = nullptr;
static NimBLEAdvertisementData g_advData;
static NimBLEAdvertisementData g_scanData;
static uint32_t g_nextAdvBcastMs = 0;

// --- BLE Mesh (origin) fields ---
static uint8_t  g_originMac[6] = {0};
static uint16_t g_meshSeq = 1;

// --- časovače pro notify po connectu (MUSÍ být deklarované před callbacky) ---
static uint32_t g_nextSensorMs = 0;
static uint32_t g_nextNotifyMs = 0;

static uint32_t g_firstNotifyAfterConnectMs = 0;
static bool     g_firstNotifyAfterConnectDone = false;

// “kick” logika
static bool     g_kickPending = true;
static bool     g_kickSent = true;
static bool     g_kickSentAfterConnect = false;
static volatile bool g_kickAfterConnectPending = false;

// Payload přesně 6B dle S3 meteoOnNotify():
// int16 temp_x10 LE, uint8 hum, uint16 press_hPa LE, int8 trend
static uint8_t  g_frame[6];
static bool     g_haveReading = false;

// poslední hodnoty
static int16_t   g_tempX10 = 0;
static uint8_t   g_hum = 0;
static uint16_t  g_pressHpa = 0;
static int8_t    g_trend = 0;
static uint16_t  g_lux = 0; // debug

// --- Advertising watchdog ---
static const uint32_t ADV_ENSURE_PERIOD_MS = 4000;
static uint32_t g_nextAdvEnsureMs = 0;
static uint32_t g_lastAdvStartMs = 0;

static void bleRequestAdvRestart() {
  g_advRestartPending = true;
}

static void bleEnsureAdvertising(uint32_t now, const char* reason) {
  if ((int32_t)(now - g_lastAdvStartMs) < 500) return;
  g_lastAdvStartMs = now;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::startAdvertising();

  if (reason) {
    Serial.printf("[BLE] Advertising ensure/start (%s)\n", reason);
  }
}

// CRC8 Dallas/Maxim (poly 0x31 reflected => 0x8C)
static uint8_t crc8_maxim(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t inbyte = data[i];
    for (uint8_t j = 0; j < 8; j++) {
      uint8_t mix = (crc ^ inbyte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

static void bleUpdateAdvBroadcast() {
  if (!g_adv) return;
  if (!g_haveReading) return;

  // Manufacturer data:
  // [companyIdLE][ver][ttl][hops][seqLE][originMac6][temp_x10LE][hum][pressLE][crc8]
  uint8_t md[19];
  md[0] = (uint8_t)(METEO_MFG_ID & 0xFF);
  md[1] = (uint8_t)((METEO_MFG_ID >> 8) & 0xFF);
  md[2] = METEO_BCAST_VER;
  md[3] = MESH_TTL_DEFAULT;
  md[4] = 0; // hops (origin)

  const uint16_t seq = g_meshSeq++;
  md[5] = (uint8_t)(seq & 0xFF);
  md[6] = (uint8_t)((seq >> 8) & 0xFF);

  for (int i = 0; i < 6; i++) md[7 + i] = g_originMac[i];

  md[13] = g_frame[0];
  md[14] = g_frame[1];
  md[15] = g_frame[2];
  md[16] = g_frame[3];
  md[17] = g_frame[4];
  md[18] = crc8_maxim(md, 18);

  g_advData.setManufacturerData(std::string((const char*)md, sizeof(md)));
  g_adv->setAdvertisementData(g_advData);
}

static void bleUpdateCharacteristicCachedValue() {
  if (!g_chMeteo) return;
  if (!g_haveReading) return;
  g_chMeteo->setValue(g_frame, sizeof(g_frame));
}

static void meteoNotifyNow(const char* reason) {
  if (!g_connected || !g_haveReading || !g_chMeteo) return;

  g_chMeteo->setValue(g_frame, sizeof(g_frame));
  bool ok = g_chMeteo->notify();
  Serial.printf("[BLE] notify %s (len=%u) reason=%s\n", ok ? "OK" : "FAIL", (unsigned)sizeof(g_frame), reason ? reason : "-");
}

// -------------------- Trend z tlaku (jednoduché) --------------------
struct PressSample { uint32_t ms; uint16_t hpa; };
static const size_t PRESS_BUF_SZ = 64;
static PressSample g_pressBuf[PRESS_BUF_SZ];
static size_t g_pressHead = 0;
static bool g_pressInit = false;

static void pressHistoryPush(uint16_t hpa, uint32_t now) {
  g_pressBuf[g_pressHead] = { now, hpa };
  g_pressHead = (g_pressHead + 1) % PRESS_BUF_SZ;
  g_pressInit = true;
}

static bool pressHistoryGetOlder(uint32_t now, uint32_t ageMs, uint16_t& outHpa) {
  if (!g_pressInit) return false;

  const uint32_t target = now - ageMs;
  bool found = false;
  uint32_t bestMs = 0;
  uint16_t bestHpa = 0;

  for (size_t i = 0; i < PRESS_BUF_SZ; i++) {
    const PressSample& s = g_pressBuf[i];
    if (s.ms == 0) continue;
    if ((int32_t)(s.ms - target) <= 0) { // s.ms <= target
      if (!found || (int32_t)(s.ms - bestMs) > 0) {
        found = true;
        bestMs = s.ms;
        bestHpa = s.hpa;
      }
    }
  }
  if (!found) return false;
  outHpa = bestHpa;
  return true;
}

static int8_t computeTrend(uint16_t currentHpa, uint32_t now) {
  uint16_t oldHpa = 0;
  if (!pressHistoryGetOlder(now, 10UL * 60UL * 1000UL, oldHpa)) return 0;

  int16_t delta = (int16_t)currentHpa - (int16_t)oldHpa;
  if (delta >= 2) return +1;
  if (delta <= -2) return -1;
  return 0;
}

// -------------------- BLE callbacks --------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    (void)s; (void)connInfo;
    g_connected = true;

    // jednorázový kick po connectu
    g_kickSentAfterConnect = false;
    g_kickAfterConnectPending = true;

    // první notify po connectu se zpožděním (čas na CCCD subscribe)
    const uint32_t now = millis();
    g_firstNotifyAfterConnectMs = now + 1200;
    g_firstNotifyAfterConnectDone = false;

    // a rychlé „oživení“ periodiky po connectu
    g_nextNotifyMs = now + 1500;

    Serial.println("[BLE] Client connected");
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)connInfo;
    Serial.printf("[BLE] Client disconnected (reason=%d)\n", reason);
    g_connected = false;

    // reset plánování po connectu
    g_firstNotifyAfterConnectDone = false;
    g_firstNotifyAfterConnectMs = 0;

    bleRequestAdvRestart();
  }
};

static ServerCallbacks g_srvCbs;

// -------------------- Čtení senzorů --------------------
static bool readSensorsOnce(uint32_t now) {
  float tC = NAN;
  float h  = NAN;

  if (g_hasHTU) {
    tC = g_htu.readTemperature();
    h  = g_htu.readHumidity();
  }

  int32_t pressPa = -1;
  if (g_hasBMP) {
    pressPa = g_bmp.readPressure(); // Pa
  }

  float luxF = NAN;
  if (g_hasBH) {
    luxF = g_bh1750.readLightLevel();
  }

  bool anyOk = false;

  if (isfinite(tC)) {
    int32_t tx10 = (int32_t)lroundf(tC * 10.0f);
    tx10 = constrain(tx10, -400, 850);
    g_tempX10 = (int16_t)tx10;
    anyOk = true;
  }

  if (isfinite(h)) {
    int32_t hi = (int32_t)lroundf(h);
    hi = constrain(hi, 0, 100);
    g_hum = (uint8_t)hi;
    anyOk = true;
  }

  if (pressPa > 0) {
    uint32_t hPa = (uint32_t)(pressPa / 100); // Pa -> hPa
    hPa = constrain(hPa, 300, 1100);
    g_pressHpa = (uint16_t)hPa;
    pressHistoryPush(g_pressHpa, now);
    g_trend = computeTrend(g_pressHpa, now);
    anyOk = true;
  }

  if (isfinite(luxF)) {
    uint32_t lux = (uint32_t)lroundf(luxF);
    lux = min<uint32_t>(lux, 65535);
    g_lux = (uint16_t)lux;
    anyOk = true;
  }

  if (!anyOk && !g_haveReading) return false;

  // 6B rámec pro S3
  g_frame[0] = (uint8_t)(g_tempX10 & 0xFF);
  g_frame[1] = (uint8_t)((g_tempX10 >> 8) & 0xFF);
  g_frame[2] = g_hum;
  g_frame[3] = (uint8_t)(g_pressHpa & 0xFF);
  g_frame[4] = (uint8_t)((g_pressHpa >> 8) & 0xFF);
  g_frame[5] = (uint8_t)g_trend;

  g_haveReading = true;
  bleUpdateCharacteristicCachedValue();
  return true;
}

// -------------------- BLE init --------------------
static void bleInit() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_srvCbs);

  g_svc = g_server->createService(UUID_SVC_METEO);

  g_chMeteo = g_svc->createCharacteristic(
    UUID_CH_METEO,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );

  uint8_t zero6[6] = {0};
  g_chMeteo->setValue(zero6, sizeof(zero6));

  g_svc->start();

  g_adv = NimBLEDevice::getAdvertising();

  g_advData = NimBLEAdvertisementData();
  g_advData.setFlags(0x06);
  g_advData.addServiceUUID(UUID_SVC_METEO);

  g_scanData = NimBLEAdvertisementData();
  g_scanData.setName(DEVICE_NAME);

  bleUpdateAdvBroadcast();

  g_adv->setAdvertisementData(g_advData);
  g_adv->setScanResponseData(g_scanData);

  g_adv->setMinInterval(0x20);
  g_adv->setMaxInterval(0x40);

  bleEnsureAdvertising(millis(), "init");
  Serial.println("[BLE] Advertising started");
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP32-C3 BLE Meteo Sensor starting...");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);

  esp_read_mac(g_originMac, ESP_MAC_BT);
  Serial.printf("[BLE] Origin MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    g_originMac[0], g_originMac[1], g_originMac[2], g_originMac[3], g_originMac[4], g_originMac[5]
  );

  g_hasHTU = g_htu.begin();
  Serial.printf("[I2C] HTU21D: %s\n", g_hasHTU ? "OK" : "NOT FOUND");

  g_hasBH = g_bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.printf("[I2C] BH1750: %s\n", g_hasBH ? "OK" : "NOT FOUND");

  g_hasBMP = g_bmp.begin();
  Serial.printf("[I2C] BMP085: %s\n", g_hasBMP ? "OK" : "NOT FOUND");

  g_kickPending = true;
  g_kickSent = false;
  g_kickSentAfterConnect = false;

  const uint32_t now = millis();
  g_nextAdvBcastMs = now + 200;

  readSensorsOnce(now);
  bleInit();
  bleUpdateCharacteristicCachedValue();

  g_nextSensorMs = now + 100;
  g_nextNotifyMs = now + NOTIFY_PERIOD_MS;

  g_nextAdvEnsureMs = now + 500;
}

void loop() {
  const uint32_t now = millis();
  const uint32_t up = now;

  // Okamžitý kick po connectu (cached data)
  if (g_connected && g_haveReading) {
    const bool needKick = (g_kickPending && !g_kickSent) || (g_kickAfterConnectPending && !g_kickSentAfterConnect);
    if (needKick) {
      meteoNotifyNow("kick_immediate");
      g_kickSent = true;
      g_kickPending = false;
      g_kickSentAfterConnect = true;
      g_kickAfterConnectPending = false;
    }
  }

  // První notify po connectu se zpožděním (čas na CCCD subscribe)
  if (g_connected && g_haveReading && !g_firstNotifyAfterConnectDone && g_firstNotifyAfterConnectMs != 0) {
    if ((int32_t)(now - g_firstNotifyAfterConnectMs) >= 0) {
      meteoNotifyNow("first_after_connect_delayed");
      g_firstNotifyAfterConnectDone = true;
    }
  }

  // čtení senzorů
  if ((int32_t)(now - g_nextSensorMs) >= 0) {
    const uint32_t sp = (up < FAST_START_MS) ? FAST_SENSOR_PERIOD_MS : SENSOR_PERIOD_MS;
    g_nextSensorMs = now + sp;

    const bool ok = readSensorsOnce(now);

    if (ok) {
      Serial.printf("[DATA] T=%.1fC H=%u%% P=%uhPa trend=%d lux=%u\n",
        (float)g_tempX10 / 10.0f,
        (unsigned)g_hum,
        (unsigned)g_pressHpa,
        (int)g_trend,
        (unsigned)g_lux
      );

      // cached value pro read/notify
      bleUpdateCharacteristicCachedValue();

      // po změně dat obnov ADV payload
      if (!g_connected) {
        bleUpdateAdvBroadcast();
        bleRequestAdvRestart();
      }
    }
  }

  // broadcast do advertising (bez spojení)
  if (!g_connected && g_haveReading && (int32_t)(now - g_nextAdvBcastMs) >= 0) {
    const uint32_t bp = (up < FAST_START_MS) ? FAST_ADV_BCAST_PERIOD_MS : ADV_BCAST_PERIOD_MS;
    g_nextAdvBcastMs = now + bp;
    bleUpdateAdvBroadcast();
    bleRequestAdvRestart();
  }

  // periodické notify
  if ((int32_t)(now - g_nextNotifyMs) >= 0) {
    g_nextNotifyMs = now + NOTIFY_PERIOD_MS;
    if (g_connected && g_haveReading) {
      meteoNotifyNow("periodic");
    }
  }

  // housekeeping advertising
  if (g_advRestartPending) {
    g_advRestartPending = false;
    bleEnsureAdvertising(now, "pending_restart");
  }

  if (!g_connected && (int32_t)(now - g_nextAdvEnsureMs) >= 0) {
    g_nextAdvEnsureMs = now + ADV_ENSURE_PERIOD_MS;
    bleEnsureAdvertising(now, "periodic_ensure");
  }
}
