(function(){
  let timers = [];
  let offline = false;
  // Zabraň „request stormu“: pokud je některý polling request pomalý/hung,
  // další tick ho znovu nespustí a nezahlcuje webserver.
  const inFlight = Object.create(null);

  const runSafe = async (fn) => {
    try {
      await fn();
      if (offline) {
        offline = false;
        window.Core?.toast?.show?.('Znovu online', 'good');
      }
    } catch (e) {
      if (!offline) {
        offline = true;
        window.Core?.toast?.show?.('Zařízení offline nebo API nedostupné', 'warn');
      }
    }
  };

  const runLocked = async (key, fn) => {
    if (inFlight[key]) return;
    inFlight[key] = true;
    try {
      await runSafe(fn);
    } finally {
      inFlight[key] = false;
    }
  };

  const start = () => {
    stop();
    const legacy = window.Core?.legacy;
    if (!legacy) return;

    // Polling musí být šetrný – u ESP (hlavně s BLE/NimBLE) se /api/status může protáhnout.
    // Krátké intervaly sice díky inFlight nevytvoří paralelní storm, ale UI pak tráví
    // prakticky celý čas čekáním na HTTP a působí "zamrzle".
    timers.push(setInterval(() => runLocked('status', legacy.loadStatus), 2500));
    timers.push(setInterval(() => runLocked('dash', legacy.loadDash), 3000));
    timers.push(setInterval(() => runLocked('config', legacy.loadConfig), 20000));
    timers.push(setInterval(() => runLocked('ble', legacy.loadBle), 4000));
  };

  const stop = () => {
    timers.forEach((t) => clearInterval(t));
    timers = [];
  };

  window.Core = window.Core || {};
  window.Core.polling = { start, stop };
})();
