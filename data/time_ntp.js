/* Time/NTP config module (v1) */
(() => {
  const App = window.App;
  if (!App) return;

  const $ = App.$;
  const toast = App.toast;
  const apiPostJson = App.apiPostJson;
  const apiGetJson = App.apiGetJson;

  const ensure = (cfg) => {
    cfg.time = (cfg.time && typeof cfg.time === "object") ? cfg.time : {};
    cfg.time.ntpEnabled = !!cfg.time.ntpEnabled;
    cfg.time.server1 = String(cfg.time.server1 || "pool.ntp.org");
    cfg.time.server2 = String(cfg.time.server2 || "time.google.com");
    cfg.time.tz = String(cfg.time.tz || "Europe/Prague");
    cfg.time.syncIntervalMin = Number.isFinite(Number(cfg.time.syncIntervalMin)) ? Number(cfg.time.syncIntervalMin) : 60;
  };

  const render = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return;
    ensure(cfg);

    if ($("#ntpEnabled")) $("#ntpEnabled").checked = !!cfg.time.ntpEnabled;
    if ($("#ntpServer1")) $("#ntpServer1").value = cfg.time.server1;
    if ($("#ntpServer2")) $("#ntpServer2").value = cfg.time.server2;
    if ($("#ntpTz")) $("#ntpTz").value = cfg.time.tz;
    if ($("#ntpIntervalMin")) $("#ntpIntervalMin").value = String(cfg.time.syncIntervalMin);

    const bt = $("#browserTime");
    if (bt) bt.textContent = new Date().toLocaleString();
  };

  const collect = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return null;
    ensure(cfg);
    cfg.time.ntpEnabled = !!$("#ntpEnabled")?.checked;
    cfg.time.server1 = String($("#ntpServer1")?.value || "pool.ntp.org");
    cfg.time.server2 = String($("#ntpServer2")?.value || "time.google.com");
    cfg.time.tz = String($("#ntpTz")?.value || "Europe/Prague");
    cfg.time.syncIntervalMin = Number($("#ntpIntervalMin")?.value ?? 60);
    return cfg;
  };

  const tryReadTime = async () => {
    const info = $("#timeInfo");
    const dt = $("#deviceTime");
    try {
      const r = await apiGetJson("/api/time");
      const iso = r?.iso || r?.time || null;
      if (dt) dt.textContent = iso ? String(iso) : "—";
      if (info) info.textContent = iso ? "Načteno z /api/time" : "Neznámý formát /api/time";
    } catch (e) {
      if (dt) dt.textContent = "—";
      if (info) info.textContent = "Zařízení zatím neposkytuje /api/time (OK).";
    }
  };

  const bind = () => {
    if (!$("#cfg-time")) return;

    $("#btnSaveTime")?.addEventListener("click", async () => {
      const cfg = collect();
      if (!cfg) return;
      try {
        await apiPostJson("/api/config", cfg);
        toast("NTP konfigurace uložena");
      } catch (err) {
        toast("Chyba ukládání: " + (err?.message || err), "bad");
      }
    });

    $("#btnTryReadTime")?.addEventListener("click", tryReadTime);

    setInterval(() => {
      const bt = $("#browserTime");
      if (bt) bt.textContent = new Date().toLocaleString();
    }, 1000);
  };

  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    try { prev && prev(cfg); } catch(_) {}
    render();
  };

  window.addEventListener("DOMContentLoaded", () => {
    bind();
    if (App.getConfig?.()) render();
  });
})();