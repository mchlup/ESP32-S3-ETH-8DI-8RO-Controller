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
