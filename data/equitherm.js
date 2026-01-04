/* Equitherm tab (Dallas/MQTT + 3-way valve control)
 * - Dynamicky načte aktivní DS18B20 (rom + teplota) z /api/dash
 * - Uloží konfiguraci do cfg.equitherm (kompatibilní s firmware)
 * - Zobrazuje runtime stav z /api/status (equitherm)
 */
(() => {
  const $ = (id) => document.getElementById(id);

  const el = {
    btnSave: $("btnSaveEquitherm"),
    btnUseRoleOutdoor: $("btnEqUseRoleOutdoor"),
    btnUseRoleFlow: $("btnEqUseRoleFlow"),
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

    akuTopSource: $("eqAkuTopSource"),
    akuTopDallasRow: $("eqAkuTopDallasRow"),
    akuTopDallas: $("eqAkuTopDallas"),
    akuTopMqttRow: $("eqAkuTopMqttRow"),
    akuTopMqttPreset: $("eqAkuTopMqttPreset"),
    akuTopTopic: $("eqAkuTopTopic"),
    akuTopJsonKey: $("eqAkuTopJsonKey"),
    akuTopBleRow: $("eqAkuTopBleRow"),
    akuTopBle: $("eqAkuTopBle"),

    akuMidSource: $("eqAkuMidSource"),
    akuMidDallasRow: $("eqAkuMidDallasRow"),
    akuMidDallas: $("eqAkuMidDallas"),
    akuMidMqttRow: $("eqAkuMidMqttRow"),
    akuMidMqttPreset: $("eqAkuMidMqttPreset"),
    akuMidTopic: $("eqAkuMidTopic"),
    akuMidJsonKey: $("eqAkuMidJsonKey"),
    akuMidBleRow: $("eqAkuMidBleRow"),
    akuMidBle: $("eqAkuMidBle"),

    akuBottomSource: $("eqAkuBottomSource"),
    akuBottomDallasRow: $("eqAkuBottomDallasRow"),
    akuBottomDallas: $("eqAkuBottomDallas"),
    akuBottomMqttRow: $("eqAkuBottomMqttRow"),
    akuBottomMqttPreset: $("eqAkuBottomMqttPreset"),
    akuBottomTopic: $("eqAkuBottomTopic"),
    akuBottomJsonKey: $("eqAkuBottomJsonKey"),
    akuBottomBleRow: $("eqAkuBottomBleRow"),
    akuBottomBle: $("eqAkuBottomBle"),

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
    curveOffsetC: $("eqCurveOffsetC"),
    maxBoilerInC: $("eqMaxBoilerInC"),
    noFlowDetectEnabled: $("eqNoFlowDetectEnabled"),
    noFlowTimeoutS: $("eqNoFlowTimeoutS"),
    requireHeatCall: $("eqRequireHeatCall"),
    noHeatCallBehavior: $("eqNoHeatCallBehavior"),
    akuSupportEnabled: $("eqAkuSupportEnabled"),
    akuNoSupportBehavior: $("eqAkuNoSupportBehavior"),
    akuMinTopC: $("eqAkuMinTopC"),
    akuMinDeltaToTargetC: $("eqAkuMinDeltaToTargetC"),
    akuMinDeltaToBoilerInC: $("eqAkuMinDeltaToBoilerInC"),
    fallbackOutdoorC: $("eqFallbackOutdoorC"),
    outdoorMaxAgeMin: $("eqOutdoorMaxAgeMin"),
    systemProfile: $("eqSystemProfile"),
    nightModeSource: $("eqNightModeSource"),
    nightModeManual: $("eqNightModeManual"),

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

  // Normalizace hodnot z configu (kompatibilita se staršími verzemi, kde se ukládalo např. v desetinách).
  function normFlow(v, defV){
    if (typeof v !== 'number' || !isFinite(v)) return defV;
    // typicky 250 = 25.0
    if (v > 120 && v <= 2000) return v / 10.0;
    return v;
  }
  function normShift(v, defV){
    if (typeof v !== 'number' || !isFinite(v)) return defV;
    if (Math.abs(v) > 60 && Math.abs(v) <= 600) return v / 10.0;
    return v;
  }
  function normSlope(v, defV){
    if (typeof v !== 'number' || !isFinite(v)) return defV;
    // někde se ukládalo v setinách (např. 100 = 1.00)
    if (v > 10 && v <= 500) return v / 100.0;
    return v;
  }
  function normDeadband(v, defV){
    if (typeof v !== 'number' || !isFinite(v)) return defV;
    if (v > 5 && v <= 50) return v / 10.0;
    return v;
  }

  function readNumber(value, fallback) {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  }
  function readInt(value, fallback) {
    const n = parseInt(value, 10);
    return Number.isFinite(n) ? n : fallback;
  }
  function clampPct(value) {
    const n = Number.isFinite(value) ? value : 0;
    if (n < 0) return 0;
    if (n > 100) return 100;
    return n;
  }

  function fmtAge(ms) {
    if (!Number.isFinite(ms)) return "--";
    if (ms < 1000) return `${Math.round(ms)} ms`;
    if (ms < 60000) return `${(ms / 1000).toFixed(1)} s`;
    return `${(ms / 60000).toFixed(1)} min`;
  }

  function computeTargetFlow(tout, slope, shift, minFlow, maxFlow, offset = 0) {
    // stejný vzorec jako firmware/UI: Tflow = (20 - Tout) * slope + 20 + shift
    const t = (20 - tout) * slope + 20 + shift + offset;
    return clamp(t, minFlow, maxFlow);
  }

  function drawCurve(status) {
    const c = el.curveCanvas;
    if (!c || !c.getContext) return;
    const ctx = c.getContext("2d");

    // Canvas je v DOM často škálovaný přes CSS. Při změně šířky okna (layout 1/2 sloupce)
    // proto přepočítáme interní rozlišení na aktuální velikost v px, aby se nerozbilo vykreslení.
    const rect = c.getBoundingClientRect();
    const dpr = (window.devicePixelRatio || 1);
    const cssW = Math.max(10, Math.round(rect.width));
    const cssH = Math.max(10, Math.round(rect.height));
    const needW = Math.round(cssW * dpr);
    const needH = Math.round(cssH * dpr);
    if (c.width !== needW || c.height !== needH) {
      c.width = needW;
      c.height = needH;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    // čti hodnoty z UI (i když nejsou uložené)
    const slopeDay = parseFloat(el.slopeDay.value || "1");
    const shiftDay = parseFloat(el.shiftDay.value || "0");
    const slopeNight = parseFloat(el.slopeNight.value || "1");
    const shiftNight = parseFloat(el.shiftNight.value || "0");
    const minFlow = parseFloat(el.minFlow.value || "0");
    const maxFlow = parseFloat(el.maxFlow.value || "100");
    const offsetC = parseFloat(el.curveOffsetC.value || "0");

    const W = cssW, H = cssH;
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
        const y = computeTargetFlow(x, slope, shift, minFlow, maxFlow, offsetC);
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
      const yCalc = computeTargetFlow(x, slope, shift, minFlow, maxFlow, offsetC);
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

  function buildMqttPresetOptions(cfg, currentMqttIdx, currentTopic, currentJsonKey) {
    const list = (cfg?.thermometers && Array.isArray(cfg.thermometers.mqtt)) ? cfg.thermometers.mqtt : [];
    const opts = [];

    // Always allow custom (manual) MQTT topic
    opts.push({ value: "custom", label: "Vlastní (ruční)" });

    // Build preset choices (1..2). If not configured => keep visible but disabled.
    for (let i = 0; i < 2; i++) {
      const it = list[i] || {};
      const topic = String(it.topic || "").trim();
      const name = String(it.name || "").trim() || `MQTT teploměr ${i + 1}`;
      const key = String(it.jsonKey || "tempC").trim();
      const label = topic
        ? `${name} • ${topic}${key ? ` • ${key}` : ""}`
        : `${name} (nenastaveno)`;
      opts.push({ value: String(i + 1), label, disabled: !topic });
    }

    // Determine which preset should be selected based on config
    let selectedValue = "custom";
    const idx = Number(currentMqttIdx || 0);
    if (Number.isFinite(idx) && idx >= 1 && idx <= 2) {
      const it = list[idx - 1] || {};
      if (String(it.topic || "").trim()) selectedValue = String(idx);
    }
    if (selectedValue === "custom") {
      const curT = String(currentTopic || "").trim();
      const curK = String(currentJsonKey || "").trim();
      if (curT) {
        for (let i = 0; i < 2; i++) {
          const it = list[i] || {};
          const t = String(it.topic || "").trim();
          const k = String(it.jsonKey || "tempC").trim();
          if (t && t === curT && k === curK) {
            selectedValue = String(i + 1);
            break;
          }
        }
      }
    }

    return { options: opts, selectedValue };
  }

  function applyMqttPresetToInputs(presetValue, cfg, topicInput, jsonKeyInput) {
    if (!topicInput || !jsonKeyInput) return;
    const list = (cfg?.thermometers && Array.isArray(cfg.thermometers.mqtt)) ? cfg.thermometers.mqtt : [];
    const v = String(presetValue || "");

    // custom => keep editable
    if (v === "custom") {
      topicInput.readOnly = false;
      jsonKeyInput.readOnly = false;
      topicInput.classList.remove("ro");
      jsonKeyInput.classList.remove("ro");
      return;
    }

    const idx = Number(v);
    if (!Number.isFinite(idx) || idx < 1 || idx > 2) return;

    const it = list[idx - 1] || {};
    const topic = String(it.topic || "").trim();
    const key = String(it.jsonKey || "tempC").trim();
    if (!topic) return;

    topicInput.value = topic;
    jsonKeyInput.value = key;

    // if user picked preset, lock fields to make intent clear
    topicInput.readOnly = true;
    jsonKeyInput.readOnly = true;
    topicInput.classList.add("ro");
    jsonKeyInput.classList.add("ro");
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
      { v: "opentherm_boiler", t: "OpenTherm – kotel (flow)" },
      { v: "opentherm_return", t: "OpenTherm – vratná voda" },
      { v: "opentherm_outdoor", t: "OpenTherm – venek (OT)" },
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

    const ats = el.akuTopSource.value;
    el.akuTopDallasRow.style.display = (ats === "dallas") ? "" : "none";
    el.akuTopMqttRow.style.display   = (ats === "mqtt") ? "" : "none";
    el.akuTopBleRow.style.display    = (ats === "ble") ? "" : "none";

    const ams = el.akuMidSource.value;
    el.akuMidDallasRow.style.display = (ams === "dallas") ? "" : "none";
    el.akuMidMqttRow.style.display   = (ams === "mqtt") ? "" : "none";
    el.akuMidBleRow.style.display    = (ams === "ble") ? "" : "none";

    const abs = el.akuBottomSource.value;
    el.akuBottomDallasRow.style.display = (abs === "dallas") ? "" : "none";
    el.akuBottomMqttRow.style.display   = (abs === "mqtt") ? "" : "none";
    el.akuBottomBleRow.style.display    = (abs === "ble") ? "" : "none";
  }

  // Quick helper: copy the system role assignment (Teploměry -> "Význam teploměrů") into the ekviterm source UI.
  function applyRoleToEq(kind, roleCfg, cfg) {
    if (!roleCfg || typeof roleCfg !== "object") return false;
    const src = String(roleCfg.source || "none").trim();
    if (src === "none") return false;

    const isOutdoor = (kind === "outdoor");
    const sourceSel = isOutdoor ? el.outdoorSource : el.flowSource;
    const dallasSel = isOutdoor ? el.outdoorDallas : el.flowDallas;
    const presetSel = isOutdoor ? el.outdoorMqttPreset : el.flowMqttPreset;
    const topicInp  = isOutdoor ? el.outdoorTopic : el.flowTopic;
    const keyInp    = isOutdoor ? el.outdoorJsonKey : el.flowJsonKey;
    const bleSel    = isOutdoor ? el.outdoorBle : el.flowBle;

    // set source + toggle rows
    if (sourceSel) sourceSel.value = src;
    updateSourceRows();

    if (src === "dallas") {
      const gpio = Number.isFinite(Number(roleCfg.gpio)) ? Number(roleCfg.gpio) : 0;
      const rom = String(roleCfg.rom || roleCfg.addr || "").toUpperCase();
      const v = makeDallasValue(gpio, rom);
      if (dallasSel) {
        dallasSel.value = v;
        // fallback if specific ROM isn't present in the current option set
        if (String(dallasSel.value) !== v) dallasSel.value = makeDallasValue(gpio, "");
      }
      return true;
    }

    if (src === "mqtt") {
      const idx = Number(roleCfg.mqttIdx || roleCfg.preset || 0);
      if (presetSel) {
        if (idx >= 1 && idx <= 2) {
          presetSel.value = String(idx);
          applyMqttPresetToInputs(presetSel.value, cfg, topicInp, keyInp);
        } else {
          presetSel.value = "custom";
          applyMqttPresetToInputs("custom", cfg, topicInp, keyInp);
          if (topicInp) topicInp.value = String(roleCfg.topic || "");
          if (keyInp) keyInp.value = String(roleCfg.jsonKey || roleCfg.key || roleCfg.field || "");
        }
      } else {
        if (topicInp) topicInp.value = String(roleCfg.topic || "");
        if (keyInp) keyInp.value = String(roleCfg.jsonKey || roleCfg.key || roleCfg.field || "");
      }
      return true;
    }

    if (src === "ble") {
      const id = String(roleCfg.bleId || roleCfg.id || cfg?.thermometers?.ble?.id || "meteo.tempC");
      if (bleSel) bleSel.value = id;
      return true;
    }

    // legacy temp1..temp8
    if (/^temp\d+$/.test(src)) {
      if (sourceSel) sourceSel.value = src;
      updateSourceRows();
      return true;
    }

    return false;
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

    // boiler_in (legacy: flow)
    const f = e.boilerIn || e.flow || {};
    el.flowSource.value = f.source || "dallas";
    if ((f.source || "dallas") === "dallas") {
      const gpio = (typeof f.gpio === "number") ? f.gpio : 0;
      const rom = f.rom || f.addr || "";
      el.flowDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.flowTopic.value = f.topic || "";
    el.flowJsonKey.value = f.jsonKey || f.key || f.field || "";
    el.flowBle.value = f.bleId || f.id || "meteo.tempC";

    const akuTop = e.akuTop || {};
    el.akuTopSource.value = akuTop.source || "none";
    if ((akuTop.source || "none") === "dallas") {
      const gpio = (typeof akuTop.gpio === "number") ? akuTop.gpio : 0;
      const rom = akuTop.rom || akuTop.addr || "";
      el.akuTopDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.akuTopTopic.value = akuTop.topic || "";
    el.akuTopJsonKey.value = akuTop.jsonKey || akuTop.key || akuTop.field || "";
    el.akuTopBle.value = akuTop.bleId || akuTop.id || "meteo.tempC";

    const akuMid = e.akuMid || {};
    el.akuMidSource.value = akuMid.source || "none";
    if ((akuMid.source || "none") === "dallas") {
      const gpio = (typeof akuMid.gpio === "number") ? akuMid.gpio : 0;
      const rom = akuMid.rom || akuMid.addr || "";
      el.akuMidDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.akuMidTopic.value = akuMid.topic || "";
    el.akuMidJsonKey.value = akuMid.jsonKey || akuMid.key || akuMid.field || "";
    el.akuMidBle.value = akuMid.bleId || akuMid.id || "meteo.tempC";

    const akuBottom = e.akuBottom || {};
    el.akuBottomSource.value = akuBottom.source || "none";
    if ((akuBottom.source || "none") === "dallas") {
      const gpio = (typeof akuBottom.gpio === "number") ? akuBottom.gpio : 0;
      const rom = akuBottom.rom || akuBottom.addr || "";
      el.akuBottomDallas.value = makeDallasValue(gpio, String(rom).toUpperCase());
    }
    el.akuBottomTopic.value = akuBottom.topic || "";
    el.akuBottomJsonKey.value = akuBottom.jsonKey || akuBottom.key || akuBottom.field || "";
    el.akuBottomBle.value = akuBottom.bleId || akuBottom.id || "meteo.tempC";

    // valve
    const vm = (e.valve && typeof e.valve.master === "number") ? e.valve.master : 0;
    el.valveMaster.value = String(vm || 0);

    // curve – sane defaults (do not saturate maxFlow in common temps)
    el.minFlow.value = normFlow(e.minFlow, 25);
    el.maxFlow.value = normFlow(e.maxFlow, 55);

    // Formula: Tflow = (20 - Tout) * slope + 20 + shift
    // Defaults correspond to day refs: (-10 -> 55), (15 -> 30) => slope=1, shift=5
    // and night refs: (-10 -> 50), (15 -> 25) => slope=1, shift=0
    el.slopeDay.value   = normSlope(e.slopeDay, 1.0);
    el.shiftDay.value   = normShift(e.shiftDay, 5.0);
    el.slopeNight.value = normSlope(e.slopeNight, 1.0);
    el.shiftNight.value = normShift(e.shiftNight, 0.0);

    // control
    const c = e.control || {};
    el.deadbandC.value = normDeadband(c.deadbandC, 0.5);
    el.stepPct.value   = (typeof c.stepPct === "number") ? c.stepPct : 4;
    // Default minimum adjustment period for 3c valve: 30 s
    el.periodS.value   = Math.round(((typeof c.periodMs === "number") ? c.periodMs : 30000) / 1000);
    el.minPct.value    = (typeof c.minPct === "number") ? c.minPct : 0;
    el.maxPct.value    = (typeof c.maxPct === "number") ? c.maxPct : 100;

    el.curveOffsetC.value = (typeof e.curveOffsetC === "number") ? e.curveOffsetC : 0;
    el.maxBoilerInC.value = (typeof e.maxBoilerInC === "number") ? e.maxBoilerInC : 55;
    el.noFlowDetectEnabled.checked = (typeof e.noFlowDetectEnabled === "boolean") ? e.noFlowDetectEnabled : true;
    el.noFlowTimeoutS.value = Math.round(((typeof e.noFlowTimeoutMs === "number") ? e.noFlowTimeoutMs : 180000) / 1000);
    el.requireHeatCall.checked = (typeof e.requireHeatCall === "boolean") ? e.requireHeatCall : true;
    el.noHeatCallBehavior.value = e.noHeatCallBehavior || "hold";
    el.akuSupportEnabled.checked = (typeof e.akuSupportEnabled === "boolean") ? e.akuSupportEnabled : true;
    el.akuNoSupportBehavior.value = e.akuNoSupportBehavior || "close";
    el.akuMinTopC.value = (typeof e.akuMinTopC === "number") ? e.akuMinTopC : 40;
    el.akuMinDeltaToTargetC.value = (typeof e.akuMinDeltaToTargetC === "number") ? e.akuMinDeltaToTargetC : 2;
    el.akuMinDeltaToBoilerInC.value = (typeof e.akuMinDeltaToBoilerInC === "number") ? e.akuMinDeltaToBoilerInC : 3;
    el.fallbackOutdoorC.value = (typeof e.fallbackOutdoorC === "number") ? e.fallbackOutdoorC : 0;
    const maxAgeMs = (typeof cfg?.sensors?.outdoor?.maxAgeMs === "number") ? cfg.sensors.outdoor.maxAgeMs : 900000;
    el.outdoorMaxAgeMin.value = Math.round(maxAgeMs / 60000);

    el.systemProfile.value = cfg?.system?.profile || "standard";
    el.nightModeSource.value = cfg?.system?.nightModeSource || "input";
    el.nightModeManual.checked = !!cfg?.system?.nightModeManual;

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
    e.outdoor.mqttIdx = 0;

    if (os === "dallas") {
      const sel = parseDallasValue(el.outdoorDallas.value) || { gpio: 0, rom: "" };
      e.outdoor.gpio = sel.gpio;
      e.outdoor.rom  = sel.rom || "";
    } else if (os === "mqtt") {
      const preset = el.outdoorMqttPreset ? String(el.outdoorMqttPreset.value || "custom") : "custom";
      const idx = Number(preset);
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2) e.outdoor.mqttIdx = idx;
      e.outdoor.topic = (el.outdoorTopic.value || "").trim();
      e.outdoor.jsonKey = (el.outdoorJsonKey.value || "").trim();
    } else if (os === "ble") {
      e.outdoor.bleId = String(el.outdoorBle.value || "meteo.tempC");
    }

    // boiler_in (legacy: flow)
    e.boilerIn = e.boilerIn || {};
    e.flow = e.flow || {};
    const fs = el.flowSource.value || "dallas";
    for (const target of [e.boilerIn, e.flow]) {
      target.source = fs;
      target.gpio = 0;
      target.rom = "";
      target.topic = "";
      target.jsonKey = "";
      target.bleId = "";
      target.mqttIdx = 0;
    }

    if (fs === "dallas") {
      const sel = parseDallasValue(el.flowDallas.value) || { gpio: 0, rom: "" };
      e.boilerIn.gpio = sel.gpio;
      e.boilerIn.rom  = sel.rom || "";
      e.flow.gpio = sel.gpio;
      e.flow.rom = sel.rom || "";
    } else if (fs === "mqtt") {
      const preset = el.flowMqttPreset ? String(el.flowMqttPreset.value || "custom") : "custom";
      const idx = Number(preset);
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2) {
        e.boilerIn.mqttIdx = idx;
        e.flow.mqttIdx = idx;
      }
      e.boilerIn.topic = (el.flowTopic.value || "").trim();
      e.boilerIn.jsonKey = (el.flowJsonKey.value || "").trim();
      e.flow.topic = e.boilerIn.topic;
      e.flow.jsonKey = e.boilerIn.jsonKey;
    } else if (fs === "ble") {
      e.boilerIn.bleId = String(el.flowBle.value || "meteo.tempC");
      e.flow.bleId = e.boilerIn.bleId;
    }

    const saveAku = (srcSel, dallasSel, mqttPreset, topicInp, keyInp, bleSel, target) => {
      target.source = srcSel.value || "none";
      target.gpio = 0;
      target.rom = "";
      target.topic = "";
      target.jsonKey = "";
      target.bleId = "";
      target.mqttIdx = 0;
      if (target.source === "dallas") {
        const sel = parseDallasValue(dallasSel.value) || { gpio: 0, rom: "" };
        target.gpio = sel.gpio;
        target.rom = sel.rom || "";
      } else if (target.source === "mqtt") {
        const preset = mqttPreset ? String(mqttPreset.value || "custom") : "custom";
        const idx = Number(preset);
        if (Number.isFinite(idx) && idx >= 1 && idx <= 2) target.mqttIdx = idx;
        target.topic = (topicInp.value || "").trim();
        target.jsonKey = (keyInp.value || "").trim();
      } else if (target.source === "ble") {
        target.bleId = String(bleSel.value || "meteo.tempC");
      }
    };

    e.akuTop = e.akuTop || {};
    e.akuMid = e.akuMid || {};
    e.akuBottom = e.akuBottom || {};
    saveAku(el.akuTopSource, el.akuTopDallas, el.akuTopMqttPreset, el.akuTopTopic, el.akuTopJsonKey, el.akuTopBle, e.akuTop);
    saveAku(el.akuMidSource, el.akuMidDallas, el.akuMidMqttPreset, el.akuMidTopic, el.akuMidJsonKey, el.akuMidBle, e.akuMid);
    saveAku(el.akuBottomSource, el.akuBottomDallas, el.akuBottomMqttPreset, el.akuBottomTopic, el.akuBottomJsonKey, el.akuBottomBle, e.akuBottom);

    // valve
    e.valve = e.valve || {};
    const vm = parseInt(el.valveMaster.value || "0", 10);
    e.valve.master = Number.isFinite(vm) ? vm : 0;

    // curve
    e.minFlow = readNumber(el.minFlow.value, e.minFlow ?? 25);
    e.maxFlow = readNumber(el.maxFlow.value, e.maxFlow ?? 55);
    e.slopeDay = readNumber(el.slopeDay.value, e.slopeDay ?? 1.0);
    e.shiftDay = readNumber(el.shiftDay.value, e.shiftDay ?? 5.0);
    e.slopeNight = readNumber(el.slopeNight.value, e.slopeNight ?? 1.0);
    e.shiftNight = readNumber(el.shiftNight.value, e.shiftNight ?? 0.0);

    // control
    e.control = e.control || {};
    e.control.deadbandC = readNumber(el.deadbandC.value, e.control.deadbandC ?? 0.5);
    e.control.stepPct   = clamp(readInt(el.stepPct.value, e.control.stepPct ?? 4), 1, 25);
    // Default: 30 s (anti-hunt). Clamp to >= 500 ms.
    const periodS = readNumber(el.periodS.value, Math.round((e.control.periodMs ?? 30000) / 1000));
    e.control.periodMs  = Math.max(500, Math.round(periodS * 1000));
    e.control.minPct    = clampPct(readInt(el.minPct.value, e.control.minPct ?? 0));
    e.control.maxPct    = clampPct(readInt(el.maxPct.value, e.control.maxPct ?? 100));
    e.deadbandC = e.control.deadbandC;
    e.stepPct = e.control.stepPct;
    e.controlPeriodMs = e.control.periodMs;

    e.curveOffsetC = readNumber(el.curveOffsetC.value, e.curveOffsetC ?? 0);
    e.maxBoilerInC = readNumber(el.maxBoilerInC.value, e.maxBoilerInC ?? 55);
    e.noFlowDetectEnabled = !!el.noFlowDetectEnabled.checked;
    e.noFlowTimeoutMs = Math.max(10000, readNumber(el.noFlowTimeoutS.value, (e.noFlowTimeoutMs ?? 180000) / 1000) * 1000);
    e.requireHeatCall = !!el.requireHeatCall.checked;
    e.noHeatCallBehavior = String(el.noHeatCallBehavior.value || "hold");
    e.akuSupportEnabled = !!el.akuSupportEnabled.checked;
    e.akuNoSupportBehavior = String(el.akuNoSupportBehavior.value || "close");
    e.akuMinTopC = readNumber(el.akuMinTopC.value, e.akuMinTopC ?? 40);
    e.akuMinDeltaToTargetC = readNumber(el.akuMinDeltaToTargetC.value, e.akuMinDeltaToTargetC ?? 2);
    e.akuMinDeltaToBoilerInC = readNumber(el.akuMinDeltaToBoilerInC.value, e.akuMinDeltaToBoilerInC ?? 3);
    e.fallbackOutdoorC = readNumber(el.fallbackOutdoorC.value, e.fallbackOutdoorC ?? 0);

    cfg.sensors = cfg.sensors || {};
    cfg.sensors.outdoor = cfg.sensors.outdoor || {};
    cfg.sensors.outdoor.maxAgeMs = Math.max(1000, readNumber(el.outdoorMaxAgeMin.value, (cfg.sensors.outdoor.maxAgeMs ?? 900000) / 60000) * 60000);

    cfg.system = cfg.system || {};
    cfg.system.profile = String(el.systemProfile.value || "standard");
    cfg.system.nightModeSource = String(el.nightModeSource.value || "input");
    cfg.system.nightModeManual = !!el.nightModeManual.checked;

    // legacy refs
    e.refs = e.refs || {};
    e.refs.day = e.refs.day || {};
    e.refs.night = e.refs.night || {};
    e.refs.day.tout1  = readNumber(el.heatTout1.value, e.refs.day.tout1 ?? -10);
    e.refs.day.tflow1 = readNumber(el.heatTflow1.value, e.refs.day.tflow1 ?? 55);
    e.refs.day.tout2  = readNumber(el.heatTout2.value, e.refs.day.tout2 ?? 15);
    e.refs.day.tflow2 = readNumber(el.heatTflow2.value, e.refs.day.tflow2 ?? 30);

    e.refs.night.tout1  = readNumber(el.nightTout1.value, e.refs.night.tout1 ?? -10);
    e.refs.night.tflow1 = readNumber(el.nightTflow1.value, e.refs.night.tflow1 ?? 50);
    e.refs.night.tout2  = readNumber(el.nightTout2.value, e.refs.night.tout2 ?? 15);
    e.refs.night.tflow2 = readNumber(el.nightTflow2.value, e.refs.night.tflow2 ?? 25);
  }

  function updateValveOptions(dash, cfg, keep) {
    const opts = [{ value: "0", label: "Nepoužívat" }];
    const seen = new Set();
    const relayNames = Array.isArray(cfg?.relayNames) ? cfg.relayNames : [];

    const addOption = (master, peer) => {
      const m = Number(master);
      if (!Number.isFinite(m) || m < 1 || m > 8 || seen.has(m)) return;
      const rawName = String(relayNames[m - 1] || "").trim();
      const base = rawName || `Ventil ${m}`;
      const peerTxt = (peer && Number.isFinite(Number(peer))) ? ` / peer ${peer}` : "";
      opts.push({ value: String(m), label: `${base} (master ${m}${peerTxt})` });
      seen.add(m);
    };

    const valves = (dash && Array.isArray(dash.valves)) ? dash.valves : [];
    for (const v of valves) addOption(v?.master, v?.peer);

    const outs = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
    outs.forEach((o, idx) => {
      const r = String(o?.role || "");
      // kompatibilita: starší role "valve_3way_2rel" se bere jako směšovací
      if (r !== "valve_3way_mix" && r !== "valve_3way_2rel") return;
      const params = (o && typeof o.params === "object") ? o.params : {};
      const peer = params.peerRel ?? params.partnerRelay;
      addOption(idx + 1, peer);
    });

    // If config has a value not present in list, include it
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
      rebuildSourceSelect(el.akuTopSource, false);
      rebuildSourceSelect(el.akuMidSource, false);
      rebuildSourceSelect(el.akuBottomSource, false);

      const dallasOpts = buildDallasOptions(cfg, dash);
      setSelectOptions(el.outdoorDallas, dallasOpts, keepSelections);
      setSelectOptions(el.flowDallas, dallasOpts, keepSelections);
      setSelectOptions(el.akuTopDallas, dallasOpts, keepSelections);
      setSelectOptions(el.akuMidDallas, dallasOpts, keepSelections);
      setSelectOptions(el.akuBottomDallas, dallasOpts, keepSelections);

      const bleOpts = buildBleOptions(dash);
      setSelectOptions(el.outdoorBle, bleOpts, keepSelections);
      setSelectOptions(el.flowBle, bleOpts, keepSelections);
      setSelectOptions(el.akuTopBle, bleOpts, keepSelections);
      setSelectOptions(el.akuMidBle, bleOpts, keepSelections);
      setSelectOptions(el.akuBottomBle, bleOpts, keepSelections);

      // MQTT presets from "Teploměry" (2×)
      {
        const outSrc = cfg?.equitherm?.outdoor || {};
        const flowSrc = cfg?.equitherm?.boilerIn || cfg?.equitherm?.flow || {};
        const akuTopSrc = cfg?.equitherm?.akuTop || {};
        const akuMidSrc = cfg?.equitherm?.akuMid || {};
        const akuBottomSrc = cfg?.equitherm?.akuBottom || {};
        const outCurTopic = (outSrc.source === "mqtt") ? String(outSrc.topic || "") : "";
        const outCurKey   = (outSrc.source === "mqtt") ? String(outSrc.jsonKey || "") : "";
        const outCurIdx   = (outSrc.source === "mqtt") ? Number(outSrc.mqttIdx || outSrc.preset || 0) : 0;
        const flowCurTopic = (flowSrc.source === "mqtt") ? String(flowSrc.topic || "") : "";
        const flowCurKey   = (flowSrc.source === "mqtt") ? String(flowSrc.jsonKey || "") : "";
        const flowCurIdx   = (flowSrc.source === "mqtt") ? Number(flowSrc.mqttIdx || flowSrc.preset || 0) : 0;
        const akuTopCurTopic = (akuTopSrc.source === "mqtt") ? String(akuTopSrc.topic || "") : "";
        const akuTopCurKey   = (akuTopSrc.source === "mqtt") ? String(akuTopSrc.jsonKey || "") : "";
        const akuTopCurIdx   = (akuTopSrc.source === "mqtt") ? Number(akuTopSrc.mqttIdx || akuTopSrc.preset || 0) : 0;
        const akuMidCurTopic = (akuMidSrc.source === "mqtt") ? String(akuMidSrc.topic || "") : "";
        const akuMidCurKey   = (akuMidSrc.source === "mqtt") ? String(akuMidSrc.jsonKey || "") : "";
        const akuMidCurIdx   = (akuMidSrc.source === "mqtt") ? Number(akuMidSrc.mqttIdx || akuMidSrc.preset || 0) : 0;
        const akuBottomCurTopic = (akuBottomSrc.source === "mqtt") ? String(akuBottomSrc.topic || "") : "";
        const akuBottomCurKey   = (akuBottomSrc.source === "mqtt") ? String(akuBottomSrc.jsonKey || "") : "";
        const akuBottomCurIdx   = (akuBottomSrc.source === "mqtt") ? Number(akuBottomSrc.mqttIdx || akuBottomSrc.preset || 0) : 0;

        if (el.outdoorMqttPreset) {
          const res = buildMqttPresetOptions(cfg, outCurIdx, outCurTopic, outCurKey);
          setSelectOptions(el.outdoorMqttPreset, res.options, false);
          el.outdoorMqttPreset.value = res.selectedValue;
          applyMqttPresetToInputs(res.selectedValue, cfg, el.outdoorTopic, el.outdoorJsonKey);
        }
        if (el.flowMqttPreset) {
          const res = buildMqttPresetOptions(cfg, flowCurIdx, flowCurTopic, flowCurKey);
          setSelectOptions(el.flowMqttPreset, res.options, false);
          el.flowMqttPreset.value = res.selectedValue;
          applyMqttPresetToInputs(res.selectedValue, cfg, el.flowTopic, el.flowJsonKey);
        }
        if (el.akuTopMqttPreset) {
          const res = buildMqttPresetOptions(cfg, akuTopCurIdx, akuTopCurTopic, akuTopCurKey);
          setSelectOptions(el.akuTopMqttPreset, res.options, false);
          el.akuTopMqttPreset.value = res.selectedValue;
          applyMqttPresetToInputs(res.selectedValue, cfg, el.akuTopTopic, el.akuTopJsonKey);
        }
        if (el.akuMidMqttPreset) {
          const res = buildMqttPresetOptions(cfg, akuMidCurIdx, akuMidCurTopic, akuMidCurKey);
          setSelectOptions(el.akuMidMqttPreset, res.options, false);
          el.akuMidMqttPreset.value = res.selectedValue;
          applyMqttPresetToInputs(res.selectedValue, cfg, el.akuMidTopic, el.akuMidJsonKey);
        }
        if (el.akuBottomMqttPreset) {
          const res = buildMqttPresetOptions(cfg, akuBottomCurIdx, akuBottomCurTopic, akuBottomCurKey);
          setSelectOptions(el.akuBottomMqttPreset, res.options, false);
          el.akuBottomMqttPreset.value = res.selectedValue;
          applyMqttPresetToInputs(res.selectedValue, cfg, el.akuBottomTopic, el.akuBottomJsonKey);
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

    lines.push(`Tout: ${fmtTemp(st.outdoorC)} • boiler_in: ${fmtTemp(st.actualC ?? st.flowC)} • Target: ${fmtTemp(st.targetC ?? st.targetFlowC)}`);
    if (typeof st.outdoorValid !== "undefined") {
      const oValid = st.outdoorValid ? "OK" : "NE";
      const age = (typeof st.outdoorAgeMs === "number") ? fmtAge(st.outdoorAgeMs) : "--";
      const reason = st.outdoorReason ? ` • ${st.outdoorReason}` : "";
      lines.push(`Outdoor source: ${oValid} • age ${age}${reason}`);
    }

    if (typeof st.valveMaster === "number" && st.valveMaster > 0) {
      const m = st.valveMaster;
      const pos = (typeof st.valvePosPct === "number") ? st.valvePosPct : 0;
      const tgt = (typeof st.valveTargetPct === "number") ? st.valveTargetPct : 0;
      lines.push(`Valve: master ${m} • pos ${pos}% • target ${tgt}%${st.valveMoving ? " • moving" : ""}`);
      if (typeof st.lastAdjustMs === "number" && st.lastAdjustMs > 0) lines.push(`Last adjust: ${Math.round(st.lastAdjustMs)} ms since boot`);
    } else {
      lines.push(`Valve: --`);
    }

    if (typeof st.akuSupportActive !== "undefined") {
      const support = st.akuSupportActive ? "ON" : "OFF";
      const reason = st.akuSupportReason ? ` • ${st.akuSupportReason}` : "";
      lines.push(`AKU support: ${support}${reason}`);
    }
    if (typeof st.akuTopC === "number" || typeof st.akuMidC === "number" || typeof st.akuBottomC === "number") {
      lines.push(`AKU: top ${fmtTemp(st.akuTopC)} • mid ${fmtTemp(st.akuMidC)} • bottom ${fmtTemp(st.akuBottomC)}`);
    }

    el.statusBox.textContent = lines.join("\n");
    drawCurve(status);
  }

  function bindEvents() {
    el.outdoorSource.addEventListener("change", updateSourceRows);
    el.flowSource.addEventListener("change", updateSourceRows);
    el.akuTopSource.addEventListener("change", updateSourceRows);
    el.akuMidSource.addEventListener("change", updateSourceRows);
    el.akuBottomSource.addEventListener("change", updateSourceRows);

    // Quick apply: copy "Význam teploměrů" (Teploměry) into ekviterm sources
    if (el.btnUseRoleOutdoor) {
      el.btnUseRoleOutdoor.addEventListener("click", () => {
        const cfg = (App.ensureConfigShape ? App.ensureConfigShape() : App.getConfig());
        const role = cfg?.thermometers?.roles?.outdoor;
        if (!applyRoleToEq("outdoor", role, cfg)) {
          App.toast("Role 'Venkovní teplota' není nastavena (Teploměry -> Význam teploměrů).", true);
        } else {
          App.toast("Použito přiřazení role: Venkovní teplota.");
        }
      });
    }
    if (el.btnUseRoleFlow) {
      el.btnUseRoleFlow.addEventListener("click", () => {
        const cfg = (App.ensureConfigShape ? App.ensureConfigShape() : App.getConfig());
        const role = cfg?.thermometers?.roles?.flow;
        if (!applyRoleToEq("flow", role, cfg)) {
          App.toast("Role 'Teplota otopné vody' není nastavena (Teploměry -> Význam teploměrů).", true);
        } else {
          App.toast("Použito přiřazení role: Teplota otopné vody.");
        }
      });
    }

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
    if (el.akuTopMqttPreset) {
      el.akuTopMqttPreset.addEventListener("change", () => {
        const cfg = App.getConfig();
        applyMqttPresetToInputs(el.akuTopMqttPreset.value, cfg, el.akuTopTopic, el.akuTopJsonKey);
      });
    }
    if (el.akuMidMqttPreset) {
      el.akuMidMqttPreset.addEventListener("change", () => {
        const cfg = App.getConfig();
        applyMqttPresetToInputs(el.akuMidMqttPreset.value, cfg, el.akuMidTopic, el.akuMidJsonKey);
      });
    }
    if (el.akuBottomMqttPreset) {
      el.akuBottomMqttPreset.addEventListener("change", () => {
        const cfg = App.getConfig();
        applyMqttPresetToInputs(el.akuBottomMqttPreset.value, cfg, el.akuBottomTopic, el.akuBottomJsonKey);
      });
    }

    // redraw curve on edits
    const redraw = () => drawCurve(lastStatus);
    [el.slopeDay, el.shiftDay, el.slopeNight, el.shiftNight, el.minFlow, el.maxFlow].forEach((x) => {
      if (x) x.addEventListener("input", redraw);
    });

    // Při změně velikosti okna se často přepíná layout (1/2 sloupce) => přepočítat canvas.
    let resizeT = null;
    window.addEventListener("resize", () => {
      if (resizeT) clearTimeout(resizeT);
      resizeT = setTimeout(() => drawCurve(lastStatus), 120);
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
    // po prvním renderu layoutu dej šanci layoutu dopočítat velikosti
    setTimeout(() => drawCurve(lastStatus), 0);
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
