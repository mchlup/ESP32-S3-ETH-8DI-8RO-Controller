# Changelog

## Opravy
- Ukládání `/api/config` nyní serializuje vyfiltrovaný JSON, aby se do konfigurace nedostaly nechtěné klíče.
- BLE konfigurace a allowlist ukládají soubory atomicky a při poškození se obnovují z `.bak`.
- BLE meteo klient preferuje uloženou MAC, ale při opakovaných chybách nebo dlouhé absenci dat spouští fallback scan.
- BLE status má bezpečné ošetření `pairingRemainingSec` a LED logika rozlišuje párování, připojení, scan a chybu.
- Funkce Rule engine byla odstraněna z UI i API.

## Testy
- `arduino-cli version` (selhalo: arduino-cli není k dispozici v prostředí)
