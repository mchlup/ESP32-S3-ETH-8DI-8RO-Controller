(function(){
  let timers = [];
  let offline = false;

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

  const start = () => {
    stop();
    const legacy = window.Core?.legacy;
    if (!legacy) return;
    timers.push(setInterval(() => runSafe(legacy.loadStatus), 1200));
    timers.push(setInterval(() => runSafe(legacy.loadDash), 1500));
    timers.push(setInterval(() => runSafe(legacy.loadConfig), 15000));
    timers.push(setInterval(() => runSafe(legacy.loadBle), 2500));
  };

  const stop = () => {
    timers.forEach((t) => clearInterval(t));
    timers = [];
  };

  window.Core = window.Core || {};
  window.Core.polling = { start, stop };
})();
