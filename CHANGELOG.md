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
