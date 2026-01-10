(function () {
  const intervals = {
    status: 1200,
    dash: 1500,
    config: 15000,
    ble: 2500,
  };

  let timers = {};
  let pendingConfig = null;
  let lastConfigHash = "";

  const hashConfig = (cfg) => {
    try {
      return JSON.stringify(cfg || {});
    } catch (_) {
      return "";
    }
  };

  const notifyConfigChanged = () => {
    if (!window.Core?.toast?.show) return;
    window.Core.toast.show("Konfigurace se změnila – obnovit?", "warn", {
      actionLabel: "Obnovit",
      onAction: () => {
        if (pendingConfig) {
          window.Core.store.setConfig(pendingConfig);
          lastConfigHash = hashConfig(pendingConfig);
          pendingConfig = null;
        }
      },
    });
  };

  const loadStatus = async () => {
    try {
      const status = await window.Core.api.getJson("/api/status");
      window.Core.store.setStatus(status);
      if (window.App?.onStatusLoaded) window.App.onStatusLoaded(status);
    } catch (e) {
      window.Core.toast.show(`Status: ${e.message}`, "bad");
    }
  };

  const loadDash = async () => {
    try {
      const dash = await window.Core.api.getJson("/api/dash");
      window.Core.store.setDash(dash);
      if (window.App?.onDashLoaded) window.App.onDashLoaded(dash);
    } catch (e) {
      window.Core.toast.show(`Dash: ${e.message}`, "bad");
    }
  };

  const loadConfig = async (force = false) => {
    try {
      const cfg = await window.Core.api.getJson("/api/config");
      const shaped = window.Core.configShape.ensure(cfg);
      const nextHash = hashConfig(shaped);
      const dirty = window.Core.dirty?.isDirty?.("form");
      if (dirty && !force) {
        if (nextHash && nextHash !== lastConfigHash) {
          pendingConfig = shaped;
          notifyConfigChanged();
        }
        return;
      }
      pendingConfig = null;
      lastConfigHash = nextHash;
      window.Core.store.setConfig(shaped);
      if (window.App?.onConfigLoaded) window.App.onConfigLoaded(shaped);
    } catch (e) {
      window.Core.toast.show(`Config: ${e.message}`, "bad");
    }
  };

  const loadBle = async () => {
    if (window.Core?.legacy?.loadBle) {
      try {
        await window.Core.legacy.loadBle();
      } catch (_) {}
    }
  };

  const start = () => {
    stop();
    timers.status = setInterval(loadStatus, intervals.status);
    timers.dash = setInterval(loadDash, intervals.dash);
    timers.config = setInterval(() => loadConfig(false), intervals.config);
    timers.ble = setInterval(loadBle, intervals.ble);
  };

  const stop = () => {
    Object.values(timers).forEach((t) => clearInterval(t));
    timers = {};
  };

  window.Core = window.Core || {};
  window.Core.polling = { start, stop, loadStatus, loadDash, loadConfig, loadBle };
})();
