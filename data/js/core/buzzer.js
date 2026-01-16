(() => {
  "use strict";
  let initialized = false;

  // Musí odpovídat patternům ve firmware (BuzzerController.cpp)
  const patterns = [
    "off",
    "short",
    "chirp",
    "chirpDown",
    "notify",
    "long",
    "double",
    "triple",
    "error",
  ];

  const apiGet = () => window.App.apiGetJson("/api/buzzer");
  const apiSet = (cfg) => window.App.apiPostJson("/api/buzzer", { action: "set_config", config: cfg });
  const apiTest = (pattern) => window.App.apiPostJson("/api/buzzer", { action: "test", pattern });
  const apiStop = () => window.App.apiPostJson("/api/buzzer", { action: "stop" });

  // Předdefinované funkce (můžeš volat i odjinud)
  window.Buzzer = {
    beepShort: () => apiTest("short"),
    beepLong: () => apiTest("long"),
    beepDouble: () => apiTest("double"),
    beepTriple: () => apiTest("triple"),
    beepChirp: () => apiTest("chirp"),
    beepChirpDown: () => apiTest("chirpDown"),
    beepNotify: () => apiTest("notify"),
    beepError: () => apiTest("error"),
    stop: () => apiStop(),
  };

  const el = (id) => document.getElementById(id);

  const clampInt = (v, min, max, fallback) => {
    const n = parseInt(v, 10);
    if (Number.isNaN(n)) return fallback;
    return Math.max(min, Math.min(max, n));
  };

  const renderEventsTable = (container, events) => {
    // události odpovídají back-endu (viz BuzzerController)
    const rows = [
      ["control_auto", "Přepnutí do AUTO"],
      ["control_manual", "Přepnutí do MANUAL"],
      ["manual_mode", "Změna režimu (MODE1..MODE5)"],
      ["relay_on", "Relé sepne"],
      ["relay_off", "Relé rozepne"],
      ["error", "Chyba / varování"],
    ];

    container.innerHTML = "";
    for (const [k, label] of rows) {
      const row = document.createElement("div");
      row.className = "trow";

      const c1 = document.createElement("div");
      c1.className = "tcell";
      c1.textContent = label;

      const c2 = document.createElement("div");
      c2.className = "tcell";
      const sel = document.createElement("select");
      sel.dataset.eventKey = k;
      for (const p of patterns) {
        const opt = document.createElement("option");
        opt.value = p;
        opt.textContent = p;
        sel.appendChild(opt);
      }
      sel.value = (events && events[k]) ? events[k] : "off";
      c2.appendChild(sel);

      row.appendChild(c1);
      row.appendChild(c2);
      container.appendChild(row);
    }
  };

  const collectEvents = (container) => {
    const out = {};
    const sels = container.querySelectorAll("select[data-event-key]");
    sels.forEach(s => out[s.dataset.eventKey] = s.value);
    return out;
  };

  const applyPwmVisibility = () => {
    const box = el("bzPwmBox");
    const use = !!el("bzUsePwm")?.checked;
    if (!box) return;
    box.style.display = use ? "" : "none";
  };

  const loadUi = async () => {
    const st = await apiGet();

    el("bzEnabled").checked = !!st.enabled;
    el("bzActive").value = st.activeHigh ? "high" : "low";

    // PWM
    el("bzUsePwm").checked = (st.usePwm !== undefined) ? !!st.usePwm : true;
    el("bzPwmFreqHz").value = (st.pwmFreqHz !== undefined) ? st.pwmFreqHz : 3000;
    el("bzPwmDutyPct").value = (st.pwmDutyPct !== undefined) ? st.pwmDutyPct : 50;
    applyPwmVisibility();

    renderEventsTable(el("bzEvents"), st.events || {});
  };

  const saveUi = async () => {
    const usePwm = !!el("bzUsePwm").checked;

    const cfg = {
      enabled: el("bzEnabled").checked,
      activeHigh: el("bzActive").value === "high",
      usePwm,
      pwmFreqHz: clampInt(el("bzPwmFreqHz").value, 100, 20000, 3000),
      pwmDutyPct: clampInt(el("bzPwmDutyPct").value, 0, 100, 50),
      events: collectEvents(el("bzEvents")),
    };
    await apiSet(cfg);
    window.App.toast("BUZZER uložen", "ok");
  };

  const bind = () => {
    const btnSave = el("btnBuzzerSave");
    if (btnSave) btnSave.addEventListener("click", () => saveUi().catch(e => window.App.toast(e.message, "bad")));

    el("bzUsePwm")?.addEventListener("change", applyPwmVisibility);

    el("btnBeepShort")?.addEventListener("click", () => window.Buzzer.beepShort().catch(() => {}));
    el("btnBeepChirp")?.addEventListener("click", () => window.Buzzer.beepChirp().catch(() => {}));
    el("btnBeepChirpDown")?.addEventListener("click", () => window.Buzzer.beepChirpDown().catch(() => {}));
    el("btnBeepNotify")?.addEventListener("click", () => window.Buzzer.beepNotify().catch(() => {}));
    el("btnBeepLong")?.addEventListener("click", () => window.Buzzer.beepLong().catch(() => {}));
    el("btnBeepDouble")?.addEventListener("click", () => window.Buzzer.beepDouble().catch(() => {}));
    el("btnBeepTriple")?.addEventListener("click", () => window.Buzzer.beepTriple().catch(() => {}));
    el("btnBeepError")?.addEventListener("click", () => window.Buzzer.beepError().catch(() => {}));
    el("btnBeepStop")?.addEventListener("click", () => window.Buzzer.stop().catch(() => {}));
  };

  const init = () => {
    if (initialized) return;
    initialized = true;
    // app.js musí být načtené jako první (window.App)
    if (!window.App || !window.App.apiGetJson) return;

    bind();

    // Načti hned při startu (pokud je tab viditelný), jinak až při aktivaci záložky
    const buzTabBtn = document.querySelector('.tab[data-ctab="buzzer"]');
    const buzPage = document.getElementById("cfg-buzzer");

    const refreshIfVisible = () => {
      if (!buzPage) return;
      if (buzPage.classList.contains("active")) loadUi().catch(() => {});
    };

    // start
    refreshIfVisible();

    // při kliknutí na tab vždy refresh (ať je to "živé")
    if (buzTabBtn) buzTabBtn.addEventListener("click", () => loadUi().catch(() => {}));

    // fallback: pokud se active třída přepíná jinak, hlídej změny
    const mo = new MutationObserver(() => refreshIfVisible());
    if (buzPage) mo.observe(buzPage, { attributes: true, attributeFilter: ["class"] });
  };

  window.addEventListener("app:pageMounted", (ev) => {
    if (ev?.detail?.id === "system") init();
  });
})();
