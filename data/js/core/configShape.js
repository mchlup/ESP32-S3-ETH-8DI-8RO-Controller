(function () {
  const MODE_IDS = ["MODE1", "MODE2", "MODE3", "MODE4", "MODE5"];
  const MODE_DEFAULT_NAMES = ["MODE1", "MODE2", "MODE3", "MODE4", "MODE5"];
  const RELAY_COUNT = 8;
  const INPUT_COUNT = 8;

  const ensureConfigShape = (inputCfg) => {
    const cfg = inputCfg || window.Core?.store?.getConfig?.() || {};
    if (typeof cfg !== "object" || cfg === null) return {};

    cfg.inputNames = Array.isArray(cfg.inputNames) ? cfg.inputNames : Array(INPUT_COUNT).fill("");
    cfg.relayNames = Array.isArray(cfg.relayNames) ? cfg.relayNames : Array(RELAY_COUNT).fill("");
    cfg.inputActiveLevels = Array.isArray(cfg.inputActiveLevels) ? cfg.inputActiveLevels : Array(INPUT_COUNT).fill(0);

    cfg.inputs = Array.isArray(cfg.inputs) ? cfg.inputs : Array.from({ length: INPUT_COUNT }, () => ({}));
    for (let i = 0; i < INPUT_COUNT; i++) {
      if (typeof cfg.inputs[i] !== "object" || !cfg.inputs[i]) cfg.inputs[i] = {};
      if (!("activeLevel" in cfg.inputs[i])) cfg.inputs[i].activeLevel = (cfg.inputActiveLevels[i] ? "HIGH" : "LOW");
    }

    cfg.relayMap = Array.isArray(cfg.relayMap) ? cfg.relayMap : Array.from({ length: RELAY_COUNT }, () => ({ input: 0, polarity: 1 }));
    for (let r = 0; r < RELAY_COUNT; r++) {
      const o = cfg.relayMap[r] || {};
      cfg.relayMap[r] = { input: Number(o.input || 0), polarity: (o.polarity == null ? 1 : Number(!!o.polarity)) };
    }

    cfg.modeNames = Array.isArray(cfg.modeNames) ? cfg.modeNames : MODE_DEFAULT_NAMES.slice();
    cfg.modeDescriptions = Array.isArray(cfg.modeDescriptions) ? cfg.modeDescriptions : ["", "", "", "", ""];

    cfg.modes = Array.isArray(cfg.modes) ? cfg.modes : [];
    for (let mi = 0; mi < MODE_IDS.length; mi++) {
      const id = MODE_IDS[mi];
      const existing = cfg.modes.find((m) => String(m?.id || "").toUpperCase() === id);
      if (!existing) {
        cfg.modes.push({ id, triggerInput: 0, relayStates: Array(RELAY_COUNT).fill(false) });
      } else {
        existing.triggerInput = Number(existing.triggerInput || 0);
        if (!Array.isArray(existing.relayStates)) existing.relayStates = Array(RELAY_COUNT).fill(false);
        existing.relayStates = existing.relayStates.map((v) => !!v).slice(0, RELAY_COUNT);
        while (existing.relayStates.length < RELAY_COUNT) existing.relayStates.push(false);
      }
    }

    cfg.mqtt = (cfg.mqtt && typeof cfg.mqtt === "object") ? cfg.mqtt : {};

    cfg.system = (cfg.system && typeof cfg.system === "object") ? cfg.system : {};
    cfg.system.profile = cfg.system.profile || "standard";
    cfg.system.nightModeSource = cfg.system.nightModeSource || "heat_call";
    cfg.system.nightModeManual = !!cfg.system.nightModeManual;

    cfg.sensors = (cfg.sensors && typeof cfg.sensors === "object") ? cfg.sensors : {};
    cfg.sensors.outdoor = (cfg.sensors.outdoor && typeof cfg.sensors.outdoor === "object") ? cfg.sensors.outdoor : {};
    cfg.sensors.outdoor.maxAgeMs = (typeof cfg.sensors.outdoor.maxAgeMs === "number") ? cfg.sensors.outdoor.maxAgeMs : 900000;

    cfg.dhwRecirc = (cfg.dhwRecirc && typeof cfg.dhwRecirc === "object") ? cfg.dhwRecirc : {};
    const r = cfg.dhwRecirc;
    r.enabled = !!r.enabled;
    r.mode = r.mode || "on_demand";
    r.demandInput = Number.isFinite(Number(r.demandInput)) ? Number(r.demandInput) : 0;
    r.pumpRelay = Number.isFinite(Number(r.pumpRelay)) ? Number(r.pumpRelay) : 0;
    r.onDemandRunMs = (typeof r.onDemandRunMs === "number") ? r.onDemandRunMs : 120000;
    r.minOffMs = (typeof r.minOffMs === "number") ? r.minOffMs : 300000;
    r.minOnMs = (typeof r.minOnMs === "number") ? r.minOnMs : 30000;
    r.stopTempC = (typeof r.stopTempC === "number") ? r.stopTempC : 42;
    r.tempReturnSource = (r.tempReturnSource && typeof r.tempReturnSource === "object") ? r.tempReturnSource : {};
    r.tempReturnSource.source = r.tempReturnSource.source || "none";
    r.tempReturnSource.gpio = (typeof r.tempReturnSource.gpio === "number") ? r.tempReturnSource.gpio : 0;
    r.tempReturnSource.rom = r.tempReturnSource.rom || r.tempReturnSource.addr || "";
    r.tempReturnSource.topic = r.tempReturnSource.topic || "";
    r.tempReturnSource.jsonKey = r.tempReturnSource.jsonKey || r.tempReturnSource.key || r.tempReturnSource.field || "";
    r.tempReturnSource.mqttIdx = Number.isFinite(Number(r.tempReturnSource.mqttIdx || r.tempReturnSource.preset)) ? Number(r.tempReturnSource.mqttIdx || r.tempReturnSource.preset) : 0;
    r.tempReturnSource.bleId = r.tempReturnSource.bleId || r.tempReturnSource.id || "";
    r.windows = Array.isArray(r.windows) ? r.windows : [];

    cfg.equitherm = cfg.equitherm || {};
    const e = cfg.equitherm;
    e.enabled = !!e.enabled;
    e.outdoor = e.outdoor || {};
    e.flow = e.flow || {};
    e.boilerIn = e.boilerIn || {};
    e.akuTop = e.akuTop || {};
    e.akuMid = e.akuMid || {};
    e.akuBottom = e.akuBottom || {};
    for (const s of [e.outdoor, e.flow, e.boilerIn, e.akuTop, e.akuMid, e.akuBottom]) {
      s.source = s.source || "dallas";
      s.gpio = (typeof s.gpio === "number") ? s.gpio : 0;
      s.rom = s.rom || s.addr || "";
      s.topic = s.topic || "";
      s.jsonKey = s.jsonKey || s.key || s.field || "";
      s.mqttIdx = Number.isFinite(Number(s.mqttIdx || s.preset)) ? Number(s.mqttIdx || s.preset) : 0;
      s.bleId = s.bleId || s.id || "";
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

    e.valve = e.valve || {};
    e.valve.master = (typeof e.valve.master === "number") ? e.valve.master : 0;
    const legacyEqValve = Number.isFinite(Number(e.valveMaster)) ? Number(e.valveMaster) : 0;
    if (!e.valve.master && legacyEqValve) e.valve.master = legacyEqValve;

    e.control = e.control || {};
    e.control.deadbandC = (typeof e.control.deadbandC === "number") ? e.control.deadbandC : 0.5;
    e.control.stepPct = (typeof e.control.stepPct === "number") ? e.control.stepPct : 4;
    e.control.periodMs = (typeof e.control.periodMs === "number") ? e.control.periodMs : 30000;
    e.control.minPct = (typeof e.control.minPct === "number") ? e.control.minPct : 0;
    e.control.maxPct = (typeof e.control.maxPct === "number") ? e.control.maxPct : 100;
    e.deadbandC = (typeof e.deadbandC === "number") ? e.deadbandC : e.control.deadbandC;
    e.stepPct = (typeof e.stepPct === "number") ? e.stepPct : e.control.stepPct;
    e.controlPeriodMs = (typeof e.controlPeriodMs === "number") ? e.controlPeriodMs : e.control.periodMs;

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
    e.slopeDay = (typeof e.slopeDay === "number") ? e.slopeDay : 1.0;
    e.shiftDay = (typeof e.shiftDay === "number") ? e.shiftDay : 5.0;
    e.slopeNight = (typeof e.slopeNight === "number") ? e.slopeNight : 1.0;
    e.shiftNight = (typeof e.shiftNight === "number") ? e.shiftNight : 0.0;

    e.refs = e.refs || {};
    e.refs.day = e.refs.day || {};
    e.refs.night = e.refs.night || {};
    const defRefs = (o, tout1, tflow1, tout2, tflow2) => {
      o.tout1 = (typeof o.tout1 === "number") ? o.tout1 : tout1;
      o.tflow1 = (typeof o.tflow1 === "number") ? o.tflow1 : tflow1;
      o.tout2 = (typeof o.tout2 === "number") ? o.tout2 : tout2;
      o.tflow2 = (typeof o.tflow2 === "number") ? o.tflow2 : tflow2;
    };
    defRefs(e.refs.day, -10, 55, 15, 30);
    defRefs(e.refs.night, -10, 50, 15, 25);

    e.heatPoints = Array.isArray(e.heatPoints) ? e.heatPoints : null;
    e.nightPoints = Array.isArray(e.nightPoints) ? e.nightPoints : null;

    cfg.iofunc = (cfg.iofunc && typeof cfg.iofunc === "object") ? cfg.iofunc : {};
    cfg.iofunc.inputs = Array.isArray(cfg.iofunc.inputs) ? cfg.iofunc.inputs : Array.from({ length: INPUT_COUNT }, () => ({ role: "none", params: {} }));
    cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : Array.from({ length: RELAY_COUNT }, () => ({ role: "none", params: {} }));
    cfg.iofunc.inputs = cfg.iofunc.inputs.slice(0, INPUT_COUNT);
    cfg.iofunc.outputs = cfg.iofunc.outputs.slice(0, RELAY_COUNT);
    while (cfg.iofunc.inputs.length < INPUT_COUNT) cfg.iofunc.inputs.push({ role: "none", params: {} });
    while (cfg.iofunc.outputs.length < RELAY_COUNT) cfg.iofunc.outputs.push({ role: "none", params: {} });
    cfg.iofunc.inputs = cfg.iofunc.inputs.map((o) => ({ role: String(o?.role || "none"), params: (o?.params && typeof o.params === "object") ? o.params : {} }));
    cfg.iofunc.outputs = cfg.iofunc.outputs.map((o) => ({ role: String(o?.role || "none"), params: (o?.params && typeof o.params === "object") ? o.params : {} }));

    cfg.time = (cfg.time && typeof cfg.time === "object") ? cfg.time : {};
    cfg.time.ntpEnabled = !!cfg.time.ntpEnabled;
    cfg.time.server1 = String(cfg.time.server1 || "pool.ntp.org");
    cfg.time.server2 = String(cfg.time.server2 || "time.google.com");
    cfg.time.tz = String(cfg.time.tz || "Europe/Prague");
    cfg.time.syncIntervalMin = Number.isFinite(Number(cfg.time.syncIntervalMin)) ? Number(cfg.time.syncIntervalMin) : 60;

    cfg.schedules = Array.isArray(cfg.schedules) ? cfg.schedules : [];
    cfg.schedules = cfg.schedules.map((s) => ({
      id: String(s?.id || (Date.now().toString(36) + Math.random().toString(36).slice(2, 6))),
      enabled: s?.enabled !== false,
      name: String(s?.name || "Plán"),
      days: Array.isArray(s?.days) ? s.days.map((n) => Number(n)).filter((n) => n >= 1 && n <= 7) : [1, 2, 3, 4, 5, 6, 7],
      at: String(s?.at || "06:00"),
      kind: String(s?.kind || "set_mode"),
      value: (s && typeof s.value === "object") ? s.value : {},
    }));

    cfg.tuv = (cfg.tuv && typeof cfg.tuv === "object") ? cfg.tuv : {};
    cfg.tuv.enabled = !!cfg.tuv.enabled;
    cfg.tuv.demandInput = Number.isFinite(Number(cfg.tuv.demandInput)) ? Number(cfg.tuv.demandInput) : 0;
    cfg.tuv.requestRelay = Number.isFinite(Number(cfg.tuv.requestRelay)) ? Number(cfg.tuv.requestRelay) : 0;
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

    cfg.boiler = (cfg.boiler && typeof cfg.boiler === "object") ? cfg.boiler : {};
    cfg.boiler.dhwRequestRelay = Number.isFinite(Number(cfg.boiler.dhwRequestRelay)) ? Number(cfg.boiler.dhwRequestRelay) : 0;
    cfg.boiler.nightModeRelay = Number.isFinite(Number(cfg.boiler.nightModeRelay)) ? Number(cfg.boiler.nightModeRelay) : 0;

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

    cfg.dallasNames = Array.isArray(cfg.dallasNames) ? cfg.dallasNames : ["", "", "", ""];
    cfg.dallasAddrs = Array.isArray(cfg.dallasAddrs) ? cfg.dallasAddrs : ["", "", "", ""];
    cfg.dallasNames = cfg.dallasNames.slice(0, 4);
    cfg.dallasAddrs = cfg.dallasAddrs.slice(0, 4);
    while (cfg.dallasNames.length < 4) cfg.dallasNames.push("");
    while (cfg.dallasAddrs.length < 4) cfg.dallasAddrs.push("");
    cfg.dallasNames = cfg.dallasNames.map((v) => String(v || ""));
    cfg.dallasAddrs = cfg.dallasAddrs.map((v) => String(v || ""));

    cfg.thermometers = (cfg.thermometers && typeof cfg.thermometers === "object") ? cfg.thermometers : {};
    cfg.thermometers.mqtt = Array.isArray(cfg.thermometers.mqtt) ? cfg.thermometers.mqtt : [];
    cfg.thermometers.mqtt = cfg.thermometers.mqtt.slice(0, 2);
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
    cfg.thermometers.ble.id = String(cfg.thermometers.ble.id || "meteo.tempC");

    cfg.thermometers.roles = (cfg.thermometers.roles && typeof cfg.thermometers.roles === "object") ? cfg.thermometers.roles : {};
    const roles = cfg.thermometers.roles;
    const ensureRole = (key) => {
      const role = (roles[key] && typeof roles[key] === "object") ? roles[key] : {};
      role.source = String(role.source || "none");
      role.gpio = Number.isFinite(role.gpio) ? role.gpio : 0;
      role.rom = String(role.rom || role.addr || "");
      role.topic = String(role.topic || "");
      role.jsonKey = String(role.jsonKey || role.key || role.field || "");
      role.mqttIdx = Number.isFinite(Number(role.mqttIdx || role.preset)) ? Number(role.mqttIdx || role.preset) : 0;
      role.bleId = String(role.bleId || role.id || "");
      roles[key] = role;
    };
    ["outdoor", "flow", "return", "dhw", "tankTop", "tankMid", "tankBottom"].forEach(ensureRole);

    cfg.opentherm = (cfg.opentherm && typeof cfg.opentherm === "object") ? cfg.opentherm : {};
    cfg.opentherm.enabled = !!cfg.opentherm.enabled;
    cfg.opentherm.mode = String(cfg.opentherm.mode || "off");
    cfg.opentherm.inPin = Number.isFinite(cfg.opentherm.inPin) ? cfg.opentherm.inPin : -1;
    cfg.opentherm.outPin = Number.isFinite(cfg.opentherm.outPin) ? cfg.opentherm.outPin : -1;
    cfg.opentherm.pollMs = Number.isFinite(cfg.opentherm.pollMs) ? cfg.opentherm.pollMs : 1000;
    cfg.opentherm.chEnable = (cfg.opentherm.chEnable !== undefined) ? !!cfg.opentherm.chEnable : true;
    cfg.opentherm.manualSetpointC = Number.isFinite(cfg.opentherm.manualSetpointC) ? cfg.opentherm.manualSetpointC : 45;

    return cfg;
  };

  window.Core = window.Core || {};
  window.Core.configShape = { ensure: ensureConfigShape };
})();
