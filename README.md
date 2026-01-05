# ESP-HeatAndDomesticController

Modulární řídicí jednotka pro chytré řízení topení / TUV a souvisejících technologií na **ESP32-S3** (primárně Waveshare **ESP32-S3-POE-ETH-8DI-8RO/8DO**).  
Zařízení propojuje kotel, ventily, čidla a nadřazený systém (např. Home Assistant) přes **Web UI**, **REST API** a **MQTT**; volitelně také přes **BLE**.

> Cíl: stabilní, rozšiřitelné “middleware” pro topení a TUV bez blokování (`delay()`).

---

## Hardware

Cílová deska (pinmap je v `config_pins.h`):

- Waveshare ESP32-S3-POE-ETH-8DI-8RO/8DO  
  https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO

### Použité periférie (aktuální build)
- **8× DI**: GPIO 4..11 (`INPUT1_PIN..INPUT8_PIN`)
- **Relé**: přes I/O expander **TCA9554** na I2C
- **I2C**: SCL=GPIO41, SDA=GPIO42 (RTC + TCA9554)
- **RTC**: PCF85063 (pokud je osazen / dostupný)
- **RGB LED (WS2812)**: GPIO38
- **Buzzer**: GPIO46
- **NTC ADC piny**: GPIO1..GPIO3 jsou rezervované (v projektu připravené, ale v této verzi nejsou aktivně používané v loopu)

---

## Hlavní vlastnosti

- ✅ Modulární architektura (oddělené controllery podle funkcí)
- ✅ Neblokující běh (bez `delay()`)
- ✅ Webové UI uložené v **LittleFS** (`/data`)
- ✅ Konfigurace zařízení přes web / REST (bez rekompilace)
- ✅ Síť: **Wi-Fi + Ethernet (W5500)**
  - Wi-Fi konfigurace pomocí **WiFiManager**
  - Pokud je připojen RJ45 a link je UP, zařízení **přeskočí WiFiManager AP portál** a jede po Ethernetu (DHCP)
  - Za běhu hlídá připojení kabelu a umí přepínat Wi-Fi/ETH (preferuje ETH pokud má IP / během DHCP grace)
- ✅ Teploty:
  - DS18B20 (Dallas/OneWire) – čtení a publikace do UI/MQTT
  - “virtuální” teploměry z MQTT a BLE (přes `ThermometerController`)
- ✅ Řízení relé + 3cestných ventilů:
  - mix ventil 0–100 % (čas přejezdu, odhad polohy, kalibrace v UI)
  - přepínací 3c ventil (A/B)
  - bezpečnost proti přímému spínání “peer” relé ventilů přes `/api/relay`
  - perzistence odhadované polohy mix ventilů přes restart (`/state/valves.json`)
- ✅ Ekvitermní regulace (AUTO režim)
- ✅ TUV logika + recirkulace (včetně časových oken a cyklování)
- ✅ MQTT + Home Assistant (discovery a základní entity)
- ✅ OTA aktualizace
- ✅ (volitelně) Rule engine (`FEATURE_RULE_ENGINE` v `config_pins.h`)
- ✅ (připraveno) OpenTherm controller endpoint/status

---

## Struktura projektu

```

ESP32-S3-ETH-8DI-8RO-Controller-main/
├── ESP-D1-HeatControl.ino
├── config_pins.h
├── FsController.*              # LittleFS mount + utilitky
├── NetworkController.*         # Wi-Fi/Ethernet + WiFiManager + čas/RTC
├── WebServerController.*       # Web + REST API + FS upload
├── MqttController.*            # MQTT + HA discovery
├── RelayController.*           # relé přes TCA9554
├── InputController.*           # DI vstupy + debounce
├── LogicController.*           # režimy, ventily, ekviterm, TUV, recirkulace, schedules
├── DallasController.*          # DS18B20
├── ThermometerController.*     # MQTT/BLE teploměry
├── BleController.*             # BLE pairing/status
├── OpenThermController.*       # OpenTherm status (rozšiřitelné)
├── RuleEngine.*                # volitelné pravidla
├── OtaController.*             # OTA
├── partitions.csv              # partition layout (LittleFS)
└── data/                       # Web UI do LittleFS
├── index.html
├── dash_v2.js / dash_v2.css
├── equitherm.js, tuv.js, dhw_recirc.js, schedules.js, ...
└── ble.html, ota.html, rules.html, update.html, ...

````

---

## Build & nahrání

### 1) Firmware
- Arduino IDE / PlatformIO (dle vašeho workflow)
- Nahraj firmware do ESP32-S3

### 2) Web UI do LittleFS
Soubory v `data/` musí být nahrané do LittleFS.
- Arduino IDE: „ESP32 Sketch Data Upload“ (LittleFS)
- PlatformIO: `pio run -t uploadfs`

> Doporučení: používej partition schéma dle `partitions.csv`, aby LittleFS mělo dost prostoru (UI + config + `/state/*`).

---

## Síť (Wi-Fi / Ethernet)

### Chování při startu
1) Spustí se Ethernet (W5500) + event handler.
2) Krátce se čeká na detekci linku (bez `delay()`, používá se `yield()`).
3) Pokud je **ETH link UP**:
   - Wi-Fi se vypne
   - WiFiManager se **nespouští** (žádný AP portál)
4) Pokud **ETH link není**:
   - spustí se WiFiManager `autoConnect()`
   - pokud nejsou uložené Wi-Fi údaje, otevře se konfigurační AP

### Za běhu
- Pokud ETH získá IP → preferuje ETH a Wi-Fi vypne
- Pokud ETH link spadne → znovu povolí Wi-Fi a zkouší reconnect uloženými údaji

Po získání IP zařízení vypíše do Serial debug odkazy (UI + API).

---

## Web UI

- `http://<IP>/index.html` (dashboard)
- další stránky: BLE, OTA, Rules, Update, Valve calib, …

UI čte a ukládá konfiguraci přes `/api/config` a stav přes `/api/status` + `/api/dash`.

---

## REST API (aktuální endpointy)

### Základní
- `GET  /api/status` – stav zařízení (síť, relé, vstupy, teploty, režimy, …)
- `GET  /api/dash` – dashboard JSON pro UI
- `GET  /api/caps` – capabilities (co je dostupné/aktivní)
- `GET  /api/time` – čas (epoch/ISO + zdroj)

### Konfigurace
- `GET  /api/config`
- `POST /api/config` – uloží a aplikuje (ArduinoJson filter je sekční, doc=32768)

### Relé / režimy
- `GET /api/relay?id=<1..8>&cmd=on|off|toggle`
  - bezpečnost: pokud je relé peer ventilu → `on/toggle` je blokováno (HTTP 409)
  - pokud je relé master mix ventilu → doporučeno použít `/api/valve`
- `GET /api/mode_ctrl?...` – ovládání AUTO/MANUAL + režimy (používá UI)

### Ventily
- `GET /api/valve?id=<id>&pct=<0..100>`
- `GET /api/valve?id=<id>&cmd=a|b|toggle`

### FS (LittleFS)
- `GET  /api/fs/list`
- `POST /api/fs/upload`
- `POST /api/fs/delete`

### OTA / reboot
- `POST /api/reboot`
- (OTA běží přes UI stránky + OTA controller)

### BLE
- `GET  /api/ble/status`
- `GET  /api/ble/paired`
- `POST /api/ble/pair`
- `POST /api/ble/pair/stop`
- `POST /api/ble/remove`
- `GET/POST /api/ble/config`

### Rule Engine (pokud `FEATURE_RULE_ENGINE=1`)
- `GET/POST /api/rules`
- `GET /api/rules/status`

### OpenTherm
- `GET /api/opentherm/status`

---

## MQTT

Konfigurace je v objektu `mqtt` (v JSON configu):

```json
{
  "mqtt": {
    "enabled": true,
    "host": "192.168.1.10",
    "port": 1883,
    "user": "",
    "pass": "",
    "clientId": "",
    "baseTopic": "espheat",
    "haPrefix": "homeassistant"
  },
  "relayNames": ["R1","R2","R3","R4","R5","R6","R7","R8"],
  "inputNames": ["I1","I2","I3","I4","I5","I6","I7","I8"]
}
````

* `baseTopic` default: `espheat`
* `haPrefix` default: `homeassistant`

---

## Thermometers (MQTT/BLE teploměry)

Konfigurace je v objektu `thermometers`:

```json
{
  "thermometers": {
    "mqtt": [
      { "name": "T_out", "topic": "sensors/outdoor", "jsonKey": "tempC" },
      { "name": "T_room", "topic": "sensors/room", "jsonKey": "tempC" }
    ],
    "ble": { "name": "BLE Meteo", "id": "meteo.tempC" }
  }
}
```

---

## Recirkulace TUV (`dhwRecirc`) – aktuální JSON struktura

UI soubor `data/dhw_recirc.js` používá následující klíče (v ms):

```json
{
  "dhwRecirc": {
    "enabled": true,
    "mode": "window_cycle",
    "pumpRelay": 6,

    "demandInput": 1,
    "onDemandRunMs": 300000,

    "minOnMs": 60000,
    "minOffMs": 60000,

    "stopTempC": 45.0,
    "tempReturnSource": {
      "source": "mqtt",
      "mqttPreset": 0,
      "topic": "sensors/dhw_return",
      "jsonKey": "tempC"
    },

    "windows": [
      { "start": "06:00", "end": "20:00", "days": [1,2,3,4,5,6,7] }
    ],

    "cycleOnMs": 300000,
    "cycleOffMs": 900000
  }
}
```

### `mode` hodnoty

* `off`
* `time_windows`
* `on_demand`
* `hybrid`
* `window_cycle` (nové): v oknech pulzuje ON/OFF podle `cycleOnMs/cycleOffMs`
  **Pozn.:** v tomto režimu se uplatní i `minOnMs/minOffMs`.

---

## Perzistence ventilů přes restart

Odhad polohy mix ventilů se ukládá do LittleFS:

* soubor: `/state/valves.json`
* ukládá se po doběhu pohybu (šetří flash zápisy)
* obnovuje se po bootu (pro UI i řízení)

Doporučený formát:

```json
{
  "version": 1,
  "updatedAt": 0,
  "valves": [
    { "id": 0, "type": "mix", "posPct": 35, "invertDir": false }
  ]
}
```

---

## Poznámky k bezpečnosti ovládání relé (ventily)

* `on/toggle` peer relé ventilů je blokováno v `/api/relay`
* mix ventily ovládej přes `/api/valve` (ne přímým spínáním relé)

---

