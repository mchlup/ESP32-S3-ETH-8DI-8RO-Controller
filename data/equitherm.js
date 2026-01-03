/* Equitherm tab (Dallas/MQTT + 3-way valve control)
 * - Dynamicky načte aktivní DS18B20 (rom + teplota) z /api/dash
 * - Uloží konfiguraci do cfg.equitherm (kompatibilní s firmware)
 * - Zobrazuje runtime stav z /api/status (equitherm)
 */
(() => {
  const $ = (id) => document.getElementById(id);

  const el = {
    btnSave: $("btnSaveEquitherm"),
    enabled: $("eqEnabled"),

    outdoorSource: $("eqOutdoorSource"),
    outdoorDallasRow: $("eqOutdoorDallasRow"),
    outdoorDallas: $("eqOutdoorDallas"),
    outdoorMqttRow: $("eqOutdoorMqttRow"),
    outdoorMqttPreset: $("eqOutdoorMqttPreset"),
    outdoorTopic: $("eqOutdoorTopic"),
    outdoorJsonKey: $("eqOutdoorJsonKey"),
    outdoorBleRow: $("eqOutdoorBleRow"),
    outdoorBle: $("eqOutdoorBle"),

    flowSource: $("eqFlowSource"),
    flowDallasRow: $("eqFlowDallasRow"),
    flowDallas: $("eqFlowDallas"),
    flowMqttRow: $("eqFlowMqttRow"),
    flowMqttPreset: $("eqFlowMqttPreset"),
    flowTopic: $("eqFlowTopic"),
    flowJsonKey: $("eqFlowJsonKey"),
    flowBleRow: $("eqFlowBleRow"),
    flowBle: $("eqFlowBle"),

    valveMaster: $("eqValveMaster"),

    minFlow: $("eqMinFlow"),
    maxFlow: $("eqMaxFlow"),
    slopeDay: $("eqSlopeDay"),
    shiftDay: $("eqShiftDay"),
    slopeNight: $("eqSlopeNight"),
    shiftNight: $("eqShiftNight"),

    deadbandC: $("eqDeadbandC"),
    stepPct: $("eqStepPct"),
    periodS: $("eqPeriodS"),
    minPct: $("eqMinPct"),
    maxPct: $("eqMaxPct"),

    heatTout1: $("eqHeatTout1"),
    heatTflow1: $("eqHeatTflow1"),
    heatTout2: $("eqHeatTout2"),
    heatTflow2: $("eqHeatTflow2"),
    nightTout1: $("eqNightTout1"),
    nightTflow1: $("eqNightTflow1"),
    nightTout2: $("eqNightTout2"),
    nightTflow2: $("eqNightTflow2"),

    statusBox: $("eqStatusBox"),

    curveCanvas: $("eqCurveCanvas"),
  };

  if (!window.App || !el.btnSave) return;

  let lastDash = null;
  let lastStatus = null;

  function fmtTemp(v) {
    if (typeof v !== "number" || !isFinite(v)) return "--";
    return `${v.toFixed(1)}°C`;
  }
  function clamp(v, a, b) {
    if (!isFinite(v)) return a;
    if (v < a) return a;
    if (v > b) return b;
    return v;
  }

  function computeTargetFlow(tout, slope, shift, minFlow, maxFlow) {
    // stejný vzorec jako firmware/UI: Tflow = (20 - Tout) * slope + 20 + shift
    const t = (20 - tout) * slope + 20 + shift;
    return clamp(t, minFlow, maxFlow);
  }

  function drawCurve(status) {
    const c = el.curveCanvas;
    if (!c || !c.getContext) return;
    const ctx = c.getContext("2d");

    // čti hodnoty z UI (i když nejsou uložené)
    const slopeDay = parseFloat(el.slopeDay.value || "1");
    const shiftDay = parseFloat(el.shiftDay.value || "0");
    const slopeNight = parseFloat(el.slopeNight.value || "1");
    const shiftNight = parseFloat(el.shiftNight.value || "0");
    const minFlow = parseFloat(el.minFlow.value || "0");
    const maxFlow = parseFloat(el.maxFlow.value || "100");

    const W = c.width, H = c.height;
    const fg = (window.getComputedStyle ? getComputedStyle(c).color : "#000") || "#000";
    ctx.clearRect(0, 0, W, H);

    // rozsah os
    const xMin = -20, xMax = 20;              // venkovní teplota
    let yMin = clamp(minFlow, -20, 120);
    let yMax = clamp(maxFlow, -20, 120);
    if (!isFinite(yMin)) yMin = 0;
    if (!isFinite(yMax)) yMax = 100;
    if (Math.abs(yMax - yMin) < 0.1) yMax = yMin + 1;
    const padL = 44, padR = 14, padT = 14, padB = 28;

    const x2px = (x) => padL + ((x - xMin) / (xMax - xMin)) * (W - padL - padR);
    const y2px = (y) => (H - padB) - ((y - yMin) / (yMax - yMin)) * (H - padT - padB);

    // mřížka
    ctx.globalAlpha = 0.35;
    ctx.lineWidth = 1;
    ctx.strokeStyle = fg;
    ctx.beginPath();
    for (let x = -20; x <= 20; x += 10) {
      const px = x2px(x);
      ctx.moveTo(px, padT);
      ctx.lineTo(px, H - padB);
    }
    for (let y = Math.ceil(yMin/10)*10; y <= yMax; y += 10) {
      const py = y2px(y);
      ctx.moveTo(padL, py);
      ctx.lineTo(W - padR, py);
    }
    ctx.stroke();
    ctx.globalAlpha = 1;

    // osy
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(padL, padT);
    ctx.lineTo(padL, H - padB);
    ctx.lineTo(W - padR, H - padB);
    ctx.stroke();

    // popisky os
    ctx.font = "12px system-ui, -apple-system, Segoe UI, Roboto, Arial";
    ctx.fillStyle = fg;
    ctx.fillText("Tout (°C)", W - padR - 70, H - 8);
    ctx.save();
    ctx.translate(12, padT + 90);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText("Target Tflow (°C)", 0, 0);
    ctx.restore();

    function plotLine(slope, shift, dashed) {
      ctx.save();
      if (dashed) ctx.setLineDash([6, 5]); else ctx.setLineDash([]);
      ctx.lineWidth = 2;
      ctx.beginPath();
      let first = true;
      for (let x = xMin; x <= xMax; x += 1) {
        const y = computeTargetFlow(x, slope, shift, minFlow, maxFlow);
        const px = x2px(x);
        const py = y2px(y);
        if (first) { ctx.moveTo(px, py); first = false; }
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
      ctx.restore();
    }

    // den (plná), noc (čárkovaná)
    plotLine(slopeDay, shiftDay, false);
    plotLine(slopeNight, shiftNight, true);

    // aktuální bod z /api/status
    const st = status && status.equitherm ? status.equitherm : null;
    if (st && typeof st.outdoorC === "number" && isFinite(st.outdoorC)) {
      const x = clamp(st.outdoorC, xMin, xMax);
      const isNight = !!st.night;
      const slope = isNight ? slopeNight : slopeDay;
      const shift = isNight ? shiftNight : shiftDay;
      const yCalc = computeTargetFlow(x, slope, shift, minFlow, maxFlow);
      const y = clamp(yCalc, yMin, yMax);
      const px = x2px(x), py = y2px(y);

      ctx.beginPath();
      ctx.arc(px, py, 5, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillText(`${x.toFixed(1)} → ${y.toFixed(1)}${isNight ? " (noc)" : ""}`, px + 8, py - 8);
    }
  }


  function parseDallasValue(v) {
    // value = "dallas:<gpio>:<romHex>"
    if (!v || typeof v !== "string") return null;
    const parts = v.split(":");
    if (parts.length < 2 || parts[0] !== "dallas") return null;
    const gpio = parseInt(parts[1], 10);
    const rom = (parts[2] || "").trim();
    if (!Number.isFinite(gpio)) return null;
    return { gpio, rom };
  }

  function makeDallasValue(gpio, rom) {
    return `dallas:${gpio}:${rom || ""}`;
  }

  function getDallasName(cfg, gpio) {
    const names = (cfg && Array.isArray(cfg.dallasNames)) ? cfg.dallasNames : [];
    const n = names[gpio];
    return (n && String(n).trim().length) ? String(n).trim() : `GPIO${gpio}`;
  }

  function buildDallasOptions(cfg, dash) {
    const opts = [];
    const dallas = (dash && Array.isArray(dash.dallas)) ? dash.dallas : [];

    for (let gpio = 0; gpio < 4; gpio++) {
      const name = getDallasName(cfg, gpio);
      const st = dallas.find((x) => x && Number(x.gpio) === gpio) || null;
      const devices = (st && Array.isArray(st.devices)) ? st.devices : [];

      // Auto option (no ROM)
      const autoLabel = devices.length
        ? `${name} (GPIO${gpio}, auto)`
        : `${name} (GPIO${gpio}, auto) — nenalezeno`;
      opts.push({ value: makeDallasValue(gpio, ""), label: autoLabel });

      for (const dv of devices) {
        if (!dv) continue;
        const rom = String(dv.rom || "").toUpperCase();
        const tC = (typeof dv.tempC === "number") ? dv.tempC : NaN;
        const ok = !!dv.valid;
        const lbl = `${name} (GPIO${gpio}) • ${rom}${ok ? ` • ${fmtTemp(tC)}` : " • invalid"}`;
        opts.push({ value: makeDallasValue(gpio, rom), label: lbl });
      }
    }
    return opts;
  }

  function buildBleOptions(dash) {
    const opts = [];
    const arr = (dash && Array.isArray(dash.bleTemps)) ? dash.bleTemps : [];
    if (!arr.length) {
      opts.push({ value: "meteo.tempC", label: "BLE — žádná data" });
      return opts;
    }
    for (const it of arr) {
      if (!it) continue;
      const id = String(it.id || "");
      if (!id) continue;
      const lbl = String(it.label || id);
      const tC = (typeof it.tempC === "number") ? it.tempC : NaN;
      opts.push({ value: id, label: `${lbl} • ${fmtTemp(tC)}` });
    }
    return opts.length ? opts : [{ value: "meteo.tempC", label: "BLE — žádná data" }];
  }

  function buildMqttPresetOptions(cfg, currentTopic, currentJsonKey) {
    const list = (cfg?.thermometers && Array.isArray(cfg.thermometers.mqtt)) ? cfg.thermometers.mqtt : [];
    const opts = [];

    opts.push({ value: "custom", label: "Vlastní (ruční)" });

    for (let i = 0; i < 2; i++) {
      const it = list[i] || {};
      const topic = String(it.topic || "").trim();
      const name = String(it.name || "").trim() || `MQTT teploměr ${i + 1}`;
      const key = String(it.jsonKey || "tempC").trim();
      if (!topic) continue;
      // Požadavek: pokud je MQTT teploměr konfigurován v "Teploměry",
      // tak ho v ekvitermu nelze přímo zvolit (musí být "Vlastní").
      // Necháme ho pouze informativně jako disabled položku.
      const label = `${name} • ${topic}${key ? ` • ${key}` : ""} (spravováno v Teploměry)`;
      opts.push({ value: String(i + 1), label, disabled: true });
    }

    // pokud je aktuální konfigurace MQTT a neodpovídá žádnému přednastavení, přidáme ji jako readonly položku
    const curT = String(currentTopic || "").trim();
    const curK = String(currentJsonKey || "").trim();
    if (curT) {
      const match = opts.some(o => {
        if (!o || !o.value || o.value === "custom") return false;
        const idx = Number(o.value) - 1;
        const it = list[idx] || {};
        return String(it.topic || "").trim() === curT && String(it.jsonKey || "").trim() === curK;
      });
      if (!match) {
        opts.push({ value: "custom-cur", label: `Vlastní: ${curT}${curK ? ` • ${curK}` : ""}` });
      }
    }

    return opts;
  }

  function applyMqttPresetToInputs(presetValue, cfg, topicInput, jsonKeyInput) {
    if (!topicInput || !jsonKeyInput) return;
    const list = (cfg?.thermometers && Array.isArray(cfg.thermometers.mqtt)) ? cfg.thermometers.mqtt : [];
    const v = String(presetValue || "");
    const idx = Number(v);
    if (!Number.isFinite(idx) || idx < 1 || idx > 2) return;

    // Viz buildMqttPresetOptions(): položky 1..2 jsou disabled (informativní),
    // takže se sem běžně nedostaneme. Necháváme jako kompatibilitu.

    const it = list[idx - 1] || {};
    const topic = String(it.topic || "").trim();
    const key = String(it.jsonKey || "tempC").trim();
    if (!topic) return;

    topicInput.value = topic;
    jsonKeyInput.value = key;
  }

  function setSelectOptions(selectEl, options, keepValue) {
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

  function hasAnyConfiguredMqttThermo(cfg) {
    const list = (cfg?.thermometers && Array.isArray(cfg.thermometers.mqtt)) ? cfg.thermometers.mqtt : [];
    return list.some((it) => String(it?.topic || "").trim().length > 0);
  }

  function rebuildSourceSelect(selectEl, allowLegacyTemps) {
    if (!selectEl) return;

    const cur = String(selectEl.value || "");

    const base = [
      { v: "dallas", t: "DS18B20 (Dallas)" },
    ];
    if (allowLegacyTemps) {
      for (let i = 1; i <= 8; i++) base.push({ v: `temp${i}`, t: `Vstup TEMP${i} (legacy)` });
    }
    base.push(
      { v: "mqtt", t: "MQTT" },
      { v: "ble", t: "BLE teploměr" },
      { v: "none", t: "Nepoužít" },
    );

    selectEl.innerHTML = base
      .map((o) => `<option value="${o.v}">${o.t}</option>`)
      .join("");

    const exists = base.some((o) => o.v === cur);
    selectEl.value = exists ? cur : "dallas";
  }

  function updateSourceRows() {
    const os = el.outdoorSource.value;
    el.outdoorDallasRow.style.display = (os === "dallas") ? "" : "none";
    el.outdoorMqttRow.style.display   = (os === "mqtt") ? "" : "none";
    el.outdoorBleRow.style.display    = (os === "ble") ? "" : "none";

    const fs = el.flowSource.value;
    el.flowDallasRow.style.display = (fs === "dallas") ? "" : "none";
    el.flowMqttRow.style.display   = (fs === "mqtt") ? "" : "none";
    el.flowBleRow.style.display    = (fs === "ble") ? "" : "none";
  }

  function loadFromConfig(cfg) {
    App.ensureConfigShape(cfg);
    const e = cfg.equitherm || {};
    el.enabled.checked = !!e.enabled;

    // outdoor
    const o = e.outdoor || {};
    el.outdoorSource.value = o.source || "dallas";
    if ((o.source || "dallas") === "dallas") {
      const gpio = (typeof o.gpio === "number") ? o.gpio : 0;
      const rom = o.rom || o.addr || "";
      el.outdoorDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.outdoorTopic.value = o.topic || "";
    el.outdoorJsonKey.value = o.jsonKey || o.key || o.field || "";
    el.outdoorBle.value = o.bleId || o.id || "meteo.tempC";

    // flow
    const f = e.flow || {};
    el.flowSource.value = f.source || "dallas";
    if ((f.source || "dallas") === "dallas") {
      const gpio = (typeof f.gpio === "number") ? f.gpio : 0;
      const rom = f.rom || f.addr || "";
      el.flowDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.flowTopic.value = f.topic || "";
    el.flowJsonKey.value = f.jsonKey || f.key || f.field || "";
    el.flowBle.value = f.bleId || f.id || "meteo.tempC";

    // valve
    const vm = (e.valve && typeof e.valve.master === "number") ? e.valve.master : 0;
    el.valveMaster.value = String(vm || 0);

    // curve
    el.minFlow.value = (typeof e.minFlow === "number") ? e.minFlow : 22;
    el.maxFlow.value = (typeof e.maxFlow === "number") ? e.maxFlow : 50;

    el.slopeDay.value   = (typeof e.slopeDay === "number") ? e.slopeDay : 1.25;
    el.shiftDay.value   = (typeof e.shiftDay === "number") ? e.shiftDay : 30;
    el.slopeNight.value = (typeof e.slopeNight === "number") ? e.slopeNight : 1.0;
    el.shiftNight.value = (typeof e.shiftNight === "number") ? e.shiftNight : 25;

    // control
    const c = e.control || {};
    el.deadbandC.value = (typeof c.deadbandC === "number") ? c.deadbandC : 0.5;
    el.stepPct.value   = (typeof c.stepPct === "number") ? c.stepPct : 4;
    el.periodS.value   = Math.round(((typeof c.periodMs === "number") ? c.periodMs : 6000) / 1000);
    el.minPct.value    = (typeof c.minPct === "number") ? c.minPct : 0;
    el.maxPct.value    = (typeof c.maxPct === "number") ? c.maxPct : 100;

    // legacy refs
    const r = e.refs || {};
    const day = r.day || {};
    const night = r.night || {};
    el.heatTout1.value = (typeof day.tout1 === "number") ? day.tout1 : -10;
    el.heatTflow1.value = (typeof day.tflow1 === "number") ? day.tflow1 : 55;
    el.heatTout2.value = (typeof day.tout2 === "number") ? day.tout2 : 15;
    el.heatTflow2.value = (typeof day.tflow2 === "number") ? day.tflow2 : 30;

    el.nightTout1.value = (typeof night.tout1 === "number") ? night.tout1 : -10;
    el.nightTflow1.value = (typeof night.tflow1 === "number") ? night.tflow1 : 50;
    el.nightTout2.value = (typeof night.tout2 === "number") ? night.tout2 : 15;
    el.nightTflow2.value = (typeof night.tflow2 === "number") ? night.tflow2 : 25;

    updateSourceRows();
  }

  function saveToConfig(cfg) {
    App.ensureConfigShape(cfg);
    const e = cfg.equitherm || {};
    cfg.equitherm = e;

    e.enabled = !!el.enabled.checked;

    // outdoor
    e.outdoor = e.outdoor || {};
    const os = el.outdoorSource.value || "dallas";
    e.outdoor.source = os;
    e.outdoor.gpio = 0;
    e.outdoor.rom = "";
    e.outdoor.topic = "";
    e.outdoor.jsonKey = "";
    e.outdoor.bleId = "";

    if (os === "dallas") {
      const sel = parseDallasValue(el.outdoorDallas.value) || { gpio: 0, rom: "" };
      e.outdoor.gpio = sel.gpio;
      e.outdoor.rom  = sel.rom || "";
    } else if (os === "mqtt") {
      e.outdoor.topic = (el.outdoorTopic.value || "").trim();
      e.outdoor.jsonKey = (el.outdoorJsonKey.value || "").trim();
    } else if (os === "ble") {
      e.outdoor.bleId = String(el.outdoorBle.value || "meteo.tempC");
    }

    // flow
    e.flow = e.flow || {};
    const fs = el.flowSource.value || "dallas";
    e.flow.source = fs;
    e.flow.gpio = 0;
    e.flow.rom = "";
    e.flow.topic = "";
    e.flow.jsonKey = "";
    e.flow.bleId = "";

    if (fs === "dallas") {
      const sel = parseDallasValue(el.flowDallas.value) || { gpio: 0, rom: "" };
      e.flow.gpio = sel.gpio;
      e.flow.rom  = sel.rom || "";
    } else if (fs === "mqtt") {
      e.flow.topic = (el.flowTopic.value || "").trim();
      e.flow.jsonKey = (el.flowJsonKey.value || "").trim();
    } else if (fs === "ble") {
      e.flow.bleId = String(el.flowBle.value || "meteo.tempC");
    }

    // valve
    e.valve = e.valve || {};
    const vm = parseInt(el.valveMaster.value || "0", 10);
    e.valve.master = Number.isFinite(vm) ? vm : 0;

    // curve
    e.minFlow = parseFloat(el.minFlow.value);
    e.maxFlow = parseFloat(el.maxFlow.value);
    e.slopeDay = parseFloat(el.slopeDay.value);
    e.shiftDay = parseFloat(el.shiftDay.value);
    e.slopeNight = parseFloat(el.slopeNight.value);
    e.shiftNight = parseFloat(el.shiftNight.value);

    // control
    e.control = e.control || {};
    e.control.deadbandC = parseFloat(el.deadbandC.value);
    e.control.stepPct   = parseInt(el.stepPct.value, 10);
    e.control.periodMs  = Math.max(500, (parseInt(el.periodS.value, 10) || 6) * 1000);
    e.control.minPct    = parseInt(el.minPct.value, 10);
    e.control.maxPct    = parseInt(el.maxPct.value, 10);

    // legacy refs
    e.refs = e.refs || {};
    e.refs.day = e.refs.day || {};
    e.refs.night = e.refs.night || {};
    e.refs.day.tout1  = parseFloat(el.heatTout1.value);
    e.refs.day.tflow1 = parseFloat(el.heatTflow1.value);
    e.refs.day.tout2  = parseFloat(el.heatTout2.value);
    e.refs.day.tflow2 = parseFloat(el.heatTflow2.value);

    e.refs.night.tout1  = parseFloat(el.nightTout1.value);
    e.refs.night.tflow1 = parseFloat(el.nightTflow1.value);
    e.refs.night.tout2  = parseFloat(el.nightTout2.value);
    e.refs.night.tflow2 = parseFloat(el.nightTflow2.value);
  }

  function updateValveOptions(dash, cfg, keep) {
    const opts = [{ value: "0", label: "Nepoužívat" }];
    const valves = (dash && Array.isArray(dash.valves)) ? dash.valves : [];
    for (const v of valves) {
      if (!v || typeof v.master !== "number") continue;
      const m = v.master;
      const label = v.label ? String(v.label) : `Ventil ${m}`;
      const peer = (typeof v.peer === "number" && v.peer) ? ` / peer ${v.peer}` : "";
      opts.push({ value: String(m), label: `${label} (master ${m}${peer})` });
    }

    // If config has a value not present in dash list, include it
    const cur = (cfg && cfg.equitherm && cfg.equitherm.valve) ? cfg.equitherm.valve.master : 0;
    if (cur && !opts.some((o) => o.value === String(cur))) {
      opts.push({ value: String(cur), label: `Relé ${cur} (nenakonfigurováno jako 3c)` });
    }

    setSelectOptions(el.valveMaster, opts, keep);
  }

  async function refreshDash(cfg, keepSelections) {
    try {
      const dash = await App.apiGetJson("/api/dash");
      lastDash = dash;

      // Pokud už uživatel používá nové MQTT teploměry ("Teploměry"),
      // odebereme z výběru zdrojů ekvitermu legacy TEMP1..TEMP8.
      // (Zamezí zmatkům v UI.)
      const allowLegacyTemps = !hasAnyConfiguredMqttThermo(cfg);
      rebuildSourceSelect(el.outdoorSource, allowLegacyTemps);
      rebuildSourceSelect(el.flowSource, allowLegacyTemps);

      const dallasOpts = buildDallasOptions(cfg, dash);
      setSelectOptions(el.outdoorDallas, dallasOpts, keepSelections);
      setSelectOptions(el.flowDallas, dallasOpts, keepSelections);

      const bleOpts = buildBleOptions(dash);
      setSelectOptions(el.outdoorBle, bleOpts, keepSelections);
      setSelectOptions(el.flowBle, bleOpts, keepSelections);

      // MQTT presets from "Teploměry" (2×)
      {
        const outCurTopic = (cfg?.equitherm?.outdoor?.source === "mqtt") ? String(cfg.equitherm.outdoor.topic || "") : "";
        const outCurKey = (cfg?.equitherm?.outdoor?.source === "mqtt") ? String(cfg.equitherm.outdoor.jsonKey || "") : "";
        const flowCurTopic = (cfg?.equitherm?.flow?.source === "mqtt") ? String(cfg.equitherm.flow.topic || "") : "";
        const flowCurKey = (cfg?.equitherm?.flow?.source === "mqtt") ? String(cfg.equitherm.flow.jsonKey || "") : "";

        if (el.outdoorMqttPreset) {
          const opts = buildMqttPresetOptions(cfg, outCurTopic, outCurKey);
          setSelectOptions(el.outdoorMqttPreset, opts, false);
          const sel = opts.find(o => o.selected);
          el.outdoorMqttPreset.value = sel ? sel.value : "custom";
        }
        if (el.flowMqttPreset) {
          const opts = buildMqttPresetOptions(cfg, flowCurTopic, flowCurKey);
          setSelectOptions(el.flowMqttPreset, opts, false);
          const sel = opts.find(o => o.selected);
          el.flowMqttPreset.value = sel ? sel.value : "custom";
        }
      }

      updateValveOptions(dash, cfg, keepSelections);
      updateSourceRows();
    } catch (e) {
      // ignore; status box will show reason anyway
    }
  }

  function updateStatusBox(status, cfg) {
    const st = status && status.equitherm ? status.equitherm : null;
    if (!st) {
      el.statusBox.textContent = "Status: --";
      return;
    }

    const lines = [];
    lines.push(`Enabled: ${st.enabled ? "YES" : "NO"} • Active: ${st.active ? "YES" : "NO"} • Night: ${st.night ? "YES" : "NO"}`);
    if (st.reason) lines.push(`Reason: ${st.reason}`);

    lines.push(`Tout: ${fmtTemp(st.outdoorC)} • Tflow: ${fmtTemp(st.flowC)} • Target: ${fmtTemp(st.targetFlowC)}`);

    if (typeof st.valveMaster === "number" && st.valveMaster > 0) {
      const m = st.valveMaster;
      const pos = (typeof st.valvePosPct === "number") ? st.valvePosPct : 0;
      const tgt = (typeof st.valveTargetPct === "number") ? st.valveTargetPct : 0;
      lines.push(`Valve: master ${m} • pos ${pos}% • target ${tgt}%${st.valveMoving ? " • moving" : ""}`);
      if (typeof st.lastAdjustMs === "number" && st.lastAdjustMs > 0) lines.push(`Last adjust: ${Math.round(st.lastAdjustMs)} ms since boot`);
    } else {
      lines.push(`Valve: --`);
    }

    el.statusBox.textContent = lines.join("\n");
    drawCurve(status);
  }

  function bindEvents() {
    el.outdoorSource.addEventListener("change", updateSourceRows);
    el.flowSource.addEventListener("change", updateSourceRows);

    // MQTT presets (záložka "Teploměry")
    if (el.outdoorMqttPreset) {
      el.outdoorMqttPreset.addEventListener("change", () => {
        const cfg = App.getConfig();
        applyMqttPresetToInputs(el.outdoorMqttPreset.value, cfg, el.outdoorTopic, el.outdoorJsonKey);
      });
    }
    if (el.flowMqttPreset) {
      el.flowMqttPreset.addEventListener("change", () => {
        const cfg = App.getConfig();
        applyMqttPresetToInputs(el.flowMqttPreset.value, cfg, el.flowTopic, el.flowJsonKey);
      });
    }

    // redraw curve on edits
    const redraw = () => drawCurve(lastStatus);
    [el.slopeDay, el.shiftDay, el.slopeNight, el.shiftNight, el.minFlow, el.maxFlow].forEach((x) => {
      if (x) x.addEventListener("input", redraw);
    });

    el.btnSave.addEventListener("click", async () => {
      const cfg = App.getConfig();
      saveToConfig(cfg);
      await App.saveConfig(cfg);
      // po uložení znovu načti dash, aby se aktualizovaly seznamy
      refreshDash(cfg, true);
    });
  }

  // Chain App handlers
  const prevOnConfigLoaded = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    if (typeof prevOnConfigLoaded === "function") prevOnConfigLoaded(cfg);
    bindEvents();
    loadFromConfig(cfg);
    refreshDash(cfg, true);
    drawCurve(lastStatus);
  };

  const prevOnStatusLoaded = App.onStatusLoaded;
  App.onStatusLoaded = (status) => {
    if (typeof prevOnStatusLoaded === "function") prevOnStatusLoaded(status);
    lastStatus = status;
    updateStatusBox(status, App.getConfig());
  };

  // Periodic refresh (options + valve list)
  setInterval(() => {
    const cfg = App.getConfig();
    refreshDash(cfg, true);
  }, 2500);

})();
