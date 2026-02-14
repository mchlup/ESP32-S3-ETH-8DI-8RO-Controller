# ESP32-S3-ETH-8DI-8RO Heat Controller

Řídicí jednotka topné soustavy postavená na platformě **Waveshare ESP32-S3-ETH-8DI-8RO**.

Projekt implementuje:
- Ekvitermní regulaci směšovacího ventilu
- Prioritní ohřev teplé užitkové vody (DHW / TUV)
- Komunikaci s kotlem přes OpenTherm
- BLE venkovní senzor
- Dallas DS18B20 teplotní senzory
- Webové rozhraní s realtime aktualizací (SSE)
- Ethernet + Wi‑Fi (AP + STA)
- RTC + NTP synchronizaci času

---

# Hardware

## Řídicí jednotka
- Waveshare ESP32-S3-ETH-8DI-8RO
- Ethernet W5500
- 8× digitální vstup
- 8× relé výstup

## Teplotní senzory
- Dallas DS18B20
- Až 3 senzory na jeden GPIO
- GPIO 0–3

## BLE venkovní senzor
- ESP32-C3
- Periodické vysílání teploty

---

# Mapování I/O

## Digitální vstupy

| Vstup | Funkce |
|--------|--------|
| IN1 | Přepnutí denní/noční ekvitermní křivky (aktivní = noční) |
| IN2 | Požadavek ohřevu TUV |
| IN3 | Požadavek cirkulace TUV |
| IN4–IN8 | Rezerva |

## Relé výstupy

| Relé | Funkce |
|-------|--------|
| R1 + R2 | Směšovací ventil (Ekviterm) |
| R3 | Přepínací ventil TUV |
| R4 | Cirkulační čerpadlo TUV |
| R5 | Požadavek TUV na kotel |
| R6 | Přepnutí den/noc na kotli |
| R7 | Omezovací relé výkonu |
| R8 | Stykač topné tyče (akumulační nádrž 450 L) |

---

# Funkce systému

## Ekvitermní regulace
- Řízení směšovacího ventilu podle venkovní teploty
- Denní a noční křivka
- Konfigurovatelný sklon, posun, hystereze
- Nastavení času přesunu ventilu

## Ohřev TUV (DHW)
- Prioritní režim
- Přepnutí ventilu do režimu TUV
- Aktivace požadavku na kotel
- Automatický návrat po dosažení teploty

## Cirkulace TUV
- Časové řízení nebo vstupem
- Možnost cyklického provozu

## OpenTherm
- Nastavení požadované teploty
- Čtení stavů kotle
- Diagnostika komunikace

## BLE
- Pasivní příjem dat
- Kontrola stáří dat
- RSSI monitoring

---

# Webové rozhraní

Obsahuje:
- Dashboard (widgety, grid layout)
- Ekviterm
- OpenTherm
- BLE
- Síťová konfigurace
- Diagnostika

Realtime aktualizace přes:
/api/events (SSE)

---

# Síť

- Ethernet (prioritní)
- Wi‑Fi STA
- Wi‑Fi AP fallback (WiFiManager)
- NTP synchronizace
- RTC podpora

---

# Struktura projektu

ESP32-S3-ETH-8DI-8RO-Controller/
- BleController.*
- EquithermController.*
- LogicController.*
- OpenTherm*
- RelayController.*
- InputController.*
- ThermometerController.*
- DallasController.*
- OneWireESP32.*
- WebServerController.*
- ConfigStore.*
- NetworkController.*
- data/ (Web UI)

---

# Kompilace

Platforma:
- ESP32-S3
- Arduino framework

Doporučeno:
- Arduino IDE 2.x
- Aktuální ESP32 core

Knihovny:
- WiFiManager
- ArduinoJson
- PubSubClient
- NimBLE
- OneWire
- DallasTemperature

---

# Stav projektu

- Ekviterm: funkční
- TUV: funkční
- BLE: stabilní
- OpenTherm: připraveno k provoznímu testování
- Web UI: aktivní vývoj

---

# Licence

Projekt je určen pro experimentální a soukromé použití.
