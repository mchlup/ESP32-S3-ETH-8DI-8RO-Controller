#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <Adafruit_HTU21DF.h>
#include <BH1750.h>
#include <Adafruit_BMP085.h>

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

// Broadcast do advertising (Manufacturer Data) každých 5s:
// Format (little-endian):
//   uint16 companyId (0xFFFF)
//   uint8  version (1)
//   int16  temp_x10
//   uint8  hum_pct
//   uint16 press_hPa
static const uint32_t ADV_BCAST_PERIOD_MS = 5000;
static const uint16_t METEO_MFG_ID = 0xFFFF;
static const uint8_t  METEO_BCAST_VER = 1;

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

// BLE housekeeping (bez delay):
// - po disconnectu NESPOUŠTĚT advertising přímo z callbacku (v praxi se občas stává,
//   že startAdvertising() z callback kontextu neproběhne spolehlivě a zařízení přestane
//   být nalezitelné, dokud se nerestartuje).
// - v loopu pravidelně "hlídat" že když není klient připojený, tak periferie opravdu inzeruje.
static const uint32_t ADV_ENSURE_PERIOD_MS = 4000;
static uint32_t g_nextAdvEnsureMs = 0;
static uint32_t g_lastAdvStartMs = 0;

static void bleRequestAdvRestart() {
  g_advRestartPending = true;
}

static void bleEnsureAdvertising(uint32_t now, const char* reason) {
  // jednoduchý debounce, aby se to nespouštělo 100× za sekundu
  if ((int32_t)(now - g_lastAdvStartMs) < 500) return;
  g_lastAdvStartMs = now;

  // Restart advertising to ensure updated payload is broadcasted reliably.
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::startAdvertising();

  if (reason) {
    Serial.printf("[BLE] Advertising ensure/start (%s)\n", reason);
  }
}

// Payload přesně 6B dle S3 meteoOnNotify():
// int16 temp_x10 LE, uint8 hum, uint16 press_hPa LE, int8 trend
static uint8_t  g_frame[6];
static bool     g_haveReading = false;

static void bleUpdateAdvBroadcast() {
  if (!g_adv) return;
  if (!g_haveReading) return;

  // Manufacturer data = [companyIdLE][ver][temp_x10LE][hum][press_hPaLE]
  uint8_t md[2 + 1 + 2 + 1 + 2];
  md[0] = (uint8_t)(METEO_MFG_ID & 0xFF);
  md[1] = (uint8_t)((METEO_MFG_ID >> 8) & 0xFF);
  md[2] = METEO_BCAST_VER;
  md[3] = g_frame[0];
  md[4] = g_frame[1];
  md[5] = g_frame[2];
  md[6] = g_frame[3];
  md[7] = g_frame[4];

  g_advData.setManufacturerData(std::string((const char*)md, sizeof(md)));
  g_adv->setAdvertisementData(g_advData);
}

// “kick” logika: pošli 1× notify hned po prvním validním měření
static bool     g_kickPending = true;   // po startu čekáme na první validní data
static bool     g_kickSent = true;     // jednorázově po startu
static bool     g_kickSentAfterConnect = false; // jednorázově po connectu (pokud klient přijde pozdě)
static volatile bool g_kickAfterConnectPending = false; // notify hned po připojení (bez čekání na další čtení senzorů)

// poslední hodnoty
static int16_t   g_tempX10 = 0;
static uint8_t   g_hum = 0;
static uint16_t  g_pressHpa = 0;
static int8_t    g_trend = 0;
static uint16_t  g_lux = 0; // jen debug (na S3 se teď neposílá)

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

// -------------------- Notify helper --------------------
static void meteoNotifyNow(const char* reason) {
  if (!g_connected || !g_haveReading || !g_chMeteo) return;

  g_chMeteo->setValue(g_frame, sizeof(g_frame));
  bool ok = g_chMeteo->notify();
  Serial.printf("[BLE] notify %s (len=%u) reason=%s\n", ok ? "OK" : "FAIL", (unsigned)sizeof(g_frame), reason ? reason : "-");
}

// -------------------- BLE callbacks --------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    (void)s; (void)connInfo;
    g_connected = true;

    // kdyby byl pending restart advertisingu, tak ho po connectu zahodíme
    g_advRestartPending = false;

    // klient se právě připojil -> dovolíme jednorázový "kick" po connectu
    g_kickSentAfterConnect = false;
    g_kickAfterConnectPending = true;

    Serial.println("[BLE] Client connected");
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)connInfo;
    Serial.printf("[BLE] Client disconnected (reason=%d)\n", reason);
    g_connected = false;

    // po disconnectu znovu inzerovat (odloženo do loopu pro stabilitu)
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
    g_lux = (uint16_t)lux; // jen debug
    anyOk = true;
  }

  if (!anyOk && !g_haveReading) return false;

  // Sestavit přesně 6B rámec pro S3
  g_frame[0] = (uint8_t)(g_tempX10 & 0xFF);
  g_frame[1] = (uint8_t)((g_tempX10 >> 8) & 0xFF);
  g_frame[2] = g_hum;
  g_frame[3] = (uint8_t)(g_pressHpa & 0xFF);
  g_frame[4] = (uint8_t)((g_pressHpa >> 8) & 0xFF);
  g_frame[5] = (uint8_t)g_trend;

  g_haveReading = true;

  // Udržuj hodnotu charakteristiky vždy aktuální, aby si ji klient (S3) mohl přečíst ihned po connectu
  // i v případě, že ještě neproběhl notify.
  if (g_chMeteo) {
    g_chMeteo->setValue(g_frame, sizeof(g_frame));
  }
  return true;
}

// -------------------- BLE init (NimBLE-Arduino kompatibilní) --------------------
static void bleInit() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Stabilnější pro klienta: PUBLIC address
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

  // relaxed security
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

// Embed first broadcast payload (if already available)
bleUpdateAdvBroadcast();

g_adv->setAdvertisementData(g_advData);
g_adv->setScanResponseData(g_scanData);

  // Krátký advertising interval = rychlejší objevení a připojení po restartu S3.
  // Jednotky: 0.625ms. 0x20=20ms, 0x40=40ms.
  g_adv->setMinInterval(0x20);
  g_adv->setMaxInterval(0x40);

  bleEnsureAdvertising(millis(), "init");
  Serial.println("[BLE] Advertising started");
}

// -------------------- Setup / Loop (non-blocking) --------------------
static uint32_t g_nextSensorMs = 0;
static uint32_t g_nextNotifyMs = 0;

void setup() {
  Serial.begin(115200);
  // žádné zbytečné delay - necháme to naběhnout neblokujícím způsobem
  Serial.println();
  Serial.println("ESP32-C3 BLE Meteo Sensor starting...");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);

  g_hasHTU = g_htu.begin();
  Serial.printf("[I2C] HTU21D: %s\n", g_hasHTU ? "OK" : "NOT FOUND");

  g_hasBH = g_bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.printf("[I2C] BH1750: %s\n", g_hasBH ? "OK" : "NOT FOUND");

  g_hasBMP = g_bmp.begin();
  Serial.printf("[I2C] BMP085: %s\n", g_hasBMP ? "OK" : "NOT FOUND");

  // kick po startu chceme vždy
  g_kickPending = true;
  g_kickSent = false;
  g_kickSentAfterConnect = false;

  bleInit();

  const uint32_t now = millis();
  g_nextAdvBcastMs = now + 200;
  // První pokus o čtení hned po startu: characteristic dostane validní data co nejdřív.
  readSensorsOnce(now);

  // Další čtení brzo znovu (pro případ, že senzory ještě nebyly ready).
  g_nextSensorMs = now + 250;
  g_nextNotifyMs = now + NOTIFY_PERIOD_MS; // první periodický notify až za interval

  // watchdog inzerce
  g_nextAdvEnsureMs = now + 500;
}

void loop() {
  const uint32_t now = millis();

  // Pokud se klient (S3) připojí mezi dvěma měřeními, pošli poslední známý rámec okamžitě.
  // Tím se výrazně zkrátí doba do prvních validních dat po restartu S3.
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

  // čtení senzorů
  if ((int32_t)(now - g_nextSensorMs) >= 0) {
    g_nextSensorMs = now + SENSOR_PERIOD_MS;

    const bool ok = readSensorsOnce(now);

    if (ok) {
      Serial.printf("[DATA] T=%.1fC H=%u%% P=%uhPa trend=%d lux=%u\n",
        (float)g_tempX10 / 10.0f,
        (unsigned)g_hum,
        (unsigned)g_pressHpa,
        (int)g_trend,
        (unsigned)g_lux
      );

      // Prime adv-broadcast immediately after the first valid reading (no need to wait for the periodic bcast tick).
      if (!g_connected && g_haveReading && (int32_t)(now - g_nextAdvBcastMs) < 0) {
        bleUpdateAdvBroadcast();
        bleRequestAdvRestart();
        g_nextAdvBcastMs = now + ADV_BCAST_PERIOD_MS;
      }

      // 1) Kick po startu: první validní data -> okamžitě notify (pokud je klient už připojený)
      if (g_kickPending && !g_kickSent && g_connected) {
        meteoNotifyNow("kick_start_first_valid");
        g_kickSent = true;
        g_kickPending = false;
      }

      // 2) Pokud se klient připojil později a my už data máme -> jednorázově pošli hned po connectu
      if (g_connected && !g_kickSentAfterConnect) {
        meteoNotifyNow("kick_after_connect");
        g_kickSentAfterConnect = true;
      }
    } else {
      Serial.println("[DATA] No valid reading yet");
    }
  }

// broadcast do advertising (bez spojení) – užitečné pro "pasivní poslech" na S3.
if (!g_connected && g_haveReading && (int32_t)(now - g_nextAdvBcastMs) >= 0) {
  g_nextAdvBcastMs = now + ADV_BCAST_PERIOD_MS;
  bleUpdateAdvBroadcast();
  bleRequestAdvRestart();
}

  // periodické notify
  if ((int32_t)(now - g_nextNotifyMs) >= 0) {
    g_nextNotifyMs = now + NOTIFY_PERIOD_MS;

    if (g_connected && g_haveReading && g_chMeteo) {
      meteoNotifyNow("periodic");
    }
  }

  // --- BLE housekeeping (stabilita "nalezení" periferie) ---
  // 0) Ošetření případného "rozjetí" stavu: callback mohl být vynechán,
  //    ale server ví, kolik klientů je skutečně připojeno.
  if (g_server) {
    const bool actualConn = (g_server->getConnectedCount() > 0);
    if (g_connected && !actualConn) {
      g_connected = false;
      bleRequestAdvRestart();
      Serial.println("[BLE] Connection state corrected -> disconnected");
    } else if (!g_connected && actualConn) {
      g_connected = true;
      g_advRestartPending = false;
      Serial.println("[BLE] Connection state corrected -> connected");
    }
  }

  // 1) Pokud přišel požadavek na restart inzerce (např. po disconnectu), proveď ho tady.
  if (g_advRestartPending) {
    g_advRestartPending = false;
    bleEnsureAdvertising(now, "pending_restart");
  }

  // 2) Pravidelně hlídej, že když není klient připojený, tak stále inzerujeme.
  //    (když se NimBLE stack dostane do stavu, kdy advertising neběží, S3 ho nenajde.)
  if (!g_connected && (int32_t)(now - g_nextAdvEnsureMs) >= 0) {
    g_nextAdvEnsureMs = now + ADV_ENSURE_PERIOD_MS;
    bleEnsureAdvertising(now, "periodic_ensure");
  }

  // žádné delay()
}
