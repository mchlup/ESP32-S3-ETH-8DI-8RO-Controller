# UI refaktor (LittleFS)

Tento refaktor rozděluje původní monolitické `index.html`/`app.js` do modulární struktury vhodné pro LittleFS bez bundleru.

## Struktura složek

```
data/
  index.html                 # shell + layout + <main id="view">
  css/
    base.css                 # základní styly (původní styles.css)
    layout.css               # layout (sidebar/topbar)
    components.css           # komponenty (card, btn, toast)
    pages.css                # page overrides
    schematic.css            # schéma
    dash.css                 # dashboard
  js/
    app.js                   # bootstrap
    core/                    # společné moduly
    pages/                   # page moduly (mount/unmount)
  pages/
    *.html                   # HTML fragmenty pro router
```

## Router a lazy loading

* `js/core/router.js` načítá HTML fragmenty `/pages/<page>.html` až při navigaci.
* Fragmenty se cachují v paměti (bez opakovaných requestů).
* Po vložení fragmentu router volá `Pages.<id>.mount(ctx)`.

## Core moduly

* `core/dom.js` – DOM helpery (`$`, `$$`, `delegate`…)
* `core/api.js` – `getJson`, `getText`, `postJson` s timeoutem
* `core/store.js` – centrální store (status/dash/config)
* `core/polling.js` – polling `/api/status`, `/api/dash`, `/api/config`
* `core/router.js` – hash router + lazy loading
* `core/toast.js` – notifikace
* `core/dirty.js` – dirty guard
* `core/roles.js` – mapování rolí
* `core/configShape.js` – defaulty konfigurace

## Přidání nové stránky

1. **HTML fragment**: vytvoř `/pages/nazev.html` (bez `<html>/<head>`), pouze obsah.
2. **JS modul**: vytvoř `/js/pages/nazev.js` a zaregistruj `window.Pages.nazev`:
   ```js
   (function () {
     const mount = (ctx) => {
       // ctx.root = element s vloženým HTML
     };
     window.Pages = window.Pages || {};
     window.Pages.nazev = { id: "nazev", mount, unmount() {} };
   })();
   ```
3. **Navigace**: přidej link do `index.html` s `href="#nazev"`.

## Poznámky

* Vše je bez ES modulů – klasické `<script>` a `window.App/Core/Pages` namespace.
* `core/legacy.js` poskytuje kompatibilní `window.App` API pro existující skripty.
