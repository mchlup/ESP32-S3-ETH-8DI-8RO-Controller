# Web UI pro ESP32-S3 řídicí jednotku

Tato složka obsahuje statické soubory pro **LittleFS**:

```
/index.html
/css/app.css
/js/app.js
/assets/*
```

## Nahrání do LittleFS

1. Nahrajte celou složku `LittleFS` do souborového systému zařízení.
2. Ověřte, že se soubory servírují na rootu web serveru (`/index.html`, `/css/app.css`, `/js/app.js`).

## Ověření funkčnosti

* Dashboard načítá:
  * `GET /api/status`
  * `GET /api/dash`
  * `GET /api/caps`
* Konfigurace používá:
  * `GET /api/config`
  * `POST /api/config`
* BLE používá:
  * `GET /api/ble/config`, `POST /api/ble/config`
  * `GET /api/ble/status`
  * `GET /api/ble/paired`, `POST /api/ble/pair`, `POST /api/ble/pair/stop`, `POST /api/ble/remove`
  * `POST /api/ble/meteo/retry`
* Buzzer používá:
  * `GET /api/buzzer`, `POST /api/buzzer`

## Doporučené intervaly obnovy

* `/api/status` každé **2 s** (přepínač „Pozastavit“ v UI).
* `/api/dash` každých **8 s**.

## Poznámky k UX a limitům

* UI je navrženo jako lehké SPA bez externích knihoven.
* Při manuálním ovládání relé a kalibracích se zobrazí varování, protože zařízení přepíná do MANUAL.
* Sekce jsou automaticky skrývány podle `/api/caps`.
