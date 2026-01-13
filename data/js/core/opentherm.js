(() => {
  const $ = (id) => document.getElementById(id);
  const toast = (msg, type) => (window.App && App.toast) ? App.toast(msg, type) : alert(msg);

  let els = null;
  let initialized = false;
  const getEls = () => ({
    enabled: $("otEnabled"),
    mode: $("otMode"),
    inPin: $("otInPin"),
    outPin: $("otOutPin"),
    pollMs: $("otPollMs"),
    chEnable: $("otChEnable"),
    manualSetpointC: $("otManualSetpoint"),
    btnSave: $("btnSaveOpenTherm"),
    statusBox: $("otStatus"),
  });

  const ensureShape = (cfg) => {
    cfg = cfg || {};
    cfg.opentherm = (cfg.opentherm && typeof cfg.opentherm === "object") ? cfg.opentherm : {};
    cfg.opentherm.enabled = !!cfg.opentherm.enabled;
    cfg.opentherm.mode = String(cfg.opentherm.mode || "off");
    cfg.opentherm.inPin = Number.isFinite(Number(cfg.opentherm.inPin)) ? Number(cfg.opentherm.inPin) : -1;
    cfg.opentherm.outPin = Number.isFinite(Number(cfg.opentherm.outPin)) ? Number(cfg.opentherm.outPin) : -1;
    cfg.opentherm.pollMs = Math.max(200, Math.min(60000, Number(cfg.opentherm.pollMs || 1000)));
    cfg.opentherm.chEnable = (cfg.opentherm.chEnable !== false);
    cfg.opentherm.manualSetpointC = Math.max(10, Math.min(80, Number(cfg.opentherm.manualSetpointC || 45)));
    return cfg;
  };

  const loadUiFromConfig = () => {
    const cfg = ensureShape(App.getConfig?.() || {});
    if (els.enabled) els.enabled.checked = !!cfg.opentherm.enabled;
    if (els.mode) els.mode.value = cfg.opentherm.mode;
    if (els.inPin) els.inPin.value = cfg.opentherm.inPin;
    if (els.outPin) els.outPin.value = cfg.opentherm.outPin;
    if (els.pollMs) els.pollMs.value = cfg.opentherm.pollMs;
    if (els.chEnable) els.chEnable.checked = !!cfg.opentherm.chEnable;
    if (els.manualSetpointC) els.manualSetpointC.value = cfg.opentherm.manualSetpointC;
  };

  const saveUiToConfig = async () => {
    const cfg = ensureShape(App.getConfig?.() || {});
    cfg.opentherm.enabled = !!els.enabled?.checked;
    cfg.opentherm.mode = String(els.mode?.value || "off");
    cfg.opentherm.inPin = Number(els.inPin?.value ?? -1);
    cfg.opentherm.outPin = Number(els.outPin?.value ?? -1);
    cfg.opentherm.pollMs = Number(els.pollMs?.value ?? 1000);
    cfg.opentherm.chEnable = !!els.chEnable?.checked;
    cfg.opentherm.manualSetpointC = Number(els.manualSetpointC?.value ?? 45);

    // write back
    Object.assign(App.getConfig(), cfg);
    await App.saveConfig();
    toast("OpenTherm konfigurace uložena", "ok");
  };

  const refreshStatus = async () => {
    if (!els.statusBox) return;
    try {
      const r = await fetch("/api/opentherm/status");
      const j = await r.json();
      els.statusBox.textContent = JSON.stringify(j, null, 2);
    } catch (e) {
      els.statusBox.textContent = `Chyba: ${e.message}`;
    }
  };

  const init = () => {
    if (initialized) return;
    initialized = true;
    els = getEls();
    if (!els.enabled || !els.mode) return; // page not present

    // App hook
    // IMPORTANT:
    // - legacy.js volá App.onConfigLoaded(cfg) bez await.
    // - nesmíme proto vracet Promise (jinak "Uncaught (in promise)" a rozbití dalších modulů).
    // - vždy předáme cfg dál, jinak navazující moduly dostanou undefined.
    if (window.App) {
      App.onConfigLoaded = ((prev) => (cfg) => {
        try { if (typeof prev === "function") prev(cfg); } catch (_) {}
        loadUiFromConfig(cfg);
      })(App.onConfigLoaded);
    }

    els.btnSave && els.btnSave.addEventListener("click", () => saveUiToConfig().catch(e => toast(e.message, "bad")));
    refreshStatus().catch(() => {});
  };

  window.addEventListener("app:pageMounted", (ev) => {
    if (ev?.detail?.id === "system") init();
  });
})();
