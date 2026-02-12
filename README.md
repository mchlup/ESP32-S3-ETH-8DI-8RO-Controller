
# ESP32 Heat & Domestic Water Controller

Modulární řídicí jednotka pro **řízení topení a teplé užitkové vody (TUV / DHW)** postavená na **ESP32-S3** s podporou **Web UI, MQTT, Home Assistant, BLE senzorů a OpenTherm**.

Projekt je navržen jako **dlouhodobě udržitelný**, **neblokující** a **rozšiřitelný** systém pro reálné nasazení v topných soustavách.

---
## Náhled UI
<img width="1263" height="1139" alt="Snímek obrazovky 2026-02-12 122324" src="https://github.com/user-attachments/assets/35599247-7922-475f-a402-914ef1e62894" />

---
## Hlavní funkce

- 🔥 Řízení topení
  - 3c směšovací ventil (ekvitermní regulace)
  - podpora akumulační nádrže
  - řízení oběhových čerpadel
- 🚿 Teplá užitková voda (TUV / DHW)
  - přepínací ventil
  - časové plánování
  - cirkulace TUV
- 🌡️ Snímání teplot
  - DS18B20 (1-Wire, více senzorů na vstup)
  - externí BLE venkovní senzor (ESP32-C3)
  - MQTT senzory
- 📡 Komunikace
  - Web UI (LittleFS)
  - REST API
  - MQTT + Home Assistant auto-discovery
  - BLE (NimBLE)
  - OpenTherm (plánováno / rozšiřitelné)
- ⚙️ Konfigurace
  - webové rozhraní
  - persistentní konfigurace v LittleFS
  - validace a fallback na defaulty
- 🧠 Architektura
  - plně neblokující běh
  - stavové automaty (BLE, retry)
  - oddělení logiky, IO, UI a komunikace

---

## Použitý hardware

- **ESP32-S3-POE-ETH-8DI-8DO**  
  https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO
- I2C expander pro relé / vstupy
- DS18B20 teplotní senzory
- ESP32-C3 BLE meteosenzor (venkovní)

---

## Struktura projektu

```text
/
├── ESP-D1-HeatControl/        # Hlavní firmware (ESP32-S3)
│   ├── *.ino
│   ├── controllers/          # Logic, BLE, MQTT, Web, FS, Dallas, OTA…
│   ├── utils/                # JSON utils, retry policy, helpers
│   ├── data/                 # Web UI (LittleFS)
│   │   ├── index.html
│   │   └── js/
│   └── include/
│
├── ESP32C3_BLE_MeteoSensor/   # BLE venkovní senzor (ESP32-C3)
│   └── ESP32C3_BLE_MeteoSensor.ino
│
└── README.md
````

---

## Architektonické principy

### Nezablokovaný běh

* žádné `delay()` v produkční logice
* všechny opakované operace řízeny časovačem / stavem
* plynulý běh UI, MQTT i BLE i při chybách periferií

### Stavové automaty

* BLE client (scan → connect → subscribe → connected → retry)
* retry/backoff pro I2C, BLE, síťové operace

### Konfigurace & JSON

* centrální práce s JSON (`ArduinoJson`)
* dynamická kapacita dokumentů
* validace vstupů + rozsahů
* atomický zápis konfigurace (ochrana proti poškození)

### Oddělení odpovědností

* každý subsystém má vlastní controller
* minimální vazby mezi moduly
* jasně definované API

---

## Web UI

* běží přímo na zařízení
* uložené v LittleFS
* responzivní dashboard
* dynamické widgety podle aktivních funkcí
* konfigurační stránky:

  * Ekviterm
  * ohřev TUV (DHW)
  * Cirkulace TUV (DHW)
  * podpora topení z Akumulační nádrže
  * Senzory
  * MQTT / Síť

---

## REST API

* jednotný JSON response kontrakt:

```json
{ "ok": true, "data": { ... }, "warnings": [] }
```

```json
{ "ok": false, "error": { "code": "...", "message": "...", "details": [] } }
```

* `/api/status` – aktuální stav systému
* `/api/config/*` – konfigurace jednotlivých modulů
* všechny endpointy:

  * validují vstup
  * vrací defaulty při chybě
  * nikdy neselžou „tiše“

---

## BLE meteosenzor

* ESP32-C3 jako BLE server
* periodické odesílání dat:

  * teplota
  * vlhkost
  * tlak
  * trend
* ESP32-S3 jako BLE client:

  * stavový automat
  * řízený reconnect s backoffem
  * watchdog na příjem dat

---

## MQTT & Home Assistant

* MQTT publish:

  * teploty
  * stavy relé
  * diagnostika
* Home Assistant:

  * auto-discovery
  * senzory
  * přepínače
  * stavové entity

---
