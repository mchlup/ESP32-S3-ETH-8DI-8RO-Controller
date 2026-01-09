/* AKU heater – konfigurace */
(() => {
  const $ = (id) => document.getElementById(id);

  const el = {
    btnSave: $("btnSaveAkuHeater"),
    enabled: $("akuHeaterEnabled"),
    relay: $("akuHeaterRelay"),
    mode: $("akuHeaterMode"),
    manualOn: $("akuHeaterManualOn"),
    targetTopC: $("akuHeaterTargetTopC"),
    hysteresisC: $("akuHeaterHysteresisC"),
    maxOnMin: $("akuHeaterMaxOnMin"),
    minOffMin: $("akuHeaterMinOffMin"),
    windows: $("akuHeaterWindows"),
    addWindow: $("akuHeaterAddWindow"),
  };

  if (!window.App || !el.btnSave) return;

  const RELAY_COUNT = 8;
  const roleOnlyEls = [el.relay].filter(Boolean);
  const setRoleOnly = () => {
    roleOnlyEls.forEach((node) => {
      node.disabled = true;
      node.classList.add("readOnly");
    });
  };

  const syncRolesToAkuHeater = (cfg) => {
    const map = App.getRoleMap?.();
    if (!map) return;
    cfg.akuHeater = cfg.akuHeater || {};
    if (map.outputs?.heater_aku) cfg.akuHeater.relay = map.outputs.heater_aku.index;
    else cfg.akuHeater.relay = 0;
  };

  const renderRoleList = () => {
    const list = document.getElementById("akuHeaterRoleList");
    const map = App.getRoleMap?.();
    if (!list || !map) return;
    const fmt = (role) => (role ? role.label : "Nepřiřazeno");
    const fmtTemp = (role) => {
      if (!role || role.source === "none") return "Nepřiřazeno";
      return `${role.label} (${role.detail})`;
    };
    list.innerHTML = `
      <li><span>Relé dohřevu</span><strong>${fmt(map.outputs?.heater_aku)}</strong></li>
      <li><span>AKU top</span><strong>${fmtTemp(map.temps?.tankTop)}</strong></li>
      <li><span>AKU mid</span><strong>${fmtTemp(map.temps?.tankMid)}</strong></li>
      <li><span>AKU bottom</span><strong>${fmtTemp(map.temps?.tankBottom)}</strong></li>
    `;
  };

  function setSelectOptions(selectEl, options, keepValue) {
    if (!selectEl) return;
    const prev = keepValue ? selectEl.value : null;
    selectEl.innerHTML = "";
    for (const o of options) {
      const opt = document.createElement("option");
      opt.value = o.value;
      opt.textContent = o.label;
      if (o.disabled) opt.disabled = true;
      selectEl.appendChild(opt);
    }
    if (keepValue && prev) {
      const exists = Array.from(selectEl.options).some((o) => o.value === prev);
      if (exists) selectEl.value = prev;
    }
  }

  function buildRelayOptions(cfg) {
    const opts = [{ value: "0", label: "—" }];
    const names = Array.isArray(cfg?.relayNames) ? cfg.relayNames : [];
    const outputs = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
    for (let i = 0; i < RELAY_COUNT; i++) {
      const name = String(names[i] || "").trim() || `Relé ${i + 1}`;
      const role = String(outputs[i]?.role || "none");
      const suffix = (role === "heater_aku") ? "" : " (nastaví roli heater_aku)";
      opts.push({ value: String(i + 1), label: `${i + 1} – ${name}${suffix}` });
    }
    return opts;
  }

  function renderWindows(cfg) {
    if (!el.windows) return;
    const win = Array.isArray(cfg?.akuHeater?.windows) ? cfg.akuHeater.windows : [];
    el.windows.innerHTML = "";

    win.forEach((w, idx) => {
      const row = document.createElement("div");
      row.className = "row";
      row.style.flexWrap = "wrap";
      row.style.gap = "8px";
      const days = Array.isArray(w.days) ? w.days : [];
      const dayChecks = [1,2,3,4,5,6,7].map((d) => {
        const checked = days.includes(d) ? "checked" : "";
        return `<label class="mini"><input type="checkbox" data-day="${d}" ${checked}>${d}</label>`;
      }).join(" ");

      row.innerHTML = `
        <input type="time" data-k="start" value="${w.start || "06:00"}">
        <input type="time" data-k="end" value="${w.end || "07:00"}">
        <div class="mini" data-k="days">${dayChecks}</div>
        <button class="btn ghost mini" data-k="remove" type="button">×</button>
      `;

      row.querySelector('[data-k="remove"]').addEventListener("click", () => {
        win.splice(idx, 1);
        renderWindows(cfg);
      });

      el.windows.appendChild(row);
    });
  }

  function loadFromConfig(cfg) {
    App.ensureConfigShape(cfg);
    syncRolesToAkuHeater(cfg);
    const h = cfg.akuHeater || {};
    el.enabled.checked = !!h.enabled;
    el.relay.value = String(h.relay || 0);
    el.mode.value = h.mode || "manual";
    el.manualOn.checked = !!h.manualOn;
    el.targetTopC.value = Number.isFinite(h.targetTopC) ? h.targetTopC : 50;
    el.hysteresisC.value = Number.isFinite(h.hysteresisC) ? h.hysteresisC : 2;
    el.maxOnMin.value = Math.round((h.maxOnMs ?? 2 * 60 * 60 * 1000) / 60000);
    el.minOffMin.value = Math.round((h.minOffMs ?? 10 * 60 * 1000) / 60000);
    renderWindows(cfg);
    renderRoleList();
    setRoleOnly();
  }

  function saveToConfig(cfg) {
    App.ensureConfigShape(cfg);
    const h = cfg.akuHeater || {};
    h.enabled = !!el.enabled.checked;
    h.relay = Number(el.relay.value || 0);
    h.mode = String(el.mode.value || "manual");
    h.manualOn = !!el.manualOn.checked;
    h.targetTopC = Number(el.targetTopC.value || 50);
    h.hysteresisC = Number(el.hysteresisC.value || 2);
    h.maxOnMs = Math.max(60000, Number(el.maxOnMin.value || 120) * 60000);
    h.minOffMs = Math.max(60000, Number(el.minOffMin.value || 10) * 60000);

    const win = [];
    const rows = el.windows ? Array.from(el.windows.querySelectorAll(".row")) : [];
    rows.forEach((row) => {
      const start = row.querySelector('[data-k="start"]')?.value || "06:00";
      const end = row.querySelector('[data-k="end"]')?.value || "07:00";
      const days = Array.from(row.querySelectorAll('[data-k="days"] input[type="checkbox"]'))
        .filter((c) => c.checked)
        .map((c) => Number(c.dataset.day))
        .filter((d) => d >= 1 && d <= 7);
      win.push({ start, end, days: days.length ? days : [1,2,3,4,5,6,7] });
    });
    h.windows = win;
    cfg.akuHeater = h;
    syncRolesToAkuHeater(cfg);
  }

  function bindEvents() {
    if (el.relay) {
      el.relay.addEventListener("change", () => {
        const cfg = App.getConfig?.();
        if (!cfg) return;
        const idx = Number(el.relay.value || 0);
        if (!Number.isFinite(idx) || idx < 1 || idx > RELAY_COUNT) return;
        App.ensureConfigShape(cfg);
        cfg.iofunc = cfg.iofunc || {};
        cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : [];
        while (cfg.iofunc.outputs.length < RELAY_COUNT) cfg.iofunc.outputs.push({ role: "none", params: {} });
        cfg.iofunc.outputs[idx - 1].role = "heater_aku";
      });
    }

    el.addWindow?.addEventListener("click", () => {
      const cfg = App.getConfig();
      App.ensureConfigShape(cfg);
      cfg.akuHeater = cfg.akuHeater || {};
      cfg.akuHeater.windows = Array.isArray(cfg.akuHeater.windows) ? cfg.akuHeater.windows : [];
      cfg.akuHeater.windows.push({ start: "06:00", end: "07:00", days: [1,2,3,4,5,6,7] });
      renderWindows(cfg);
    });

    el.btnSave.addEventListener("click", async () => {
      const cfg = App.getConfig();
      saveToConfig(cfg);
      await App.saveConfig(cfg);
    });
  }

  const prevOnConfigLoaded = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    if (typeof prevOnConfigLoaded === "function") prevOnConfigLoaded(cfg);
    const relayOpts = buildRelayOptions(cfg);
    setSelectOptions(el.relay, relayOpts, false);
    loadFromConfig(cfg);
  };

  window.addEventListener("DOMContentLoaded", () => {
    bindEvents();
    const cfg = App.getConfig();
    if (cfg) {
      const relayOpts = buildRelayOptions(cfg);
      setSelectOptions(el.relay, relayOpts, false);
      loadFromConfig(cfg);
    }
  });
})();
