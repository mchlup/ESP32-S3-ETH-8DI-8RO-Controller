/* New UI (v2) – uses the same backend endpoints (/api/*) */
(() => {
  // NOTE: pro externí moduly (např. buzzer.js) vystavíme minimální API přes window.App
  const $ = (sel, root=document) => root.querySelector(sel);
  const $$ = (sel, root=document) => Array.from(root.querySelectorAll(sel));

  const MODE_IDS = ["MODE1","MODE2","MODE3","MODE4","MODE5"]; // backend IDs
  const MODE_DEFAULT_NAMES = ["MODE1","MODE2","MODE3","MODE4","MODE5"]; // UI defaults
  const RELAY_COUNT = 8;
  const INPUT_COUNT = 8;

  const state = {
    status: null,
    config: null,
    bleStatus: null,
    bleConfig: null,
    blePaired: null,
    rules: null,
    rulesStatus: null,
    files: null,
  ui: { lock: { controlUntil: 0, systemUntil: 0 }, timers: { autoApply: null } },
  };

  // ---------- utils ----------
  const toast = (msg, kind="") => {
    const el = $("#toast");
    el.textContent = msg;
    el.className = "toast show" + (kind ? " " + kind : "");
    clearTimeout(toast._t);
    toast._t = setTimeout(() => el.classList.remove("show"), 2200);
  };

  const fmtMs = (ms) => {
    if (ms == null || !isFinite(ms)) return "—";
    ms = Math.max(0, Math.floor(ms));
    const s = Math.floor(ms/1000);
    const m = Math.floor(s/60);
    const h = Math.floor(m/60);
    const d = Math.floor(h/24);
    if (d>0) return `${d}d ${h%24}h`;
    if (h>0) return `${h}h ${m%60}m`;
    if (m>0) return `${m}m ${s%60}s`;
    return `${s}s`;
  };

  const safeJson = (txt) => {
    try { return JSON.parse(txt); } catch { return null; }
  };

  const prettyJson = (obj) => JSON.stringify(obj, null, 2);

  const apiGet = async (url) => {
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    const ct = r.headers.get("content-type") || "";
    if (ct.includes("application/json")) return await r.json();
    return await r.text();
  };

  const apiPostJson = async (url, obj) => {
    const r = await fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(obj),
    });
    const ct = r.headers.get("content-type") || "";
    const body = ct.includes("application/json") ? await r.json().catch(()=>null) : await r.text().catch(()=>null);
    if (!r.ok) throw new Error((body && body.error) ? body.error : `${r.status} ${r.statusText}`);
    return body;
  };

  const apiPostText = async (url, text) => {
    const r = await fetch(url, { method: "POST", headers: { "content-type":"application/json" }, body: text });
    const ct = r.headers.get("content-type") || "";
    const body = ct.includes("application/json") ? await r.json().catch(()=>null) : await r.text().catch(()=>null);
    if (!r.ok) throw new Error((body && body.error) ? body.error : `${r.status} ${r.statusText}`);
    return body;
  };
  
  // Expose minimal helpers for modular UI files (buzzer.js, etc.)
  window.App = window.App || {};
  window.App.apiGetJson = apiGet;
  window.App.apiPostJson = apiPostJson;
  window.App.toast = toast;
  window.App.$ = $;
  window.App.$$ = $$;

  // ---------- navigation ----------
  const setCfgTab = (name) => {
    // FIX: scope pouze na stránku Konfigurace (jinak se deaktivují tabPage i v Rule engine)
    const root = $("#page-config");
    if (!root) return;
    $$(".tab", root).forEach(b => b.classList.toggle("active", b.dataset.ctab === name));
    $$(".tabPage", root).forEach(p => p.classList.toggle("active", p.id === `cfg-${name}`));

    // pro ostatní moduly (např. kalibrace ventilů) – re-render při přepnutí záložky
    try{
      window.dispatchEvent(new CustomEvent("app:cfgTabChanged", { detail: { tab: name } }));
    }catch(_){ }
  };

  const showPage = (name, ctab = "") => {
    const sub = String(ctab || "");
    $$(".navItem").forEach(b => {
      const p = String(b.dataset.page || "");
      const t = String(b.dataset.ctab || "");
      b.classList.toggle("active", p === name && t === sub);
    });
    $$(".page").forEach(p => p.classList.toggle("active", p.id === `page-${name}`));

    if (name === "config" && sub) setCfgTab(sub);

    const h = sub ? `#${name}/${sub}` : `#${name}`;
    if (location.hash !== h) history.replaceState(null, "", h);
  };

  // ---------- render helpers ----------
  const relayName = (i) => (state.config?.relayNames?.[i] || `Relé ${i+1}`).toString();
  const inputName = (i) => (state.config?.inputNames?.[i] || `Vstup ${i+1}`).toString();
  const modeName = (idx) => {
    const cfg = state.config || {};
    const legacy = cfg.modeNames || cfg.mode_names || [];
    const v = (legacy[idx] || "").toString().trim();
    if (v.toUpperCase() === 'MODE1' && idx === 0) return 'MODE1';
    return v || MODE_DEFAULT_NAMES[idx];
  };

  const fillModeSelect = () => {
    const sel = $("#selSystemMode");
    sel.innerHTML = "";
    MODE_IDS.forEach((id, idx) => {
      const opt = document.createElement("option");
      opt.value = id;
      opt.textContent = `${modeName(idx)} (${id})`;
      sel.appendChild(opt);
    });
  };

  // ---------- render: dashboard ----------
  const renderTop = () => {
    const st = state.status || {};
    const wifi = st.wifiConnected ?? st?.wifi?.connected ?? false;
    const mqtt = st.mqttConnected ?? st?.mqtt?.connected ?? false;

    $("#chipWifi").textContent = "Wi‑Fi: " + (wifi ? "online" : "offline");
    $("#chipMqtt").textContent = "MQTT: " + (mqtt ? "connected" : "disconnected");

    $("#pillIp").textContent = "IP: " + (st.ip || "—");
    $("#pillUptime").textContent = "uptime: " + fmtMs(st.uptimeMs ?? st.uptime ?? null);

    $("#brandSub").textContent = (st.ip ? `IP ${st.ip}` : "—");
    {
    const sys = st.systemMode || st.mode || "—";
    const ctrl = st.controlMode || "—";
    const eq = st.equitherm || {};
    const tuv = st.tuv || {};
    let eqHint = "";
    let tuvHint = "";
    if (typeof eq.enabled !== "undefined") {
      if (!eq.enabled) eqHint = " • Ekviterm: vypnuto";
      else if (eq.active) eqHint = " • Ekviterm: aktivní";
      else eqHint = ` • Ekviterm: čeká${eq.reason ? " ("+eq.reason+")" : ""}`;
    }
    if (typeof tuv.active !== "undefined") {
      if (tuv.active) tuvHint = " • TUV: aktivní";
      else if (tuv.scheduleEnabled) tuvHint = " • TUV: čeká";
    } else if (typeof tuv.modeActive !== "undefined") {
      if (tuv.modeActive) tuvHint = " • TUV: aktivní";
      else if (tuv.scheduleEnabled) tuvHint = " • TUV: čeká";
    }
    $("#topHint").textContent = `System: ${sys} • Control: ${ctrl}${eqHint}${tuvHint}`;
  }

    $("#kvSystemMode").textContent = st.systemMode || st.mode || "—";
    $("#kvControlMode").textContent = st.controlMode || "—";
    $("#kvRssi").textContent = (st?.wifi?.rssi != null) ? `${st.wifi.rssi} dBm` : "—";

    // BLE + meteo optional fields (may be missing in older firmware)
    const cc = st.serverConnectedCount ?? 0;
    $("#kvBle").textContent = (cc ? `připojeno (${cc})` : "ne");
    $("#kvPairing").textContent = st.pairingWindow ? `ANO (${st.pairingRemainingSec ?? 0}s)` : "ne";

    if (!st.meteoEnabled) $("#kvMeteo").textContent = "OFF";
    else if (st.meteoConnected && st.meteoFix) {
      const m = st.meteo || {};
      $("#kvMeteo").textContent = `${m.tempC ?? "?"}°C / ${m.hum ?? "?"}%`;
    } else if (st.meteoConnected) $("#kvMeteo").textContent = "connected (bez dat)";
    else $("#kvMeteo").textContent = "ne";

    const cm = (st.controlMode || "").toLowerCase();
    const badge = $("#badgeControl");
    badge.className = "badge " + (cm.includes("manual") ? "warn" : "ok");
    badge.textContent = cm || "—";
  };

  
// ---------- input logic (ACTIVE level) ----------
// Firmware variants:
// - st.inputs: may be RAW GPIO (older) or LOGICAL active (newer)
// - st.inputsRaw: if present, st.inputs is logical and inputsRaw is raw
const computeInputActive = (i) => {
  const st = state.status || {};
  const cfg = state.config || {};
  const hasRaw = Array.isArray(st.inputsRaw);
  const rawArr = hasRaw ? st.inputsRaw : (Array.isArray(st.inputs) ? st.inputs : null);
  const raw = rawArr ? !!rawArr[i] : false;

  // Determine active level. Default = LOW (active when GPIO reads 0/false).
  let level = "LOW";
  if (Array.isArray(cfg.inputs) && cfg.inputs[i] && typeof cfg.inputs[i] === "object" && cfg.inputs[i].activeLevel) {
    level = String(cfg.inputs[i].activeLevel).toUpperCase();
  } else if (Array.isArray(cfg.inputActiveLevels)) {
    level = cfg.inputActiveLevels[i] ? "HIGH" : "LOW";
  }

  const activeFromRaw = (level === "HIGH") ? raw : !raw;

  // If firmware already provides logical inputs (detected by inputsRaw),
  // use that directly (no inversion).
  if (hasRaw && Array.isArray(st.inputs)) return !!st.inputs[i];
  return activeFromRaw;
};

const renderIO = () => {
    const cfg = ensureConfigShape();
    const st = state.status || {};
    const relays = Array.isArray(st.relays) ? st.relays : [];
    const inputs = Array.isArray(st.inputs) ? st.inputs : [];

    // peer relé definované v nastavení 3c ventilu (i když nejsou explicitně označené rolí)
    const peerRelays = new Set();
    const outsCfg = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
    for (let mi = 0; mi < outsCfg.length; mi++) {
      const r = String(outsCfg[mi]?.role || "");
      if (r !== "valve_3way_mix" && r !== "valve_3way_2rel") continue;
      const p = outsCfg[mi]?.params || {};
      const peerRel = Number(p.peerRel ?? (mi + 2)); // 1-based
      const peer0 = peerRel - 1;
      if (Number.isFinite(peer0) && peer0 >= 0 && peer0 < RELAY_COUNT) peerRelays.add(peer0);
    }

    const rg = $("#relayGrid");
    rg.innerHTML = "";
    for (let i=0;i<RELAY_COUNT;i++){
      // 3c ventil (2 relé) se neovládá jako jednoduché relé – je zobrazen jako widget v dashboardu
      const role = String(cfg?.iofunc?.outputs?.[i]?.role || "none");
      if (role === "valve_3way_mix" || role === "valve_3way_2rel" || role === "valve_3way_peer" || peerRelays.has(i)) continue;
      const isOn = !!relays[i];
      const el = document.createElement("div");
      el.className = "io";
      el.innerHTML = `
        <div>
          <div class="name">${escapeHtml(relayName(i))}</div>
          <div class="meta">Relé ${i+1}</div>
        </div>
        <div class="sw ${isOn ? "on":""}" role="switch" aria-checked="${isOn}" tabindex="0" data-relay="${i}"></div>
      `;
      rg.appendChild(el);
    }

    const ig = $("#inputGrid");
    ig.innerHTML = "";
    for (let i=0;i<INPUT_COUNT;i++){
      const active = computeInputActive(i);
      const el = document.createElement("div");
      el.className = "io";
      el.innerHTML = `
        <div>
          <div class="name">${escapeHtml(inputName(i))}</div>
          <div class="meta">Vstup ${i+1}</div>
        </div>
        <span class="badge ${active ? "ok":"warn"}">${active ? "ACTIVE" : "inactive"}</span>
      `;
      ig.appendChild(el);
    }
  };

  // ---------- render: config tables ----------
  const ensureConfigShape = () => {
    if (!state.config || typeof state.config !== "object") state.config = {};
    const cfg = state.config;

    cfg.inputNames = Array.isArray(cfg.inputNames) ? cfg.inputNames : Array(INPUT_COUNT).fill("");
    cfg.relayNames = Array.isArray(cfg.relayNames) ? cfg.relayNames : Array(RELAY_COUNT).fill("");
    cfg.inputActiveLevels = Array.isArray(cfg.inputActiveLevels) ? cfg.inputActiveLevels : Array(INPUT_COUNT).fill(0);

    // some older configs used cfg.inputs[i].activeLevel = "LOW"/"HIGH"
    cfg.inputs = Array.isArray(cfg.inputs) ? cfg.inputs : Array.from({length: INPUT_COUNT}, () => ({}));
    for (let i=0;i<INPUT_COUNT;i++){
      if (typeof cfg.inputs[i] !== "object" || !cfg.inputs[i]) cfg.inputs[i] = {};
      if (!("activeLevel" in cfg.inputs[i])) cfg.inputs[i].activeLevel = (cfg.inputActiveLevels[i] ? "HIGH" : "LOW");
    }

    cfg.relayMap = Array.isArray(cfg.relayMap) ? cfg.relayMap : Array.from({length: RELAY_COUNT}, () => ({ input: 0, polarity: 1 }));
    for (let r=0;r<RELAY_COUNT;r++){
      const o = cfg.relayMap[r] || {};
      cfg.relayMap[r] = { input: Number(o.input||0), polarity: (o.polarity==null ? 1 : Number(!!o.polarity)) };
    }

    cfg.modeNames = Array.isArray(cfg.modeNames) ? cfg.modeNames : MODE_DEFAULT_NAMES.slice();
    cfg.modeDescriptions = Array.isArray(cfg.modeDescriptions) ? cfg.modeDescriptions : ["","","","",""];

    cfg.modes = Array.isArray(cfg.modes) ? cfg.modes : [];
    // ensure mode objects
    for (let mi=0; mi<MODE_IDS.length; mi++){
      const id = MODE_IDS[mi];
      const existing = cfg.modes.find(m => String(m?.id||"").toUpperCase() === id);
      if (!existing){
        cfg.modes.push({ id, triggerInput: 0, relayStates: Array(RELAY_COUNT).fill(false) });
      } else {
        existing.triggerInput = Number(existing.triggerInput || 0);
        if (!Array.isArray(existing.relayStates)) existing.relayStates = Array(RELAY_COUNT).fill(false);
        existing.relayStates = existing.relayStates.map(v => !!v).slice(0, RELAY_COUNT);
        while (existing.relayStates.length < RELAY_COUNT) existing.relayStates.push(false);
      }
    }

    // MQTT group
    cfg.mqtt = (cfg.mqtt && typeof cfg.mqtt === "object") ? cfg.mqtt : {};

    // System profile
    cfg.system = (cfg.system && typeof cfg.system === "object") ? cfg.system : {};
    cfg.system.profile = cfg.system.profile || "standard";
    cfg.system.nightModeSource = cfg.system.nightModeSource || "heat_call";
    cfg.system.nightModeManual = !!cfg.system.nightModeManual;

    // Sensors
    cfg.sensors = (cfg.sensors && typeof cfg.sensors === "object") ? cfg.sensors : {};
    cfg.sensors.outdoor = (cfg.sensors.outdoor && typeof cfg.sensors.outdoor === "object") ? cfg.sensors.outdoor : {};
    cfg.sensors.outdoor.maxAgeMs = (typeof cfg.sensors.outdoor.maxAgeMs === "number") ? cfg.sensors.outdoor.maxAgeMs : 900000;

    // DHW recirc
    cfg.dhwRecirc = (cfg.dhwRecirc && typeof cfg.dhwRecirc === "object") ? cfg.dhwRecirc : {};
    const r = cfg.dhwRecirc;
    r.enabled = !!r.enabled;
    r.mode = r.mode || "on_demand";
    r.demandInput = Number.isFinite(Number(r.demandInput)) ? Number(r.demandInput) : 0;
    r.pumpRelay = Number.isFinite(Number(r.pumpRelay)) ? Number(r.pumpRelay) : 0;
    r.onDemandRunMs = (typeof r.onDemandRunMs === "number") ? r.onDemandRunMs : 120000;
    r.minOffMs = (typeof r.minOffMs === "number") ? r.minOffMs : 300000;
    r.minOnMs = (typeof r.minOnMs === "number") ? r.minOnMs : 30000;
    r.cycleOnMs = (typeof r.cycleOnMs === "number") ? r.cycleOnMs : 300000;
    r.cycleOffMs = (typeof r.cycleOffMs === "number") ? r.cycleOffMs : 900000;
    r.stopTempC = (typeof r.stopTempC === "number") ? r.stopTempC : 42;
    r.tempReturnSource = (r.tempReturnSource && typeof r.tempReturnSource === "object") ? r.tempReturnSource : {};
    r.tempReturnSource.source = r.tempReturnSource.source || "none";
    r.tempReturnSource.gpio = (typeof r.tempReturnSource.gpio === "number") ? r.tempReturnSource.gpio : 0;
    r.tempReturnSource.rom = r.tempReturnSource.rom || r.tempReturnSource.addr || "";
    r.tempReturnSource.topic = r.tempReturnSource.topic || "";
    r.tempReturnSource.jsonKey = r.tempReturnSource.jsonKey || r.tempReturnSource.key || r.tempReturnSource.field || "";
    r.tempReturnSource.mqttIdx = Number.isFinite(Number(r.tempReturnSource.mqttIdx || r.tempReturnSource.preset)) ? Number(r.tempReturnSource.mqttIdx || r.tempReturnSource.preset) : 0;
    r.tempReturnSource.bleId = r.tempReturnSource.bleId || r.tempReturnSource.id || "";
    r.windows = Array.isArray(r.windows) ? r.windows : (Array.isArray(r.timeWindows) ? r.timeWindows : []);

    
// Ekviterm (weather compensation) + role-based I/O mapping
cfg.equitherm = cfg.equitherm || {};
const e = cfg.equitherm;

e.enabled = !!e.enabled;

// Sources
e.outdoor = e.outdoor || {};
e.flow    = e.flow || {};
e.boilerIn = e.boilerIn || {};
e.akuTop = e.akuTop || {};
e.akuMid = e.akuMid || {};
e.akuBottom = e.akuBottom || {};
for (const s of [e.outdoor, e.flow, e.boilerIn, e.akuTop, e.akuMid, e.akuBottom]) {
  s.source = s.source || "dallas"; // default DS18B20
  s.gpio   = (typeof s.gpio === "number") ? s.gpio : 0;
  s.rom    = s.rom || s.addr || "";   // addr legacy
  s.topic  = s.topic || "";
  s.jsonKey = s.jsonKey || s.key || s.field || "";
  s.mqttIdx = Number.isFinite(Number(s.mqttIdx || s.preset)) ? Number(s.mqttIdx || s.preset) : 0;
  s.bleId  = s.bleId || s.id || "";
}
if (!e.boilerIn.source || e.boilerIn.source === "none") {
  e.boilerIn.source = e.flow.source;
  e.boilerIn.gpio = e.flow.gpio;
  e.boilerIn.rom = e.flow.rom;
  e.boilerIn.topic = e.flow.topic;
  e.boilerIn.jsonKey = e.flow.jsonKey;
  e.boilerIn.mqttIdx = e.flow.mqttIdx;
  e.boilerIn.bleId = e.flow.bleId;
}

// Valve (3c)
e.valve = e.valve || {};
e.valve.master = (typeof e.valve.master === "number") ? e.valve.master : 0; // 0 = none
const legacyEqValve = Number.isFinite(Number(e.valveMaster)) ? Number(e.valveMaster) : 0;
if (!e.valve.master && legacyEqValve) e.valve.master = legacyEqValve;

// Control parameters
e.control = e.control || {};
e.control.deadbandC = (typeof e.control.deadbandC === "number") ? e.control.deadbandC : 0.5;
e.control.stepPct   = (typeof e.control.stepPct === "number") ? e.control.stepPct : 4;
// Default minimum adjustment period for 3c valve: 30 s (anti-hunt)
e.control.periodMs  = (typeof e.control.periodMs === "number") ? e.control.periodMs : 30000;
e.control.minPct    = (typeof e.control.minPct === "number") ? e.control.minPct : 0;
e.control.maxPct    = (typeof e.control.maxPct === "number") ? e.control.maxPct : 100;
e.deadbandC = (typeof e.deadbandC === "number") ? e.deadbandC : e.control.deadbandC;
e.stepPct = (typeof e.stepPct === "number") ? e.stepPct : e.control.stepPct;
e.controlPeriodMs = (typeof e.controlPeriodMs === "number") ? e.controlPeriodMs : e.control.periodMs;

// Curve
e.minFlow = (typeof e.minFlow === "number") ? e.minFlow : 25;
e.maxFlow = (typeof e.maxFlow === "number") ? e.maxFlow : 55;
e.curveOffsetC = (typeof e.curveOffsetC === "number") ? e.curveOffsetC : 0;
e.requireHeatCall = (typeof e.requireHeatCall === "boolean") ? e.requireHeatCall : false;
e.noHeatCallBehavior = e.noHeatCallBehavior || "hold";
e.akuSupportEnabled = (typeof e.akuSupportEnabled === "boolean") ? e.akuSupportEnabled : true;
e.akuMinTopC = (typeof e.akuMinTopC === "number") ? e.akuMinTopC : 42;
e.akuMinDeltaToTargetC = (typeof e.akuMinDeltaToTargetC === "number") ? e.akuMinDeltaToTargetC : 2;
e.akuMinDeltaToBoilerInC = (typeof e.akuMinDeltaToBoilerInC === "number") ? e.akuMinDeltaToBoilerInC : 3;
e.akuMinTopC_day = (typeof e.akuMinTopC_day === "number") ? e.akuMinTopC_day : e.akuMinTopC;
e.akuMinTopC_night = (typeof e.akuMinTopC_night === "number") ? e.akuMinTopC_night : 45;
e.akuMinDeltaToTargetC_day = (typeof e.akuMinDeltaToTargetC_day === "number") ? e.akuMinDeltaToTargetC_day : e.akuMinDeltaToTargetC;
e.akuMinDeltaToTargetC_night = (typeof e.akuMinDeltaToTargetC_night === "number") ? e.akuMinDeltaToTargetC_night : 3;
e.akuMinDeltaToBoilerInC_day = (typeof e.akuMinDeltaToBoilerInC_day === "number") ? e.akuMinDeltaToBoilerInC_day : e.akuMinDeltaToBoilerInC;
e.akuMinDeltaToBoilerInC_night = (typeof e.akuMinDeltaToBoilerInC_night === "number") ? e.akuMinDeltaToBoilerInC_night : 4;
e.maxPct_day = (typeof e.maxPct_day === "number") ? e.maxPct_day : e.control.maxPct;
e.maxPct_night = (typeof e.maxPct_night === "number") ? e.maxPct_night : 50;
e.akuNoSupportBehavior = e.akuNoSupportBehavior || "close";
e.maxBoilerInC = (typeof e.maxBoilerInC === "number") ? e.maxBoilerInC : 55;
e.noFlowDetectEnabled = (typeof e.noFlowDetectEnabled === "boolean") ? e.noFlowDetectEnabled : true;
e.noFlowTimeoutMs = (typeof e.noFlowTimeoutMs === "number") ? e.noFlowTimeoutMs : 180000;
e.noFlowTestPeriodMs = (typeof e.noFlowTestPeriodMs === "number") ? e.noFlowTestPeriodMs : e.noFlowTimeoutMs;
e.fallbackOutdoorC = (typeof e.fallbackOutdoorC === "number") ? e.fallbackOutdoorC : 0;

// Formula: Tflow = (20 - Tout) * slope + 20 + shift
// Defaults correspond to refs: day (-10 -> 55), (15 -> 30) => slope=1, shift=5
// and night (-10 -> 50), (15 -> 25) => slope=1, shift=0
e.slopeDay   = (typeof e.slopeDay === "number") ? e.slopeDay : 1.0;
e.shiftDay   = (typeof e.shiftDay === "number") ? e.shiftDay : 5.0;
e.slopeNight = (typeof e.slopeNight === "number") ? e.slopeNight : 1.0;
e.shiftNight = (typeof e.shiftNight === "number") ? e.shiftNight : 0.0;

// Legacy refs (fallback)
e.refs = e.refs || {};
e.refs.day = e.refs.day || {};
e.refs.night = e.refs.night || {};
const defRefs = (o, tout1, tflow1, tout2, tflow2) => {
  o.tout1  = (typeof o.tout1 === "number")  ? o.tout1  : tout1;
  o.tflow1 = (typeof o.tflow1 === "number") ? o.tflow1 : tflow1;
  o.tout2  = (typeof o.tout2 === "number")  ? o.tout2  : tout2;
  o.tflow2 = (typeof o.tflow2 === "number") ? o.tflow2 : tflow2;
};
defRefs(e.refs.day,   -10, 55,  15, 30);
defRefs(e.refs.night, -10, 50,  15, 25);

// backwards compatibility: keep old points arrays if present
e.heatPoints  = Array.isArray(e.heatPoints) ? e.heatPoints : null;
e.nightPoints = Array.isArray(e.nightPoints) ? e.nightPoints : null;

cfg.iofunc = (cfg.iofunc && typeof cfg.iofunc === "object") ? cfg.iofunc : {};
    cfg.iofunc.inputs = Array.isArray(cfg.iofunc.inputs) ? cfg.iofunc.inputs : Array.from({length: INPUT_COUNT}, () => ({ role: "none", params: {} }));
    cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : Array.from({length: RELAY_COUNT}, () => ({ role: "none", params: {} }));
    cfg.iofunc.inputs = cfg.iofunc.inputs.slice(0, INPUT_COUNT);
    cfg.iofunc.outputs = cfg.iofunc.outputs.slice(0, RELAY_COUNT);
    while (cfg.iofunc.inputs.length < INPUT_COUNT) cfg.iofunc.inputs.push({ role: "none", params: {} });
    while (cfg.iofunc.outputs.length < RELAY_COUNT) cfg.iofunc.outputs.push({ role: "none", params: {} });
    cfg.iofunc.inputs = cfg.iofunc.inputs.map(o => ({ role: String(o?.role || "none"), params: (o?.params && typeof o.params === "object") ? o.params : {} }));
    cfg.iofunc.outputs = cfg.iofunc.outputs.map(o => ({ role: String(o?.role || "none"), params: (o?.params && typeof o.params === "object") ? o.params : {} }));

    // ---------- Time/NTP + schedules (UI only, firmware can use later) ----------
    cfg.time = (cfg.time && typeof cfg.time === "object") ? cfg.time : {};
    cfg.time.ntpEnabled = !!cfg.time.ntpEnabled;
    cfg.time.server1 = String(cfg.time.server1 || "pool.ntp.org");
    cfg.time.server2 = String(cfg.time.server2 || "time.google.com");
    cfg.time.tz = String(cfg.time.tz || "Europe/Prague");
    cfg.time.syncIntervalMin = Number.isFinite(Number(cfg.time.syncIntervalMin)) ? Number(cfg.time.syncIntervalMin) : 60;

    cfg.schedules = Array.isArray(cfg.schedules) ? cfg.schedules : [];
    cfg.schedules = cfg.schedules.map(s => ({
      id: String(s?.id || (Date.now().toString(36) + Math.random().toString(36).slice(2,6))),
      enabled: s?.enabled !== false,
      name: String(s?.name || "Plán"),
      days: Array.isArray(s?.days) ? s.days.map(n=>Number(n)).filter(n=>n>=1&&n<=7) : [1,2,3,4,5,6,7],
      at: String(s?.at || "06:00"),
      kind: String(s?.kind || "set_mode"), // set_mode | set_control_mode | dhw_enable | night_mode
      value: (s && typeof s.value === "object") ? s.value : {},
    }));

    // ---------- TUV (ohřev bojleru) ----------
    cfg.tuv = (cfg.tuv && typeof cfg.tuv === "object") ? cfg.tuv : {};
    cfg.tuv.enabled = !!cfg.tuv.enabled;
    cfg.tuv.demandInput = Number.isFinite(Number(cfg.tuv.demandInput)) ? Number(cfg.tuv.demandInput) : 0; // 1..8
    cfg.tuv.requestRelay = Number.isFinite(Number(cfg.tuv.requestRelay)) ? Number(cfg.tuv.requestRelay) : 0; // 1..8
    const legacyTuvRelay = Number.isFinite(Number(cfg.tuv.relay)) ? Number(cfg.tuv.relay) : 0;
    if (!cfg.tuv.requestRelay && legacyTuvRelay) cfg.tuv.requestRelay = legacyTuvRelay;
    cfg.tuv.relay = cfg.tuv.requestRelay;
    cfg.tuv.eqValveTargetPct = Number.isFinite(Number(cfg.tuv.eqValveTargetPct)) ? Number(cfg.tuv.eqValveTargetPct) : 0;
    cfg.tuv.valveMaster = Number.isFinite(Number(cfg.tuv.valveMaster)) ? Number(cfg.tuv.valveMaster) : 0;
    cfg.tuv.valveTargetPct = Number.isFinite(Number(cfg.tuv.valveTargetPct)) ? Number(cfg.tuv.valveTargetPct) : 0;
    cfg.tuv.restoreEqValveAfter = (typeof cfg.tuv.restoreEqValveAfter === "boolean") ? cfg.tuv.restoreEqValveAfter : true;
    cfg.tuv.bypassValve = (cfg.tuv.bypassValve && typeof cfg.tuv.bypassValve === "object") ? cfg.tuv.bypassValve : {};
    cfg.tuv.bypassValve.enabled = (typeof cfg.tuv.bypassValve.enabled === "boolean") ? cfg.tuv.bypassValve.enabled : true;
    cfg.tuv.bypassValve.mode = String(cfg.tuv.bypassValve.mode || "single_relay_spdt");
    cfg.tuv.bypassValve.masterRelay = Number.isFinite(Number(cfg.tuv.bypassValve.masterRelay)) ? Number(cfg.tuv.bypassValve.masterRelay) : 0;
    cfg.tuv.bypassValve.bypassPct = Number.isFinite(Number(cfg.tuv.bypassValve.bypassPct)) ? Number(cfg.tuv.bypassValve.bypassPct) : 100;
    cfg.tuv.bypassValve.chPct = Number.isFinite(Number(cfg.tuv.bypassValve.chPct)) ? Number(cfg.tuv.bypassValve.chPct) : 100;
    cfg.tuv.bypassValve.invert = !!cfg.tuv.bypassValve.invert;

    // ---------- Boiler relays ----------
    cfg.boiler = (cfg.boiler && typeof cfg.boiler === "object") ? cfg.boiler : {};
    cfg.boiler.dhwRequestRelay = Number.isFinite(Number(cfg.boiler.dhwRequestRelay)) ? Number(cfg.boiler.dhwRequestRelay) : 0;
    cfg.boiler.nightModeRelay = Number.isFinite(Number(cfg.boiler.nightModeRelay)) ? Number(cfg.boiler.nightModeRelay) : 0;

    // ---------- AKU heater ----------
    cfg.akuHeater = (cfg.akuHeater && typeof cfg.akuHeater === "object") ? cfg.akuHeater : {};
    cfg.akuHeater.enabled = !!cfg.akuHeater.enabled;
    cfg.akuHeater.relay = Number.isFinite(Number(cfg.akuHeater.relay)) ? Number(cfg.akuHeater.relay) : 0;
    cfg.akuHeater.mode = String(cfg.akuHeater.mode || "manual");
    cfg.akuHeater.manualOn = !!cfg.akuHeater.manualOn;
    cfg.akuHeater.targetTopC = Number.isFinite(Number(cfg.akuHeater.targetTopC)) ? Number(cfg.akuHeater.targetTopC) : 50;
    cfg.akuHeater.hysteresisC = Number.isFinite(Number(cfg.akuHeater.hysteresisC)) ? Number(cfg.akuHeater.hysteresisC) : 2;
    cfg.akuHeater.maxOnMs = Number.isFinite(Number(cfg.akuHeater.maxOnMs)) ? Number(cfg.akuHeater.maxOnMs) : 2 * 60 * 60 * 1000;
    cfg.akuHeater.minOffMs = Number.isFinite(Number(cfg.akuHeater.minOffMs)) ? Number(cfg.akuHeater.minOffMs) : 10 * 60 * 1000;
    cfg.akuHeater.windows = Array.isArray(cfg.akuHeater.windows) ? cfg.akuHeater.windows : [];

    // ---------- Dallas header thermometers (GPIO0..3) ----------
    // Independent from 8 terminal DI inputs; used for DS18B20 on pin header.
    cfg.dallasNames = Array.isArray(cfg.dallasNames) ? cfg.dallasNames : ["","","",""];
    cfg.dallasAddrs = Array.isArray(cfg.dallasAddrs) ? cfg.dallasAddrs : ["","","",""];
    cfg.dallasNames = cfg.dallasNames.slice(0,4);
    cfg.dallasAddrs = cfg.dallasAddrs.slice(0,4);
    while (cfg.dallasNames.length < 4) cfg.dallasNames.push("");
    while (cfg.dallasAddrs.length < 4) cfg.dallasAddrs.push("");
    cfg.dallasNames = cfg.dallasNames.map(v => String(v || ""));
    cfg.dallasAddrs = cfg.dallasAddrs.map(v => String(v || ""));

    // ---------- Extra thermometers (MQTT/BLE) ----------
    cfg.thermometers = (cfg.thermometers && typeof cfg.thermometers === "object") ? cfg.thermometers : {};
    cfg.thermometers.mqtt = Array.isArray(cfg.thermometers.mqtt) ? cfg.thermometers.mqtt : [];
    cfg.thermometers.mqtt = cfg.thermometers.mqtt.slice(0,2);
    while (cfg.thermometers.mqtt.length < 2) cfg.thermometers.mqtt.push({ name: "", topic: "", jsonKey: "tempC" });
    cfg.thermometers.mqtt = cfg.thermometers.mqtt.map((o) => {
      o = (o && typeof o === "object") ? o : {};
      return {
        name: String(o.name || ""),
        topic: String(o.topic || ""),
        jsonKey: String(o.jsonKey || "tempC"),
      };
    });

    cfg.thermometers.ble = (cfg.thermometers.ble && typeof cfg.thermometers.ble === "object") ? cfg.thermometers.ble : {};
    cfg.thermometers.ble.name = String(cfg.thermometers.ble.name || "BLE Meteo");
    cfg.thermometers.ble.id   = String(cfg.thermometers.ble.id || "meteo.tempC");

    // ---------- System temperature roles (meaning within the heating system) ----------
    // Each role points to a temperature source (Dallas/MQTT/BLE/legacy tempX).
    // Other parts of the system can then reference the role instead of duplicating sensor selection.
    cfg.thermometers.roles = (cfg.thermometers.roles && typeof cfg.thermometers.roles === "object") ? cfg.thermometers.roles : {};
    const roles = cfg.thermometers.roles;
    const ensureRole = (key) => {
      const r = (roles[key] && typeof roles[key] === "object") ? roles[key] : {};
      r.source  = String(r.source || "none");
      r.gpio    = Number.isFinite(r.gpio) ? r.gpio : 0;
      r.rom     = String(r.rom || r.addr || "");
      r.topic   = String(r.topic || "");
      r.jsonKey = String(r.jsonKey || r.key || r.field || "");
      r.mqttIdx = Number.isFinite(Number(r.mqttIdx || r.preset)) ? Number(r.mqttIdx || r.preset) : 0;
      r.bleId   = String(r.bleId || r.id || "");
      roles[key] = r;
    };
    ["outdoor","flow","return","dhw","tankTop","tankMid","tankBottom"].forEach(ensureRole);

    // ---------- OpenTherm (kotle) ----------
    cfg.opentherm = (cfg.opentherm && typeof cfg.opentherm === "object") ? cfg.opentherm : {};
    cfg.opentherm.enabled = !!cfg.opentherm.enabled;
    cfg.opentherm.mode = String(cfg.opentherm.mode || "off"); // off | manual | equitherm
    cfg.opentherm.inPin = Number.isFinite(cfg.opentherm.inPin) ? cfg.opentherm.inPin : -1;
    cfg.opentherm.outPin = Number.isFinite(cfg.opentherm.outPin) ? cfg.opentherm.outPin : -1;
    cfg.opentherm.pollMs = Number.isFinite(cfg.opentherm.pollMs) ? cfg.opentherm.pollMs : 1000;
    cfg.opentherm.chEnable = (cfg.opentherm.chEnable !== undefined) ? !!cfg.opentherm.chEnable : true;
    cfg.opentherm.manualSetpointC = Number.isFinite(cfg.opentherm.manualSetpointC) ? cfg.opentherm.manualSetpointC : 45;

    return cfg;
};
  // expose for feature-specific modules
  window.App = window.App || {};
  window.App.ensureConfigShape = ensureConfigShape;


  // --- external module hooks (equitherm.js, iofunc.js, ...) ---
  window.App.getConfig = () => state.config;
  window.App.getStatus = () => state.status;
  window.App.setConfig = (cfg) => { state.config = cfg; ensureConfigShape(); };
  window.App.saveConfig = async (cfg) => {
    if (cfg && typeof cfg === "object") state.config = cfg;
    ensureConfigShape();
    return apiPostJson("/api/config", state.config);
  };
  window.App.onConfigLoaded = window.App.onConfigLoaded || null;
  window.App.onStatusLoaded = window.App.onStatusLoaded || null;


  const renderInputsTable = () => {
    const cfg = ensureConfigShape();
    const host = $("#tblInputs");
    host.innerHTML = "";
    host.appendChild(makeTableHead(["#", "Název", "Active level"]));
    for (let i=0;i<INPUT_COUNT;i++){
      const row = document.createElement("div");
      row.className = "trow";
      row.innerHTML = `
        <div class="col1">${i+1}</div>
        <div class="col2"><input data-in-name="${i}" value="${escapeAttr(cfg.inputNames[i]||"")}" placeholder="Např. termostat zóna ${i+1}"></div>
        <div class="col3">
          <select data-in-level="${i}">
            <option value="LOW">LOW aktivní</option>
            <option value="HIGH">HIGH aktivní</option>
          </select>
        </div>
        <div class="col4 muted">LOW aktivní = sepnutí na GND</div>
      `;
      host.appendChild(row);
      const sel = row.querySelector(`[data-in-level="${i}"]`);
      sel.value = (cfg.inputs[i]?.activeLevel || (cfg.inputActiveLevels[i] ? "HIGH" : "LOW"));
    }
  };

  const renderRelaysTable = () => {
    const cfg = ensureConfigShape();
    const host = $("#tblRelays");
    host.innerHTML = "";
    host.appendChild(makeTableHead(["#", "Název", "AUTO mapování", "Polarity"]));
    for (let r=0;r<RELAY_COUNT;r++){
      const map = cfg.relayMap[r] || { input: 0, polarity: 1 };
      const row = document.createElement("div");
      row.className = "trow";
      row.innerHTML = `
        <div class="col1">${r+1}</div>
        <div class="col2"><input data-re-name="${r}" value="${escapeAttr(cfg.relayNames[r]||"")}" placeholder="Např. ventil zóna ${r+1}"></div>
        <div class="col3">
          <select data-re-input="${r}">
            <option value="0">— (bez mapování)</option>
            ${Array.from({length: INPUT_COUNT}, (_,i)=>`<option value="${i+1}">${i+1} – ${escapeHtml(inputName(i))}</option>`).join("")}
          </select>
        </div>
        <div class="col4">
          <select data-re-pol="${r}">
            <option value="1">normal (ACTIVE=ON)</option>
            <option value="0">invert (ACTIVE=OFF)</option>
          </select>
        </div>
      `;
      host.appendChild(row);
      row.querySelector(`[data-re-input="${r}"]`).value = String(map.input||0);
      row.querySelector(`[data-re-pol="${r}"]`).value = String(map.polarity ? 1 : 0);
    }
  };

  const renderModes = () => {
    const cfg = ensureConfigShape();
    const host = $("#modesGrid");
    host.innerHTML = "";

    for (let mi=0; mi<MODE_IDS.length; mi++){
      const id = MODE_IDS[mi];
      const obj = cfg.modes.find(m => String(m.id||"").toUpperCase() === id) || { id, triggerInput: 0, relayStates: Array(RELAY_COUNT).fill(false) };

      const card = document.createElement("div");
      card.className = "modeCard";
      card.innerHTML = `
        <div class="modeHead">
          <div>
            <div class="modeTitle">${escapeHtml(modeName(mi))}</div>
            <div class="muted">${escapeHtml(id)}</div>
          </div>
          <div style="min-width: 220px">
            <label>Trigger vstup</label>
            <select data-mode-trig="${id}">
              <option value="0">— (žádný)</option>
              ${Array.from({length: INPUT_COUNT}, (_,i)=>`<option value="${i+1}">${i+1} – ${escapeHtml(inputName(i))}</option>`).join("")}
            </select>
          </div>
        </div>

        <div class="row" style="padding: 10px 0 0">
          <div class="field">
            <label>Název (modeNames)</label>
            <input data-mode-name="${mi}" value="${escapeAttr(cfg.modeNames[mi]||"")}" placeholder="Např. TUV / topení / zóna...">
          </div>
          <div class="field">
            <label>Popis (modeDescriptions)</label>
            <input data-mode-desc="${mi}" value="${escapeAttr(cfg.modeDescriptions[mi]||"")}" placeholder="Volitelné">
          </div>
        </div>

        <div class="relMini">
          ${Array.from({length: RELAY_COUNT}, (_,r)=>{
            const on = !!obj.relayStates[r];
            return `
              <div class="io" style="padding:10px">
                <div>
                  <div class="name" style="font-size:12px">${escapeHtml(relayName(r))}</div>
                  <div class="meta">Relé ${r+1}</div>
                </div>
                <div class="sw ${on ? "on":""}" data-mode-rel="${id}" data-rel="${r}" role="switch" aria-checked="${on}" tabindex="0"></div>
              </div>
            `;
          }).join("")}
        </div>
      `;
      host.appendChild(card);
      card.querySelector(`[data-mode-trig="${id}"]`).value = String(obj.triggerInput || 0);
    }
  };

  const makeTableHead = (cols) => {
    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = cols.map((c,i)=>`<div class="col${i+1}">${escapeHtml(c)}</div>`).join("");
    return head;
  };

  // ---------- actions ----------
  const setRelay = async (idx, on) => {
    await apiPostJson("/api/mode_ctrl", { action: "relay", relay: idx+1, value: !!on });
  };

  const setControlMode = async (mode) => {
    const v = String(mode).toLowerCase();
    await apiPostJson("/api/mode_ctrl", { action: "control_mode", value: v });
  };

  const setSystemMode = async (modeId) => {
    // FIX: backend očekává action="manual_mode" (ne "mode")
    // Jinak vrací 400 {"error":"unknown action"}.
    // modeId je zde např. "MODE1".."MODE5" (viz MODE_IDS).
    await apiPostJson("/api/mode_ctrl", { action: "manual_mode", value: String(modeId) });
  };

  const autoRecompute = async () => {
    await apiPostJson("/api/mode_ctrl", { action: "auto_recompute" });
  };

  const saveConfig = async () => {
    const cfg = ensureConfigShape();
    $("#cfgJson").value = prettyJson(cfg);
    await apiPostText("/api/config", $("#cfgJson").value);
  };

  const saveInputsFromForm = async () => {
    const cfg = ensureConfigShape();
    $$("[data-in-name]").forEach(el => {
      const i = Number(el.dataset.inName);
      cfg.inputNames[i] = el.value || "";
    });
    $$("[data-in-level]").forEach(el => {
      const i = Number(el.dataset.inLevel);
      const v = el.value === "HIGH" ? 1 : 0;
      cfg.inputActiveLevels[i] = v;
      cfg.inputs[i].activeLevel = (v ? "HIGH" : "LOW");
    });
    await saveConfig();
    toast("Vstupy uloženy");
    await loadAll();
  };

  const saveRelaysFromForm = async () => {
    const cfg = ensureConfigShape();
    $$("[data-re-name]").forEach(el => {
      const r = Number(el.dataset.reName);
      cfg.relayNames[r] = el.value || "";
    });
    $$("[data-re-input]").forEach(el => {
      const r = Number(el.dataset.reInput);
      cfg.relayMap[r].input = Number(el.value || 0);
    });
    $$("[data-re-pol]").forEach(el => {
      const r = Number(el.dataset.rePol);
      cfg.relayMap[r].polarity = Number(el.value === "1");
    });
    await saveConfig();
    toast("Relé uložena");
    await loadAll();
  };

  const saveModesFromForm = async () => {
    const cfg = ensureConfigShape();

    // mode names + desc
    $$("[data-mode-name]").forEach(el => {
      const mi = Number(el.dataset.modeName);
      cfg.modeNames[mi] = el.value || "";
    });
    $$("[data-mode-desc]").forEach(el => {
      const mi = Number(el.dataset.modeDesc);
      cfg.modeDescriptions[mi] = el.value || "";
    });

    // trigger + relay states
    MODE_IDS.forEach((id) => {
      const obj = cfg.modes.find(m => String(m.id||"").toUpperCase() === id);
      if (!obj) return;
      const trig = $(`[data-mode-trig="${id}"]`);
      obj.triggerInput = Number(trig?.value || 0);
    });

    // switches (mode relay toggles were edited live)
    await saveConfig();
    toast("Režimy uloženy");
    await loadAll();
  };

  const saveConfigJsonFromEditor = async () => {
    const txt = $("#cfgJson").value;
    const parsed = safeJson(txt);
    if (!parsed) { toast("Neplatný JSON", "bad"); return; }
    await apiPostText("/api/config", JSON.stringify(parsed));
    toast("Konfigurace uložena");
    await loadAll();
  };

  const saveMqtt = async () => {
    const cfg = ensureConfigShape();
    cfg.mqtt.enabled = !!$("#mqttEnabled").checked;
    cfg.mqtt.host = $("#mqttHost").value || "";
    cfg.mqtt.port = Number($("#mqttPort").value || 1883);
    cfg.mqtt.user = $("#mqttUser").value || "";
    cfg.mqtt.pass = $("#mqttPass").value || "";
    cfg.mqtt.clientId = $("#mqttClientId").value || "";
    cfg.mqtt.baseTopic = $("#mqttBaseTopic").value || "";
    cfg.mqtt.haPrefix = $("#mqttHaPrefix").value || "";
    await saveConfig();
    toast("MQTT uloženo");
    await loadAll();
  };

  const mqttDiscovery = async () => {
    await apiPostJson("/api/mode_ctrl", { action: "mqtt_discovery" });
    toast("Discovery odesláno");
  };

  const loadBle = async () => {
    state.bleStatus = await apiGet("/api/ble/status").catch(()=>null);
    state.bleConfig = await apiGet("/api/ble/config").catch(()=>null);
    state.blePaired = await apiGet("/api/ble/paired").catch(()=>null);
    renderBle();
  };

  const saveBle = async () => {
    const cfg = state.bleConfig || {};
    cfg.enabled = !!$("#bleEnabled").checked;
    cfg.deviceName = $("#bleDeviceName").value || "";
    cfg.advertise = ($("#bleAdvertise").value === "true");
    cfg.securityMode = Number($("#bleSecurityMode").value || 0);
    cfg.passkey = Number($("#blePasskey").value || 0);
    cfg.allowlistEnforced = !!$("#bleAllowlist").checked;

    cfg.meteoEnabled = !!$("#meteoEnabled").checked;
    cfg.meteoMac = $("#meteoMac").value || "";
    cfg.meteoScanMs = Number($("#meteoScanMs").value || 0);
    cfg.meteoReconnectMs = Number($("#meteoReconnectMs").value || 0);

    await apiPostText("/api/ble/config", prettyJson(cfg));
    toast("BLE uloženo");
    await loadBle();
    await loadStatus();
  };

  const startPair = async () => {
    const seconds = Number($("#pairSeconds").value || 120);
    const role = $("#pairRole").value || "";
    await apiPostJson("/api/ble/pair", { seconds, role });
    toast("Párování spuštěno");
    await loadBle();
    await loadStatus();
  };

  const stopPair = async () => {
    await apiPostJson("/api/ble/pair/stop", {});
    toast("Párování ukončeno");
    await loadBle();
    await loadStatus();
  };

  const removePaired = async (mac) => {
    await apiPostJson("/api/ble/remove", { mac });
    toast("Odebráno");
    await loadBle();
  };

  const loadRules = async () => {
    state.rules = await apiGet("/api/rules").catch(()=>null);
    state.rulesStatus = await apiGet("/api/rules/status").catch(()=>null);
    $("#rulesJson").value = state.rules ? prettyJson(state.rules) : "{\n  \"enabled\": false,\n  \"rules\": []\n}";
    $("#rulesStatus").textContent = state.rulesStatus ? prettyJson(state.rulesStatus) : "—";
      renderRulesTable();
  };

  const saveRules = async () => {
    // sync editor -> JSON
    if ($("#rulesEnabled")) {
      const r = rulesEnsureShape();
      r.enabled = $("#rulesEnabled").checked;
      r.defaultOffControlled = $("#rulesDefaultOff").checked;
      rulesToTextarea();
    }
    const txt = $("#rulesJson").value;
    const parsed = safeJson(txt);
    if (!parsed) { toast("Neplatný JSON", "bad"); return; }
    await apiPostText("/api/rules", JSON.stringify(parsed));
    toast("Rules uloženy");
    await loadRules();
    await loadStatus();
  };
  // ---------- rules editor ----------
  const rulesEnsureShape = () => {
    if (!state.rules || typeof state.rules !== "object") state.rules = { enabled:false, defaultOffControlled:true, rules:[] };
    if (!Array.isArray(state.rules.rules)) state.rules.rules = [];
    if (state.rules.defaultOffControlled == null) state.rules.defaultOffControlled = true;
    return state.rules;
  };

  const renderRulesTable = () => {
    const root = $("#rulesTable");
    if (!root) return;
    const r = rulesEnsureShape();
    $("#rulesEnabled").checked = !!r.enabled;
    $("#rulesDefaultOff").checked = !!r.defaultOffControlled;

    const rows = r.rules || [];
    if (!rows.length){
      root.innerHTML = `<div class="muted" style="padding:12px 0">Zatím žádná pravidla.</div>`;
      return;
    }

    const head = `
      <div class="trow head">
        <div>ID</div><div>Název</div><div>WHEN/THEN</div><div>Akce</div>
      </div>`;
    const body = rows.map((it, idx) => {
      const id = it.id ?? (idx+1);
      const name = escapeHtml(it.name || `Rule ${id}`);
      const en = it.enabled === false ? "OFF" : "ON";
      const wItems = it.when?.items || [];
      const aItems = it.then || [];

      const whenTxt = (wItems.length)
        ? wItems.map(w => {
            if ((w.type || "input") === "time") return `time ${w.from||"00:00"}–${w.to||"00:00"}`;
            if ((w.type || "input") === "mqtt") return `mqtt ${w.topic||"?"}=${w.value||""}`;
            return `in${w.input||"?"}=${w.state||"ACTIVE"}`;
          }).join(` ${it.when?.op || "AND"} `)
        : "—";

      const thenTxt = (aItems.length)
        ? aItems.map(a => {
            const t = a.type || "relay_set";
            if (t === "mqtt_publish") return `mqtt_pub ${a.topic||"?"}`;
            if (t === "relay_pulse") return `pulse R${a.relay||"?"} ${a.ms||500}ms`;
            return `R${a.relay||"?"}=${a.value ? "ON" : "OFF"}`;
          }).join(", ")
        : "—";
      return `
        <div class="trow">
          <div>${id}<div class="muted">${en}</div></div>
          <div>${name}<div class="muted">${escapeHtml(it.desc||"")}</div></div>
          <div class="muted">${escapeHtml(whenTxt)} • ${escapeHtml(thenTxt)}</div>
          <div>
            <button class="btn ghost" data-rule-edit="${id}">Upravit</button>
          </div>
        </div>`;
    }).join("");

    root.innerHTML = head + body;
  };

  const rulesToTextarea = () => {
    if ($("#rulesJson")) $("#rulesJson").value = prettyJson(rulesEnsureShape());
  };

  const textareaToRules = () => {
    const parsed = safeJson($("#rulesJson")?.value || "");
    if (!parsed) return false;
    state.rules = parsed;
    return true;
  };

  const inputOptionsHtml = () =>
    Array.from({ length: INPUT_COUNT }, (_, i) => `<option value="${i + 1}">Vstup ${i + 1}</option>`).join("");

  const relayOptionsHtml = () =>
    Array.from({ length: RELAY_COUNT }, (_, i) => `<option value="${i + 1}">Relé ${i + 1}</option>`).join("");

  const applyCondRowVisibility = (row) => {
    const type = row.querySelector(".condType")?.value || "input";
    row.querySelectorAll("[data-cond='input']").forEach(el => el.style.display = (type === "input") ? "" : "none");
    row.querySelectorAll("[data-cond='time']").forEach(el => el.style.display = (type === "time") ? "" : "none");
    row.querySelectorAll("[data-cond='mqtt']").forEach(el => el.style.display = (type === "mqtt") ? "" : "none");
  };

  const createCondRow = (cond = {}) => {
    const type = (cond.type || "input");
    const el = document.createElement("div");
    el.className = "row";
    el.style.padding = "0";
    el.style.gap = "10px";
    el.innerHTML = `
      <div class="field">
        <label>Typ</label>
        <select class="condType">
          <option value="input">Input</option>
          <option value="time">Time</option>
          <option value="mqtt">MQTT</option>
        </select>
      </div>

      <div class="field" data-cond="input">
        <label>Vstup</label>
        <select class="condInput">${inputOptionsHtml()}</select>
      </div>
      <div class="field" data-cond="input">
        <label>Stav</label>
        <select class="condState">
          <option value="ACTIVE">ACTIVE</option>
          <option value="INACTIVE">INACTIVE</option>
        </select>
      </div>

      <div class="field" data-cond="time">
        <label>Od</label>
        <input class="condFrom" type="time" />
      </div>
      <div class="field" data-cond="time">
        <label>Do</label>
        <input class="condTo" type="time" />
      </div>

      <div class="field" data-cond="mqtt">
        <label>Topic</label>
        <input class="condTopic" placeholder="např. home/xxx/state" />
      </div>
      <div class="field" data-cond="mqtt">
        <label>Hodnota</label>
        <input class="condValue" placeholder="např. ON" />
      </div>

      <div class="field" style="display:flex;align-items:flex-end">
        <button class="btn bad ghost btnRemoveCond" title="Odebrat">×</button>
      </div>
    `;

    el.querySelector(".condType").value = type;
    el.querySelector(".condInput").value = String(cond.input ?? 1);
    el.querySelector(".condState").value = String(cond.state ?? "ACTIVE");
    el.querySelector(".condFrom").value = String(cond.from ?? "00:00");
    el.querySelector(".condTo").value = String(cond.to ?? "00:00");
    el.querySelector(".condTopic").value = String(cond.topic ?? "");
    el.querySelector(".condValue").value = String(cond.value ?? "");

    el.querySelector(".condType").addEventListener("change", () => applyCondRowVisibility(el));
    el.querySelector(".btnRemoveCond").addEventListener("click", (e) => { e.preventDefault(); el.remove(); });

    applyCondRowVisibility(el);
    return el;
  };

  const applyActionRowVisibility = (row) => {
    const type = row.querySelector(".actType")?.value || "relay_set";
    row.querySelectorAll("[data-act='relay_set']").forEach(el => el.style.display = (type === "relay_set") ? "" : "none");
    row.querySelectorAll("[data-act='relay_pulse']").forEach(el => el.style.display = (type === "relay_pulse") ? "" : "none");
    row.querySelectorAll("[data-act='mqtt_publish']").forEach(el => el.style.display = (type === "mqtt_publish") ? "" : "none");
  };

  const createActionRow = (a = {}) => {
    const type = (a.type || "relay_set");
    const el = document.createElement("div");
    el.className = "row";
    el.style.padding = "0";
    el.style.gap = "10px";
    el.innerHTML = `
      <div class="field">
        <label>Typ</label>
        <select class="actType">
          <option value="relay_set">Relay set</option>
          <option value="relay_pulse">Relay pulse (JSON)</option>
          <option value="mqtt_publish">MQTT publish (JSON)</option>
        </select>
      </div>

      <div class="field" data-act="relay_set">
        <label>Relé</label>
        <select class="actRelay">${relayOptionsHtml()}</select>
      </div>
      <div class="field" data-act="relay_set">
        <label>Hodnota</label>
        <select class="actValue">
          <option value="true">ON</option>
          <option value="false">OFF</option>
        </select>
      </div>

      <div class="field" data-act="relay_pulse">
        <label>Relé</label>
        <select class="actPulseRelay">${relayOptionsHtml()}</select>
      </div>
      <div class="field" data-act="relay_pulse">
        <label>ms</label>
        <input class="actPulseMs" type="number" min="1" step="10" />
      </div>

      <div class="field" data-act="mqtt_publish">
        <label>Topic</label>
        <input class="actMqttTopic" />
      </div>
      <div class="field" data-act="mqtt_publish">
        <label>Payload</label>
        <input class="actMqttPayload" />
      </div>

      <div class="field" style="display:flex;align-items:flex-end">
        <button class="btn bad ghost btnRemoveAct" title="Odebrat">×</button>
      </div>
    `;

    el.querySelector(".actType").value = type;
    el.querySelector(".actRelay").value = String(a.relay ?? 1);
    el.querySelector(".actValue").value = (a.value ? "true" : "false");
    el.querySelector(".actPulseRelay").value = String(a.relay ?? 1);
    el.querySelector(".actPulseMs").value = String(a.ms ?? 500);
    el.querySelector(".actMqttTopic").value = String(a.topic ?? "");
    el.querySelector(".actMqttPayload").value = String(a.payload ?? "");

    el.querySelector(".actType").addEventListener("change", () => applyActionRowVisibility(el));
    el.querySelector(".btnRemoveAct").addEventListener("click", (e) => { e.preventDefault(); el.remove(); });

    applyActionRowVisibility(el);
    return el;
  };

  const openRuleModal = (ruleId) => {
    const r = rulesEnsureShape();
    const modal = $("#ruleModal");
    if (!modal) return;

    const idx = r.rules.findIndex(x => String(x.id) === String(ruleId));
    const isNew = idx < 0;
    const it = isNew ? {
      id: (Date.now() & 0xffffffff) >>> 0,
      enabled: true,
      priority: 50,
      stopOnMatch: false,
      debounceMs: 0,
      minOnMs: 0,
      minOffMs: 0,
      name: "",
      desc: "",
      when: { op: "AND", items: [ { type:"input", input: 1, state: "ACTIVE" } ] },
      then: [ { type:"relay_set", relay: 1, value: true } ]
    } : JSON.parse(JSON.stringify(r.rules[idx]));

    modal.dataset.ruleId = String(it.id);
    modal.dataset.isNew = isNew ? "1" : "0";
    $("#ruleModalTitle").textContent = isNew ? "Nové pravidlo" : `Upravit pravidlo #${it.id}`;

    $("#ruleName").value = it.name || "";
    $("#ruleDesc").value = it.desc || "";
    $("#ruleEnabled").value = (it.enabled === false) ? "false" : "true";
    $("#rulePriority").value = String(it.priority ?? 50);
    $("#ruleStopOnMatch").value = (it.stopOnMatch ? "true" : "false");
    $("#ruleDebounceMs").value = String(it.debounceMs ?? 0);
    $("#ruleMinOnMs").value = String(it.minOnMs ?? 0);
    $("#ruleMinOffMs").value = String(it.minOffMs ?? 0);

    $("#ruleWhenOp").value = String(it.when?.op ?? "AND");
    const wl = $("#ruleWhenList");
    if (wl) {
      wl.innerHTML = "";
      const items = (it.when?.items && it.when.items.length) ? it.when.items : [ { type:"input", input: 1, state:"ACTIVE" } ];
      items.forEach(c => wl.appendChild(createCondRow(c)));
    }

    const tl = $("#ruleThenList");
    if (tl) {
      tl.innerHTML = "";
      const acts = (it.then && it.then.length) ? it.then : [ { type:"relay_set", relay: 1, value: true } ];
      acts.forEach(a => tl.appendChild(createActionRow(a)));
    }

    $("#btnRuleDelete").classList.toggle("hidden", isNew);

    modal.classList.remove("hidden");
  };

  const closeRuleModal = () => {
    const modal = $("#ruleModal");
    if (modal) modal.classList.add("hidden");
  };

  const saveRuleFromModal = () => {
    const modal = $("#ruleModal");
    const r = rulesEnsureShape();
    if (!modal) return;

    const id = Number(modal.dataset.ruleId || 0) || ((Date.now() & 0xffffffff) >>> 0);
    const isNew = modal.dataset.isNew === "1";
    const whenItems = [];
    $("#ruleWhenList")?.querySelectorAll(".row").forEach(row => {
      const type = row.querySelector(".condType")?.value || "input";
      if (type === "input") {
        whenItems.push({
          type: "input",
          input: Number(row.querySelector(".condInput")?.value || 1),
          state: String(row.querySelector(".condState")?.value || "ACTIVE"),
        });
      } else if (type === "time") {
        whenItems.push({
          type: "time",
          from: String(row.querySelector(".condFrom")?.value || "00:00"),
          to: String(row.querySelector(".condTo")?.value || "00:00"),
        });
      } else if (type === "mqtt") {
        whenItems.push({
          type: "mqtt",
          topic: String(row.querySelector(".condTopic")?.value || ""),
          value: String(row.querySelector(".condValue")?.value || ""),
        });
      }
    });

    const thenActs = [];
    $("#ruleThenList")?.querySelectorAll(".row").forEach(row => {
      const type = row.querySelector(".actType")?.value || "relay_set";
      if (type === "relay_set") {
        thenActs.push({
          type: "relay_set",
          relay: Number(row.querySelector(".actRelay")?.value || 1),
          value: (row.querySelector(".actValue")?.value === "true"),
        });
      } else if (type === "relay_pulse") {
        thenActs.push({
          type: "relay_pulse",
          relay: Number(row.querySelector(".actPulseRelay")?.value || 1),
          ms: Number(row.querySelector(".actPulseMs")?.value || 500),
        });
      } else if (type === "mqtt_publish") {
        thenActs.push({
          type: "mqtt_publish",
          topic: String(row.querySelector(".actMqttTopic")?.value || ""),
          payload: String(row.querySelector(".actMqttPayload")?.value || ""),
        });
      }
    });

    if (!whenItems.length) whenItems.push({ type:"input", input: 1, state:"ACTIVE" });
    if (!thenActs.length) thenActs.push({ type:"relay_set", relay: 1, value: true });

    const it = {
      id,
      name: $("#ruleName").value || "",
      desc: $("#ruleDesc").value || "",
      enabled: $("#ruleEnabled").value === "true",
      priority: Number($("#rulePriority").value || 50),
      stopOnMatch: $("#ruleStopOnMatch").value === "true",
      debounceMs: Number($("#ruleDebounceMs").value || 0),
      minOnMs: Number($("#ruleMinOnMs").value || 0),
      minOffMs: Number($("#ruleMinOffMs").value || 0),
      when: { op: $("#ruleWhenOp").value || "AND", items: whenItems },
      then: thenActs,
    };

    if (isNew){
      r.rules.push(it);
    } else {
      const idx = r.rules.findIndex(x => String(x.id) === String(id));
      if (idx >= 0) r.rules[idx] = it;
      else r.rules.push(it);
    }

    rulesToTextarea();
    renderRulesTable();
    closeRuleModal();
  };

  const deleteRuleFromModal = () => {
    const modal = $("#ruleModal");
    const r = rulesEnsureShape();
    if (!modal) return;
    const id = Number(modal.dataset.ruleId || 0);
    r.rules = r.rules.filter(x => String(x.id) !== String(id));
    rulesToTextarea();
    renderRulesTable();
    closeRuleModal();
  };


  const loadFiles = async () => {
    state.files = await apiGet("/api/fs/list").catch(()=>null);
    renderFiles();
  };

  const deleteFile = async (path) => {
    const url = "/api/fs/delete?path=" + encodeURIComponent(path);
    const r = await fetch(url, { method: "POST" });
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    toast("Smazáno");
    await loadFiles();
  };

  const uploadFile = async (file, targetPath) => {
    const fd = new FormData();
    // backend typicky bere pole "file"
    fd.append("file", file, file.name);
    if (targetPath) fd.append("path", targetPath);

    const r = await fetch("/api/fs/upload", { method: "POST", body: fd });
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    toast("Nahráno");
    await loadFiles();
  };

  const reboot = async () => {
    await apiPostJson("/api/reboot", { reboot: true });
    toast("Restart...");
  };

  // ---------- render: BLE ----------
  const renderBle = () => {
    const cfg = state.bleConfig || {};
    const st = state.bleStatus || {};
    const paired = state.blePaired?.devices || [];

    $("#bleEnabled").checked = !!cfg.enabled;
    const pairCard = $("#blePairCard");
    if (pairCard) pairCard.classList.toggle("hidden", !cfg.enabled);
    $("#bleDeviceName").value = cfg.deviceName || "";
    $("#bleAdvertise").value = (cfg.advertise ? "true" : "false");
    $("#bleSecurityMode").value = String(cfg.securityMode ?? 0);
    $("#blePasskey").value = String(cfg.passkey ?? "");
    $("#bleAllowlist").checked = !!cfg.allowlistEnforced;

    $("#meteoEnabled").checked = !!cfg.meteoEnabled;
    $("#meteoMac").value = cfg.meteoMac || "";
    $("#meteoScanMs").value = String(cfg.meteoScanMs ?? "");
    $("#meteoReconnectMs").value = String(cfg.meteoReconnectMs ?? "");

    // status
    $("#bleState").textContent = st.enabled ? "enabled" : "disabled";
    $("#blePairState").textContent = st.pairingWindow ? `ANO (${st.pairingRemainingSec ?? 0}s)` : "ne";
    if (!st.meteoEnabled) $("#bleMeteoState").textContent = "OFF";
    else if (st.meteoConnected && st.meteoFix) {
      const m = st.meteo || {};
      $("#bleMeteoState").textContent = `${m.tempC ?? "?"}°C / ${m.hum ?? "?"}%`;
    } else if (st.meteoConnected) $("#bleMeteoState").textContent = "connected (bez dat)";
    else $("#bleMeteoState").textContent = "ne";

    const host = $("#tblBlePaired");
    host.innerHTML = "";
    host.appendChild(makeTableHead(["MAC", "Name", "Role", "Added", "Akce"]));
    for (const d of paired){
      const row = document.createElement("div");
      row.className = "trow";
      row.style.gridTemplateColumns = "1.2fr 1fr .8fr .8fr 120px";
      row.innerHTML = `
        <div class="col1">${escapeHtml(d.mac||"")}</div>
        <div class="col2">${escapeHtml(d.name||"")}</div>
        <div class="col3">${escapeHtml(d.role||"")}</div>
        <div class="col4">${escapeHtml(d.addedAt||"")}</div>
        <div class="col5"><button class="btn ghost" data-del-mac="${escapeAttr(d.mac||"")}">Odebrat</button></div>
      `;
      host.appendChild(row);
    }
    $$("[data-del-mac]").forEach(b => b.addEventListener("click", () => removePaired(b.dataset.delMac).catch(e=>toast(e.message,"bad"))));
  };

  // ---------- render: files ----------
  const renderFiles = () => {
    const host = $("#tblFiles");
    host.innerHTML = "";
    host.appendChild(makeTableHead(["Soubor", "Velikost", "Akce"]));

    const arr = Array.isArray(state.files) ? state.files : [];
    for (const f of arr){
      const row = document.createElement("div");
      row.className = "trow";
      row.style.gridTemplateColumns = "1.2fr .5fr 140px";
      row.innerHTML = `
        <div class="col1">${escapeHtml(f.name||"")}</div>
        <div class="col2 muted">${fmtBytes(f.size||0)}</div>
        <div class="col3"><button class="btn ghost" data-del-path="${escapeAttr(f.name||"")}">Smazat</button></div>
      `;
      host.appendChild(row);
    }
    $$("[data-del-path]").forEach(b => b.addEventListener("click", () => deleteFile(b.dataset.delPath).catch(e=>toast(e.message,"bad"))));
  };

  const fmtBytes = (n) => {
    n = Number(n||0);
    if (n < 1024) return `${n} B`;
    if (n < 1024*1024) return `${(n/1024).toFixed(1)} KB`;
    return `${(n/(1024*1024)).toFixed(2)} MB`;
  };

  // ---------- status load / app init ----------
  const loadStatus = async () => {
    state.status = await apiGet("/api/status").catch(()=>null);
    renderTop();
    renderIO();

    const st = state.status || {};
    // status box on mqtt page
    $("#stWifi").textContent = (st.wifiConnected ?? st?.wifi?.connected) ? "online" : "offline";
    $("#stMqtt").textContent = (st.mqttConnected ?? st?.mqtt?.connected) ? "connected" : "disconnected";
    $("#stIp").textContent = st.ip || "—";
    $("#stUptime").textContent = fmtMs(st.uptimeMs ?? null);

    // control mode selectors
    const nowUi = Date.now();
    const cm = (st.controlMode || "auto").toLowerCase();
    const cmVal = cm.includes("manual") ? "manual" : "auto";
    const selCM = $("#selControlMode");
    if (nowUi > (state.ui?.lock?.controlUntil || 0) && document.activeElement !== selCM) {
      selCM.value = cmVal;
    }
    $("#kvControlMode").textContent = selCM.value;

    const sm = (st.systemMode || st.mode || MODE_IDS[0]).toUpperCase();
    const selSM = $("#selSystemMode");
    selSM.disabled = (selCM.value === "auto");
    if (nowUi > (state.ui?.lock?.systemUntil || 0) && document.activeElement !== selSM) {
      selSM.value = MODE_IDS.includes(sm) ? sm : MODE_IDS[0];
    }

    // keep config editor synced if open
    if (state.config) $("#cfgJson").value = prettyJson(state.config);

    if (typeof window.App?.onStatusLoaded === "function") {
      try { window.App.onStatusLoaded(state.status); } catch(e) {}
    }
  };

  const loadConfig = async () => {
    const cfg = await apiGet("/api/config").catch(()=>null);
    state.config = (typeof cfg === "string") ? safeJson(cfg) : cfg;
    ensureConfigShape();
    fillModeSelect();
    renderInputsTable();
    renderRelaysTable();
    renderModes();
    $("#cfgJson").value = prettyJson(state.config);

    // mqtt fields
    const m = state.config.mqtt || {};
    $("#mqttEnabled").checked = !!m.enabled;
    $("#mqttHost").value = m.host || "";
    $("#mqttPort").value = String(m.port ?? 1883);
    $("#mqttUser").value = m.user || "";
    $("#mqttPass").value = m.pass || "";
    $("#mqttClientId").value = m.clientId || "";
    $("#mqttBaseTopic").value = m.baseTopic || "";
    $("#mqttHaPrefix").value = m.haPrefix || "";

    if (typeof window.App?.onConfigLoaded === "function") {
      try { window.App.onConfigLoaded(state.config); } catch(e) {}
    }
  };

  const loadAll = async () => {
    await loadConfig();
    await loadStatus();
    await loadBle();
    await loadRules();
    await loadFiles();
  };

  // ---------- DOM events ----------
  const wireEvents = () => {
    // nav
    $$(".navItem").forEach(b => b.addEventListener("click", () => showPage(String(b.dataset.page||"dash"), String(b.dataset.ctab||""))));

    // theme
    const themeKey = "heatui_theme";
    const applyTheme = (t) => {
      document.documentElement.classList.toggle("light", t === "light");
    };
    const current = localStorage.getItem(themeKey) || "dark";
    applyTheme(current);
    $("#btnTheme").addEventListener("click", () => {
      const next = document.documentElement.classList.contains("light") ? "dark" : "light";
      localStorage.setItem(themeKey, next);
      applyTheme(next);
    });

    // config tabs
    $$(".tab").forEach(b => b.addEventListener("click", () => setCfgTab(b.dataset.ctab)));

    
    // rules tabs
    $$(".rtab").forEach(b => b.addEventListener("click", () => {
      $$(".rtab").forEach(x => x.classList.toggle("active", x === b));
      $("#rtab-editor").classList.toggle("active", b.dataset.rtab === "editor");
      $("#rtab-json").classList.toggle("active", b.dataset.rtab === "json");
    }));

    // rules editor
    $("#btnAddRule")?.addEventListener("click", () => openRuleModal("NEW"));
    $("#rulesTable")?.addEventListener("click", (e) => {
      const btn = e.target.closest("[data-rule-edit]");
      if (!btn) return;
      openRuleModal(btn.dataset.ruleEdit);
    });
    $("#rulesEnabled")?.addEventListener("change", () => { const r = rulesEnsureShape(); r.enabled = $("#rulesEnabled").checked; rulesToTextarea(); });
    $("#rulesDefaultOff")?.addEventListener("change", () => { const r = rulesEnsureShape(); r.defaultOffControlled = $("#rulesDefaultOff").checked; rulesToTextarea(); });

    // modal events
    $("#btnRuleClose")?.addEventListener("click", closeRuleModal);
    $("#btnRuleSave")?.addEventListener("click", (e) => { e.preventDefault(); saveRuleFromModal(); });
    $("#btnRuleDelete")?.addEventListener("click", (e) => { e.preventDefault(); deleteRuleFromModal(); });
    $("#btnRuleAddCond")?.addEventListener("click", (e) => {
      e.preventDefault();
      $("#ruleWhenList")?.appendChild(createCondRow({ type:"input", input:1, state:"ACTIVE" }));
    });
    $("#btnRuleAddAction")?.addEventListener("click", (e) => {
      e.preventDefault();
      $("#ruleThenList")?.appendChild(createActionRow({ type:"relay_set", relay:1, value:true }));
    });
    $("#ruleModal")?.addEventListener("click", (e) => {
      if (e.target && e.target.dataset && e.target.dataset.close) closeRuleModal();
    });
// relay toggles (dashboard)
    $("#relayGrid").addEventListener("click", async (e) => {
      const sw = e.target.closest(".sw[data-relay]");
      if (!sw) return;
      const idx = Number(sw.dataset.relay);
      const on = !sw.classList.contains("on");
      try{
        await setRelay(idx, on);
        toast(`Relé ${idx+1} → ${on ? "ON":"OFF"}`);
        await loadStatus();
      }catch(err){ toast("Chyba: " + err.message, "bad"); }
    });

    // mode relay toggles (config -> modes)
    $("#modesGrid").addEventListener("click", (e) => {
      const sw = e.target.closest(".sw[data-mode-rel]");
      if (!sw) return;
      const id = sw.dataset.modeRel;
      const r = Number(sw.dataset.rel);
      const cfg = ensureConfigShape();
      const obj = cfg.modes.find(m => String(m.id||"").toUpperCase() === String(id).toUpperCase());
      if (!obj) return;
      obj.relayStates[r] = !obj.relayStates[r];
      sw.classList.toggle("on", obj.relayStates[r]);
    });

    // apply control/mode
    $("#btnApplyMode").addEventListener("click", async () => {
      try{
        const cm = ($("#selControlMode").value || "auto").toLowerCase();
          await setControlMode(cm);

          // FIX: V AUTO se NESMÍ posílat manual_mode (jinak to "shodí" zpět do MANUAL).
          if (cm === "manual") {
            await setSystemMode($("#selSystemMode").value);
          }
        toast("Uloženo");
        await loadStatus();
      }catch(err){ toast("Chyba: " + err.message, "bad"); }
    });

    
    // auto-apply control mode / active mode (prevents "bounce" back to MANUAL)
    const lockUI = (which, ms=1500) => {
      const t = Date.now() + ms;
      if (!state.ui) state.ui = { lock:{}, timers:{} };
      if (!state.ui.lock) state.ui.lock = {};
      if (which === "control") state.ui.lock.controlUntil = t;
      if (which === "system") state.ui.lock.systemUntil = t;
    };

    const scheduleAutoApply = () => {
      if (state.ui.timers.autoApply) clearTimeout(state.ui.timers.autoApply);
      state.ui.timers.autoApply = setTimeout(async () => {
        try{
          lockUI("control"); lockUI("system");
          const cm = ($("#selControlMode").value || "auto").toLowerCase();
          await setControlMode(cm);

          // FIX: v AUTO nesmí UI posílat manual_mode, jinak to shodí zpět do MANUAL
          if (cm === "manual") {
            await setSystemMode($("#selSystemMode").value);
          }

          toast("Režim uložen");
          await loadStatus();
        }catch(err){ toast("Chyba: " + err.message, "bad"); }
      }, 220);
    };

    $("#selControlMode").addEventListener("change", () => { lockUI("control", 2500); scheduleAutoApply(); });
    $("#selSystemMode").addEventListener("change", () => { lockUI("system", 2500); scheduleAutoApply(); });
$("#btnAutoRecompute").addEventListener("click", async () => {
      try{ await autoRecompute(); toast("AUTO přepočteno"); await loadStatus(); }
      catch(err){ toast("Chyba: " + err.message, "bad"); }
    });

    // save config (forms)
    $("#btnSaveInputs").addEventListener("click", () => saveInputsFromForm().catch(e=>toast(e.message,"bad")));
    $("#btnSaveRelays").addEventListener("click", () => saveRelaysFromForm().catch(e=>toast(e.message,"bad")));
    $("#btnSaveModes").addEventListener("click", () => saveModesFromForm().catch(e=>toast(e.message,"bad")));

    // cfg json
    $("#btnFmtCfg").addEventListener("click", () => {
      const obj = safeJson($("#cfgJson").value);
      if (!obj) return toast("Neplatný JSON", "bad");
      $("#cfgJson").value = prettyJson(obj);
    });
    $("#btnSaveCfgJson").addEventListener("click", () => saveConfigJsonFromEditor().catch(e=>toast(e.message,"bad")));

    // mqtt
    $("#btnSaveMqtt").addEventListener("click", () => saveMqtt().catch(e=>toast(e.message,"bad")));
    $("#btnMqttDiscovery").addEventListener("click", () => mqttDiscovery().catch(e=>toast(e.message,"bad")));

    // ble
    $("#btnSaveBle").addEventListener("click", () => saveBle().catch(e=>toast(e.message,"bad")));
    $("#btnStartPair").addEventListener("click", () => startPair().catch(e=>toast(e.message,"bad")));
    $("#btnStopPair").addEventListener("click", () => stopPair().catch(e=>toast(e.message,"bad")));

    // rules
    $("#btnFmtRules").addEventListener("click", () => {
      const obj = safeJson($("#rulesJson").value);
      if (!obj) return toast("Neplatný JSON", "bad");
      $("#rulesJson").value = prettyJson(obj);
    });
    $("#btnSaveRules").addEventListener("click", () => saveRules().catch(e=>toast(e.message,"bad")));
    $("#btnReloadRules").addEventListener("click", () => loadRules().catch(e=>toast(e.message,"bad")));

    // files
    $("#btnRefreshFiles").addEventListener("click", () => loadFiles().catch(e=>toast(e.message,"bad")));
    $("#uploadForm").addEventListener("submit", async (e) => {
      e.preventDefault();
      const file = $("#filePick").files?.[0];
      if (!file) return toast("Vyber soubor", "bad");
      const path = $("#filePath").value || "";
      try{ await uploadFile(file, path); }
      catch(err){ toast("Upload chyba: " + err.message, "bad"); }
    });

    // reboot
    $("#btnReboot").addEventListener("click", async () => {
      if (!confirm("Opravdu restartovat zařízení?")) return;
      try{ await reboot(); }
      catch(err){ toast("Chyba: " + err.message, "bad"); }
    });
  };

  // ---------- tiny html escaping ----------
  function escapeHtml(s){
    return String(s ?? "").replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  }
  function escapeAttr(s){ return escapeHtml(s).replace(/\n/g, " "); }

  // ---------- start ----------
  const init = async () => {
    wireEvents();
    const startRaw = (location.hash || "").replace("#","") || "dash";
    const parts = startRaw.split("/");
    const page = String(parts[0] || "dash").trim() || "dash";
    const sub  = String(parts[1] || "").trim();

    // default config tab
    if (page === "config" && !sub) setCfgTab("io");
    showPage(page, sub);
    try{
      await loadAll();
      toast("Připraveno");
    }catch(err){
      toast("Načtení selhalo: " + err.message, "bad");
    }
    // polling (non aggressive)
    setInterval(() => loadStatus().catch(()=>{}), 1200);
    setInterval(() => loadBle().catch(()=>{}), 2500);
  };

  window.addEventListener("load", init);
})();
