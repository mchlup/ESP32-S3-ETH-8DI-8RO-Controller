## 3.5.3 - Odezva API, OT polling a diagnostika

- OpenTherm polling byl rozdělen na jednotlivé transakce; jeden průchod hlavní smyčky již neprovádí dlouhý řetězec ID0/25/28/26/17/18/volitelných ID. Tím se odstranilo blokování HTTP a WebSocket obsluhy při timeoutu kotle.
- Interní požadavky ekvitermu a TUV se nyní frontují a aplikují v `openthermLoop()` po jedné OT transakci. `/api/dhw/cmd` proto vrací okamžitě a nečeká na několik OpenTherm zápisů a následný kompletní poll.
- Opakované uložení stejné OT konfigurace už neničí a znovu neinicializuje živý OpenTherm transport ani nemaže platnou telemetrii.
- Stav linky používá čas posledního skutečně přijatého OpenTherm rámce, nikoli pouze interní stav knihovny.
- ID15 (maximální výkon kotle / minimální modulace) se čte nízkofrekvenčně v OT round-robin a posílá v `/api/fast`; nepodporovaný ID15 už nevytváří falešnou chybu `missing maxCapacityKw`.
- `/api/opentherm/scan/status` standardně neposílá 128 položek; plný seznam je vyžádán pouze volbou „Zobrazit vše“. Výchozí zobrazení je omezené na podporované ID.
- Opravena struktura profilu scan ve front-endu (`profile.hasProfile`, `supportedIds`, `items`).
- Pokud topení nebo TUV používá OpenTherm, pokus o přepnutí OT do `readOnly`/vypnutí se ve webu bezpečně normalizuje zpět na `control` namísto chybového dialogu.
- Opraveny popisky Data-ID v diagnostice: ID15 = kapacita/min. modulace, ID25 = teplota kotle, ID26 = teplota TUV.

## 3.5.2 - Návrat k funkčnímu OpenTherm transportu 3.3.14

- Porovnáním s přiloženou funkční verzí 3.3.14 bylo ověřeno, že `OpenTherm.cpp/.h`, `OTBusESP32Pro.cpp/.h`, `OpenThermController.cpp/.h`, `TemperatureManager.cpp`, `config_pins.h` a `OpenThermDataIds.h` byly v původní 3.5.0 shodné s funkční verzí; problém tedy nevznikl změnou pinů ani původní OT knihovny mezi 3.3.14 a 3.5.0.
- Změny fyzické OT vrstvy z verze 3.5.1 (runtime inverze RX/TX a autodetekce polarity) byly odstraněny. `OpenTherm.cpp/.h` a `OTBusESP32Pro.cpp/.h` jsou opět byte-for-byte shodné s přiloženou funkční verzí.
- OpenTherm běží na původním zapojení RX GPIO48 / TX GPIO47 a používá původní volání `begin(rx, tx, true)`. Piny jsou nyní pro tuto desku v OT vrstvě pevně vynucené, takže je nemůže změnit starý/importovaný LittleFS snapshot. Staré hodnoty `invertRx`, `invertTx` a `autoDetectLogic` se z konfigurace z bezpečnostních důvodů ignorují.
- Opravena nadřazená logika čtení: neúspěšný ID0 již automaticky nezablokuje zdroj teplot. Při selhání ID0 se provede jediný ověřovací dotaz ID25; pokud odpoví, pokračuje čtení ID25/28/26 a dalších hodnot. Na odpojené sběrnici se zároveň nespouští řetězec mnoha sekundových timeoutů.
- `DATA_INVALID` / `UNKNOWN_DATA_ID` na ID0 se rozpoznají z raw rámce bez zásahu do původního fyzického transportu.
- `TemperatureManager` používá age-checked OpenTherm gettery. OT hodnoty přepisují DS18B20 pouze dokud jsou čerstvé; při ztrátě OT se zdroje Return/DHW/Outside vrátí na DS18B20/BLE fallback a Flow se zneplatní.
- Webová diagnostika zobrazuje transportní profil `legacy-3.3.14`; asset verze byla zvýšena na 3.5.2, aby prohlížeč nepoužil starý JS z cache.

## 3.5.1 - Oprava OpenTherm a volitelné zdroje teplot

- Opraveno skutečné použití konfigurace `invertRx` / `invertTx` v OpenTherm vrstvě.
- Přidána automatická detekce polarity OpenTherm adaptéru (RX/TX) s bezpečným návratem na poslední funkční kombinaci.
- `DATA_INVALID` a `UNKNOWN_DATA_ID` jsou správně rozlišeny jako platné OpenTherm rámce, nikoliv porucha transportu.
- Pokud kotel nevrátí použitelný status ID0, čtení teplotních Data-ID pokračuje a není globálně zablokováno.
- Opraveny identifikátory v OpenTherm UI: CH teplota = ID25, TUV teplota = ID26.
- `TemperatureManager` doplněn o explicitní zdroj `opentherm_return` a zachování DS-only zdroje `return_dallas`.
- Při ztrátě konkrétní volitelné OT teploty se její stará hodnota zneplatní, takže `TemperatureManager` může korektně přejít na DS18B20/BLE fallback.
- Zdroje teplot směšovacího ventilu lze podle portu volit mezi OpenTherm a DS18B20; volba se správně ukládá v `ConfigStore`.
- OpenTherm diagnostika ve webu zobrazuje aktuální piny, polaritu RX/TX a stav auto-detekce.
- Stav „OT: OK“ nyní vyžaduje skutečně přijatou odpověď; interní `ready` a chybový příznak kotle se již nezaměňují se stavem komunikace.
- Ekvitermní řízení považuje OpenTherm za dostupný až po skutečně dokončené výměně s kotlem, ne pouze podle interního stavu knihovny `ready`.

## 3.5.0 - Průvodce nastavením a rozšíření TECH EU-i-3

- Přidán kompletní front-end/back-end „Průvodce nastavením zařízení“ s perzistentním stavem dokončení.
- Směšovací ventil podporuje profily ÚT, podlaha, ochrana zpátečky, bazén a ventilace.
- Podlahový profil v režimu TECH i-3 vynucuje ochranu maximální teploty a bezpečnou kalibraci do B / 0 %.
- Přidán konfigurovatelný směr otevírání bez přepojování vodičů R1/R2.
- TECH i-3 režim používá pevnou automatickou kalibraci po 48 h a profilovou bezpečnou referenční polohu.
- Kalibrační a krajní přejezdy respektují celý zadaný čas servopohonu i u pomalých servopohonů nad 60 s.
- Přidán samostatný týdenní program úplného zavření ventilu po 30 minutách.
- Přidána týdenní hodinová korekce cílové teploty ventilu v rozsahu -20 až +20 °C.
- Venkovní uzavření má vlastní začátek dne/noci, prahy a hysterezi.
- Ochrana zdroje používá volitelný teplotní zdroj A („Čidlo ÚT“), zpátečka zdroj B a regulace výstupu AB.
- Rozšířeno API, stavová telemetrie, validační logika a snapshot konfigurace.
- Webová konfigurace směšovače byla doplněna o programy, zdroje teplot, kalibrační příkazy a responzivní průvodce.

## 3.3.14 - Filemanager and plain static assets

- `/filemanager` now always serves the embedded service file manager, independently of LittleFS `/index.html`.
- Disabled automatic serving of precompressed `.gz` UI assets.
- Main UI now loads plain `/app.css` and `/app.js` reliably on all supported browsers and Arduino cores.
- Removed generated `.gz` files from the project package.
- Updated UI asset version query to `3.3.14`.

# v3.3.13-ui-refactor

- Web UI: diagnostická I/O stránka již přímo nepřepíná provozní relé.
- Web UI: přidáno verzování CSS/JS, kompaktnější mobilní rozvržení a optimalizace vykreslování.
- Web portal: statické JS/CSS již nejsou cacheovány jako immutable bez fingerprintu.
- Web portal: servisní puls relé je neblokující; odstraněn `delay()` z HTTP handleru.
- Web UI: opraven text odpojení WebSocketu a omezen souběžný refresh po návratu stránky.
- Připraveny předkomprimované `.gz` assety pro rychlejší přenos z LittleFS.

# Changelog

## Opravy
