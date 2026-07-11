# ESP32-S3-ETH-8DI-8RO Heat Controller

Firmware pro řízení topné soustavy na desce **Waveshare ESP32-S3-ETH-8DI-8RO**.

Aktuální verze projektu: **3.3.15**

Projekt spojuje řízení kotle přes OpenTherm, ekvitermní regulaci, trojcestný směšovací ventil, podporu vytápění z akumulační nádrže, ohřev a cirkulaci TUV, DS18B20 teploměry, Wi-Fi, Ethernet, MQTT/Home Assistant a moderní webové rozhraní.

> [!WARNING]
> Firmware ovládá kotel, oběhová zařízení, ventily a reléové výstupy. Před automatickým provozem ověřte směr ventilu, přiřazení relé, bezpečné krajní polohy, polaritu vstupů a reakci kotle. Software nenahrazuje havarijní termostaty, tlakové pojistky ani další hardwarové bezpečnostní prvky.

## Obsah

- [Hlavní funkce](#hlavní-funkce)
- [Hardware a zapojení](#hardware-a-zapojení)
- [Mapování vstupů a relé](#mapování-vstupů-a-relé)
- [Teplotní zdroje](#teplotní-zdroje)
- [Ekvitermní regulace](#ekvitermní-regulace)
- [Směšovací ventil a podpora z AKU](#směšovací-ventil-a-podpora-z-aku)
- [TUV a cirkulace](#tuv-a-cirkulace)
- [OpenTherm](#opentherm)
- [Síť a čas](#síť-a-čas)
- [Webové rozhraní](#webové-rozhraní)
- [MQTT a Home Assistant](#mqtt-a-home-assistant)
- [Konfigurace a ukládání](#konfigurace-a-ukládání)
- [Aktualizace firmware a LittleFS](#aktualizace-firmware-a-littlefs)
- [Kompilace](#kompilace)
- [První spuštění](#první-spuštění)
- [Diagnostika](#diagnostika)
- [Struktura projektu](#struktura-projektu)
- [Známá omezení](#známá-omezení)

## Hlavní funkce

- ekvitermní výpočet požadované teploty topné vody,
- samostatná denní a noční křivka,
- režimy `auto`, `day` a `night`,
- týdenní plán vytápění,
- volitelné přepnutí DEN/NOC vstupem IN1,
- letní režim s hysterezí,
- řízení kotle přes OpenTherm,
- čtení teplot, tlaku, modulace, chyb a provozních stavů kotle,
- řízení trojcestného směšovacího ventilu pomocí R1/R2,
- podpora kotle teplem z akumulační nádrže,
- prioritní ohřev TUV,
- cirkulace TUV podle vstupu, plánu a pulzního režimu,
- anti-legionella režim,
- až čtyři samostatné OneWire sběrnice DS18B20,
- BLE zdroj venkovní teploty,
- Wi-Fi STA, konfigurační AP a W5500 Ethernet,
- SNTP synchronizace času,
- MQTT telemetrie, příkazy a Home Assistant Discovery,
- webové UI s rychlými aktualizacemi přes WebSocket,
- OTA přes Arduino IDE,
- aktualizace firmware a filesystemu z webového správce,
- diagnostika I²C, relé, teploměrů, OpenTherm, MQTT, paměti a událostí.

## Architektura řízení

Zjednodušený tok dat:

```text
OpenTherm / DS18B20 / BLE
            │
            ▼
   TemperatureManager
            │
      ┌─────┴─────┐
      ▼           ▼
 Equitherm       DHW
      │           │
      ├──── OpenTherm arbitráž
      │
      └──── směšovací ventil R1/R2

Síť ── Web UI / WebSocket / REST / MQTT / OTA
```

`TemperatureManager` je centrální zdroj teplotních rolí. Řídicí moduly proto nemají používat vlastní paralelní čtení stejného čidla, pokud pro danou hodnotu existuje odpovídající role.

## Hardware a zapojení

### Cílová deska

- Waveshare ESP32-S3-ETH-8DI-8RO
- ESP32-S3
- 8 digitálních vstupů
- 8 reléových výstupů přes TCA9554
- Ethernet W5500 přes SPI
- WS2812 RGB LED
- bzučák
- I²C sběrnice pro TCA9554 a zařízení desky
- čtyři OneWire vstupy GPIO0 až GPIO3

### Pevné piny

| Funkce | Pin |
|---|---:|
| IN1 | GPIO4 |
| IN2 | GPIO5 |
| IN3 | GPIO6 |
| IN4 | GPIO7 |
| IN5 | GPIO8 |
| IN6 | GPIO9 |
| IN7 | GPIO10 |
| IN8 | GPIO11 |
| I²C SCL | GPIO41 |
| I²C SDA | GPIO42 |
| RGB LED | GPIO38 |
| Bzučák | GPIO46 |
| OpenTherm TX | GPIO47 |
| OpenTherm RX | GPIO48 |
| DS18B20 sběrnice 1 | GPIO0 |
| DS18B20 sběrnice 2 | GPIO1 |
| DS18B20 sběrnice 3 | GPIO2 |
| DS18B20 sběrnice 4 | GPIO3 |

### W5500 Ethernet

| Signál | Pin |
|---|---:|
| INT | GPIO12 |
| MOSI | GPIO13 |
| MISO | GPIO14 |
| SCK | GPIO15 |
| CS | GPIO16 |

Ethernetová implementace používá API dostupné v **Arduino-ESP32 3.x**. Na starším core se W5500 nespustí.

## Mapování vstupů a relé

### Digitální vstupy

| Vstup | Výchozí funkce |
|---|---|
| IN1 | přepnutí DEN/NOC; aktivní stav může v režimu AUTO vynutit noc |
| IN2 | požadavek na ohřev TUV |
| IN3 | požadavek na cirkulaci TUV |
| IN4 až IN7 | rezerva / servisní použití |
| IN8 | při aktivním stavu během bootu vynutí WiFiManager portál |

Polarita každého vstupu je konfigurovatelná. Vstupy používají interní `INPUT_PULLUP` a softwarový debounce.

### Reléové výstupy

| Relé | Výchozí funkce |
|---|---|
| R1 | směšovací ventil směrem k A; teplá větev z AKU; zvyšuje AB; poloha 100 % |
| R2 | směšovací ventil směrem k B; vratná/chladnější větev; snižuje AB; poloha 0 % |
| R3 | přepínací ventil TUV/CH |
| R4 | cirkulační čerpadlo TUV |
| R5 | reléový požadavek TUV na kotel |
| R6 | volitelný vstup DEN/NOC kotle |
| R7 | omezovací relé výkonu kotle |
| R8 | stykač elektrické topné tyče AKU |

R1 a R2 mají centralizovaný interlock. Požadavek na současné sepnutí obou směrů je blokován.

Pro dvojici R1/R2 existuje atomický zápis a kontrolní čtení výstupního registru TCA9554. Regulační puls se považuje za spuštěný až po úspěšném potvrzení zápisu.

## Teplotní zdroje

### Teplotní role

| Role | Výchozí priorita / zdroj |
|---|---|
| `flow` | měřená CH teplota z OpenTherm |
| `return` | OpenTherm, případně DS18B20 podle role |
| `dhw_tank` | OpenTherm TUV |
| `outside` | OpenTherm → DS18B20 GPIO0 → BLE |
| `tank_top` | DS18B20 na GPIO3 |
| `tank_mid` | DS18B20 na GPIO3 |
| `tank_bottom` | DS18B20 na GPIO3 |
| `dhw_return` | DS18B20 na GPIO1 |

### Doporučené fyzické rozdělení DS18B20

| GPIO | Použití |
|---:|---|
| GPIO0 | volitelná venkovní teplota |
| GPIO1 | zpátečka cirkulace TUV |
| GPIO2 | zpátečka topného okruhu / port B |
| GPIO3 | AKU nahoře, uprostřed a dole |

Přiřazení konkrétních ROM adres k rolím se ukládá do NVS. Prázdná ROM znamená automatický výběr podle pravidel dané role.

### Zdroje směšovacího ventilu

Aktuální hydraulické mapování:

| Port | Výchozí zdroj |
|---|---|
| A | `tank_mid` – teplota akumulační nádrže uprostřed |
| B | `return_dallas` – samostatné DS18B20 na vratné větvi |
| AB | `opentherm_ch` – kotlem měřená CH teplota |

> [!NOTE]
> AB z OpenTherm je teplota hlášená kotlem. Její změnu může ovlivnit také provoz kotle, čerpadla a zpoždění měření. Pro fyzicky nezávislou zpětnou vazbu je přesnější samostatné čidlo umístěné přímo za směšovacím ventilem.

## Ekvitermní regulace

Regulátor počítá cílovou teplotu topné vody lineární interpolací mezi dvěma body zvolené křivky:

```text
venkovní chladný bod → požadovaná vysoká teplota vody
venkovní teplý bod   → požadovaná nízká teplota vody
```

K dispozici jsou samostatné křivky DEN a NOC.

Výsledek je omezen:

- globálním minimem a maximem topné vody,
- minimem a maximem CH setpointu,
- případně limity načtenými z kotle,
- volitelným maximem OpenTherm ID57.

### Režimy

- `auto` – podle týdenního plánu, případně IN1,
- `day` – trvale denní křivka,
- `night` – trvale noční křivka.

### Letní režim

Při povoleném letním režimu se topení vypne nad nastavenou venkovní teplotou a znovu povolí až pod spodní hranicí. Dvě oddělené hranice tvoří hysterezi.

### Odesílání do kotle

Ekvitermní modul předává OpenTherm požadavek centrální arbitráži. Odesílání omezuje:

- minimální interval mezi zápisy,
- minimální změna cílové teploty,
- priorita TUV,
- aktivní nebo blokovaný provozní režim.

## Směšovací ventil a podpora z AKU

### Fyzický význam směrů

- **A / R1 / 100 %** – teplá větev z akumulační nádrže; správně zapojený ventil má zvyšovat teplotu AB.
- **B / R2 / 0 %** – vratná nebo chladnější větev; správně zapojený ventil má snižovat teplotu AB.

Před automatickým provozem proveďte ruční krátký puls A a B a ověřte skutečnou reakci teploty a mechaniky. Pokud je reakce opačná, opravte zapojení pohonu; neměňte pouze názvy v UI.

### Výchozí časování

| Parametr | Výchozí hodnota |
|---|---:|
| plný přejezd | 6000 ms |
| regulační puls | 300 ms |
| minimální interval / potvrzení cíle | 30000 ms |
| dosednutí při kalibraci | 1500 ms |

Hodnoty uložené z UI mají přednost před výchozími hodnotami.

### Regulační cyklus

1. Vyhodnotí se rozdíl AB vůči cílové teplotě.
2. Pokud je rozdíl mimo mrtvou zónu a dovoluje to časování, spustí se puls A nebo B.
3. Puls se vždy dokončí nebo explicitně zastaví.
4. Po vypnutí relé začne běžet `Min. interval mezi pulsy`.
5. Během intervalu se sleduje ustálení teploty AB.
6. Další puls je povolen pouze tehdy, když je AB mimo cílové pásmo a předchozí reakce se již ustálila.
7. Pokud AB zůstane v cílovém pásmu po celý interval, cíl se považuje za potvrzený.

Běžný neúčinný regulační puls nezneplatňuje polohu a nespouští automatickou kalibraci.

### Odhad polohy

Poloha se odhaduje podle:

- nastavené doby plného přejezdu,
- směru relé,
- skutečné délky pulsu,
- poslední důvěryhodné krajní polohy.

Jde o časový odhad, nikoliv fyzické snímání polohy.

### Kalibrace

Kalibrace provede mechanický přejezd do známé krajní polohy a obnoví důvěryhodnost odhadu.

Automatická rekalibrace:

- hodnota `0 h` znamená skutečně vypnuto,
- neprobíhá v letním režimu,
- neprobíhá bez požadavku na topení,
- neprobíhá během priority TUV,
- neprovádí se jako reakce na běžný neúčinný puls,
- je oddělena od standardního regulačního cyklu.

### Podpora kotle z akumulační nádrže

Podpora z AKU používá dva oddělené cíle.

Příklad:

```text
EQ cíl:                   22 °C
Offset cíle podpory:       5 °C
OpenTherm cíl kotle:      22 °C
Cíl směšovacího ventilu:  27 °C
```

Offset se nikdy nepřičítá k CH setpointu odesílanému kotli. Používá se pouze jako zvýšený cíl směšovacího ventilu během aktivní podpory.

Podpora se aktivuje pouze tehdy, když:

- je funkce povolena,
- směšovací ventil je povolen,
- je platná teplota A,
- teplota A dokáže dodat zvýšený cíl,
- topný režim a priority umožňují provoz.

Po potvrzeném dosažení cíle lze zvolit:

- `hold` – obě relé zůstanou vypnuta a ventil setrvá v aktuální poloze,
- `return_a` – po splnění časových podmínek proběhne přejezd do A.

Pokud AB opustí cílové pásmo s hysterezí, stav dosaženého cíle se odblokuje a regulace může pokračovat.

## TUV a cirkulace

### Ohřev TUV

Požadavek může vzniknout:

- digitálním vstupem IN2,
- týdenním plánem,
- ručním příkazem z UI nebo MQTT,
- anti-legionella režimem.

Sekvence může zahrnovat:

1. přepnutí ventilu R3,
2. čekání na mechanický předstih ventilu,
3. sepnutí R5 nebo vytvoření OpenTherm DHW požadavku,
4. sledování teploty zásobníku,
5. ukončení požadavku,
6. bezpečné přepnutí ventilu zpět.

Při aktivní prioritě TUV může být ekvitermní regulace a směšování blokováno.

### Cirkulace TUV

Cirkulace přes R4 podporuje:

- vstup IN3,
- týdenní plán,
- pulzní režim ON/OFF,
- ruční spuštění z UI nebo MQTT.

### Anti-legionella

Konfigurovatelné parametry:

- den v týdnu,
- čas startu,
- cílová teplota,
- minimální doba výdrže.

## OpenTherm

### Zapojení

```text
ESP32 TX GPIO47 → vstup OpenTherm adaptéru
ESP32 RX GPIO48 ← výstup OpenTherm adaptéru
```

Logické invertování lze upravit v konfiguraci OpenTherm.

### Režimy

- `readOnly` – čtení kotle bez aktivního řízení,
- `control` – čtení a odesílání požadavků.

### Hlavní data

- stav CH a DHW,
- aktivita hořáku,
- měřená CH teplota,
- teplota TUV,
- venkovní teplota,
- tlak soustavy,
- relativní modulace,
- fault flagy a OEM kód,
- požadovaný CH a DHW setpoint,
- podporované limity kotle.

### Data-ID diagnostika

Webové UI umožňuje:

- spustit neblokující scan Data-ID 0 až 127,
- zobrazit nalezené hodnoty,
- živě číst vybraný Data-ID,
- při výslovném povolení provádět raw zápis.

Raw zápis je ve výchozím stavu vypnutý.

## Síť a čas

### Wi-Fi

- režim STA,
- WiFiManager pro prvotní nastavení,
- automatický reconnect,
- vypnutý Wi-Fi power save,
- konfigurační AP `ESP-HeatCtrl-XXXXXX`,
- IN8 při bootu vynutí konfigurační portál.

### Ethernet

W5500 se inicializuje souběžně s Wi-Fi. Zařízení se považuje za online, pokud má platnou IP alespoň jedno rozhraní.

`networkGetIp()` upřednostňuje Ethernetovou IP a při nedostupném Ethernetu použije Wi-Fi IP.

### Čas

Výchozí konfigurace:

```text
TZ: CET-1CEST,M3.5.0,M10.5.0/3
NTP1: pool.ntp.org
NTP2: europe.pool.ntp.org
NTP3: time.nist.gov
```

Čas je považován za platný po úspěšné SNTP synchronizaci. Aktuální implementace nevyužívá RTC jako aktivní zdroj času.

## Webové rozhraní

### Adresy

```text
http://<IP zařízení>/
http://<IP zařízení>/filemanager
```

`/filemanager` je obsluhován přímo z firmware a zůstává dostupný i při chybějícím nebo poškozeném `/data/index.html`.

### Assety

Primární UI se načítá z LittleFS:

```text
/data/index.html
/data/app.css
/data/app.js
```

Ve firmware je zabudované servisní fallback rozhraní. Předkomprimované `.gz` varianty jsou v projektu přítomné, ale aktuální server má jejich automatické servírování vypnuté pomocí `kServePrecompressedAssets = false`.

### Realtime komunikace

- HTTP server: port 80
- WebSocket server: port 81
- rychlé plné a rozdílové rámce `fast_full` / `fast_patch`
- prioritní kanál `mix_cmd` pro okamžité ruční ovládání směšovacího ventilu
- UI odesílá ventilové akce už při `pointerdown`

Prioritní WebSocket kanál přijímá pouze omezenou sadu ventilových akcí:

```text
pulse_a
pulse_b
end_a
end_b
stop
```

Příkaz je potvrzen rámcem `mix_ack`, který obsahuje výsledek příkazu, masku relé a výsledek kontrolního zápisu.

### Hlavní REST endpointy

#### Rychlý stav

```text
GET /api/fast
GET /api/bootstrap
```

#### Konfigurace

```text
GET  /api/config
POST /api/config/apply
POST /api/config/export
POST /api/config/import

GET/POST /api/config/inputs
GET/POST /api/config/equitherm
GET/POST /api/config/opentherm
GET/POST /api/config/ble
GET/POST /api/config/ota
GET/POST /api/config/dhw
GET/POST /api/config/dallas
GET/POST /api/config/alerts
GET/POST /api/config/mqtt
GET/POST /api/config/time
```

#### Regulace

```text
GET  /api/equitherm/status
POST /api/equitherm/cmd
GET  /api/dhw/status
POST /api/dhw/cmd
POST /api/system/cmd
POST /api/relay
```

#### OpenTherm

```text
GET  /api/opentherm/status
POST /api/opentherm/cmd
GET  /api/opentherm/scan/status
GET  /api/opentherm/scan/profile
POST /api/opentherm/scan/start
POST /api/opentherm/scan/stop
POST /api/opentherm/dataid/read
POST /api/opentherm/dataid/write
```

#### Diagnostika

```text
GET /api/dallas/status
GET /api/ble/status
GET /api/ota/status
GET /api/mqtt/status
GET /api/events
GET /api/history
```

#### LittleFS a aktualizace

```text
GET  /api/fs/list
GET  /api/fs/read
POST /api/fs/write
POST /api/fs/mkdir
POST /api/fs/rename
POST /api/fs/delete
POST /api/fs/upload
POST /api/update/firmware
POST /api/update/filesystem
```

## MQTT a Home Assistant

MQTT klient používá ESP-MQTT z Arduino-ESP32 / ESP-IDF.

### Připojení

Konfigurace:

- povoleno / zakázáno,
- host bez prefixu `mqtt://`,
- port, výchozí 1883,
- uživatelské jméno a heslo,
- unikátní Client ID,
- základní topic,
- interval publikování,
- Home Assistant Discovery prefix a Node ID.

URI se skládá interně:

```text
mqtt://<host>:<port>
```

Aktuální implementace nepoužívá TLS.

### Publikované topicy

Při základním topicu `esp32-controller`:

| Topic | Obsah | QoS | Retain |
|---|---|---:|---|
| `esp32-controller/state` | kompletní provozní stav | 0 | ano |
| `esp32-controller/availability` | `online` / `offline` | 1 | ano |
| `esp32-controller/info` | identita a síť zařízení | 1 | ano |

`availability` používá Last Will `offline`.

### Stavový JSON

`state` obsahuje:

- uptime,
- stav Wi-Fi a Ethernetu,
- IP adresu,
- teploty a jejich zdroj/stáří,
- masku relé,
- masku aktivních vstupů,
- OpenTherm rychlý stav,
- BLE a OTA stav,
- platnost času,
- stav EQ a směšovacího ventilu,
- stav TUV a cirkulace.

### Přijímané příkazy

Zařízení odebírá:

```text
<baseTopic>/cmd/#
```

#### Relé

```text
<baseTopic>/cmd/relay/1/set
...
<baseTopic>/cmd/relay/8/set
```

Payload:

```text
ON
OFF
TOGGLE
1 / 0
TRUE / FALSE
```

> [!CAUTION]
> Obecné MQTT relé příkazy zapisují přímo do reléové vrstvy. Při automatickém provozu mohou kolidovat s vyšší řídicí logikou TUV, EQ nebo servisními omezeními. Pro běžnou automatizaci preferujte funkční příkazy TUV, EQ a směšovacího ventilu.

#### Režim EQ

```text
<baseTopic>/cmd/equitherm/mode/set
```

Payload:

```text
auto
day
night
```

#### TUV

```text
<baseTopic>/cmd/dhw/heat/set
```

Payload:

```text
ON
OFF
BOOST
```

`BOOST` vytvoří ruční požadavek s výchozí délkou 15 minut.

#### Cirkulace

```text
<baseTopic>/cmd/dhw/circ/set
```

Payload:

```text
ON
OFF
```

#### Směšovací ventil

```text
<baseTopic>/cmd/mix/pulse/set
```

Payload:

```text
A
OPEN
B
CLOSE
STOP
A_END
B_END
```

### Home Assistant Discovery

Discovery topicy mají tvar:

```text
<discoveryPrefix>/<component>/<nodeId>/<objectId>/config
```

Aktuálně se publikuje 24 entit:

- 10 senzorů:
  - venkovní teplota,
  - topná voda,
  - zpátečka,
  - TUV,
  - AKU nahoře,
  - AKU uprostřed,
  - AKU dole,
  - poloha směšovacího ventilu,
  - tlak systému,
  - požadovaná CH teplota,
- 8 přepínačů relé,
- 3 binární senzory vstupů,
- přepínač ohřevu TUV,
- přepínač cirkulace,
- výběr režimu EQ.

Discovery konfigurace používá QoS 1 a retained zprávy.

### Kontrola MQTT

Stav zařízení:

```text
GET /api/mqtt/status
```

Přímá kontrola brokeru pomocí Mosquitto:

```bash
mosquitto_sub -h 10.0.6.2 -p 1883 \
  -u mqtt -P 'HESLO' \
  -v -t 'esp32-controller/#'
```

Discovery:

```bash
mosquitto_sub -h 10.0.6.2 -p 1883 \
  -u mqtt -P 'HESLO' \
  -v -t 'homeassistant/+/esp32_controller/+/config'
```

### Význam runtime stavu

| Stav | Význam |
|---|---|
| `disabled` | MQTT je vypnuté |
| `waiting_network` | není dostupná platná Wi-Fi ani Ethernet IP |
| `missing_host` | není vyplněný broker |
| `connecting` | klient navazuje spojení |
| `connected` | klient je připojen |
| `disconnected` | spojení bylo ukončeno |
| `error` | MQTT nebo síťová chyba |
| `init_failed` | klient se nepodařilo vytvořit |
| `start_failed` | klient se nepodařilo spustit |

## Konfigurace a ukládání

Provozní konfigurace se ukládá do NVS pomocí `Preferences`.

Persistují zejména:

- polarita digitálních vstupů,
- OpenTherm režim a časování,
- DS18B20 role a ROM adresy,
- BLE nastavení,
- EQ křivky, limity, plán a letní režim,
- nastavení směšovacího ventilu,
- zdroje A/B/AB,
- podpora z AKU,
- TUV, cirkulace a anti-legionella,
- MQTT a Home Assistant,
- NTP a časová zóna,
- OTA,
- alarmy.

Soubor `data/config.json` je výchozí nebo exportovaný snapshot pro webové UI. Skutečný runtime stav může být po uložení přes UI odlišný, protože rozhodující hodnoty jsou v NVS.

Konfiguraci lze exportovat a importovat přes webové API.

## Aktualizace firmware a LittleFS

### Arduino OTA

- síťový port v Arduino IDE,
- mDNS služba `_arduino._tcp`,
- výchozí port 3232,
- volitelné heslo,
- prakticky používá Wi-Fi rozhraní.

### Webový správce

Na `/filemanager` lze:

- prohlížet LittleFS,
- vytvářet složky a textové soubory,
- číst a upravovat soubory,
- nahrávat jednotlivé UI assety,
- nahrát firmware `.bin`,
- nahrát kompletní filesystem image.

Firmware upload kontroluje maximální velikost cílové OTA partition.

Filesystem image musí přesně odpovídat velikosti filesystem partition definované v `partitions.csv`.

### Partition tabulka

| Partition | Velikost |
|---|---:|
| NVS | `0x5000` |
| OTA data | `0x2000` |
| app0 | `0x640000` |
| app1 | `0x640000` |
| LittleFS/SPIFFS data | `0x360000` |
| coredump | `0x10000` |

## Kompilace

### Doporučené prostředí

- Arduino IDE 2.x
- Arduino-ESP32 **3.x**
- cílová deska ESP32-S3 s odpovídající velikostí flash a PSRAM
- vlastní partition tabulka `partitions.csv`

### Externí knihovny

Projekt používá mimo komponenty dodávané s Arduino-ESP32 zejména:

- ArduinoJson
- WiFiManager
- arduinoWebSockets (`WebSocketsServer`)
- NimBLE-Arduino
- Adafruit NeoPixel
- Adafruit HTU21DF
- Adafruit BMP085
- BH1750

OpenTherm, OneWire a související implementace jsou součástí zdrojového stromu projektu.

### Důležitá nastavení

- správný cílový ESP32-S3 profil,
- povolená PSRAM, pokud ji deska nabízí,
- partition scheme odpovídající přiloženému `partitions.csv`,
- USB/serial nastavení podle použitého programovacího rozhraní,
- flash size odpovídající fyzické desce.

### Nahrání UI

Po nahrání firmware nahrajte obsah složky `data/` do LittleFS nebo jednotlivé soubory přes `/filemanager`.

Minimální sada:

```text
/index.html
/app.css
/app.js
```

## První spuštění

1. Zkontrolujte zapojení OpenTherm adaptéru a I/O.
2. Nahrajte firmware a LittleFS data.
3. Připojte se k AP `ESP-HeatCtrl-XXXXXX`.
4. Nastavte Wi-Fi přístupové údaje.
5. Otevřete IP zařízení.
6. V konfiguraci teploměrů přiřaďte ROM adresy DS18B20.
7. Ověřte hodnoty A, B a AB.
8. Ručně otestujte krátký puls A a B.
9. Ověřte, že R1 zvyšuje AB a R2 ji snižuje.
10. Nastavte plný přejezd, délku pulsu, mrtvou zónu a minimální interval.
11. Proveďte kalibraci ventilu.
12. Nastavte EQ křivky a bezpečné limity.
13. Ověřte OpenTherm nejprve v read-only režimu.
14. Teprve poté povolte aktivní řízení kotle.
15. Nastavte TUV a ověřte pořadí ventil → kotel → návrat.
16. Nastavte unikátní MQTT Client ID, Base Topic a Node ID.

## Diagnostika

### Sériová konzole

Rychlost:

```text
115200 baud
```

Dostupné příkazy:

```text
HELP
STATE
INPUTS
TEMP
OT
OTSCAN START
OTSCAN ALL
OTSCAN STATUS
OTSCAN STOP
BLE
OTA
EQ
EQ MODE DAY
EQ MODE NIGHT
EQ MODE AUTO
R1 ON / OFF / TOGGLE
...
R8 ON / OFF / TOGGLE
WIFI PORTAL
```

### Doporučené diagnostické endpointy

```text
/api/fast
/api/equitherm/status
/api/dhw/status
/api/opentherm/status
/api/dallas/status
/api/mqtt/status
/api/ota/status
/api/events
/api/history
```

### Relé a I²C

`/api/fast` poskytuje:

- aktuální masku relé,
- stav reléového driveru,
- počet I²C chyb,
- počet zotavení sběrnice.

Při chybě potvrzení R1/R2 se regulační příkaz nepovažuje za úspěšně spuštěný a stav směšování může hlásit `fault_relay_write`.

### Bezpečné zastavení

```text
POST /api/system/cmd
```

JSON:

```json
{
  "command": "safeStop"
}
```

Příkaz vypne směšovací relé a související aktivní výstupy TUV/cirkulace/topné tyče podle implementované bezpečné sekvence.

## Struktura projektu

```text
ESP32-S3-ETH-8DI-8RO-Controller/
├── ESP32-S3-ETH-8DI-8RO-Controller.ino
├── Features.h
├── config_pins.h
├── partitions.csv
├── VERSION.txt
│
├── NetworkController.*
├── MqttController.*
├── WebPortalController.*
├── WebPortalAssets.h
├── OtaController.*
│
├── OpenThermController.*
├── OTBusESP32Pro.*
├── OpenThermPlusESP32.*
├── OpenTherm.*
│
├── TemperatureManager.*
├── DallasController.*
├── OneWireESP32.*
├── BleController.*
│
├── EquithermController.*
├── DhwController.*
├── RelayController.*
├── InputController.*
├── PressureAlarmController.*
├── BuzzerController.*
├── RgbLedController.*
│
├── ConfigStore.*
├── ConfigRuntime.*
├── EventLog.*
├── HistoryBuffer.*
├── I2cBus.*
│
└── data/
    ├── index.html
    ├── app.css
    ├── app.js
    ├── config.json
    ├── inputs.js
    └── roles.js
```

## Pořadí hlavní smyčky

Aktuální `loop()` zpracovává moduly přibližně v tomto pořadí:

1. sériová konzole,
2. digitální vstupy,
3. reléový driver,
4. DS18B20,
5. síť,
6. webový portál a WebSocket,
7. OpenTherm,
8. BLE,
9. historie,
10. sjednocení teplotních rolí,
11. OTA,
12. MQTT,
13. bzučák a alarm tlaku,
14. TUV/cirkulace,
15. ekvitermní regulace a směšovací ventil.

WebSocket je navíc obsluhován lehkým background hookem během čekání na OpenTherm, aby ruční akce ventilu neměly zbytečnou prodlevu.

## Známá omezení

- Poloha směšovacího ventilu je časový odhad bez fyzického snímače polohy.
- AB z OpenTherm není nezávislé čidlo přímo za ventilem.
- MQTT používá nešifrované `mqtt://`; TLS není implementováno.
- MQTT `state` používá QoS 0. `lastPublishMs` potvrzuje pokus o zařazení zprávy, ne potvrzení brokerem.
- `discoveryPublished=true` znamená, že firmware discovery zprávy odeslal do klienta; nevede samostatný počet brokerových ACK pro každou entitu.
- Obecné MQTT přepínače relé mohou zasahovat do automaticky řízených výstupů.
- Seznam MQTT entit v některých konfiguračních náhledech nemusí být tak úplný jako skutečný runtime Discovery seznam.
- Při změně Home Assistant `nodeId` nebo discovery prefixu se staré retained discovery topicy automaticky nemažou.
- Automatické servírování `.gz` UI assetů je v aktuální verzi vypnuté.
- RTC je na cílové desce fyzicky přítomné, ale aktuální síťová časová vrstva vrací `networkIsRtcPresent() == false` a používá SNTP.
- `WebServerController.*` představuje starší/alternativní webovou vrstvu; aktivní UI používá `WebPortalController`.

## Související dokumentace v projektu

- `CHANGELOG.md` – historie změn
- `IMPORTANT_FUNCTIONS.md` – orientace v modulech a důležitých funkcích
- `UI_REFACTOR_NOTES.md` – poznámky k webovému UI
- `WS_AUDIT.md` – audit WebSocket komunikace
- `VERSION.txt` – verze firmware

## Licence a použití

Projekt je určen pro vývojové, experimentální a soukromé použití. Nasazení do reálné topné soustavy je na odpovědnosti provozovatele a musí respektovat požadavky výrobce kotle, pohonu ventilu, elektrické instalace a platné bezpečnostní předpisy.
