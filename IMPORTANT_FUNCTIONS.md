# Důležité funkce / moduly (aktuální stav)

Tento soubor je **živý** a má se aktualizovat při každé úpravě kódu, aby nevznikaly duplicitní funkce a bylo jasné, kde se co řeší.

> Poznámka k této verzi: jde o aktuální pracovní firmware s webovým UI, OpenTherm arbitráží, Ekvitermem, TUV a směšovacím ventilem. Dokument slouží jako orientace k architektuře a musí odpovídat reálnému kódu.

## 0) Relé + vstupy – HW I/O vrstva

### `RelayController` (RelayController.h/.cpp)
**Účel:** Jednotné ovládání 8 relé přes I2C expander TCA9554 (output register) + auto-recovery při chybách na sběrnici.

- `relayInit()` / `relayUpdate()`
  - Inicializace TCA9554 (POL, CFG, OUTPUT) a non-blocking aplikace požadované masky.
  - Periodický health-check + retry/re-init pokud dojde k desynchronizaci nebo I2C chybě.
- `relaySet(id, on)` / `relayToggle(id)` / `relaySetMask(mask)`
  - Základní API pro ovládání relé.

**Mapování relé (konzole HELP):**
- `R1 + R2` = směšovací ventil (motor OPEN/CLOSE)
- `R3` = přepínací 3cestný ventil TUV/CH
- `R4` = cirkulační čerpadlo TUV
- `R5` = požadavek kotli pro TUV
- `R6` = den/noc ekvitermní křivka na kotli
- `R7` = omezovací relé výkonu kotle
- `R8` = stykač topné tyče (akumulační nádrž)

**Důležité k R1/R2 / směšovacímu ventilu:**
- Bezpečnostní interlock je centralizovaný v `RelayController` a lze ho přemapovat podle konfigurace směšovacího ventilu.
- Konzole i web API používají stejnou relé vrstvu, takže nehrozí rozdílné chování mezi UI a ručním ovládáním.

### `InputController` (InputController.h/.cpp)
**Účel:** Debounce čtení 8 digitálních vstupů (GPIO4..11) + převod na logický stav podle polarity z `ConfigStore`.

- `inputInit()` / `inputUpdate()`
  - Interně používá `INPUT_PULLUP` a debounce 50 ms.
- `inputGetRaw()` – debounced RAW úroveň (HIGH/LOW)
- `inputGetState()` – logický stav ACTIVE/INACTIVE podle `ConfigStore::getInputActiveLevel(index)`

**Mapování vstupů (konzole HELP):**
- `IN1` = denní/noční křivka (ACTIVE = noční)
- `IN2` = požadavek TUV (ACTIVE)
- `IN3` = požadavek cirkulace (ACTIVE)
- `IN8` = servis: při bootu vynutí WiFiManager portal

## 1) Teploty – jednotné role napříč programem

### `TemperatureManager` (TemperatureManager.h/.cpp)
**Účel:** Jediný „zdroj pravdy“ pro všechny teploty používané v programu (konzole, web UI, budoucí logika).

- `TemperatureManager::begin()`
  - Inicializace interní cache rolí.

- `TemperatureManager::loop()`
  - Periodicky aktualizuje role:
    - z OpenTherm (`openthermGetStatus()`)
    - z BLE (`bleGetMeteo()` – fallback pro `outside`)
    - z DS18B20 (`DallasController::getStatus(gpio)`)

- `TemperatureManager::get(role, maxAgeMs)`
  - Vrací nejlepší dostupnou hodnotu pro danou roli + informace o zdroji (`OpenTherm/Dallas/BLE`), stáří a u Dallas i `gpio/rom`.

### Teplotní role (`TempRole`)
- `Flow` – flow/boiler (preferuje OpenTherm)
- `Return` – return (preferuje OpenTherm, fallback DS18B20 na GPIO2)
- `DhwTank` – DHW tank (OpenTherm)
- `Outside` – outside (priorita: OpenTherm → DS18B20 na GPIO0 (volitelně) → BLE)
- `TankTop/TankMid/TankBottom` – akumulace (DS18B20 na GPIO3, role mapping)
- `DhwReturn` – zpátečka cirkulace TUV (DS18B20 na GPIO1)

### Dallas role mapping (ROM)
Role mapping je persistovaný v `ConfigStore`.
- `ROM = 0` znamená AUTO:
  - tank role: stabilně seřazené senzory podle ROM (index 0/1/2)
  - return/dhw_return: první validní senzor na dané sběrnici


## 2) DS18B20 – sběrnice

### `DallasController` (DallasController.h/.cpp)
**Účel:** Non-blocking obsluha DS18B20 přes RMT OneWire driver (OneWireESP32).

- `DallasController::begin()`
  - Inicializuje interní round-robin scheduler pro GPIO0..GPIO3.

- `DallasController::configureGpio(gpio, type)`
  - Zapne/vypne čtení DS na vybraném GPIO.

- `DallasController::loop()`
  - Sekvenčně obsluhuje sběrnice (discover + convert + read) bez blokování.

### Pin role (config_pins.h)
- `DALLAS_TANK_PIN = GPIO3`
- `DALLAS_RETURN_PIN = GPIO2`
- `DALLAS_DHW_RETURN_PIN = GPIO1`
- `DALLAS_IO0_PIN = GPIO0` (volitelně `outside`)


## 3) OpenTherm – čtení + ovládání

### `OpenThermController` (OpenThermController.h/.cpp)
**Účel:** Polling kotle přes OpenTherm + volitelně zápisy (pokud `mode == control`).

- `openthermInit()` / `openthermLoop()`
  - Start a periodický polling.

- `openthermGetStatus()`
  - Vrací snapshot (teploty, status flagy, tlak, modulace, fault kódy, …).

- `openthermHandleCmdJson(body, err)`
  - Zpracuje JSON příkaz pro ovládání:
    - `chEnable`, `dhwEnable`
    - `chSetpointC`, `dhwSetpointC`
    - `maxModulationPct`
    - `resetFault`
  - Funguje pouze v režimu `control`.

- `openthermScanStart/Stop/GetStatusJson()`
  - Non-blocking scan Data-ID 0..127 a cache výsledků (včetně poslední hodnoty).

- `openthermReadDataIdJson(id, reqValue)`
  - Live READ konkrétního Data-ID (vrací raw + základní decode pro UI).

- `openthermWriteDataIdJson(id, value)`
  - Live WRITE konkrétního Data-ID (raw u16). **Pozor:** vyžaduje `mode=control` a `allowRawWrite=true`.


## 3b) Ekviterm – řízení žádané teploty topné vody

### `EquithermController` (EquithermController.h/.cpp)
**Účel:** Výpočet žádané teploty topné vody z venkovní teploty (role `Outside`) a řízení kotle přes centralizované OpenTherm požadavky:
- bere teploty přes `TemperatureManager` (včetně fallbacků OpenTherm / Dallas / BLE podle role)
- zapisuje požadavek na CH přes centralizovanou OpenTherm arbitráž (`equitherm` request)
- volitelně umí upravit **Max CH setpoint** (OpenTherm **ID57**) podle nastavení a limitů z kotle
- podporuje oddělené křivky **DEN/NOC** a automatické přepínání DEN/NOC pomocí týdenního plánu
- volitelně ovládá relé (default `R6`) pro „den/noc“ vstup kotle

Hlavní prvky:
- **Křivka 2-bodová** (lineární): (Tout_cold → Tflow_cold) a (Tout_warm → Tflow_warm)
- **Limity**: `minFlowC/maxFlowC` + `minChSetpointC/maxChSetpointC`
- **Týdenní plán**: pro každý den `dayStartMin` a `nightStartMin` (minuty od půlnoci)
- **Zdroj času**: SNTP (`NetworkController`)

API:
- `GET /api/equitherm/status`
- `POST /api/equitherm/cmd` (např. `{ "mode":"auto" }`)

## 3a) TUV – ohřev + cirkulace

### `DhwController` (DhwController.h/.cpp)
**Účel:** Řízení ohřevu teplé užitkové vody a cirkulace podle vstupů, plánů a konfigurace.

- TUV může řídit přepínací ventil, relé požadavku kotli i OpenTherm požadavek.
- Při aktivním TUV blokuje Ekviterm a do OpenTherm arbitráže zapisuje vlastní `dhw` request.
- Cirkulace podporuje vstup, plán i pulzní režim ON/OFF.


## 4) Web portál – UI + API

### `WebPortalController` (WebPortalController.cpp/.h)
**Účel:** Embedded SPA + JSON API.

Hlavní endpointy:
- `GET /api/fast` – rychlý snapshot (teploty z `TemperatureManager`, relé, vstupy, fast OT/BLE)
- `GET /api/equitherm/status` – stav Ekviterm (config + status)
- `POST /api/equitherm/cmd` – rychlé příkazy (např. `{"mode":"day|night|auto"}`)
- `GET/POST /api/config` – persistovaná konfigurace (`ConfigStore`)
- `GET /api/dallas/status` – DS zařízení + role mapping (pro UI)
- `GET /api/opentherm/status` – kompletní OT status JSON
- `POST /api/opentherm/cmd` – ruční OT ovládání (JSON, zdroj `manual`)
- `GET /api/opentherm/scan/status` + `POST /api/opentherm/scan/start|stop` – Data-ID scan
- `POST /api/opentherm/dataid/read` – live read vybraného Data-ID (JSON: `{id, reqValue}`)
- `POST /api/opentherm/dataid/write` – live write vybraného Data-ID (JSON: `{id, valueRaw|valueF88|hb/lb}`)
- `POST /api/relay` – ovládání relé
- `POST /api/reboot` – restart
- `GET /api/ota/status` – OTA status (Arduino IDE upload)

### UI assety
- `WebPortalAssets.h` – obsahuje `index.html`, `app.css`, `app.js` embednuté v PROGMEM.


## 5) OTA – aktualizace FW z Arduino IDE

### `OtaController` (OtaController.h/.cpp)
**Účel:** Umožní nahrávání FW přes síť přímo z Arduino IDE jako „Network Port“ (ArduinoOTA + mDNS).

- `otaInit()` / `otaLoop()`
  - `otaLoop()` interně hlídá WiFi připojení:
    - po připojení spustí mDNS (`_arduino._tcp`) a `ArduinoOTA.begin()`
    - při výpadku WiFi OTA označí jako zastavené a po reconnectu znovu nastartuje.

- `otaGetStatusJson()`
  - Detailní JSON se stavem (enabled/started/uploading/progress/host/port).

- `otaApplyConfig(json)`
  - Uloží nastavení do `ConfigStore` a restartuje OTA instanci.

Konfigurace (persistuje `ConfigStore`):
- `enabled` (default ON)
- `hostname` (prázdné = auto z MAC)
- `port` (default 3232)
- `password` (volitelné)


## 6) Konfigurace – persistentní

### `ConfigStore` (ConfigStore.h/.cpp)
**Účel:** Preferences (NVS) persistentní hodnoty.

- vstupy: active level (LOW/HIGH)
- OpenTherm: enabled/autostart/poll/bootDelay + `mode` (readOnly/control)
  - `allowRawWrite` – povolí raw Data-ID write z web portálu (default OFF)
- BLE: enabled/namePrefix/scanInterval
- Dallas: enabled + role ROM mapping
  - `outside` ROM na GPIO0 (volitelné)
- OTA: enabled/hostname/port/password
- Time (SNTP): enabled + TZ string + NTP servery
- Ekviterm: enabled + křivky day/night + limity + týdenní plán + mapování relé den/noc


## 7) Hlavní smyčka

### `ESP32-S3-ETH-8DI-8RO-Controller.ino`
**Účel:** základní wiring modulů + konzole.

Pořadí v `loop()`:
- input/relay update
- DallasController::loop()
- OpenTherm + BLE loop
- TemperatureManager::loop() (sjednocení rolí)
- networkLoop + otaLoop + webPortalLoop
- Equitherm (výpočet + případný zápis do OpenTherm)

## 8) Směšovací ventil (R1/R2) – stav implementace

V této verzi je **pouze mapování relé + ruční ovládání** (konzole + web/API). Není zde samostatná regulační funkce,
která by na základě teplot / ekvitermní křivky automaticky řídila motor směšovače.

Co už existuje:
- HW ovládání relé přes TCA9554 (`RelayController`)
- Konzolový „interlock“ pro R1/R2 (nikdy nenechá obě ON)

Co zatím chybí (pro „funkci směšovacího ventilu“):
- Stavová/regulační logika (např. časované pulsy OPEN/CLOSE, deadband, limitace doběhu)
- Využití teplotních rolí (`TemperatureManager`) pro regulaci směšování
- Interlock i ve web API (`/api/relay`) + případně blokace současného řízení z více zdrojů
