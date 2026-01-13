# ESP-HeatAndDomesticController

**Modulární řídicí jednotka pro chytré řízení topení a domácích technologií postavená na ESP32-S3**

Tento projekt představuje univerzální a rozšiřitelnou platformu pro řízení topných systémů, ventilů, relé a senzorů s důrazem na **ekvitermní regulaci**, integraci do **Home Assistant**, komunikaci přes **MQTT**, **BLE** a lokální **webové rozhraní**.

Projekt je navržen jako mezičlánek mezi kotlem, ventily, čidly a nadřazeným systémem (např. Home Assistant, chytrý termostat, vlastní logika).

---

## ✨ Hlavní vlastnosti

* ✅ Modulární architektura (oddělené controllery podle funkcí)
* ✅ Neblokující běh (bez `delay()`)
* ✅ Ekvitermní regulace topení (AUTO režim)
* ✅ Podpora více typů teploměrů:

  * DS18B20 (Dallas / OneWire)
  * NTC (analogové vstupy)
  * MQTT teploměry (virtuální)
  * BLE (připraveno / rozšiřitelné)
* ✅ Řízení relé a 3cestných ventilů (230 V) včetně kalibrace
* ✅ Webové UI uložené v **LittleFS**
* ✅ Konfigurace zařízení přes web (bez nutnosti rekompilace)
* ✅ MQTT komunikace + Home Assistant auto-discovery
* ✅ OTA aktualizace firmware
* ✅ Wi-Fi konfigurace pomocí **WiFiManager**
* ✅ Podpora RTC
* ✅ Stavový dashboard (teploty, relé, režimy, ekviterm)

---

## 🧠 Typické použití

* Ekvitermní řízení kotle podle venkovní teploty
* Řízení směšovacích (3c) ventilů
* Ovládání kotle pomocí relé / OpenTherm (rozšiřitelné)
* Integrace chytrého termostatu (např. Nest) přes MQTT
* Zobrazení a řízení přes Home Assistant
* Univerzální I/O modul pro chytrou domácnost

---

## 🧩 Použitý hardware

Primárně cíleno na:

* **Waveshare ESP32-S3-POE-ETH-8DI-8DO**
  [https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO](https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO)

Vlastnosti desky:

* ESP32-S3
* Ethernet + PoE
* 8 digitálních vstupů
* 8 reléových výstupů
* Velká Flash (16 MB)
* Vhodné pro průmyslovější nasazení

---

## 🗂️ Struktura projektu

```
ESP-HeatAndDomesticController
├── ESP-D1-HeatControl.ino        # Hlavní sketch
├── config_pins.h                 # Mapování pinů
├── ConfigStore.*                 # Ukládání konfigurace (FS)
├── NetworkController.*           # WiFi / Ethernet / WiFiManager
├── WebServerController.*         # Web UI + REST API
├── FsController.*                # LittleFS
├── MqttController.*              # MQTT + Home Assistant
├── DallasController.*            # DS18B20
├── NtcController.*               # NTC senzory
├── ThermometerController.*       # Abstrakce teploměrů
├── RelayController.*             # Relé
├── InputController.*             # Digitální vstupy
├── LogicController.*             # Hlavní logika
├── ConditionEvaluator.*          # Vyhodnocování podmínek
├── ActionExecutor.*              # Provádění akcí
├── OpenThermController.*         # OpenTherm (rozšiřitelné)
├── BleController.*               # BLE
├── RtcController.*               # RTC
├── OtaController.*               # OTA aktualizace
├── BuzzerController.*            # Buzzer
├── RgbLedController.*            # Stavová RGB LED
├── LittleFS/
│   └── index.html                # Webové rozhraní
```

---

## 🌐 Webové rozhraní

* Dashboard se stavem systému
* Konfigurace:

  * Síť (WiFi / MQTT)
  * Vstupy a výstupy
  * Teploměry
  * Ekvitermní křivka
  * Logika a pravidla
* Responzivní rozložení
* Automatické skrývání prvků podle aktivních funkcí

---

## 🌡️ Ekvitermní regulace

* Aktivní pouze v režimu **AUTO**
* Dynamický výběr zdroje venkovní teploty:

  * DS18B20
  * NTC
  * MQTT teploměr
* Výpočet požadované teploty topné vody podle křivky
* Vizualizace křivky v UI (včetně aktuálního bodu)
* Navrženo tak, aby:

  * minimalizovalo cyklování kotle
  * šetřilo energii
  * bylo rozšiřitelné

---

## 🏠 MQTT & Home Assistant

* MQTT publish / subscribe
* Podpora až 2 MQTT teploměrů
* Jednoduché JSON path parsování
* Home Assistant auto-discovery:

  * teploměry
  * relé
  * režimy
  * stavové entity

---

## 🔧 Konfigurace & běh

* Veškerá konfigurace je ukládána do **LittleFS**
* Po restartu se:

  * načtou vstupy, výstupy, teploměry, ekviterm
  * inicializují controllery ve správném pořadí
* Senzory jsou vždy zpracovány **před logikou**

---

## 🚀 Stav projektu

Projekt je **aktivně vyvíjen**.
Některé části (např. OpenTherm, pokročilé BLE scénáře) jsou připravené k dalšímu rozšíření.


Pokud chceš, můžu:

* připravit **zkrácenou verzi README**
* doplnit **schéma zapojení**
* přidat **sekci Build / Flash / Partition scheme**
* nebo README rovnou **vygenerovat jako soubor ke stažení**
