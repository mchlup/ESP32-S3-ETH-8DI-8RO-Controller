/* Smart cirkulace TUV – konfigurace */
(() => {
  const $ = (id) => document.getElementById(id);

  const el = {
    btnSave: $("btnSaveRecirc"),
    enabled: $("recircEnabled"),
    mode: $("recircMode"),
    demandInput: $("recircDemandInput"),
    pumpRelay: $("recircPumpRelay"),
    onDemandRunS: $("recircOnDemandRunS"),
    minOffS: $("recircMinOffS"),
    minOnS: $("recircMinOnS"),
    returnSource: $("recircReturnSource"),
    returnDallasRow: $("recircReturnDallasRow"),
    returnDallas: $("recircReturnDallas"),
    returnMqttRow: $("recircReturnMqttRow"),
    returnMqttPreset: $("recircReturnMqttPreset"),
    returnTopic: $("recircReturnTopic"),
    returnJsonKey: $("recircReturnJsonKey"),
    returnBleRow: $("recircReturnBleRow"),
    returnBle: $("recircReturnBle"),
    stopTempC: $("recircStopTempC"),
    windows: $("recircWindows"),
    addWindow: $("recircAddWindow"),
  };

  if (!window.App || !el.btnSave) return;

  const roleOnlyEls = [
    el.demandInput, el.pumpRelay, el.returnSource, el.returnDallas, el.returnMqttPreset,
    el.returnTopic, el.returnJsonKey, el.returnBle,
  ].filter(Boolean);

  const setRoleOnly = () => {
    roleOnlyEls.forEach((node) => {
      node.disabled = true;
      node.classList.add("readOnly");
    });
  };

  const applyTempRole = (role, target) => {
    target.source = String(role?.source || "none");
    target.gpio = Number(role?.gpio ?? 0);
    target.rom = String(role?.rom || "");
    target.topic = String(role?.topic || "");
    target.jsonKey = String(role?.jsonKey || "");
    target.mqttIdx = Number(role?.mqttIdx || role?.preset || 0);
    target.bleId = String(role?.bleId || role?.id || "");
  };

  const syncRolesToRecirc = (cfg) => {
    const map = App.getRoleMap?.();
    if (!map) return;
    cfg.dhwRecirc = cfg.dhwRecirc || {};
    if (map.inputs?.recirc_demand) cfg.dhwRecirc.demandInput = map.inputs.recirc_demand.index;
    else cfg.dhwRecirc.demandInput = 0;
    if (map.outputs?.dhw_recirc_pump) cfg.dhwRecirc.pumpRelay = map.outputs.dhw_recirc_pump.index;
    else cfg.dhwRecirc.pumpRelay = 0;
    cfg.dhwRecirc.tempReturnSource = cfg.dhwRecirc.tempReturnSource || {};
    const roles = cfg?.thermometers?.roles || {};
    applyTempRole(roles.return || {}, cfg.dhwRecirc.tempReturnSource);
  };

  const renderRoleList = () => {
    const list = document.getElementById("recircRoleList");
    const map = App.getRoleMap?.();
    if (!list || !map) return;
    const fmt = (role) => (role ? role.label : "Nepřiřazeno");
    const fmtTemp = (role) => {
      if (!role || role.source === "none") return "Nepřiřazeno";
      return `${role.label} (${role.detail})`;
    };
    list.innerHTML = `
      <li><span>Požadavek cirkulace</span><strong>${fmt(map.inputs?.recirc_demand)}</strong></li>
      <li><span>Čerpadlo TUV</span><strong>${fmt(map.outputs?.dhw_recirc_pump)}</strong></li>
      <li><span>Návratová teplota</span><strong>${fmtTemp(map.temps?.return)}</strong></li>
    `;
  };

  const INPUT_COUNT = 8;
  const RELAY_COUNT = 8;

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

  function buildInputOptions(cfg) {
    const opts = [{ value: "0", label: "—" }];
    const names = Array.isArray(cfg?.inputNames) ? cfg.inputNames : [];
    for (let i = 0; i < INPUT_COUNT; i++) {
      const name = String(names[i] || "").trim() || `Vstup ${i + 1}`;
      opts.push({ value: String(i + 1), label: `${i + 1} – ${name}` });
    }
    return opts;
  }

  function buildRelayOptions(cfg) {
    const opts = [{ value: "0", label: "—" }];
    const names = Array.isArray(cfg?.relayNames) ? cfg.relayNames : [];
    for (let i = 0; i < RELAY_COUNT; i++) {
      const name = String(names[i] || "").trim() || `Relé ${i + 1}`;
      opts.push({ value: String(i + 1), label: `${i + 1} – ${name}` });
    }
    return opts;
  }

  function parseDallasValue(v) {
    if (!v || typeof v !== "string") return null;
    const parts = v.split(":");
    if (parts.length < 2 || parts[0] !== "dallas") return null;
    const gpio = parseInt(parts[1], 10);
    const rom = (parts[2] || "").trim();
    if (!Number.isFinite(gpio)) return null;
    return { gpio, rom };
  }

  function makeDallasValue(gpio, rom) {
    const r = (rom || "").trim();
    return r ? `dallas:${gpio}:${r}` : `dallas:${gpio}`;
  }

  function buildDallasOptions(cfg, dash) {
    const opts = [];
    const names = Array.isArray(cfg?.dallasNames) ? cfg.dallasNames : [];
    const dallas = Array.isArray(dash?.dallas) ? dash.dallas : [];
    for (const g of dallas) {
      const gpio = Number(g.gpio ?? 0);
      const label = String(names[gpio] || "").trim() || `Dallas GPIO${gpio}`;
      opts.push({ value: makeDallasValue(gpio, ""), label: `${label} (auto)` });
      const devs = Array.isArray(g.devices) ? g.devices : [];
      for (const d of devs) {
        const rom = String(d.rom || "").toUpperCase();
        if (!rom) continue;
        opts.push({ value: makeDallasValue(gpio, rom), label: `${label} • ${rom}` });
      }
    }
    if (!opts.length) opts.push({ value: "dallas:0", label: "Dallas GPIO0" });
    return opts;
  }

  function buildBleOptions(dash) {
    const opts = [];
    const bleTemps = Array.isArray(dash?.bleTemps) ? dash.bleTemps : [];
    for (const b of bleTemps) {
      const id = String(b.id || "");
      if (!id) continue;
      const label = String(b.label || id);
      opts.push({ value: id, label: `${label} (${id})` });
    }
    if (!opts.length) opts.push({ value: "meteo.tempC", label: "BLE (meteo.tempC)" });
    return opts;
  }

  function buildMqttPresetOptions(cfg, curIdx, curTopic, curKey) {
    const opts = [{ value: "custom", label: "Vlastní" }];
    const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
    for (let i = 0; i < Math.min(2, list.length); i++) {
      const it = list[i] || {};
      const name = String(it.name || "").trim() || `MQTT ${i + 1}`;
      const t = String(it.topic || "").trim();
      const k = String(it.jsonKey || "tempC").trim();
      const label = t ? `${name} (${t}${k ? ` • ${k}` : ""})` : `${name} (nenastaveno)`;
      opts.push({ value: String(i + 1), label, disabled: !t });
    }
    let selected = "custom";
    if (Number.isFinite(curIdx) && curIdx >= 1 && curIdx <= 2) selected = String(curIdx);
    else if (curTopic || curKey) selected = "custom";
    return { options: opts, selectedValue: selected };
  }

  function applyMqttPresetToInputs(presetValue, cfg, topicInp, keyInp) {
    if (!topicInp || !keyInp) return;
    const idx = Number(presetValue);
    const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
    if (Number.isFinite(idx) && idx >= 1 && idx <= 2) {
      const it = list[idx - 1] || {};
      topicInp.value = String(it.topic || "");
      keyInp.value = String(it.jsonKey || "tempC");
    }
  }

  function rebuildSourceSelect(selectEl) {
    if (!selectEl) return;
    const cur = String(selectEl.value || "");
    selectEl.innerHTML = [
      { v: "dallas", t: "DS18B20 (Dallas)" },
      { v: "mqtt", t: "MQTT" },
      { v: "ble", t: "BLE teploměr" },
      { v: "none", t: "Nepoužít" },
    ].map((o) => `<option value="${o.v}">${o.t}</option>`).join("");
    selectEl.value = cur || "none";
  }

  function updateSourceRows() {
    const rs = el.returnSource.value || "none";
    el.returnDallasRow.style.display = rs === "dallas" ? "" : "none";
    el.returnMqttRow.style.display = rs === "mqtt" ? "" : "none";
    el.returnBleRow.style.display = rs === "ble" ? "" : "none";
  }

  function renderWindows(cfg) {
    if (!el.windows) return;
    const win = Array.isArray(cfg?.dhwRecirc?.windows) ? cfg.dhwRecirc.windows : [];
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
    syncRolesToRecirc(cfg);
    const r = cfg.dhwRecirc || {};
    el.enabled.checked = !!r.enabled;
    el.mode.value = r.mode || "on_demand";
    el.demandInput.value = String(r.demandInput || 0);
    el.pumpRelay.value = String(r.pumpRelay || 0);
    el.onDemandRunS.value = Math.round((r.onDemandRunMs ?? 120000) / 1000);
    el.minOffS.value = Math.round((r.minOffMs ?? 300000) / 1000);
    el.minOnS.value = Math.round((r.minOnMs ?? 30000) / 1000);
    el.stopTempC.value = Number.isFinite(r.stopTempC) ? r.stopTempC : 42;

    const src = r.tempReturnSource || {};
    el.returnSource.value = src.source || "none";
    if (src.source === "dallas") {
      const v = makeDallasValue(Number(src.gpio || 0), String(src.rom || src.addr || "").toUpperCase());
      el.returnDallas.value = v;
    }
    el.returnTopic.value = src.topic || "";
    el.returnJsonKey.value = src.jsonKey || src.key || src.field || "";
    el.returnBle.value = src.bleId || src.id || "meteo.tempC";
    updateSourceRows();
    renderWindows(cfg);
    renderRoleList();
    setRoleOnly();
  }

  function saveToConfig(cfg) {
    App.ensureConfigShape(cfg);
    cfg.dhwRecirc = cfg.dhwRecirc || {};
    cfg.dhwRecirc.enabled = !!el.enabled.checked;
    cfg.dhwRecirc.mode = String(el.mode.value || "on_demand");
    cfg.dhwRecirc.demandInput = parseInt(el.demandInput.value || "0", 10) || 0;
    cfg.dhwRecirc.pumpRelay = parseInt(el.pumpRelay.value || "0", 10) || 0;
    cfg.dhwRecirc.onDemandRunMs = Math.max(0, (parseInt(el.onDemandRunS.value || "0", 10) || 0) * 1000);
    cfg.dhwRecirc.minOffMs = Math.max(0, (parseInt(el.minOffS.value || "0", 10) || 0) * 1000);
    cfg.dhwRecirc.minOnMs = Math.max(0, (parseInt(el.minOnS.value || "0", 10) || 0) * 1000);
    cfg.dhwRecirc.stopTempC = parseFloat(el.stopTempC.value || "0") || 0;

    cfg.dhwRecirc.tempReturnSource = cfg.dhwRecirc.tempReturnSource || {};
    const ts = cfg.dhwRecirc.tempReturnSource;
    ts.source = String(el.returnSource.value || "none");
    ts.gpio = 0;
    ts.rom = "";
    ts.topic = "";
    ts.jsonKey = "";
    ts.bleId = "";
    ts.mqttIdx = 0;

    if (ts.source === "dallas") {
      const sel = parseDallasValue(el.returnDallas.value) || { gpio: 0, rom: "" };
      ts.gpio = sel.gpio;
      ts.rom = sel.rom || "";
    } else if (ts.source === "mqtt") {
      const preset = el.returnMqttPreset ? String(el.returnMqttPreset.value || "custom") : "custom";
      const idx = Number(preset);
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2) ts.mqttIdx = idx;
      ts.topic = (el.returnTopic.value || "").trim();
      ts.jsonKey = (el.returnJsonKey.value || "").trim();
    } else if (ts.source === "ble") {
      ts.bleId = String(el.returnBle.value || "meteo.tempC");
    }

    const win = [];
    const rows = el.windows ? Array.from(el.windows.children) : [];
    rows.forEach((row) => {
      const start = row.querySelector('[data-k="start"]')?.value || "06:00";
      const end = row.querySelector('[data-k="end"]')?.value || "07:00";
      const dayEls = row.querySelectorAll('input[type="checkbox"][data-day]');
      const days = [];
      dayEls.forEach((d) => { if (d.checked) days.push(parseInt(d.dataset.day || "0", 10)); });
      win.push({ start, end, days });
    });
    cfg.dhwRecirc.windows = win;
    syncRolesToRecirc(cfg);
  }

  async function refreshDash(cfg, keepSelections) {
    try {
      const dash = await App.apiGetJson("/api/dash");
      const dallasOpts = buildDallasOptions(cfg, dash);
      setSelectOptions(el.returnDallas, dallasOpts, keepSelections);
      const bleOpts = buildBleOptions(dash);
      setSelectOptions(el.returnBle, bleOpts, keepSelections);

      const src = cfg?.dhwRecirc?.tempReturnSource || {};
      const curTopic = (src.source === "mqtt") ? String(src.topic || "") : "";
      const curKey = (src.source === "mqtt") ? String(src.jsonKey || "") : "";
      const curIdx = (src.source === "mqtt") ? Number(src.mqttIdx || src.preset || 0) : 0;
      if (el.returnMqttPreset) {
        const res = buildMqttPresetOptions(cfg, curIdx, curTopic, curKey);
        setSelectOptions(el.returnMqttPreset, res.options, false);
        el.returnMqttPreset.value = res.selectedValue;
        applyMqttPresetToInputs(res.selectedValue, cfg, el.returnTopic, el.returnJsonKey);
      }
    } catch (_) {
      // ignore
    }
  }

  function bindEvents() {
    el.returnSource.addEventListener("change", updateSourceRows);
    el.returnMqttPreset?.addEventListener("change", () => {
      const cfg = App.getConfig();
      applyMqttPresetToInputs(el.returnMqttPreset.value, cfg, el.returnTopic, el.returnJsonKey);
    });

    el.addWindow?.addEventListener("click", () => {
      const cfg = App.getConfig();
      cfg.dhwRecirc = cfg.dhwRecirc || {};
      cfg.dhwRecirc.windows = Array.isArray(cfg.dhwRecirc.windows) ? cfg.dhwRecirc.windows : [];
      cfg.dhwRecirc.windows.push({ start: "06:00", end: "07:00", days: [1,2,3,4,5] });
      renderWindows(cfg);
    });

    el.btnSave.addEventListener("click", async () => {
      const cfg = App.getConfig();
      saveToConfig(cfg);
      await App.saveConfig(cfg);
      refreshDash(cfg, true);
    });
  }

  const prevOnConfigLoaded = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    if (typeof prevOnConfigLoaded === "function") prevOnConfigLoaded(cfg);
    rebuildSourceSelect(el.returnSource);
    setSelectOptions(el.demandInput, buildInputOptions(cfg), false);
    setSelectOptions(el.pumpRelay, buildRelayOptions(cfg), false);
    loadFromConfig(cfg);
    refreshDash(cfg, true);
  };

  window.addEventListener("DOMContentLoaded", () => {
    bindEvents();
    const cfg = App.getConfig();
    if (cfg) {
      rebuildSourceSelect(el.returnSource);
      setSelectOptions(el.demandInput, buildInputOptions(cfg), false);
      setSelectOptions(el.pumpRelay, buildRelayOptions(cfg), false);
      loadFromConfig(cfg);
      refreshDash(cfg, true);
    }
  });
})();
