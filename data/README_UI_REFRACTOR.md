# Refaktor UI (LittleFS) – struktura a rozšíření

## Struktura `data/`

```
data/
  index.html

  css/
    base.css
    layout.css
    components.css
    pages.css
    schematic.css
    dash.css

  js/
    app.js
    core/
      dom.js
      util.js
      api.js
      store.js
      events.js
      router.js
      toast.js
      roles.js
      dirty.js
      polling.js
      configShape.js
      ui.js
      schematic.*.js
      buzzer.js
      valve_calib.js
      time_ntp.js
      schedules.js
      opentherm.js
      legacy.js
    pages/
      dashboard.js
      schema.js
      ekviterm.js
      tuv.js
      recirc.js
      aku.js
      aku_heater.js
      iofunc.js
      temps.js
      mqtt.js
      ble.js
      system.js
      ota.js
      files.js
      rules.js

  pages/
    dashboard.html
    schema.html
    ekviterm.html
    tuv.html
    recirc.html
    aku.html
    aku_heater.html
    iofunc.html
    temps.html
    mqtt.html
    ble.html
    system.html
    ota.html
    files.html
    rules.html
```

### Základní tok

- `index.html` je **shell** s navigací a `<div id="view"></div>`.
- Router (`js/core/router.js`) lazy-loaduje HTML fragmenty z `pages/*.html` do `#view`.
- Každá stránka má JS modul v `js/pages/`, který definuje `window.Pages.<id>` a lifecycle.
- Obecná logika (API, store, polling, toast, router) je v `js/core/`.

## Jak přidat novou stránku

1. **HTML fragment**
   - Vytvoř `data/pages/<id>.html`.
   - Fragment obsahuje pouze obsah stránky (bez `<html>`, `<head>`, `<body>`).
   - Kořenový element doporučeně `<section class="page" id="page-<id>">`.

2. **JS modul stránky**
   - Vytvoř `data/js/pages/<id>.js` a zaregistruj stránku:

```js
(function(){
  window.Pages = window.Pages || {};
  window.Pages.<id> = {
    id: "<id>",
    mount(ctx) { /* inicializace */ },
    unmount() { /* cleanup */ },
    onStoreUpdate(type, data) { /* volitelně */ }
  };
})();
```

3. **Navigace**
   - Přidej položku do menu v `data/index.html` (anchor `href="#<id>"`).

4. **Volitelné**
   - Pokud stránka potřebuje polling nebo data z API, použij `Core.api` nebo `Core.legacy.*`.
   - Pro notifikace použij `Core.toast.show()`.

## Poznámky k kompatibilitě

- Vše je v **vanilla JS** bez ES modulů.
- `window.App` zůstává jako kompatibilní fasáda pro starší skripty.
- HTML fragmenty jsou cachované v paměti pro minimalizaci requestů.
