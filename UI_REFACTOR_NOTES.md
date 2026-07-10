# Web UI refactor v3.3.13

## Změny

- Provozní relé se na stránce I/O zobrazují pouze diagnosticky. Ruční test zůstává oddělený v servisní diagnostice.
- Frontend již nepoužívá přímý endpoint `/api/relay` pro běžné ovládání funkcí.
- TUV, cirkulace, ekviterm a bezpečné zastavení používají své backendové příkazy.
- Servisní puls relé je neblokující; HTTP/WebSocket smyčka během pulsu pokračuje.
- CSS a JavaScript mají verzi v URL a rozumnou revalidaci cache.
- Přidány předkomprimované `app.js.gz` a `app.css.gz` pro LittleFS.
- Mobilní rozvržení je kompaktnější a vykreslování karet používá CSS containment.
- Respektuje se systémová volba omezených animací.

## Ověření

- `node --check data/app.js`
- kontrola duplicitních HTML ID
- kontrola integrity ZIP

## Poznámka

Přímý endpoint `/api/relay` zůstává v backendu kvůli kompatibilitě a servisním nástrojům, ale hlavní UI jej nepoužívá.
