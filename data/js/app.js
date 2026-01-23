const API_TIMEOUT_STATUS_MS = 12000;
const API_TIMEOUT_DASH_MS = 15000;

const state = {
  config: null,
  caps: {},
  status: null,
  dash: null,
  bleConfig: null,
  bleStatus: null,
  blePaired: [],
  buzzerConfig: null,
  activePage: "dashboard",
  polling: true,
  statusTimer: null,
  dashTimer: null,
  timers: {},
  inFlight: {
    status: false,
    dash: false,
    bleStatus: false,
    blePaired: false,
  },
  backoff: {
    dashUntil: 0,
    bleUntil: 0,
  },
  curveThrottle: {
    equitherm: { lastAt: 0, signature: "" },
    dashEquitherm: { lastAt: 0, signature: "" },
  },
};

const sourceKeys = [
  { key: "outdoor", label: "Outdoor" },
  { key: "flow", label: "Flow" },
  { key: "akuTop", label: "Aku top" },
  { key: "akuMid", label: "Aku mid" },
  { key: "akuBottom", label: "Aku bottom" },
  { key: "boilerIn", label: "Boiler in" },
];

const buzzerPatterns = [
  "off",
  "short",
  "long",
  "double",
  "triple",
  "chirpUp",
  "chirpDown",
  "notify",
  "error",
];

const outputRoleOptions = [
  "none",
  "valve_3way_mix",
  "valve_3way_2rel",
  "valve_3way_spring",
  "valve_3way_tuv",
  "valve_3way_dhw",
  "boiler_enable_dhw",
  "boiler_enable_nm",
  "dhw_recirc_pump",
  "circ_pump",
  "heater_aku",
];

const inputRoleOptions = [
  "none",
  "dhw_enable",
  "dhw_demand",
  "night_mode",
  "thermostat",
  "heat_call",
  "recirc_demand",
];

const thermoRoleOptions = [
  { value: "", label: "—" },
  { value: "outdoor", label: "Venkovní teplota" },
  // Canonical role keys used across the project (UI + config + Equitherm presets)
  // Keep these short and stable.
  { value: "flow", label: "Topná voda (flow)" },
  { value: "return", label: "Zpátečka (return)" },
  { value: "dhw", label: "Nádrž TUV (DHW)" },
  { value: "aku_top", label: "Nádrž AKU – horní" },
  { value: "aku_mid", label: "Nádrž AKU – uprostřed" },
  { value: "aku_bottom", label: "Nádrž AKU – dolní" },
];

function normalizeThermoRole(role) {
  const v = String(role ?? "").trim();
  if (!v) return "";
  const s = v.toLowerCase();
  // Backward compatibility (older UI/builds)
  if (s === "heating_flow" || s === "heatingflow" || s === "flow" || s === "heating") return "flow";
  if (s === "heating_return" || s === "heatingreturn" || s === "return" || s === "ret") return "return";
  if (s === "dhw_tank" || s === "dhw" || s === "tuv" || s === "hotwater") return "dhw";
  if (s === "aku_top" || s === "akutop" || s === "aku-horni" || s === "aku_horni") return "aku_top";
  if (s === "aku_mid" || s === "akumid" || s === "aku-uprostred" || s === "aku_uprostred" || s === "aku_middle") return "aku_mid";
  if (s === "aku_bottom" || s === "akubottom" || s === "aku-dolni" || s === "aku_dolni" || s === "aku_low") return "aku_bottom";
  if (s === "outdoor" || s === "venek" || s === "outside") return "outdoor";

  // If it's already one of the canonical keys, keep it.
  if (thermoRoleOptions.some((opt) => opt.value === s)) return s;
  return v; // unknown/custom
}

function findThermometerPresetByRole(role) {
  const r = normalizeThermoRole(role);
  if (!r) return null;

  // Prefer GPIO TEMP1..8 roles first (fast + always available in dashboard).
  // TEMP roles are configured on the "Teploměry" page as tempRoles[0..7].
  const tempRoles = state.config?.tempRoles || [];
  for (let i = 0; i < tempRoles.length; i += 1) {
    if (normalizeThermoRole(tempRoles[i]) !== r) continue;
    // Backend expects source strings like "temp1".."temp8".
    return { type: "temp", tempKey: `temp${i + 1}` };
  }

  // Prefer actually connected BLE meteo for outdoor.
  if (r === "outdoor") {
    const meteo = state.bleStatus?.meteo || null;
    const activeMac = meteo?.activeMac || meteo?.mac || "";
    if (activeMac) {
      return { type: "ble", bleId: activeMac };
    }
  }

  const ble = state.config?.thermometers?.ble || {};
  if (normalizeThermoRole(ble.role) === r && ble.id) {
    return { type: "ble", bleId: ble.id };
  }

  const mqtt = state.config?.thermometers?.mqtt || [];
  for (let i = 0; i < mqtt.length; i += 1) {
    const slot = mqtt[i] || {};
    if (normalizeThermoRole(slot.role) !== r) continue;
    if (!slot.topic) continue;
    return { type: "mqtt", mqttIdx: i + 1, topic: slot.topic, jsonKey: slot.jsonKey || "tempC" };
  }

  return null;
}

function applyEquithermPresetFromRole(sourceKey, role, card) {
  const preset = findThermometerPresetByRole(role);
  if (!preset) {
    toast(`Pro roli "${getThermoRoleLabel(role)}" není v konfiguraci žádný teploměr.`, "warn");
    return;
  }

  if (preset.type === "temp") {
    updateEquithermSource(sourceKey, "source", preset.tempKey);
    // UI sync
    card.querySelector("[data-source-type]").value = preset.tempKey;
    const tSel = card.querySelector("[data-temp-idx]");
    if (tSel) tSel.value = String(Number(preset.tempKey.substring(4)) || "");
    updateSourceVisibility(card, preset.tempKey);
  } else if (preset.type === "ble") {
    updateEquithermSource(sourceKey, "source", "ble");
    updateEquithermSource(sourceKey, "bleId", preset.bleId);
    // UI sync
    card.querySelector("[data-source-type]").value = "ble";
    const idEl = card.querySelector("[data-source-field=bleId]");
    if (idEl) idEl.value = preset.bleId;
    updateSourceVisibility(card, "ble");
  } else if (preset.type === "mqtt") {
    updateEquithermSource(sourceKey, "source", "mqtt");
    updateEquithermSource(sourceKey, "mqttIdx", preset.mqttIdx);
    updateEquithermSource(sourceKey, "topic", preset.topic);
    updateEquithermSource(sourceKey, "jsonKey", preset.jsonKey);
    // UI sync
    card.querySelector("[data-source-type]").value = "mqtt";
    const slotEl = card.querySelector("[data-mqtt-slot]");
    if (slotEl) slotEl.value = String(preset.mqttIdx - 1);
    const tEl = card.querySelector("[data-source-field=topic]");
    if (tEl) tEl.value = preset.topic;
    const kEl = card.querySelector("[data-source-field=jsonKey]");
    if (kEl) kEl.value = preset.jsonKey;
    updateSourceVisibility(card, "mqtt");
  }
}

// Auto-assign Equitherm sources based on thermometer roles.
// This keeps the whole UI consistent: the user sets roles on "Teploměry" and
// Equitherm can immediately use them without extra clicking.
function autoAssignEquithermSourcesFromRoles() {
  state.config = state.config || {};
  state.config.equitherm = state.config.equitherm || {};
  const eq = state.config.equitherm;

  const mapping = [
    { key: "outdoor", role: "outdoor" },
    { key: "flow", role: "flow" },
    { key: "akuTop", role: "aku_top" },
    { key: "akuMid", role: "aku_mid" },
    { key: "akuBottom", role: "aku_bottom" },
  ];

  mapping.forEach(({ key, role }) => {
    eq[key] = eq[key] || {};
    const src = String(eq[key].source || "");
    if (src && src !== "none") {
      return; // user already selected a source explicitly
    }

    const preset = findThermometerPresetByRole(role);
    if (!preset) return;

    if (preset.type === "temp") {
      eq[key].source = preset.tempKey;
    } else if (preset.type === "ble") {
      eq[key].source = "ble";
      eq[key].bleId = preset.bleId;
    } else if (preset.type === "mqtt") {
      eq[key].source = "mqtt";
      eq[key].mqttIdx = preset.mqttIdx;
      eq[key].topic = preset.topic;
      eq[key].jsonKey = preset.jsonKey;
    }
  });

  // Boiler-in: if not configured, mirror flow (backend also does this fallback).
  eq.boilerIn = eq.boilerIn || {};
  const bi = String(eq.boilerIn.source || "");
  if (!bi || bi === "none") {
    const flow = eq.flow || {};
    if (flow.source) {
      eq.boilerIn = { ...eq.boilerIn, ...flow };
    }
  }
}

const $ = (id) => document.getElementById(id);

function toast(message, variant = "info") {
  const el = $("toast");
  el.textContent = message;
  el.className = `toast show ${variant}`;
  setTimeout(() => {
    el.className = "toast";
  }, 2800);
}

async function fetchJson(url, options = {}) {
  // ESP32 can be busy (BLE scan/connect, FS, JSON). Keep UI responsive but avoid noisy console errors.
  // - Default timeout is more generous (endpoints like /api/dash may take longer under load)
  // - AbortError is expected on timeout and is NOT logged as a warning
  const { timeoutMs: timeoutMsRaw, signal: upstreamSignal, ...fetchOpts } = options;
  const timeoutMs = timeoutMsRaw ?? 8000;

  const controller = new AbortController();
  const onUpstreamAbort = () => controller.abort();
  if (upstreamSignal) {
    if (upstreamSignal.aborted) {
      controller.abort();
    } else {
      upstreamSignal.addEventListener("abort", onUpstreamAbort, { once: true });
    }
  }

  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      ...fetchOpts,
      signal: controller.signal,
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`${response.status} ${response.statusText}: ${text}`);
    }
    if (response.status === 204) {
      return null;
    }
    const payload = await response.json();
    if (payload && typeof payload === "object" && Object.prototype.hasOwnProperty.call(payload, "ok")) {
      if (!payload.ok) {
        const err = payload.error || {};
        const msg = err.message || "API error";
        throw new Error(`${msg} (${err.code || "unknown"})`);
      }
      if (Array.isArray(payload.warnings) && payload.warnings.length) {
        console.warn("API warnings:", payload.warnings);
      }
      return payload.data ?? null;
    }
    return payload;
  } catch (error) {
    // Timeout / abort is expected; do not spam the console.
    if (error && (error.name === "AbortError" || error.code === 20)) {
      return null;
    }
    console.warn(`Fetch failed for ${url}`, error);
    return null;
  } finally {
    clearTimeout(timer);
    if (upstreamSignal) {
      try {
        upstreamSignal.removeEventListener("abort", onUpstreamAbort);
      } catch (_) {
        // ignore
      }
    }
  }
}

function getActivePage() {
  return (window.location.hash || "#dashboard").replace("#", "") || "dashboard";
}

function shouldPollBleStatus() {
  const page = getActivePage();
  return page === "dashboard" || page === "thermometers" || page === "ble";
}

function shouldPollBlePaired() {
  return getActivePage() === "ble";
}

function shouldPollDash() {
  const page = getActivePage();
  return page === "dashboard" || page === "thermometers";
}


// ---- Dashboard temps helper ----
// Dashboard expects TEMP1..8 primarily from /api/dash (temps + tempsValid).
// Fallback to /api/status temps (object entries {valid,tempC}).
function getDashTempsAsObjects() {
  const temps = Array.isArray(state.dash?.temps) ? state.dash.temps : null;
  if (!temps) return null;
  const valid = Array.isArray(state.dash?.tempsValid) ? state.dash.tempsValid : null;
  const out = [];
  for (let i = 0; i < 8; i += 1) {
    const t = temps[i];
    const v = valid ? !!valid[i] : (t !== null && t !== undefined);
    out.push({ tempC: t ?? null, valid: v });
  }
  return out;
}

function getTempsForDashboard() {
  const dash = getDashTempsAsObjects();
  if (dash) return dash;
  const statusTemps = Array.isArray(state.status?.temps) ? state.status.temps : null;
  return statusTemps || [];
}


function getPath(obj, path) {
  return path.split(".").reduce((acc, key) => {
    if (acc === null || acc === undefined) {
      return undefined;
    }
    return acc[key];
  }, obj);
}

function setPath(obj, path, value) {
  const parts = path.split(".");
  let current = obj;
  parts.forEach((part, index) => {
    if (index === parts.length - 1) {
      current[part] = value;
      return;
    }
    if (current[part] === undefined) {
      current[part] = {};
    }
    current = current[part];
  });
}

function parseInputValue(input) {
  if (input.type === "checkbox") {
    return input.checked;
  }
  if (input.tagName === "SELECT" && input.dataset.asNumber === "1") {
    const value = input.value;
    return value === "" ? null : Number(value);
  }
  if (input.type === "number") {
    const raw = String(input.value ?? "").trim();
    if (raw === "") {
      return null;
    }
    const normalized = raw.replace(",", ".");
    const num = Number(normalized);
    return Number.isFinite(num) ? num : null;
  }
  return input.value;
}

function setInputValue(input, value) {
  if (input.type === "checkbox") {
    input.checked = Boolean(value);
    return;
  }
  if (value === undefined || value === null) {
    input.value = "";
    return;
  }
  input.value = value;
}

function bindInputs(container, data) {
  // Some sections may be absent (older/partial UI builds, disabled caps, etc.).
  // Do not crash the whole UI if the container is missing.
  if (!container) {
    return;
  }
  container.querySelectorAll("[data-path]").forEach((input) => {
    const path = input.dataset.path;
    const eventName = input.tagName === "SELECT" || input.type === "checkbox" ? "change" : "input";
    setInputValue(input, getPath(data, path));
    input.addEventListener(eventName, () => {
      setPath(data, path, parseInputValue(input));
      if (path.startsWith("equitherm")) {
        drawEquithermCurve();
        drawDashEquithermCurve();
      }
    });
  });
}


// Convert { "0": "x", "1": "y", ... } OR sparse objects into fixed-length array
function indexedObjectToArray(value, len, fillValue = "") {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return Array.isArray(value) ? value.slice() : [];
  }
  const out = Array(len).fill(fillValue);
  for (let i = 0; i < len; i += 1) {
    const v = value[i] ?? value[String(i)];
    if (v !== undefined) out[i] = v;
  }
  return out;
}

// Convert array -> { "0": ..., "1": ... } (backend-friendly for tempRoles)
function arrayToIndexedObject(arr, len) {
  const out = {};
  const a = Array.isArray(arr) ? arr : [];
  for (let i = 0; i < len; i += 1) {
    out[String(i)] = a[i] ?? "";
  }
  return out;
}

function buildConfigForSave(cfg) {
  const copy = JSON.parse(JSON.stringify(cfg || {}));
  // Backend expects tempRoles as object (see API warnings: wrong_type -> "Přepsáno na objekt")
  const tempRolesArr = indexedObjectToArray(copy.tempRoles, 8, "");
  copy.tempRoles = arrayToIndexedObject(tempRolesArr, 8);
  return copy;
}

function validateConfigPayload(payload, contextLabel = "config") {
  const errs = [];
  const warns = [];

  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    errs.push(`${contextLabel}: payload must be an object`);
  }

  const tr = payload?.tempRoles;
  if (tr !== undefined) {
    if (!tr || typeof tr !== "object" || Array.isArray(tr)) {
      errs.push(`${contextLabel}: tempRoles must be an object (not array)`);
    } else {
      for (let i = 0; i < 8; i += 1) {
        const v = tr[String(i)];
        if (v === undefined) warns.push(`${contextLabel}: tempRoles missing key "${i}"`);
        if (v !== undefined && v !== null && typeof v !== "string") warns.push(`${contextLabel}: tempRoles["${i}"] should be string`);
      }
    }
  }

  if (warns.length) console.warn("Config payload warnings:", warns);
  if (errs.length) {
    console.error("Config payload errors:", errs);
    throw new Error(errs[0]);
  }
}

function ensureConfigDefaults() {
  state.config = state.config || {};
  if (!state.config.inputNames && Array.isArray(state.config.inputs)) {
    state.config.inputNames = state.config.inputs.map((input) => input.name || "");
  }
  if (!state.config.inputActiveLevels && Array.isArray(state.config.inputs)) {
    state.config.inputActiveLevels = state.config.inputs.map((input) => (input.activeLevel ?? 0));
  }
  if (!state.config.relayNames && Array.isArray(state.config.relays)) {
    state.config.relayNames = state.config.relays.map((relay) => relay.name || "");
  }
  state.config.inputNames = state.config.inputNames || Array(8).fill("");
  state.config.relayNames = state.config.relayNames || Array(8).fill("");
  state.config.inputActiveLevels = state.config.inputActiveLevels || Array(8).fill(0);
  state.config.relayMap = state.config.relayMap || Array.from({ length: 8 }, () => ({ input: 0, polarity: false }));
  state.config.iofunc = state.config.iofunc || {};
  // Ensure arrays exist before accessing .length (prevents runtime crash when backend returns partial config)
  state.config.iofunc.inputs =
    Array.isArray(state.config.iofunc.inputs) && state.config.iofunc.inputs.length
      ? state.config.iofunc.inputs
      : Array.from({ length: 8 }, () => ({ role: "" }));
  state.config.iofunc.outputs =
    Array.isArray(state.config.iofunc.outputs) && state.config.iofunc.outputs.length
      ? state.config.iofunc.outputs
      : Array.from({ length: 8 }, () => ({ role: "", params: {} }));
  state.config.thermometers = state.config.thermometers || { mqtt: [{}, {}], ble: {} };
  state.config.thermometers.mqtt = state.config.thermometers.mqtt || [{}, {}];
  if (state.config.thermometers.mqtt.length < 2) {
    state.config.thermometers.mqtt = [state.config.thermometers.mqtt[0] || {}, {}];
  }
  // Accept both array and object shapes (backend may sanitize to object)
  if (Array.isArray(state.config.tempRoles)) {
    state.config.tempRoles = state.config.tempRoles.slice();
  } else if (state.config.tempRoles && typeof state.config.tempRoles === "object") {
    state.config.tempRoles = indexedObjectToArray(state.config.tempRoles, 8, "");
  } else {
    state.config.tempRoles = [];
  }
  if (state.config.tempRoles.length < 8) {
    state.config.tempRoles = state.config.tempRoles.concat(Array(8 - state.config.tempRoles.length).fill(""));
  } else if (state.config.tempRoles.length > 8) {
    state.config.tempRoles = state.config.tempRoles.slice(0, 8);
  }

  // Normalize thermometer roles for compatibility and cross-page consistency.
  state.config.tempRoles = state.config.tempRoles.map((r) => normalizeThermoRole(r));
  state.config.thermometers.ble = state.config.thermometers.ble || {};
  if (state.config.thermometers.ble.role !== undefined) {
    state.config.thermometers.ble.role = normalizeThermoRole(state.config.thermometers.ble.role);
  }
  state.config.thermometers.mqtt = (state.config.thermometers.mqtt || []).map((slot) => {
    const out = slot || {};
    if (out.role !== undefined) out.role = normalizeThermoRole(out.role);
    return out;
  });

  const relayNameDefaults = {
    4: "Požadavek TUV (kotl.)",
    5: "Denní/Noční křivka",
    6: "Cirkulace TUV",
    7: "Dohřev nádrže",
  };
  Object.entries(relayNameDefaults).forEach(([index, label]) => {
    const idx = Number(index);
    if (!state.config.relayNames[idx]) {
      state.config.relayNames[idx] = label;
    }
  });

  const inputRoleDefaults = ["dhw_demand", "night_mode", "recirc_demand", "", "", "", "", ""];
  inputRoleDefaults.forEach((role, index) => {
    if (!role) {
      return;
    }
    if (!state.config.iofunc.inputs[index].role) {
      state.config.iofunc.inputs[index].role = role;
    }
  });

  const outputRoleDefaults = [
    "valve_3way_mix",
    "",
    "valve_3way_dhw",
    "",
    "boiler_enable_dhw",
    "boiler_enable_nm",
    "dhw_recirc_pump",
    "heater_aku",
  ];
  outputRoleDefaults.forEach((role, index) => {
    if (!role) {
      return;
    }
    if (!state.config.iofunc.outputs[index].role) {
      state.config.iofunc.outputs[index].role = role;
    }
  });





  // --- Normalizace parametrů 3c ventilů (kompatibilita / sjednocení polí) ---
  const normalizeValveParams = (out) => {
    if (!out) return;
    out.params = out.params || {};
    const p = out.params;

    const toNum = (v) => {
      if (v === null || v === undefined || v === "") return null;
      const n = Number(String(v).trim().replace(",", "."));
      return Number.isFinite(n) ? n : null;
    };

    // peerRel: nové pole (1-based), legacy: peer / partnerRelay
    if (p.peerRel === undefined || p.peerRel === null || p.peerRel === "") {
      const legacyPeer = p.peer !== undefined ? p.peer : p.partnerRelay;
      const n = toNum(legacyPeer);
      if (n !== null) p.peerRel = n;
    }

    // Časy: preferujeme *bez* suffixu S (sekundy). Legacy: travelTimeS / pulseTimeS / guardTimeS.
    if (p.travelTime === undefined || p.travelTime === null || p.travelTime === "") {
      const n = toNum(p.travelTimeS);
      if (n !== null) p.travelTime = n;
    }
    if (p.pulseTime === undefined || p.pulseTime === null || p.pulseTime === "") {
      const n = toNum(p.pulseTimeS);
      if (n !== null) p.pulseTime = n;
    }
    if (p.guardTime === undefined || p.guardTime === null || p.guardTime === "") {
      const n = toNum(p.guardTimeS);
      if (n !== null) p.guardTime = n;
    }

    // Min switch: nové minSwitchS (s), legacy minSwitch (s) / minSwitchMs (ms)
    if (p.minSwitchS === undefined || p.minSwitchS === null || p.minSwitchS === "") {
      let n = toNum(p.minSwitch);
      if (n === null) {
        const ms = toNum(p.minSwitchMs);
        if (ms !== null) n = ms / 1000.0;
      }
      if (n !== null) p.minSwitchS = n;
    }

    // defaultPos: nové ("A"/"B"), legacy: default
    if (!p.defaultPos && p.default) {
      p.defaultPos = String(p.default);
    }

    // Úklid legacy klíčů, aby se do backendu neposílaly duplicity
    delete p.peer;
    delete p.partnerRelay;
    delete p.travelTimeS;
    delete p.pulseTimeS;
    delete p.guardTimeS;
    delete p.minSwitch;
    delete p.minSwitchMs;
    delete p.default;
  };

  (state.config.iofunc.outputs || []).forEach(normalizeValveParams);

  // --- Defaulty pro 3c ventily ---
  // Důvod: když hodnoty nejsou v configu explicitně uložené, UI ukazuje jen placeholder,
  // ale backend použije svoje interní defaulty (což vypadá jako "nerespektování" nastavení).
  // Držíme proto rozumné výchozí hodnoty přímo v configu.
  const ensureValveDefaults = (idx, defaults) => {
    const out = state.config.iofunc.outputs[idx] || { role: "", params: {} };
    out.params = out.params || {};
    Object.entries(defaults).forEach(([k, v]) => {
      if (out.params[k] === undefined || out.params[k] === null || out.params[k] === "") {
        out.params[k] = v;
      }
    });
    state.config.iofunc.outputs[idx] = out;
  };

  // R1+R2: směšovací ventil (typicky ~120 s plný přeběh)
  ensureValveDefaults(0, {
    peerRel: 2,
    travelTime: 120,
    pulseTime: 1.5,
    guardTime: 2,
    minSwitchS: 3,
    invertDir: false,
    defaultPos: "A",
  });

  // R3: přepínací ventil (spring-return) – hodnoty jsou méně kritické, ale ať jsou konzistentní
  ensureValveDefaults(2, {
    peerRel: 0,
    travelTime: 6,
    pulseTime: 0.8,
    guardTime: 0.3,
    minSwitchS: 3,
    invertDir: false,
    defaultPos: "A",
  });
  // --- Cirkulace TUV (dhwRecirc) defaulty ---
  state.config.dhwRecirc = state.config.dhwRecirc || {};
  const rc = state.config.dhwRecirc;
  if (rc.enabled === undefined) rc.enabled = false;
  if (!rc.mode) rc.mode = "on_demand"; // on_demand | time_windows | hybrid
  // Pevné mapování (firmware je může vynutit): IN3 + R7
  if (rc.demandInput === undefined) rc.demandInput = 3;
  if (rc.pumpRelay === undefined) rc.pumpRelay = 7;
  if (rc.onDemandRunMs === undefined) rc.onDemandRunMs = 120000;
  if (rc.minOffMs === undefined) rc.minOffMs = 300000;
  if (rc.minOnMs === undefined) rc.minOnMs = 30000;
  if (rc.cycleOnMin === undefined) rc.cycleOnMin = 0;
  if (rc.cycleOffMin === undefined) rc.cycleOffMin = 0;
  if (rc.stopTempC === undefined) rc.stopTempC = 42.0;
  rc.windows = Array.isArray(rc.windows) ? rc.windows : [];
  rc.tempReturnSource = rc.tempReturnSource || {};
  if (!rc.tempReturnSource.source) rc.tempReturnSource.source = "none";
  // Legacy alias
  if (rc.tempReturnSource.source === "mqtt0") { rc.tempReturnSource.source = "mqtt"; rc.tempReturnSource.mqttIdx = 1; }
  if (rc.tempReturnSource.source === "mqtt1") { rc.tempReturnSource.source = "mqtt"; rc.tempReturnSource.mqttIdx = 2; }


  if (!state.config.thermometers.ble.name && state.config.thermometers.ble.id) {
    state.config.thermometers.ble.name = "Outdoor";
  }

  // Default role for BLE meteo thermometer (role is used across the system)
  if (!state.config.thermometers.ble.role) {
    state.config.thermometers.ble.role = "outdoor";
  }


  // DS18B20 mapping (TEMP1..TEMP8): GPIO0..GPIO3 buses + optional ROM selection
  // dallasGpios[i] -> 0..3 (string) or "" to disable the channel
  // dallasAddrs[i] -> ROM hex ("" => AUTO/first valid device)
  state.config.dallasGpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
  state.config.dallasAddrs = Array.isArray(state.config.dallasAddrs) ? state.config.dallasAddrs : [];

  // Backward-compat: older configs may have dallasAddrs only for TEMP1..4
  if (state.config.dallasGpios.length === 0 && state.config.dallasAddrs.length > 0 && state.config.dallasAddrs.length < 8) {
    state.config.dallasGpios = ["0", "1", "2", "3"]; // TEMP1..4
  }

  while (state.config.dallasGpios.length < 8) {
    const idx = state.config.dallasGpios.length;
    state.config.dallasGpios.push(idx <= 3 ? String(idx) : "");
  }
  while (state.config.dallasAddrs.length < 8) {
    state.config.dallasAddrs.push("");
  }

  // Normalize ROM hex formatting (uppercase + strip non-hex)
  state.config.dallasAddrs = state.config.dallasAddrs.map((v) => {
    const str = String(v || "").toUpperCase();
    return str.replace(/[^0-9A-F]/g, "");
  });
  state.config.equitherm = state.config.equitherm || {};

  // --- Ekviterm defaulty (odpovídají backendu v LogicController.cpp)
  const eq = state.config.equitherm;
  // Směšovací 3c ventil je vždy pevně na R1+R2 (master=1).
  eq.valveMaster = 1;

  if (eq.enabled === undefined) eq.enabled = false;
  if (eq.minFlow === undefined) eq.minFlow = 25;
  if (eq.maxFlow === undefined) eq.maxFlow = 55;
  if (eq.slopeDay === undefined) eq.slopeDay = 1.0;
  if (eq.shiftDay === undefined) eq.shiftDay = 5.0;
  if (eq.slopeNight === undefined) eq.slopeNight = 1.0;
  if (eq.shiftNight === undefined) eq.shiftNight = 0.0;
  if (eq.curveOffsetC === undefined) eq.curveOffsetC = 0.0;
  if (eq.deadbandC === undefined) eq.deadbandC = 0.5;
  if (eq.stepPct === undefined) eq.stepPct = 0;
  if (eq.controlPeriodMs === undefined) eq.controlPeriodMs = 30000;
  if (eq.maxPct_day === undefined) eq.maxPct_day = 100;
  if (eq.maxPct_night === undefined) eq.maxPct_night = 100;
  if (eq.maxBoilerInC === undefined) eq.maxBoilerInC = 55.0;
  if (eq.noFlowDetectEnabled === undefined) eq.noFlowDetectEnabled = true;
  if (eq.noFlowTimeoutMs === undefined) eq.noFlowTimeoutMs = 180000;
  if (eq.noFlowTestPeriodMs === undefined) eq.noFlowTestPeriodMs = 180000;
  eq.refs = eq.refs || {};
  eq.refs.day = eq.refs.day || {};
  eq.refs.night = eq.refs.night || {};
  if (eq.refs.day.tout1 === undefined) eq.refs.day.tout1 = -10;
  if (eq.refs.day.tflow1 === undefined) eq.refs.day.tflow1 = 55;
  if (eq.refs.day.tout2 === undefined) eq.refs.day.tout2 = 15;
  if (eq.refs.day.tflow2 === undefined) eq.refs.day.tflow2 = 30;
  if (eq.refs.night.tout1 === undefined) eq.refs.night.tout1 = -10;
  if (eq.refs.night.tflow1 === undefined) eq.refs.night.tflow1 = 50;
  if (eq.refs.night.tout2 === undefined) eq.refs.night.tout2 = 15;
  if (eq.refs.night.tflow2 === undefined) eq.refs.night.tflow2 = 25;

  // --- TUV / DHW defaulty
  // (pevné mapování: R3 = přepínací ventil, R5 = požadavek do kotle, IN1 = demand)
  state.config.tuv = state.config.tuv || {};
  const tuv = state.config.tuv;
  // čísla jsou 1-based (R1..R8 / IN1..IN8), stejně jako ve formuláři
  // Klíče relay/demand/valveMaster ponecháváme kvůli kompatibilitě import/exportu,
  // ale firmware je v tomto projektu stejně ignoruje (fixed mapping).
  if (tuv.relay === undefined) tuv.relay = 5;
  if (tuv.requestRelay === undefined) tuv.requestRelay = 5;
  if (tuv.demandInput === undefined) tuv.demandInput = 1;
  if (tuv.valveMaster === undefined) tuv.valveMaster = 3;

  // Reálné parametry používané firmware
  if (tuv.enabled === undefined) tuv.enabled = false;
  if (tuv.valveTargetPct === undefined) tuv.valveTargetPct = 100;
  if (tuv.eqValveTargetPct === undefined && tuv.mixValveTargetPct !== undefined) tuv.eqValveTargetPct = tuv.mixValveTargetPct;
  if (tuv.eqValveTargetPct === undefined) tuv.eqValveTargetPct = 0;
  // restoreEqValveAfter = boolean (starší UI mohlo posílat číslo)
  if (typeof tuv.restoreEqValveAfter === "number") tuv.restoreEqValveAfter = Boolean(tuv.restoreEqValveAfter);
  if (tuv.restoreEqValveAfter === undefined) tuv.restoreEqValveAfter = true;
  tuv.bypassValve = tuv.bypassValve || {};
  if (tuv.bypassValve.masterRelay === undefined) tuv.bypassValve.masterRelay = 3;
  if (tuv.bypassValve.enabled === undefined) tuv.bypassValve.enabled = true;
  if (tuv.bypassValve.bypassPct === undefined) tuv.bypassValve.bypassPct = 100;
  // CH poloha pro přepínací ventil je default 0%
  if (tuv.bypassValve.chPct === undefined) tuv.bypassValve.chPct = 0;
  if (tuv.bypassValve.invert === undefined) tuv.bypassValve.invert = false;

  // Udržuj kompatibilitu: pokud uživatel nastaví jen bypassPct, drž i valveTargetPct.
  // (Když by někdo vypnul bypassValve.enabled, firmware použije valveTargetPct.)
  if (tuv.valveTargetPct === undefined || tuv.valveTargetPct === null) {
    tuv.valveTargetPct = Number(tuv.bypassValve.bypassPct) || 100;
  }



  // --- AKU heater defaulty (dotápění akumulační nádrže)
  state.config.akuHeater = state.config.akuHeater || {};
  const aku = state.config.akuHeater;
  if (aku.enabled === undefined) aku.enabled = false;
  if (aku.mode === undefined) aku.mode = "manual"; // manual | schedule | thermostatic
  if (aku.manualOn === undefined) aku.manualOn = false;
  if (aku.targetTopC === undefined) aku.targetTopC = 50.0;
  if (aku.hysteresisC === undefined) aku.hysteresisC = 2.0;
  if (aku.maxOnMs === undefined) aku.maxOnMs = 2 * 60 * 60 * 1000;
  if (aku.minOffMs === undefined) aku.minOffMs = 10 * 60 * 1000;
  // Relay je ve firmware fixně R8, ale klíč ponecháme kvůli kompatibilitě export/importu
  if (aku.relay === undefined) aku.relay = 8;
  aku.windows = Array.isArray(aku.windows) ? aku.windows : [];

  // Keep Equitherm sources aligned with roles from "Teploměry".
  autoAssignEquithermSourcesFromRoles();
}

function syncBleThermometerFromStatus() {
  const meteo = state.bleStatus?.meteo || null;
  if (!meteo) {
    return;
  }

  const activeMac = meteo.activeMac || meteo.mac || "";
  if (activeMac) {
    // Keep the thermometers table aligned with the actually connected meteo sensor
    state.config.thermometers = state.config.thermometers || { mqtt: [{}, {}], ble: {} };
    state.config.thermometers.ble = state.config.thermometers.ble || {};
    state.config.thermometers.ble.id = activeMac;
    if (!state.config.thermometers.ble.name) {
      state.config.thermometers.ble.name = "Outdoor";
    }
    if (!state.config.thermometers.ble.role) {
      state.config.thermometers.ble.role = "outdoor";
    }

    // Ekviterm: pokud uživatel výslovně nenastavil jiný zdroj, použij spárované BLE jako Outdoor.
    state.config.equitherm = state.config.equitherm || {};
    state.config.equitherm.outdoor = state.config.equitherm.outdoor || {};
    const outdoor = state.config.equitherm.outdoor;
    if (!outdoor.source || outdoor.source === "ble") {
      outdoor.source = "ble";
      outdoor.bleId = activeMac;
    }

    // Re-evaluate role-based defaults (e.g., flow/AKU can be TEMP roles).
    autoAssignEquithermSourcesFromRoles();
  }
}

function getThermoRoleLabel(role) {
  const value = String(role ?? "");
  const found = thermoRoleOptions.find((opt) => String(opt.value) === value);
  return found ? found.label : value || "";
}

function renderInputsConfig() {
  const container = $("inputConfig");
  container.innerHTML = "";
  state.config.inputNames.forEach((name, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `
      <label class="field"><span>Input ${index + 1} název</span><input data-path="inputNames.${index}" /></label>
      <label class="field"><span>Active level (0=LOW,1=HIGH)</span><input type="number" min="0" max="1" data-path="inputActiveLevels.${index}" /></label>
    `;
    container.appendChild(row);
  });
  bindInputs(container, state.config);
}

function renderRelayNames() {
  const container = $("relayNames");
  container.innerHTML = "";
  state.config.relayNames.forEach((name, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `<label class="field"><span>Relé ${index + 1} název</span><input data-path="relayNames.${index}" /></label>`;
    container.appendChild(row);
  });
  bindInputs(container, state.config);
}

function renderRelayMap() {
  const container = $("relayMap");
  container.innerHTML = "";
  state.config.relayMap.forEach((map, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `
      <label class="field"><span>Relé ${index + 1} → Vstup</span><input type="number" min="0" max="8" data-path="relayMap.${index}.input" /></label>
      <label class="field"><span>Polarity (invert)</span><input type="checkbox" data-path="relayMap.${index}.polarity" /></label>
    `;
    container.appendChild(row);
  });
  bindInputs(container, state.config);
}

function renderIofuncRoles() {
  const inputsContainer = $("iofuncInputs");
  const outputsContainer = $("iofuncOutputs");
  inputsContainer.innerHTML = "";
  outputsContainer.innerHTML = "";

  state.config.iofunc.inputs.forEach((input, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `
      <label class="field"><span>Input ${index + 1} role</span>
        <input list="inputRoles" data-path="iofunc.inputs.${index}.role" />
      </label>
    `;
    inputsContainer.appendChild(row);
  });

  state.config.iofunc.outputs.forEach((output, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `
      <label class="field"><span>Output ${index + 1} role</span>
        <input list="outputRoles" data-path="iofunc.outputs.${index}.role" />
      </label>
    `;
    outputsContainer.appendChild(row);
  });

  bindInputs(inputsContainer, state.config);
  bindInputs(outputsContainer, state.config);
  renderEquithermValveSelect();
}

function renderValveConfig() {
  const container = $("valveConfig");
  container.innerHTML = "";

  state.config.iofunc.outputs.forEach((output, index) => {
    // Ventily jsou v tomto projektu pevně mapované jen na 2 kusy:
    //  - R1 (master) + R2 (peer) = 3c směšovací (Ekviterm)
    //  - R3 = 3c přepínací / pružinový (TUV)
    // Konfiguraci R4–R8 zde nezobrazujeme (patří jiným funkcím).
    if (index !== 0 && index !== 2) return;

    const isFixedMix = index === 0;
    const isFixedTuv = index === 2;
    const isFixed = isFixedMix || isFixedTuv;

    // Pomoz uživateli: udržuj rozumné role/peer v configu (backend je má fixně,
    // ale tyto hodnoty se používají pro kalibrační parametry).
    output.params = output.params || {};
    if (isFixedMix) {
      output.role = "valve_3way_mix";
      output.params.peerRel = 2;
    }
    if (isFixedTuv) {
      output.role = "valve_3way_spring";
      output.params.peerRel = 0;
    }

    const row = document.createElement("div");
    row.className = "card";

    const title = isFixedMix
      ? `Relé ${index + 1} (V2 směšovací – Master, peer = R2)`
      : isFixedTuv
      ? `Relé ${index + 1} (V3 TUV ventil – pružinový)`
      : `Relé ${index + 1}`;

    const roleField = isFixed
      ? `<div class="field"><span>Role</span><div class="mono">${escapeHtml(output.role || "")}</div></div>`
      : `<label class="field"><span>Role</span><input list="outputRoles" data-path="iofunc.outputs.${index}.role" placeholder="valve_3way_mix" /></label>`;

    const peerField = isFixedMix
      ? `<div class="field"><span>Peer relé</span><div class="mono">R2</div></div>`
      : isFixedTuv
      ? `<div class="field"><span>Peer relé</span><div class="mono">—</div></div>`
      : `<label class="field"><span>Peer relé</span><input type="number" min="0" max="8" data-path="iofunc.outputs.${index}.params.peerRel" placeholder="2" /></label>`;

    row.innerHTML = `
      <h3>${title}</h3>
      <div class="form-grid">
        ${roleField}
        ${peerField}
        <label class="field" title="Čas přestavení 0% → 100% (jeden plný zdvih). Příklad: 120 s."><span>Travel time (s)</span><input type="number" min="0" step="0.1" data-path="iofunc.outputs.${index}.params.travelTime" placeholder="120" /></label>
        <label class="field" title="Délka jednoho pulzu (pro Pulse +/− a ekviterm AUTO krok). Příklad: 1.5 s."><span>Pulse time (s)</span><input type="number" min="0" step="0.1" data-path="iofunc.outputs.${index}.params.pulseTime" placeholder="1.5" /></label>
        <label class="field" title="Ochranná pauza po pulzu / změně směru. Příklad: 2 s."><span>Guard time (s)</span><input type="number" min="0" step="0.1" data-path="iofunc.outputs.${index}.params.guardTime" placeholder="2" /></label>
        <label class="field" title="Minimální doba mezi sepnutími relé. Příklad: 3 s."><span>Min switch (s)</span><input type="number" min="0" step="0.1" data-path="iofunc.outputs.${index}.params.minSwitchS" placeholder="3" /></label>
        <label class="field" title="Prohození směru cívky A/B (nemění škálu procent)."><span>Invert dir</span><input type="checkbox" data-path="iofunc.outputs.${index}.params.invertDir" /></label>
        <label class="field" title="Výchozí poloha po restartu (A=0%, B=100%)."><span>Default pos</span>
          <select data-path="iofunc.outputs.${index}.params.defaultPos">
            <option value="">—</option>
            <option value="A">A</option>
            <option value="B">B</option>
          </select>
        </label>
      </div>
    `;

    container.appendChild(row);
  });

  bindInputs(container, state.config);
  renderEquithermValveSelect();
  initValvesTooltips();
}

function renderThermometers() {
  // Thermometers page is now driven purely by the overview table.
  renderThermometerOverviewTable();
}

function roleOptionsHtml(selected) {
  const sel = String(selected ?? "");
  const known = thermoRoleOptions.some((opt) => String(opt.value) === sel);
  const base = thermoRoleOptions
    .map((opt) => {
      const s = String(opt.value) === sel ? "selected" : "";
      return `<option value="${escapeHtml(opt.value)}" ${s}>${escapeHtml(opt.label)}</option>`;
    })
    .join("");

  if (sel && !known) {
    return `<option value="${escapeHtml(sel)}" selected>(nepodporováno) ${escapeHtml(sel)}</option>` + base;
  }
  return base;
}

function formatThermoStatus(entry) {
  if (entry === undefined || entry === null) {
    return { text: "—", muted: true };
  }

  // Unify different payload shapes (backend + legacy UI variants)
  const normalized = normalizeTempEntry(entry);
  const valid = normalized.valid !== false;

  // Preserve age if present (BLE/MQTT may report ageMs)
  const ageMs =
    (entry && typeof entry === "object" && (entry.ageMs ?? entry.age ?? entry.age_ms) !== undefined)
      ? (entry.ageMs ?? entry.age ?? entry.age_ms)
      : normalized.ageMs;
  const age = ageMs !== undefined && ageMs !== null ? ` • age ${ageMs} ms` : "";

  if (normalized.tempC === undefined || normalized.tempC === null || Number.isNaN(Number(normalized.tempC))) {
    return { text: (valid ? "—" : "invalid") + age, muted: !valid };
  }

  return { text: `${formatTemp(normalized.tempC)}${valid ? "" : " • invalid"}${age}`, muted: !valid };
}

function escapeHtml(value) {
  const str = value === undefined || value === null ? "" : String(value);
  return str
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function renderThermometerOverviewTable() {
  const body = $("thermometerTableBody");
  if (!body) {
    return;
  }
  body.innerHTML = "";

  const rows = [];

  // DS18B20 diagnostics are provided by /api/dash as dash.dallas[gpio].devices[].rom
  function getDallasBusByGpio(gpio) {
    const arr = state.dash?.dallas;
    if (!Array.isArray(arr)) return null;
    return arr.find((b) => Number(b?.gpio) === Number(gpio)) || null;
  }

  function listDallasRomsOnBus(bus) {
    const devs = Array.isArray(bus?.devices) ? bus.devices : [];
    return devs
      .map((d) => (d?.rom ? String(d.rom).toUpperCase() : ""))
      .filter((x) => x.length);
  }

  function buildDallasGpioOptions(selected) {
    const sel = selected === undefined || selected === null ? "" : String(selected);
    const opts = [
      { v: "", t: "OFF" },
      { v: "0", t: "GPIO0" },
      { v: "1", t: "GPIO1" },
      { v: "2", t: "GPIO2" },
      { v: "3", t: "GPIO3" },
    ];
    return opts
      .map((o) => `<option value="${o.v}" ${o.v === sel ? "selected" : ""}>${o.t}</option>`)
      .join("");
  }

  function buildDallasRomOptions(gpioNum, selectedRom) {
    const sel = String(selectedRom || "").toUpperCase();
    if (!(gpioNum >= 0 && gpioNum <= 3)) {
      return `<option value="" selected>—</option>`;
    }
    const bus = getDallasBusByGpio(gpioNum);
    const roms = listDallasRomsOnBus(bus);
    const parts = [`<option value="" ${sel === "" ? "selected" : ""}>AUTO</option>`];
    roms.forEach((rom) => {
      parts.push(`<option value="${escapeHtml(rom)}" ${rom === sel ? "selected" : ""}>${escapeHtml(rom)}</option>`);
    });
    return parts.join("");
  }

  // GPIO TEMP1..8 (DS18B20 via GPIO0..3, per-channel ROM selection)
  // Prefer /api/dash temps (more reliable + already normalized), fallback to /api/status.
  const dashTemps = Array.isArray(state.dash?.temps) ? state.dash.temps : null;
  const dashValid = Array.isArray(state.dash?.tempsValid) ? state.dash.tempsValid : null;
  const statusTemps = Array.isArray(state.status?.temps) ? state.status.temps : [];
  for (let i = 0; i < 8; i += 1) {
    if (dashTemps) {
      const t = dashTemps[i];
      const v = dashValid ? !!dashValid[i] : (t !== null && t !== undefined);
      const entry = { tempC: t, valid: v };
      const role = state.config?.tempRoles?.[i] ?? "";
      const defGpio = i <= 3 ? String(i) : "";
      const selectedGpio = state.config?.dallasGpios?.[i];
      const gpioStr = selectedGpio === undefined || selectedGpio === null ? defGpio : String(selectedGpio);
      const gpioNum = gpioStr === "" ? -1 : Number(gpioStr);
      const selectedRom = state.config?.dallasAddrs?.[i] || "";
      let idTitle = "";
      if (gpioNum >= 0 && gpioNum <= 3) {
        const bus = getDallasBusByGpio(gpioNum);
        const roms = listDallasRomsOnBus(bus);
        if (roms.length) idTitle = `Detekované ROM na ${gpioStr}: ${roms.join(", ")}`;
      }
      rows.push({
        source: "GPIO",
        nameCell: `<span class="mono">TEMP${i + 1}</span>`,
        roleCell: `<select data-path="tempRoles.${i}">${roleOptionsHtml(role)}</select>`,
        idCell: `<div class="inline wrap">
          <select class="mono" data-path="dallasGpios.${i}">${buildDallasGpioOptions(gpioStr)}</select>
          <select class="mono" data-path="dallasAddrs.${i}" title="${escapeHtml(idTitle)}">${buildDallasRomOptions(gpioNum, selectedRom)}</select>
        </div>`,
        tempCell: `<span id="thermoLive_gpio_${i}" class="mono">—</span>`,
        liveEntry: entry,
        liveId: `thermoLive_gpio_${i}`,
      });
      continue;
    }
    const raw = statusTemps[i];
    const entry = typeof raw === "number" ? { tempC: raw, valid: true } : raw || {};

    const role = state.config?.tempRoles?.[i] ?? "";

    const defGpio = i <= 3 ? String(i) : "";
    const selectedGpio = state.config?.dallasGpios?.[i];
    const gpioStr = selectedGpio === undefined || selectedGpio === null ? defGpio : String(selectedGpio);
    const gpioNum = gpioStr === "" ? -1 : Number(gpioStr);

    const selectedRom = state.config?.dallasAddrs?.[i] || "";

    // Tooltip with all detected ROMs on selected bus
    let idTitle = "";
    if (gpioNum >= 0 && gpioNum <= 3) {
      const bus = getDallasBusByGpio(gpioNum);
      const roms = listDallasRomsOnBus(bus);
      if (roms.length) {
                idTitle = `DS18B20 ROM(s) on GPIO${gpioNum}:\n${roms.join("\n")}`;
      }
    }

    rows.push({
      source: "GPIO",
      nameCell: `<span class="mono">TEMP${i + 1}</span>`,
      roleCell: `<select data-path="tempRoles.${i}">${roleOptionsHtml(role)}</select>`,
      idCell: `<div class="inline wrap" title="${escapeHtml(idTitle)}">
        <select class="mono thermo-gpio-select" data-path="dallasGpios.${i}">${buildDallasGpioOptions(gpioStr)}</select>
        <select class="mono thermo-rom-select" data-path="dallasAddrs.${i}" ${gpioNum < 0 ? "disabled" : ""}>${buildDallasRomOptions(gpioNum, selectedRom)}</select>
      </div>`,
      tempCell: `<span id="thermoLive_gpio_${i}" class="mono">—</span>`,
      liveEntry: entry,
      liveId: `thermoLive_gpio_${i}`,
    });
  }

  // BLE
  const meteo = state.bleStatus?.meteo || {};
  const activeMac = meteo.activeMac || meteo.mac || "";
  const connected = !!meteo.connected;
  const bleId = state.config?.thermometers?.ble?.id || activeMac || "";
  const bleRole = state.config?.thermometers?.ble?.role || (connected ? "outdoor" : "");

  const bleReading = meteo.reading || null;
  const bleLive = bleReading
    ? { tempC: bleReading.tempC, valid: !!meteo.fix, ageMs: bleReading.ageMs }
    : (state.dash?.bleTemps || [])[0];

  rows.push({
    source: "BLE",
    nameCell: `<input data-path="thermometers.ble.name" placeholder="Název" />`,
    roleCell: `<select data-path="thermometers.ble.role">${roleOptionsHtml(bleRole)}</select>`,
    idCell: `<div class="inline wrap">
      <input class="mono" data-path="thermometers.ble.id" placeholder="BLE MAC" ${connected ? "readonly" : ""} />
      <span class="mono muted" title="Aktivní BLE MAC">${escapeHtml(activeMac || "—")}</span>
    </div>`,
    tempCell: `<span id="thermoLive_ble_0" class="mono">—</span>`,
    liveEntry: bleLive,
    liveId: "thermoLive_ble_0",
  });

  // MQTT slots
  const mqtt = state.config?.thermometers?.mqtt || [];
  const mqttLive = state.dash?.mqttTemps || [];
  mqtt.forEach((slot, i) => {
    const topic = slot?.topic || "";
    const role = slot?.role || "";
    rows.push({
      source: "MQTT",
      nameCell: `<input data-path="thermometers.mqtt.${i}.name" placeholder="Název" />`,
      roleCell: `<select data-path="thermometers.mqtt.${i}.role">${roleOptionsHtml(role)}</select>`,
      idCell: `<input class="mono" data-path="thermometers.mqtt.${i}.topic" placeholder="MQTT topic" />`,
      tempCell: `<span id="thermoLive_mqtt_${i}" class="mono">—</span>`,
      liveEntry: mqttLive[i],
      liveId: `thermoLive_mqtt_${i}`,
    });
  });

  rows.forEach((row) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escapeHtml(row.source)}</td>
      <td>${row.nameCell}</td>
      <td>${row.roleCell}</td>
      <td>${row.idCell}</td>
      <td>${row.tempCell}</td>
    `;
    body.appendChild(tr);
  });

  // bind inputs created above
  const table = $("thermometerTable");
  bindInputs(table, state.config);

  // If user changes DS18B20 GPIO bus, re-render to refresh ROM dropdown options
  table.onchange = (ev) => {
    const t = ev?.target;
    if (t && t.classList && t.classList.contains("thermo-gpio-select")) {
      // bindInputs already updated state.config.*; now refresh UI options
      renderThermometerOverviewTable();
    }
  };

  // fill live values
  rows.forEach((row) => {
    const el = document.getElementById(row.liveId);
    if (!el) return;
    const info = formatThermoStatus(row.liveEntry);
    el.textContent = info.text;
    el.classList.toggle("muted", info.muted);
  });
}


function formatThermoLive(entry) {
  if (!entry) {
    return "—";
  }
  const obj = typeof entry === "number" ? { tempC: entry, valid: true } : entry;
  const temp = obj.tempC !== undefined && obj.tempC !== null ? `${obj.tempC} °C` : "—";
  const valid = obj.valid !== false;
  const age = obj.ageMs !== undefined && obj.ageMs !== null ? ` • age ${obj.ageMs} ms` : "";
  const info = valid ? "valid" : "invalid";
  // If temp is missing, keep status info visible
  return `${info} • ${temp}${age}`;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function updateThermometerTableLive() {
  function getDallasBusByGpio(gpio) {
    const arr = state.dash?.dallas;
    if (!Array.isArray(arr)) return null;
    return arr.find((b) => Number(b?.gpio) === Number(gpio)) || null;
  }

  function pickDallasRomFromBus(bus) {
    const devs = Array.isArray(bus?.devices) ? bus.devices : [];
    if (!devs.length) return { rom: "—", title: "" };
    const valid = devs.find((d) => d && d.valid && d.rom) || devs.find((d) => d && d.rom) || null;
    const rom = valid?.rom ? String(valid.rom) : "—";
    const list = devs
      .map((d) => (d?.rom ? String(d.rom) : ""))
      .filter((s) => s.length)
      .join("\n");
    const title = list ? `DS18B20 ROM(s) on GPIO${bus?.gpio}:\n${list}` : "";
    const suffix = devs.length > 1 ? ` (x${devs.length})` : "";
    return { rom: rom + suffix, title };
  }

  // GPIO TEMP1..8
  for (let i = 0; i < 8; i += 1) {
    // Use nullish coalescing so valid zero values (0) don't get treated as "no data"
    const raw = state.status?.temps?.[i] ?? null;
    const entry = typeof raw === "number" ? { tempC: raw, valid: true } : raw || null;

    const liveEl = document.getElementById(`thermoLive_gpio_${i}`);
    if (liveEl) {
      const info = formatThermoStatus(entry);
      liveEl.textContent = info.text;
      liveEl.classList.toggle("muted", info.muted);
    }

    const idEl = document.getElementById(`thermoId_gpio_${i}`);
    if (idEl) {
      if (i <= 3) {
        const bus = getDallasBusByGpio(i);
        if (bus) {
          const picked = pickDallasRomFromBus(bus);
          idEl.textContent = picked.rom;
          if (picked.title) idEl.title = picked.title;
        } else {
          idEl.textContent = entry?.id || entry?.rom || entry?.address || entry?.addr || "—";
          idEl.title = "";
        }
      } else {
        idEl.textContent = entry?.id || entry?.rom || entry?.address || entry?.addr || "—";
        idEl.title = "";
      }
    }
  }

  // BLE (single)
  {
    const meteo = state.bleStatus?.meteo || null;
    const entry = meteo?.reading
      ? { tempC: meteo.reading.tempC, valid: !!meteo.fix, ageMs: meteo.reading.ageMs }
      : (state.dash?.bleTemps || [])[0] || null;
    const liveEl = document.getElementById("thermoLive_ble_0");
    if (liveEl) {
      const info = formatThermoStatus(entry);
      liveEl.textContent = info.text;
      liveEl.classList.toggle("muted", info.muted);
    }
  }

  // MQTT slots
  const mqttTemps = state.dash?.mqttTemps || [];
  for (let i = 0; i < mqttTemps.length; i += 1) {
    const entry = mqttTemps[i] || null;
    const liveEl = document.getElementById(`thermoLive_mqtt_${i}`);
    if (liveEl) {
      const info = formatThermoStatus(entry);
      liveEl.textContent = info.text;
      liveEl.classList.toggle("muted", info.muted);
    }
  }
}

function renderThermometerRoles() {
  const container = $("thermometerRoles");
  if (!container) {
    return;
  }
  container.innerHTML = "";
  state.config.tempRoles.forEach((role, index) => {
    const row = document.createElement("div");
    row.className = "form-grid";
    row.innerHTML = `
      <label class="field"><span>TEMP${index + 1} role</span>
        <input list="thermoRoles" data-path="tempRoles.${index}" />
      </label>
    `;
    container.appendChild(row);
  });
  bindInputs(container, state.config);
}

function renderDallasDiag() {
  const container = $("dallasDiag");
  if (!container) {
    return;
  }
  container.innerHTML = "";
  const dallas = state.dash?.dallas || [];
  if (!dallas.length) {
    container.innerHTML = "<div class=\"hint\">Dallas data nejsou k dispozici.</div>";
    return;
  }
  dallas.forEach((bus, index) => {
    const card = document.createElement("div");
    card.className = "card";
    const devices = bus.devices || [];
    const list = devices
      .map((dev) => {
        const temp = dev.tempC !== undefined ? `${dev.tempC.toFixed(2)} °C` : "—";
        const valid = dev.valid ? "valid" : "invalid";
        return `<div class="inline"><span>${dev.rom || "—"}</span><span>${temp}</span><span>${valid}</span></div>`;
      })
      .join("");
    card.innerHTML = `
      <h3>GPIO ${index}</h3>
      ${list || "<div class=\"hint\">Žádná zařízení.</div>"}
    `;
    container.appendChild(card);
  });
}

function renderEquithermSources() {
  const container = $("eqSources");
  if (!container) {
    return;
  }
  container.innerHTML = "";

  state.config.equitherm = state.config.equitherm || {};
  const eq = state.config.equitherm;

  // --- Legacy migration (old UI used equitherm.sources.* with snake_case keys) ---
  // We keep eq.sources untouched, but if the new structure is missing we seed it from legacy.
  const legacy = eq.sources && typeof eq.sources === "object" ? eq.sources : null;
  const legacyMap = {
    outdoor: "outdoor",
    flow: "flow",
    boiler_in: "boilerIn",
    aku_top: "akuTop",
    aku_mid: "akuMid",
    aku_bottom: "akuBottom",
  };
  if (legacy) {
    Object.entries(legacyMap).forEach(([oldKey, newKey]) => {
      const old = legacy[oldKey];
      if (!old || typeof old !== "object") return;
      eq[newKey] = eq[newKey] || {};
      const cur = eq[newKey];
      if (!cur.source && old.source) {
        let src = String(old.source || "");
        if (src === "mqtt0") {
          cur.source = "mqtt";
          cur.mqttIdx = 1;
        } else if (src === "mqtt1") {
          cur.source = "mqtt";
          cur.mqttIdx = 2;
        } else {
          cur.source = src;
        }
      }
      if (cur.gpio === undefined && old.gpio !== undefined) cur.gpio = old.gpio;
      if (!cur.rom && (old.rom || old.addr)) cur.rom = old.rom || old.addr;
      if (!cur.topic && old.topic) cur.topic = old.topic;
      if (!cur.jsonKey && old.jsonKey) cur.jsonKey = old.jsonKey;
      if (cur.maxAgeMs === undefined && old.maxAgeMs !== undefined) cur.maxAgeMs = old.maxAgeMs;
      if (!cur.bleId && old.bleId) cur.bleId = old.bleId;
    });
  }

  const makeSourceOptions = () => {
    const out = [];
    out.push('<option value="none">—</option>');
    out.push('<optgroup label="TEMP">');
    for (let i = 1; i <= 8; i += 1) {
      out.push(`<option value="temp${i}">TEMP${i}</option>`);
    }
    out.push('</optgroup>');
    out.push('<optgroup label="Dallas">');
    out.push('<option value="dallas">Dallas / DS18B20</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="MQTT">');
    out.push('<option value="mqtt">MQTT</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="BLE">');
    out.push('<option value="ble">BLE meteo</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="OpenTherm">');
    out.push('<option value="opentherm_boiler">OpenTherm boiler</option>');
    out.push('<option value="opentherm_return">OpenTherm return</option>');
    out.push('</optgroup>');
    return out.join("\n");
  };

  const makeDallasBusOptions = () => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const out = [];
    const maxBus = Math.max(4, gpios.length);
    for (let i = 0; i < maxBus; i += 1) {
      const gpio = gpios[i];
      const suffix = gpio ? ` (GPIO${gpio})` : "";
      out.push(`<option value="${i}">Bus ${i}${suffix}</option>`);
    }
    return out.join("\n");
  };

  const normalizeSource = (s) => {
    const v = String(s || "none").trim();
    if (!v.length) return "none";
    return v;
  };

  const normalizeDallasBus = (value) => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const v = Number(value || 0);
    if (Number.isFinite(v) && v >= 0 && v <= 3) return v;
    // If old UI stored physical GPIO, try to map it to bus index
    if (Number.isFinite(v) && v > 3 && gpios.length) {
      const idx = gpios.indexOf(v);
      if (idx >= 0) return idx;
    }
    return 0;
  };

  // Config keys expected by firmware (LogicController.cpp): outdoor/flow/boilerIn (+ AKU... but moved to AKU page)
  const sources = [
    { key: "outdoor", label: "Venkovní teplota" },
    { key: "flow", label: "Flow (výstup kotle / za směšovačem)" },
    { key: "boilerIn", label: "Boiler IN (zpátečka / feedback)" },
  ];

  sources.forEach((item) => {
    eq[item.key] = eq[item.key] || {};
    const cfg = eq[item.key];

    // Normalize source values from older variants
    if (cfg.source === "mqtt0") {
      cfg.source = "mqtt";
      cfg.mqttIdx = 1;
    } else if (cfg.source === "mqtt1") {
      cfg.source = "mqtt";
      cfg.mqttIdx = 2;
    }

    const card = document.createElement("div");
    card.className = "card compact";
    card.innerHTML = `
      <h3>${item.label}</h3>
      <div class="form-grid">
        <label class="field"><span>Source</span>
          <select class="eq-source">${makeSourceOptions()}</select>
        </label>

        <div class="eq-source-block dallas">
          <label class="field"><span>Dallas bus</span>
            <select class="eq-dallas-gpio">${makeDallasBusOptions()}</select>
          </label>
          <label class="field"><span>ROM (hex)</span>
            <input class="eq-dallas-rom" placeholder="28ff..." list="dallas-${item.key}" />
            <datalist id="dallas-${item.key}"></datalist>
          </label>
        </div>

        <div class="eq-source-block mqtt">
          <label class="field"><span>MQTT preset</span>
            <select class="eq-mqtt-preset">
              <option value="1">MQTT #1 (z Teploměry)</option>
              <option value="2">MQTT #2 (z Teploměry)</option>
              <option value="0">Custom topic</option>
            </select>
          </label>

          <div class="eq-mqtt-custom">
            <label class="field"><span>Topic</span>
              <input class="eq-mqtt-topic" placeholder="home/sensors/outdoor" />
            </label>
            <label class="field"><span>JSON key</span>
              <input class="eq-mqtt-key" placeholder="temp" />
            </label>
          </div>
        </div>

        <div class="eq-source-block ble">
          <label class="field"><span>BLE ID (volitelné)</span>
            <input class="eq-ble-id" placeholder="meteo.tempC / (prázdné)" />
          </label>
          <div class="hint">Použije BLE meteo (minimálně teplota). Pokud ID necháš prázdné, použije se default.</div>
        </div>

        <label class="field"><span>Max age ms</span>
          <input type="number" min="0" class="eq-max-age" placeholder="900000" />
        </label>
      </div>
    `;
    container.appendChild(card);

    const sourceSelect = card.querySelector(".eq-source");
    const dallasBlock = card.querySelector(".eq-source-block.dallas");
    const mqttBlock = card.querySelector(".eq-source-block.mqtt");
    const bleBlock = card.querySelector(".eq-source-block.ble");

    const dallasBus = card.querySelector(".eq-dallas-gpio");
    const dallasRom = card.querySelector(".eq-dallas-rom");
    const mqttPreset = card.querySelector(".eq-mqtt-preset");
    const mqttCustom = card.querySelector(".eq-mqtt-custom");
    const mqttTopic = card.querySelector(".eq-mqtt-topic");
    const mqttKey = card.querySelector(".eq-mqtt-key");
    const bleId = card.querySelector(".eq-ble-id");
    const maxAge = card.querySelector(".eq-max-age");

    const syncDallasList = () => {
      const list = card.querySelector("datalist");
      const addrs = state.config?.dallasAddrs || [];
      list.innerHTML = "";
      addrs.forEach((addr) => {
        const opt = document.createElement("option");
        opt.value = addr;
        list.appendChild(opt);
      });
    };

    const showFor = (type) => {
      const t = normalizeSource(type);
      dallasBlock.style.display = t === "dallas" ? "block" : "none";
      mqttBlock.style.display = t === "mqtt" ? "block" : "none";
      bleBlock.style.display = t === "ble" ? "block" : "none";
      if (mqttCustom) {
        mqttCustom.style.display = t === "mqtt" && String(mqttPreset.value) === "0" ? "block" : "none";
      }
    };

    const fill = () => {
      const src = normalizeSource(cfg.source);
      sourceSelect.value = src;
      if (!Array.from(sourceSelect.options).some((o) => o.value === src)) {
        const opt = document.createElement("option");
        opt.value = src;
        opt.textContent = src;
        sourceSelect.appendChild(opt);
        sourceSelect.value = src;
      }

      const bus = normalizeDallasBus(cfg.gpio);
      dallasBus.value = String(bus);

      dallasRom.value = String(cfg.rom || cfg.addr || "");

      const idx = Number(cfg.mqttIdx || 0);
      mqttPreset.value = idx >= 1 && idx <= 2 ? String(idx) : "0";
      mqttTopic.value = String(cfg.topic || "");
      mqttKey.value = String(cfg.jsonKey || "");

      bleId.value = String(cfg.bleId || cfg.id || "");

      maxAge.value = cfg.maxAgeMs === undefined || cfg.maxAgeMs === null ? "" : String(cfg.maxAgeMs);

      syncDallasList();
      showFor(src);
    };

    fill();

    sourceSelect.addEventListener("change", () => {
      cfg.source = normalizeSource(sourceSelect.value);
      showFor(cfg.source);
    });

    dallasBus.addEventListener("change", () => {
      cfg.gpio = normalizeDallasBus(dallasBus.value);
    });

    dallasRom.addEventListener("input", () => {
      cfg.rom = String(dallasRom.value || "");
    });

    mqttPreset.addEventListener("change", () => {
      const v = Number(mqttPreset.value || 0);
      cfg.mqttIdx = v >= 1 && v <= 2 ? v : 0;
      if (cfg.mqttIdx) {
        // Pokud je zvolen preset, necháváme topic/jsonKey volitelné (backend použije hodnoty z Teploměry).
        // Zachováme uživatelský custom v polích, ale nebude použit, dokud mqttIdx != 0.
      }
      showFor(cfg.source);
    });

    mqttTopic.addEventListener("input", () => {
      cfg.topic = String(mqttTopic.value || "");
      if (Number(mqttPreset.value || 0) !== 0) {
        mqttPreset.value = "0";
      }
      cfg.mqttIdx = 0;
      showFor(cfg.source);
    });

    mqttKey.addEventListener("input", () => {
      cfg.jsonKey = String(mqttKey.value || "");
      if (Number(mqttPreset.value || 0) !== 0) {
        mqttPreset.value = "0";
      }
      cfg.mqttIdx = 0;
      showFor(cfg.source);
    });

    bleId.addEventListener("input", () => {
      cfg.bleId = String(bleId.value || "");
    });

    maxAge.addEventListener("input", () => {
      cfg.maxAgeMs = maxAge.value === "" ? 0 : Number(maxAge.value);
      if (!Number.isFinite(cfg.maxAgeMs)) cfg.maxAgeMs = 0;
    });
  });

  initEquithermTooltips();
}

function renderBleConfig() {
  const container = $("bleConfig");
  container.innerHTML = `
    <label class="toggle"><input type="checkbox" data-path="enabled" /><span>Enabled</span></label>
    <label class="field"><span>Device name</span><input data-path="deviceName" /></label>
    <label class="toggle"><input type="checkbox" data-path="advertise" /><span>Advertise</span></label>
    <label class="field"><span>Security mode</span><input data-path="securityMode" /></label>
    <label class="field"><span>Passkey</span><input data-path="passkey" /></label>
    <label class="toggle"><input type="checkbox" data-path="allowlistEnforced" /><span>Allowlist enforced</span></label>
    <h3>Meteo klient</h3>
    <label class="toggle"><input type="checkbox" data-path="meteoEnabled" /><span>Meteo enabled</span></label>
    <label class="field"><span>Meteo MAC</span><input data-path="meteoMac" /></label>
    <label class="toggle"><input type="checkbox" data-path="meteoAutoDiscover" /><span>Auto discover</span></label>
    <label class="toggle"><input type="checkbox" data-path="meteoAutoSave" /><span>Auto save</span></label>
    <label class="field"><span>Discover interval (ms)</span><input type="number" data-path="meteoDiscoverIntervalMs" /></label>
    <label class="field"><span>Scan ms</span><input type="number" data-path="meteoScanMs" /></label>
    <label class="field"><span>Reconnect ms</span><input type="number" data-path="meteoReconnectMs" /></label>
    <label class="field"><span>Max connect fails</span><input type="number" data-path="meteoMaxConnectFails" /></label>
    <label class="field"><span>Cooldown ms</span><input type="number" data-path="meteoCooldownMs" /></label>
  `;
  bindInputs(container, state.bleConfig);
}

function renderBuzzerConfig() {
  const container = $("buzzerConfig");
  container.innerHTML = `
    <label class="toggle"><input type="checkbox" data-path="enabled" /><span>Enabled</span></label>
    <label class="toggle"><input type="checkbox" data-path="activeHigh" /><span>Active high</span></label>
    <label class="toggle"><input type="checkbox" data-path="usePwm" /><span>Use PWM</span></label>
    <label class="field"><span>PWM freq (Hz)</span><input type="number" data-path="pwmFreqHz" /></label>
    <label class="field"><span>PWM duty (%)</span><input type="number" data-path="pwmDutyPct" /></label>
    <h3>Eventy</h3>
    <label class="field"><span>control_auto</span><input data-path="events.control_auto" /></label>
    <label class="field"><span>control_manual</span><input data-path="events.control_manual" /></label>
    <label class="field"><span>manual_mode</span><input data-path="events.manual_mode" /></label>
    <label class="field"><span>relay_on</span><input data-path="events.relay_on" /></label>
    <label class="field"><span>relay_off</span><input data-path="events.relay_off" /></label>
    <label class="field"><span>error</span><input data-path="events.error" /></label>
  `;
  bindInputs(container, state.buzzerConfig);

  const testContainer = $("buzzerTests");
  testContainer.innerHTML = "";
  buzzerPatterns.forEach((pattern) => {
    const button = document.createElement("button");
    button.className = "btn secondary";
    button.textContent = pattern;
    button.addEventListener("click", () => sendBuzzerTest(pattern));
    testContainer.appendChild(button);
  });
}

function renderAdvancedJsonEditors() {
  const preview = $("configPreview");
  preview.value = "";
}

function renderModeSelect() {
  const select = $("manualModeSelect");
  if (!select) {
    return;
  }
  select.innerHTML = "";
  const modes = state.config?.modes || [];
  modes.forEach((mode) => {
    const option = document.createElement("option");
    option.value = mode.name || mode.id || "";
    option.textContent = mode.label || mode.name || mode.id || "(bez názvu)";
    select.appendChild(option);
  });
}

function initValvesTooltips() {
  const section = $("valves");
  if (!section) return;

  const getTipForPath = (path) => {
    if (!path) return null;
    if (/^iofunc\.outputs\.\d+\.role$/.test(path)) {
      return `Role výstupu (ventilu).
Použij:
• valve_3way_mix = 3c směšovací (2 relé)
• valve_3way_spring = 3c pružina (1 relé)
Příklad: pro ekviterm směšování je typicky R1/R2 = valve_3way_mix.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.peerRel$/.test(path)) {
      return `Peer relé (druhé relé pro směr).
Příklad: směšovací ventil má master=R1, peer=R2.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.travelTime$/.test(path)) {
      return `Čas přestavení 0% → 100% (v sekundách).
Příklad: 120.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.pulseTime$/.test(path)) {
      return `Délka jednoho pulzu pro Pulse + / Pulse − (v sekundách).
Příklad: 1.5.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.guardTime$/.test(path)) {
      return `Ochranná pauza po pulzu (dokončení dojezdu).
Příklad: 2.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.minSwitchS$/.test(path)) {
      return `Minimální doba mezi sepnutími (ochrana relé).
Příklad: 3.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.invertDir$/.test(path)) {
      return `Prohodí směr cívky A/B (když ventil jede opačně). Nemění škálu 0–100 %.`;
    }
    if (/^iofunc\.outputs\.\d+\.params\.defaultPos$/.test(path)) {
      return `Výchozí poloha po startu.
A = 0%, B = 100%.`;
    }
    return null;
  };

  section.querySelectorAll('label.field > span').forEach((span) => {
    const label = span.closest('label.field');
    if (!label) return;
    const input = label.querySelector('[data-path]');
    if (!input) return;
    const tipText = getTipForPath(input.dataset.path);
    if (!tipText) return;
    if (span.dataset.tipApplied === '1') return;

    const icon = document.createElement('span');
    icon.className = 'tip-i';
    icon.textContent = 'i';
    icon.dataset.tip = tipText;
    span.prepend(icon);
    span.dataset.tipApplied = '1';
  });
}

function updateStatusUI() {
  const status = state.status || {};

  // /api/status provides network under `wifi` with { connected, ip, link: "wifi"|"eth"|"down" }.
  // Older UI variants used `eth.*` which is not present.
  let network = "offline";
  if (status.wifi?.connected) {
    const link = String(status.wifi?.link || "").toLowerCase();
    const ip = status.wifi?.ip || "";
    let label = "NET";
    if (link === "wifi") label = "Wi‑Fi";
    else if (link === "eth") label = "ETH";
    else if (link === "down") label = "offline";
    network = `${label} ${ip}`.trim();
  }
  const page = getActivePage();
  const onDashboard = page === "dashboard";
  const onThermometers = page === "thermometers";
  const onEquitherm = page === "equitherm";
  const onValves = page === "valves";
  const setText = (id, text) => {
    const el = $(id);
    if (el) {
      el.textContent = text;
    }
  };

  setText("statusNetwork", `Síť: ${network}`);
  setText("statusMqtt", `MQTT: ${status.mqtt?.connected ? "online" : "offline"}`);

  const ble = state.bleStatus || null;
  const meteo = ble?.meteo || null;
  const bleOnline = Boolean(meteo?.connected) || Boolean(meteo?.fix) || Number(ble?.server?.connectedCount || 0) > 0 || Boolean(state.dash?.ble?.meteoFix);
  const bleLabel = ble?.enabled === false ? "disabled" : bleOnline ? "online" : "offline";
  setText("statusBle", `BLE: ${bleLabel}`);
  setText("statusMode", `Režim: ${status.systemMode || "?"} / ${status.controlMode || "?"}`);

// ---- Advanced diagnostics (optional UI elements) ----
// Network details
if ($("diagNet")) {
  const link = String(status.wifi?.link || "").toLowerCase();
  const ip = status.wifi?.ip || "—";
  const rssi = typeof status.wifi?.rssi === "number" ? `${status.wifi.rssi} dBm` : "—";
  setText("diagNet", `Síť: ${link || "?"} • IP ${ip}`);
  setText("diagWifiRssi", `Wi‑Fi RSSI: ${rssi}`);
}

// Time / RTC
if ($("diagTime")) {
  const t = status.time || {};
  const valid = t.valid ? "OK" : "neplatný";
  const iso = t.iso || "—";
  const src = t.source || "—";
  const rtc = t.rtcPresent ? "RTC: ano" : "RTC: ne";
  setText("diagTime", `Čas: ${valid} • ${iso} • ${src} • ${rtc}`);
}

// BLE transport health
if ($("diagBle")) {
  const b = status.ble || {};
  const st = b.state || "—";
  const age = typeof b.lastDataAge === "number" ? `${b.lastDataAge} ms` : "—";
  const rec = typeof b.reconnects === "number" ? b.reconnects : "—";
  const fail = typeof b.failCount === "number" ? b.failCount : "—";
  setText("diagBle", `BLE: ${st} • age ${age} • recon ${rec} • fail ${fail}`);
}

// I2C / relay expander diagnostics
if ($("diagI2c")) {
  const i2c = status.diag?.i2c || {};
  const ok = i2c.ok ? "OK" : "FAIL";
  const err = typeof i2c.relayErrors === "number" ? i2c.relayErrors : "—";
  const rec = typeof i2c.relayRecoveries === "number" ? i2c.relayRecoveries : "—";
  const fc = typeof i2c.failCount === "number" ? i2c.failCount : "—";
  const last = i2c.relayLastError ? ` • last: ${i2c.relayLastError}` : "";
  setText("diagI2c", `I2C/Relé: ${ok} • err ${err} • rec ${rec} • failCount ${fc}${last}`);
}

// JSON diagnostics (parsing/config)
if ($("diagJson")) {
  const j = status.diag?.json || {};
  const pe = typeof j.parseErrors === "number" ? j.parseErrors : "—";
  const le = j.lastError ? ` • last: ${j.lastError}` : "";
  setText("diagJson", `JSON: parseErrors ${pe}${le}`);
}

if ($("diagConfig")) {
  const c = status.diag?.config || {};
  const ok = c.ok ? "OK" : "FAIL";
  const w = typeof c.loadWarningsCount === "number" ? c.loadWarningsCount : "—";
  const le = c.lastError ? ` • last: ${c.lastError}` : "";
  setText("diagConfig", `Config: ${ok} • warnings ${w}${le}`);
}

  setText("dashMode", `${status.systemMode || "?"} • ${status.controlMode || "?"}`);

  const eq = status.equitherm || {};
  if (onDashboard) {
    setText("eqOutdoor", formatTemp(eq.outdoorC));
    setText("eqTarget", formatTemp(eq.targetFlowC));
    setText("eqFlow", formatTemp(eq.flowC));
    setText("eqValve", eq.valvePosPct !== undefined ? `${eq.valvePosPct}%` : "—");
    setText(
      "dashEquitherm",
      status.equitherm ? `${eq.enabled ? "ON" : "OFF"} / ${eq.active ? "active" : "idle"} / ${eq.reason || ""}` : "—",
    );

    updateValveDial($("dashEqValveDial"), eq.valvePosPct, null, eq.valveMoving, { moveId: "dashEqValveMove" });

    const tuv = status.tuv || {};
    setText("dashTuv", status.tuv ? `${tuv.enabled ? "ON" : "OFF"} / ${tuv.active ? "active" : "idle"} / ${tuv.reason || ""}` : "—");
    setText("tuvDemand", tuv.demandActive ? "active" : "inactive");
    setText("tuvRelay", tuv.boilerRelayOn ? "ON" : "OFF");
    // Dashboard: 2-polohovy ventil (R3) jako 0/100%
    updateValveDial($("dashTuvValveDial"), tuv.valvePosPct, null, tuv.valveMoving, { binary: true });

    const aku = status.akuHeater || {};
    setText("dashAku", status.akuHeater ? `${aku.enabled ? "ON" : "OFF"} / ${aku.active ? "active" : "idle"} / ${aku.reason || ""}` : "—");

    setText("heapFree", status.heap?.free ?? "—");
    setText("heapMin", status.heap?.minFree ?? "—");
    setText("heapLargest", status.heap?.largest ?? "—");

    renderRelayGrid(status.relays || [], status.valvesList || []);
    renderInputGrid(status.inputs || []);
    const tempsForDash = getTempsForDashboard();
    renderTempGrid(tempsForDash);
    renderDashDhwTemps(tempsForDash);
    renderDashAkuWidget(tempsForDash, status.relays || []);
    drawDashEquithermCurveThrottled();
  }

  if (onEquitherm) {
    // Mini status on Ekviterm page
    setText("eqOutdoorMini", formatTemp(eq.outdoorC));
    setText("eqTargetMini", formatTemp(eq.targetFlowC));
    setText("eqFlowMini", formatTemp(eq.flowC));
    setText("eqBoilerMini", formatTemp(eq.boilerInC));
    updateValveDial($("eqValveDial"), eq.valvePosPct, eq.valveTargetPct, eq.valveMoving, { targetId: "eqValveTarget" });
    drawEquithermCurveThrottled();
  }

  if (onThermometers) {
    updateThermometerTableLive();
  }

  const eqStatusEl = $("eqStatus");
  if (eqStatusEl && onEquitherm) {
    eqStatusEl.textContent = JSON.stringify(eq || {}, null, 2);
  }
  const calStatusEl = $("calStatus");
  if (calStatusEl && onValves) {
    calStatusEl.textContent = JSON.stringify(status.valves || status.valvesList || {}, null, 2);
  }
}

function updateDashUI() {
  const bleTemps = state.dash?.bleTemps || [];
  const bleEntry = bleTemps[0];
  const bleMeteoTemp = $("bleMeteoTemp");
  if (bleMeteoTemp) {
    const tempC = bleEntry?.tempC ?? state.dash?.ble?.meteoTempC;
    bleMeteoTemp.textContent = formatTemp(tempC);
  }
  const bleMeteoFix = $("bleMeteoFix");
  if (bleMeteoFix) {
    // NOTE:
    // /api/dash provides { ble: { meteoFix, meteoTempC } } and bleTemps[].valid.
    // /api/status provides BLE transport health (state/age), but not meteoFix.
    bleMeteoFix.textContent = state.dash?.ble?.meteoFix ?? bleEntry?.valid ?? "—";
  }
  const bleMeteoAge = $("bleMeteoAge");
  if (bleMeteoAge) {
    const ageMs = state.status?.ble?.lastDataAge ?? bleEntry?.ageMs;
    bleMeteoAge.textContent = typeof ageMs === "number" ? `${ageMs} ms` : "—";
  }
  if (getActivePage() === "dashboard") {
    const tempsForDash = getTempsForDashboard();
    renderTempGrid(tempsForDash);
    renderDashDhwTemps(tempsForDash);
    renderDashAkuWidget(tempsForDash, state.status?.relays || []);
  }
  if (getActivePage() === "thermometers") {
    updateThermometerTableLive();
  }
}

function renderRelayGrid(relays, valvesList) {
  const container = $("relayGrid");
  if (!container) {
    return;
  }
  container.innerHTML = "";

  // Relé 1–3 jsou vyhrazená pro 3c ventily (R1+R2 směšovací, R3 přepínací) – v UI se nezobrazují.
  for (let relayId = 4; relayId <= 8; relayId += 1) {
    const idx = relayId - 1;
    const active = Boolean(relays[idx]);
    const chip = document.createElement("button");
    chip.className = `relay-chip ${active ? "active" : ""}`;
    chip.textContent = `R${relayId} ${active ? "ON" : "OFF"}`;
    chip.addEventListener("click", () => toggleRelay(relayId, !active));
    container.appendChild(chip);
  }
}

function renderInputGrid(inputs) {
  const container = $("inputGrid");
  container.innerHTML = "";
  for (let i = 0; i < 8; i += 1) {
    // Vstupy jsou defaultně ACTIVE LOW (LOW = aktivní)
    const raw = inputs[i];
    const active = raw === 0;
    const chip = document.createElement("div");
    chip.className = `input-chip ${active ? "active" : ""}`;
    chip.textContent = `IN${i + 1} ${active ? "ON" : "OFF"}`;
    container.appendChild(chip);
  }
}


// Normalize temperature entry from /api/status (supports legacy shapes)
function normalizeTempEntry(raw) {
  const parseTempNumber = (value) => {
    if (value === undefined || value === null) return null;
    if (typeof value === "number") return Number.isFinite(value) ? value : null;
    if (typeof value === "string") {
      const cleaned = value.trim().replace(",", ".");
      const n = Number(cleaned);
      if (Number.isFinite(n)) return n;
      const f = parseFloat(cleaned);
      return Number.isFinite(f) ? f : null;
    }
    const n = Number(value);
    return Number.isFinite(n) ? n : null;
  };

  if (raw === undefined || raw === null) return { tempC: null, valid: false, ageMs: undefined };
  if (typeof raw === "number") return { tempC: raw, valid: true, ageMs: undefined };

  // Legacy/compact shapes: [tempC, valid] or [valid, tempC]
  if (Array.isArray(raw)) {
    const a = raw;
    let tempCandidate = null;
    let validCandidate = true;

    if (a.length >= 2) {
      if (typeof a[0] === "boolean") {
        validCandidate = a[0];
        tempCandidate = a[1];
      } else if (typeof a[1] === "boolean") {
        tempCandidate = a[0];
        validCandidate = a[1];
      } else {
        tempCandidate = a[0];
        validCandidate = true;
      }
    } else if (a.length === 1) {
      tempCandidate = a[0];
      validCandidate = true;
    }

    return { tempC: parseTempNumber(tempCandidate), valid: validCandidate !== false, ageMs: undefined };
  }

  if (typeof raw !== "object") return { tempC: null, valid: false, ageMs: undefined };

  // Common shapes: { valid, tempC } or { value, valid } or { c, valid }
  const tempCandidate =
    raw.tempC !== undefined
      ? raw.tempC
      : raw.value !== undefined
        ? raw.value
        : raw.c !== undefined
          ? raw.c
          : raw.t !== undefined
            ? raw.t
            : raw.temp !== undefined
              ? raw.temp
              : null;

  const valid = raw.valid !== false && raw.ok !== false;
  const ageMs = raw.ageMs ?? raw.age ?? raw.age_ms;

  return { tempC: parseTempNumber(tempCandidate), valid, ageMs };
}

function buildThermometerEntries(temps) {
  const entries = [];

  // TEMP1..8 (GPIO)
  for (let i = 0; i < 8; i += 1) {
    const raw = temps?.[i];
    const entry = normalizeTempEntry(raw);
    const role = state.config?.tempRoles?.[i] ?? "";
    entries.push({
      title: `TEMP${i + 1}`,
      role,
      tempC: entry.tempC,
      valid: entry.valid !== false,
    });
  }

  // BLE thermometer (meteo)
  const meteo = state.bleStatus?.meteo || {};
  const bleReading = meteo.reading || null;
  const bleLive = bleReading
    ? { tempC: bleReading.tempC, valid: !!meteo.fix }
    : (state.dash?.bleTemps || [])[0] || null;
  const bleRole = state.config?.thermometers?.ble?.role || "";
  if (state.config?.thermometers?.ble?.id || meteo.activeMac || meteo.mac) {
    entries.push({
      title: state.config?.thermometers?.ble?.name || "BLE",
      role: bleRole,
      tempC: bleLive?.tempC,
      valid: bleLive?.valid !== false,
    });
  }

  // MQTT thermometers
  const mqttCfg = state.config?.thermometers?.mqtt || [];
  const mqttLive = state.dash?.mqttTemps || [];
  mqttCfg.forEach((slot, idx) => {
    if (!slot) return;
    const name = slot.name || `MQTT${idx + 1}`;
    const live = mqttLive[idx] || null;
    entries.push({
      title: name,
      role: slot.role || "",
      tempC: live?.tempC,
      valid: live?.valid !== false,
    });
  });

  return entries;
}

function renderTempGrid(temps) {
  const container = $("tempGrid");
  if (!container) return;
  container.innerHTML = "";

  const entries = buildThermometerEntries(temps);

  // Render (always show 8 slots so user sees wiring/health even if missing data)
  entries.forEach((e) => {
    const chip = document.createElement("div");
    chip.className = `temp-chip ${e.valid ? "" : "invalid"}`;
    const roleLabel = getThermoRoleLabel(e.role);

    // Use formatTemp when we have a number; otherwise show placeholder
    const mainValue =
      e.tempC === undefined || e.tempC === null || Number.isNaN(Number(e.tempC))
        ? "—"
        : formatTemp(e.tempC);

    chip.innerHTML = `
      <div class="temp-main">${escapeHtml(e.title)} ${escapeHtml(mainValue)}</div>
      <div class="temp-role">${escapeHtml(roleLabel)}</div>
    `;
    container.appendChild(chip);
  });
}

function renderDashDhwTemps(temps) {
  const container = $("dashDhwTemps");
  if (!container) return;

  const entries = buildThermometerEntries(temps)
    .filter((e) => normalizeThermoRole(e.role) === "dhw")
    .filter((e) => e.tempC !== undefined && e.tempC !== null);

  if (!entries.length) {
    container.innerHTML = '<div><span class="label">Nádrž TUV</span><span>—</span></div>';
    return;
  }

  // Render up to 3 temperatures mapped to the DHW role
  container.innerHTML = entries
    .slice(0, 3)
    .map((e) => {
      const label = e.title || "DHW";
      return `<div><span class="label">${escapeHtml(label)}</span><span>${escapeHtml(formatTemp(e.tempC))}</span></div>`;
    })
    .join("");
}



function renderDashAkuWidget(temps, relays) {
  const topEl = $("dashAkuTop");
  const midEl = $("dashAkuMid");
  const botEl = $("dashAkuBottom");
  const relayEl = $("dashAkuRelay");
  if (!topEl || !midEl || !botEl || !relayEl) {
    return;
  }

  const entries = buildThermometerEntries(temps);
  const pick = (role) =>
    entries
      .filter((e) => normalizeThermoRole(e.role) === role)
      .filter((e) => e.tempC !== undefined && e.tempC !== null)
      .shift();

  const top = pick('aku_top');
  const mid = pick('aku_mid');
  const bot = pick('aku_bottom');

  topEl.textContent = top ? formatTemp(top.tempC) : '—';
  midEl.textContent = mid ? formatTemp(mid.tempC) : '—';
  botEl.textContent = bot ? formatTemp(bot.tempC) : '—';

  const r8 = Array.isArray(relays) ? Boolean(relays[7]) : false;
  relayEl.textContent = r8 ? 'ON' : 'OFF';
}
function renderCaps() {
  document.querySelectorAll("[data-cap]").forEach((section) => {
    const cap = section.dataset.cap;
    if (cap && state.caps && state.caps[cap] === false) {
      section.style.display = "none";
    }
  });
  $("capsDump").textContent = JSON.stringify(state.caps, null, 2);
}

function getConfiguredValveOutputs() {
  const outputs = state.config?.iofunc?.outputs || [];
  return outputs
    .map((out, index) => {
      const role = String(out?.role || "");
      const params = out?.params || {};
      const master = index + 1;
      const peerRaw = params.peerRel ?? params.partnerRelay;
      const peerFallback = master < 8 ? master + 1 : 0;
      const peer = Number.isFinite(peerRaw) ? Number(peerRaw) : peerFallback;
      return { role, master, peer, params };
    })
    .filter((v) => v.role);
}



function renderAkuSources() {
  const container = $("akuSources");
  if (!container) {
    return;
  }
  container.innerHTML = "";

  state.config.equitherm = state.config.equitherm || {};
  const eq = state.config.equitherm;

  const makeSourceOptions = () => {
    const out = [];
    out.push('<option value="none">None</option>');
    out.push('<optgroup label="TEMP (role)">');
    thermoRoleOptions.forEach((r) => {
      if (!r.value) return;
      out.push(`<option value="temp_role:${r.value}">${r.label}</option>`);
    });
    out.push('</optgroup>');
    out.push('<optgroup label="Dallas">');
    out.push('<option value="dallas">Dallas / DS18B20</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="MQTT">');
    out.push('<option value="mqtt">MQTT</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="BLE">');
    out.push('<option value="ble">BLE meteo</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="OpenTherm">');
    out.push('<option value="opentherm_boiler">OpenTherm boiler</option>');
    out.push('<option value="opentherm_return">OpenTherm return</option>');
    out.push('</optgroup>');
    return out.join("\n");
  };

  const makeDallasBusOptions = () => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const out = [];
    const maxBus = Math.max(4, gpios.length);
    for (let i = 0; i < maxBus; i += 1) {
      const gpio = gpios[i];
      const suffix = gpio ? ` (GPIO${gpio})` : "";
      out.push(`<option value="${i}">Bus ${i}${suffix}</option>`);
    }
    return out.join("\n");
  };

  const normalizeSource = (s) => {
    const v = String(s || "none").trim();
    if (!v.length) return "none";
    return v;
  };

  const normalizeDallasBus = (value) => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const v = Number(value || 0);
    if (Number.isFinite(v) && v >= 0 && v <= 3) return v;
    if (Number.isFinite(v) && v > 3 && gpios.length) {
      const idx = gpios.indexOf(v);
      if (idx >= 0) return idx;
    }
    return 0;
  };

  const sources = [
    { key: "akuTop", label: "Nádrž AKU – horní" },
    { key: "akuMid", label: "Nádrž AKU – uprostřed" },
    { key: "akuBottom", label: "Nádrž AKU – dolní" },
  ];

  sources.forEach((item) => {
    eq[item.key] = eq[item.key] || {};
    const cfg = eq[item.key];

    if (cfg.source === "mqtt0") {
      cfg.source = "mqtt";
      cfg.mqttIdx = 1;
    } else if (cfg.source === "mqtt1") {
      cfg.source = "mqtt";
      cfg.mqttIdx = 2;
    }

    const card = document.createElement("div");
    card.className = "card compact";
    card.innerHTML = `
      <h3>${item.label}</h3>
      <div class="form-grid">
        <label class="field"><span>Source</span>
          <select class="eq-source">${makeSourceOptions()}</select>
        </label>

        <div class="eq-source-block dallas">
          <label class="field"><span>Dallas bus</span>
            <select class="eq-dallas-gpio">${makeDallasBusOptions()}</select>
          </label>
          <label class="field"><span>ROM (hex)</span>
            <input class="eq-dallas-rom" placeholder="28ff..." list="dallas-${item.key}" />
            <datalist id="dallas-${item.key}"></datalist>
          </label>
        </div>

        <div class="eq-source-block mqtt">
          <label class="field"><span>MQTT preset</span>
            <select class="eq-mqtt-preset">
              <option value="">—</option>
              <option value="1">MQTT1</option>
              <option value="2">MQTT2</option>
            </select>
          </label>
          <label class="field"><span>Topic (custom)</span>
            <input class="eq-mqtt-topic" placeholder="home/sensors/..." />
          </label>
          <label class="field"><span>JSON key</span>
            <input class="eq-mqtt-key" placeholder="temp" />
          </label>
        </div>

        <div class="eq-source-block ble">
          <label class="field"><span>BLE id</span>
            <input class="eq-ble-id" placeholder="meteo.tempC" />
          </label>
        </div>

        <label class="field"><span>Max age (ms)</span>
          <input class="eq-max-age" type="number" min="0" />
        </label>
      </div>
    `;

    container.appendChild(card);

    const select = card.querySelector(".eq-source");
    const maxAge = card.querySelector(".eq-max-age");
    const dallasGpio = card.querySelector(".eq-dallas-gpio");
    const dallasRom = card.querySelector(".eq-dallas-rom");
    const mqttPreset = card.querySelector(".eq-mqtt-preset");
    const mqttTopic = card.querySelector(".eq-mqtt-topic");
    const mqttKey = card.querySelector(".eq-mqtt-key");
    const bleId = card.querySelector(".eq-ble-id");

    const applyVisibility = () => {
      const src = normalizeSource(cfg.source);
      card.querySelectorAll('.eq-source-block').forEach((b) => b.classList.add('hidden'));
      if (src === 'dallas') card.querySelector('.eq-source-block.dallas')?.classList.remove('hidden');
      if (src === 'mqtt') card.querySelector('.eq-source-block.mqtt')?.classList.remove('hidden');
      if (src === 'ble') card.querySelector('.eq-source-block.ble')?.classList.remove('hidden');
    };

    // Fill datalist with currently discovered ROMs (for all 4 buses)
    const list = card.querySelector(`#dallas-${item.key}`);
    const discovered = state.dash?.dallasAddrs || [];
    (discovered || []).forEach((rom) => {
      if (!rom) return;
      const opt = document.createElement('option');
      opt.value = String(rom);
      list.appendChild(opt);
    });

    select.value = normalizeSource(cfg.source);
    maxAge.value = cfg.maxAgeMs ?? 900000;
    dallasGpio.value = normalizeDallasBus(cfg.gpio ?? cfg.bus ?? 0);
    dallasRom.value = cfg.rom || '';
    mqttPreset.value = cfg.mqttIdx ? String(cfg.mqttIdx) : '';
    mqttTopic.value = cfg.topic || '';
    mqttKey.value = cfg.jsonKey || '';
    bleId.value = cfg.bleId || '';

    applyVisibility();

    select.addEventListener('change', () => {
      cfg.source = normalizeSource(select.value);
      applyVisibility();
    });
    maxAge.addEventListener('change', () => {
      cfg.maxAgeMs = Number(maxAge.value || 0);
    });
    dallasGpio.addEventListener('change', () => {
      cfg.gpio = normalizeDallasBus(dallasGpio.value);
    });
    dallasRom.addEventListener('change', () => {
      cfg.rom = String(dallasRom.value || '').toUpperCase().replace(/[^0-9A-F]/g, '');
    });
    mqttPreset.addEventListener('change', () => {
      const v = Number(mqttPreset.value || 0);
      cfg.mqttIdx = v >= 1 && v <= 2 ? v : 0;
    });
    mqttTopic.addEventListener('change', () => {
      cfg.topic = String(mqttTopic.value || '');
    });
    mqttKey.addEventListener('change', () => {
      cfg.jsonKey = String(mqttKey.value || '');
    });
    bleId.addEventListener('change', () => {
      cfg.bleId = String(bleId.value || '');
    });
  });
}
function renderEquithermValveSelect() {
  const select = $("eqValveSelect");
  if (!select) {
    return;
  }
  const eq = state.config?.equitherm || {};
  const current = Number(eq.valveMaster || 0);

  const allowedRoles = new Set(["valve_3way_mix", "valve_3way_2rel", "valve_3way_spring"]);
  const labels = {
    valve_3way_mix: "3c směšovací",
    valve_3way_2rel: "3c 2 relé",
    valve_3way_spring: "3c pružina",
  };

  const valves = getConfiguredValveOutputs().filter((v) => allowedRoles.has(v.role));

  select.innerHTML = "";
  const none = document.createElement("option");
  none.value = "0";
  none.textContent = "— (bez ventilu)";
  select.appendChild(none);

  valves.forEach((v) => {
    const opt = document.createElement("option");
    opt.value = String(v.master);
    const peer = v.peer && v.peer !== v.master ? `/${v.peer}` : "";
    opt.textContent = `R${v.master}${peer} • ${labels[v.role] || v.role}`;
    select.appendChild(opt);
  });

  // Preserve invalid/legacy value (so we don't silently change config)
  if (current && !Array.from(select.options).some((o) => Number(o.value) === current)) {
    const opt = document.createElement("option");
    opt.value = String(current);
    opt.textContent = `R${current} • (nenalezeno v I/O funkcích)`;
    select.appendChild(opt);
  }

  // Update hint
  const hint = $("eqValveHint");
  if (hint) {
    hint.style.display = valves.length ? "none" : "block";
  }
}

function updateValveDial(dialEl, pct, targetPct, moving, options = {}) {
  if (!dialEl) {
    return;
  }
  const binary = Boolean(options.binary);
  let p = Number.isFinite(pct) ? Math.max(0, Math.min(100, Number(pct))) : null;
  if (binary && p !== null) {
    p = p >= 50 ? 100 : 0;
  }
  const t = Number.isFinite(targetPct) ? Math.max(0, Math.min(100, Number(targetPct))) : null;

  const ring = dialEl.querySelector(".valve-dial-ring");
  if (ring) {
    ring.style.transform = `rotate(${(p ?? 0) * 3.6}deg)`;
  }

  const pctEl = dialEl.querySelector(".valve-dial-pct");
  if (pctEl) {
    pctEl.textContent = p === null ? "—%" : `${Math.round(p)}%`;
  }

  // Target label (optional)
  if (options.targetId) {
    const targetEl = document.getElementById(options.targetId);
    if (targetEl) {
      targetEl.textContent = t === null ? "target —%" : `target ${Math.round(t)}%`;
    }
  } else {
    const targetInline = dialEl.querySelector("#eqValveTarget");
    if (targetInline) {
      targetInline.textContent = t === null ? "target —%" : `target ${Math.round(t)}%`;
    }
  }

  // Movement label (optional)
  if (options.moveId) {
    const moveEl = document.getElementById(options.moveId);
    if (moveEl) {
      moveEl.textContent = moving ? "pohyb…" : "";
    }
  } else {
    const sub = dialEl.querySelector(".valve-dial-sub");
    if (sub && sub.id !== "eqValveTarget") {
      sub.textContent = moving ? "pohyb…" : "";
    }
  }

  dialEl.classList.toggle("moving", Boolean(moving));
}

function initEquithermTooltips() {
  const section = $("equitherm");
  if (!section) {
    return;
  }

  const tips = {
    "equitherm.enabled": `Zapnutí ekvitermního řízení.
Příklad: ON pouze v AUTO.`,
    "equitherm.valveMaster": `Vyber směšovací 3c ventil (definovaný v I/O funkcích).
Ventil pak nelze používat jako běžné relé.`,
    "equitherm.minFlow": `Minimální požadovaná teplota otopné vody (clamp).
Příklad: 25 °C.`,
    "equitherm.maxFlow": `Maximální požadovaná teplota otopné vody (clamp).
Příklad: 55 °C.`,
    "equitherm.curveOffsetC": `Posun celé křivky (přidá/uberu °C k výsledku).
Příklad: +2.0 = o 2 °C výš.`,
    "equitherm.slopeDay": `Sklon denní křivky. Vzorec: Tflow = (20 - Tout) * slope + 20 + shift.
Příklad: 1.0.`,
    "equitherm.shiftDay": `Posun denní křivky (shift ve vzorci).
Příklad: 5.0.`,
    "equitherm.slopeNight": `Sklon noční křivky.
Příklad: 1.0.`,
    "equitherm.shiftNight": `Posun noční křivky.
Příklad: 0.0.`,
    "equitherm.deadbandC": `Necitlivost regulace (°C).
Příklad: 0.5 °C.`,
    "equitherm.stepPct": `Krok změny polohy ventilu (%).
Příklad: 4 %.`,
    "equitherm.controlPeriodMs": `Minimální doba mezi korekcemi (ms).
Příklad: 30000 = 30 s.`,
    "equitherm.maxPct_day": `Maximální otevření ventilu ve dne (%).
Příklad: 100.`,
    "equitherm.maxPct_night": `Maximální otevření ventilu v noci (%).
Příklad: 80.`,
    "equitherm.maxBoilerInC": `Bezpečnostní limit teploty zpátečky / boiler_in.
Příklad: 55 °C.`,
    "equitherm.noFlowDetectEnabled": `Detekce absence průtoku (ochrana proti zbytečnému otevírání).`,
    "equitherm.noFlowTimeoutMs": `Čas bez změny flow, po kterém se vyhodnotí no-flow (ms).
Příklad: 180000 = 3 min.`,
    "equitherm.noFlowTestPeriodMs": `Jak často opakovat test no-flow (ms).
Příklad: 180000.`,

    "equitherm.refs.day.tout1": `Referenční bod den (Tout1).
Použito při dopočtu slope/shift, pokud nejsou zadány.`,
    "equitherm.refs.day.tflow1": `Referenční bod den (Tflow1).`,
    "equitherm.refs.day.tout2": `Referenční bod den (Tout2).`,
    "equitherm.refs.day.tflow2": `Referenční bod den (Tflow2).`,
    "equitherm.refs.night.tout1": `Referenční bod noc (Tout1).`,
    "equitherm.refs.night.tflow1": `Referenční bod noc (Tflow1).`,
    "equitherm.refs.night.tout2": `Referenční bod noc (Tout2).`,
    "equitherm.refs.night.tflow2": `Referenční bod noc (Tflow2).`,
  };

  const sourceFieldTips = {
    source: `Výběr zdroje teploty.
Doporučení: venkovní = role OUTDOOR (TEMP) nebo BLE/MQTT.`,
    maxAgeMs: `Maximální stáří hodnoty (ms).
Příklad: 900000 = 15 min.`,
    gpio: `Dallas bus index (0..3). Bus odpovídá položkám v Dallas nastavení (GPIO).`,
    rom: `ROM adresa DS18B20 (hex).
Prázdné = vezme první validní čidlo na dané sběrnici.`,
    mqttIdx: `Přednastavený MQTT teploměr (1/2) z "Teploměry".
Pokud zvolíš Custom, použije se Topic + JSON key.`,
    topic: `MQTT topic pro custom zdroj.
Příklad: home/sensors/outdoor`,
    jsonKey: `Klíč v MQTT JSON payloadu.
Příklad: temp`,
    bleId: `BLE identifikátor (volitelné).
Příklad: meteo.tempC nebo prázdné pro default.`,
  };

  // data-path tooltips
  section.querySelectorAll('label.field > span').forEach((span) => {
    const label = span.closest('label.field');
    if (!label) return;
    const input = label.querySelector('[data-path]');
    if (!input) return;
    const path = input.dataset.path;
    const tipText = tips[path];
    if (!tipText) return;
    if (span.dataset.tipApplied === '1') return;

    const icon = document.createElement('span');
    icon.className = 'tip-i';
    icon.textContent = 'i';
    icon.dataset.tip = tipText;
    span.prepend(icon);
    span.dataset.tipApplied = '1';
  });

  // Equitherm sources tooltips (dynamic cards)
  section.querySelectorAll('#eqSources label.field > span, #akuSources label.field > span').forEach((span) => {
    const label = span.closest('label.field');
    if (!label) return;
    const input = label.querySelector('input,select');
    if (!input) return;
    const cls = Array.from(input.classList || []);
    let key = null;
    if (cls.includes('eq-source')) key = 'source';
    if (cls.includes('eq-max-age')) key = 'maxAgeMs';
    if (cls.includes('eq-dallas-gpio')) key = 'gpio';
    if (cls.includes('eq-dallas-rom')) key = 'rom';
    if (cls.includes('eq-mqtt-key')) key = 'jsonKey';
    if (cls.includes('eq-mqtt-topic')) key = 'topic';
    if (cls.includes('eq-mqtt-preset')) key = 'mqttIdx';
    if (cls.includes('eq-ble-id')) key = 'bleId';
    const tipText = key ? sourceFieldTips[key] : null;
    if (!tipText) return;
    if (span.dataset.tipApplied === '1') return;

    const icon = document.createElement('span');
    icon.className = 'tip-i';
    icon.textContent = 'i';
    icon.dataset.tip = tipText;
    span.prepend(icon);
    span.dataset.tipApplied = '1';
  });
}

function initTuvBindings() {
  const section = $("tuv");
  if (!section) return;

  // Keep tuv.valveTargetPct aligned with bypassPct (compatibility for older configs).
  const bypassPct = section.querySelector('[data-path="tuv.bypassValve.bypassPct"]');
  if (!bypassPct) return;

  const sync = () => {
    state.config = state.config || {};
    state.config.tuv = state.config.tuv || {};
    const v = parseInputValue(bypassPct);
    if (v !== null && v !== undefined) {
      state.config.tuv.valveTargetPct = v;
    }
  };

  bypassPct.addEventListener("input", sync);
  bypassPct.addEventListener("change", sync);
  // initial sync
  sync();
}

function initTuvTooltips() {
  const section = $("tuv");
  if (!section) return;

  // Tooltips on toggle labels (switches) are handled via native "title" attribute.
  // Avoid injecting the "i" icon into toggle <span>, because it disturbs the switch layout.
  // Also cleanup any previously injected icons (in case of hot-reload / partial refresh).
  section.querySelectorAll('label.toggle .tip-i').forEach((el) => el.remove());
  section.querySelectorAll('label.toggle span').forEach((span) => {
    if (span && span.dataset) span.dataset.tipApplied = "0";
  });

  const tips = {
    "tuv.enabled": `Povolí funkci TUV/DHW (arm).
Aktivní režim TUV se spustí až při současně aktivním vstupu IN1 (role dhw_demand).

Při aktivní TUV:
- sepne se R5 (požadavek do kotle)
- přepínací ventil R3 přejde do DHW polohy
- ekvitermní směšovač se dočasně nastaví na zadané %`,

    "tuv.restoreEqValveAfter": `Po ukončení TUV vrátí směšovací ventil ekvitermu zpět na poslední cílovou polohu
(uloženou při přechodu do TUV).`,

    "tuv.bypassValve.enabled": `Doporučeno ponechat zapnuté.
Použije dvojici poloh přepínacího ventilu:
- CH poloha (%) když TUV není aktivní
- DHW poloha (%) při aktivní TUV`,

    "tuv.bypassValve.bypassPct": `Poloha přepínacího ventilu (R3) při aktivní TUV.
Typicky 100 %.`,
    "tuv.bypassValve.chPct": `Poloha přepínacího ventilu (R3) pro topný okruh (CH), když TUV není aktivní.
Typicky 0 %.`,
    "tuv.bypassValve.invert": `Invertuje směr (100 − hodnota).
Použij, pokud je ventil zapojen obráceně.`,

    "tuv.eqValveTargetPct": `Dočasná poloha směšovacího ventilu ekvitermu během ohřevu TUV.
Typicky 0 % (zavřít topný okruh).`,
  };

  const addTip = (span, tipText) => {
    if (!span || !tipText) return;
    if (span.dataset.tipApplied === "1") return;
    const icon = document.createElement("span");
    icon.className = "tip-i";
    icon.textContent = "i";
    icon.dataset.tip = tipText;
    span.prepend(icon);
    span.dataset.tipApplied = "1";
  };

  section.querySelectorAll('[data-path]').forEach((input) => {
    const path = input.dataset.path;
    const tipText = tips[path];
    if (!tipText) return;

    const field = input.closest('label.field');
    if (field) {
      const span = field.querySelector('span');
      addTip(span, tipText);
      return;
    }
  });
}



function eqSlopeShiftFromRefs(refs, fallbackSlope, fallbackShift) {
  try {
    const tout1 = Number(refs?.tout1);
    const tflow1 = Number(refs?.tflow1);
    const tout2 = Number(refs?.tout2);
    const tflow2 = Number(refs?.tflow2);
    if (!Number.isFinite(tout1) || !Number.isFinite(tout2) || !Number.isFinite(tflow1) || !Number.isFinite(tflow2) || tout1 === tout2) {
      return { slope: Number(fallbackSlope || 0), shift: Number(fallbackShift || 0), ok: false };
    }
    // Same model as firmware/UI: Tflow = (20 - Tout) * slope + 20 + shift
    // slope = (tflow1 - tflow2) / (tout2 - tout1)
    const slope = (tflow1 - tflow2) / (tout2 - tout1);
    const shift = tflow1 - ((20 - tout1) * slope) - 20;
    if (!Number.isFinite(slope) || !Number.isFinite(shift)) {
      return { slope: Number(fallbackSlope || 0), shift: Number(fallbackShift || 0), ok: false };
    }
    return { slope, shift, ok: true };
  } catch (e) {
    return { slope: Number(fallbackSlope || 0), shift: Number(fallbackShift || 0), ok: false };
  }
}

function eqPickCurveParams(eq, mode /* 'day' | 'night' */) {
  const isDay = mode === "day";
  const slopeKey = isDay ? "slopeDay" : "slopeNight";
  const shiftKey = isDay ? "shiftDay" : "shiftNight";

  // Firmware preference (LogicController.cpp): if slope/shift exist in config,
  // use them. Refs are legacy fallback.
  const hasSlope =
    (eq && (Object.prototype.hasOwnProperty.call(eq, slopeKey) || Object.prototype.hasOwnProperty.call(eq, shiftKey))) ||
    false;

  const fallbackSlope = Number(eq?.[slopeKey] ?? 0);
  const fallbackShift = Number(eq?.[shiftKey] ?? 0);

  if (hasSlope) {
    return { slope: fallbackSlope, shift: fallbackShift, from: "slope" };
  }

  const fit = eqSlopeShiftFromRefs(eq?.refs?.[mode], fallbackSlope, fallbackShift);
  return { slope: Number(fit.slope), shift: Number(fit.shift), from: fit.ok ? "refs" : "fallback" };
}

function eqDrawGrid(ctx, canvas, pad, minX, maxX, minY, maxY) {
  const w = canvas.width;
  const h = canvas.height;
  const plotW = w - pad * 2;
  const plotH = h - pad * 2;
  if (plotW <= 0 || plotH <= 0) return;

  // Nice steps: 5°C for X, 5°C for Y (fallback to 10 when range is huge)
  const xRange = Math.abs(maxX - minX) || 1;
  const yRange = Math.abs(maxY - minY) || 1;
  const xStep = xRange > 80 ? 10 : 5;
  const yStep = yRange > 80 ? 10 : 5;

  const scaleX = plotW / (maxX - minX || 1);
  const scaleY = plotH / (maxY - minY || 1);

  ctx.save();
  ctx.strokeStyle = "rgba(100,116,139,0.18)"; // subtle grid
  ctx.lineWidth = 1;

  // Vertical grid lines
  const xStart = Math.ceil(minX / xStep) * xStep;
  for (let xVal = xStart; xVal <= maxX; xVal += xStep) {
    const x = pad + (xVal - minX) * scaleX;
    ctx.beginPath();
    ctx.moveTo(x, pad);
    ctx.lineTo(x, h - pad);
    ctx.stroke();
  }

  // Horizontal grid lines
  const yStart = Math.ceil(minY / yStep) * yStep;
  for (let yVal = yStart; yVal <= maxY; yVal += yStep) {
    const y = h - pad - (yVal - minY) * scaleY;
    ctx.beginPath();
    ctx.moveTo(pad, y);
    ctx.lineTo(w - pad, y);
    ctx.stroke();
  }

  ctx.restore();
}

function drawEquithermCurve() {
  const canvas = $("eqCurve");
  if (!canvas || !state.config?.equitherm) {
    return;
  }
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const eq = state.config.equitherm;
  const day = eq.refs?.day || {};
  const night = eq.refs?.night || {};
  const minFlow = Number(eq.minFlow ?? 25);
  const maxFlow = Number(eq.maxFlow ?? 55);

  const refPoints = [
    { x: day.tout1, y: day.tflow1 },
    { x: day.tout2, y: day.tflow2 },
    { x: night.tout1, y: night.tflow1 },
    { x: night.tout2, y: night.tflow2 },
  ].filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));

  const xs = refPoints.map((p) => p.x);
  const minX = xs.length ? Math.min(-20, ...xs) : -20;
  const maxX = xs.length ? Math.max(20, ...xs) : 20;

  const pad = 22;
  const scaleX = (canvas.width - pad * 2) / (maxX - minX || 1);
  const offset = Number(eq.curveOffsetC ?? 0);
  const plotMinFlow = minFlow + Math.min(0, offset);
  const plotMaxFlow = maxFlow + Math.max(0, offset);
  const scaleY = (canvas.height - pad * 2) / (plotMaxFlow - plotMinFlow || 1);

  // subtle grid background
  eqDrawGrid(ctx, canvas, pad, minX, maxX, plotMinFlow, plotMaxFlow);

  // axis
  ctx.strokeStyle = "#cbd5f5";
  ctx.beginPath();
  ctx.moveTo(pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, pad);
  ctx.stroke();

  // axis labels (ranges)
  ctx.fillStyle = "#64748b";
  ctx.font = "12px system-ui";
  ctx.textAlign = "left";
  ctx.fillText(`${minX.toFixed(0)}°C`, pad, canvas.height - 4);
  ctx.textAlign = "right";
  ctx.fillText(`${maxX.toFixed(0)}°C`, canvas.width - pad, canvas.height - 4);
  ctx.textAlign = "right";
  ctx.fillText(`${(plotMaxFlow).toFixed(0)}°C`, pad - 6, pad + 4);
  ctx.fillText(`${(plotMinFlow).toFixed(0)}°C`, pad - 6, canvas.height - pad);

  const clampFlow = (value) => Math.min(maxFlow, Math.max(minFlow, value));
  const dayParams = eqPickCurveParams(eq, "day");
  const nightParams = eqPickCurveParams(eq, "night");
  const daySlope = Number(dayParams.slope);
  const dayShift = Number(dayParams.shift);
  const nightSlope = Number(nightParams.slope);
  const nightShift = Number(nightParams.shift);

  // NOTE: same formula as firmware: Tflow = (20 - Tout) * slope + 20 + shift
  const calcFlow = (tout, slope, shift) => {
    const base = clampFlow((20 - tout) * slope + 20 + shift);
    return base + offset;
  };

  const drawLine = (slope, shift, color) => {
    ctx.strokeStyle = color;
    ctx.beginPath();
    for (let i = 0; i <= 40; i += 1) {
      const tout = minX + ((maxX - minX) * i) / 40;
      const flow = calcFlow(tout, slope, shift);
      const x = pad + (tout - minX) * scaleX;
      const y = canvas.height - pad - (flow - plotMinFlow) * scaleY;
      if (i === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
  };

  // day (red), night (blue)
  drawLine(daySlope, dayShift, "#ef4444");
  drawLine(nightSlope, nightShift, "#2563eb");

  // current computed point (based on current outdoor + current mode)
  const currentTout = state.status?.equitherm?.outdoorC;
  if (Number.isFinite(currentTout)) {
    const isNight = Boolean(state.status?.equitherm?.night);
    const slope = Number(isNight ? nightSlope : daySlope);
    const shift = Number(isNight ? nightShift : dayShift);
    const flow = calcFlow(currentTout, slope, shift);

    const x = pad + (currentTout - minX) * scaleX;
    const y = canvas.height - pad - (flow - plotMinFlow) * scaleY;
    ctx.fillStyle = "#0f172a";
    ctx.strokeStyle = "#ffffff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(x, y, 5, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
    ctx.lineWidth = 1;
  }
}

function drawDashEquithermCurve() {
  const canvas = $("dashEqCurve");
  if (!canvas || !state.config?.equitherm) {
    return;
  }
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const eq = state.config.equitherm;
  const day = eq.refs?.day || {};
  const night = eq.refs?.night || {};
  const minFlow = Number(eq.minFlow ?? 25);
  const maxFlow = Number(eq.maxFlow ?? 55);
  const offset = Number(eq.curveOffsetC ?? 0);

  const refPoints = [
    { x: day.tout1, y: day.tflow1 },
    { x: day.tout2, y: day.tflow2 },
    { x: night.tout1, y: night.tflow1 },
    { x: night.tout2, y: night.tflow2 },
  ].filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));

  const xs = refPoints.map((p) => p.x);
  const minX = xs.length ? Math.min(-20, ...xs) : -20;
  const maxX = xs.length ? Math.max(20, ...xs) : 20;

  const pad = 18;
  const plotMinFlow = minFlow + Math.min(0, offset);
  const plotMaxFlow = maxFlow + Math.max(0, offset);
  const scaleX = (canvas.width - pad * 2) / (maxX - minX || 1);
  const scaleY = (canvas.height - pad * 2) / (plotMaxFlow - plotMinFlow || 1);

  // subtle grid background
  eqDrawGrid(ctx, canvas, pad, minX, maxX, plotMinFlow, plotMaxFlow);

  // Axes
  ctx.strokeStyle = "#e2e8f0";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, pad);
  ctx.stroke();

  // Range labels
  ctx.fillStyle = "#64748b";
  ctx.font = "12px system-ui, -apple-system, Segoe UI, Roboto, sans-serif";
  ctx.fillText(`${minX}°C`, pad, canvas.height - 4);
  ctx.fillText(`${maxX}°C`, canvas.width - pad - 28, canvas.height - 4);
  ctx.fillText(`${plotMinFlow.toFixed(0)}°C`, 2, canvas.height - pad);
  ctx.fillText(`${plotMaxFlow.toFixed(0)}°C`, 2, pad + 12);

  const clampFlow = (value) => Math.min(maxFlow, Math.max(minFlow, value));
  const dayParams = eqPickCurveParams(eq, "day");
  const nightParams = eqPickCurveParams(eq, "night");
  const daySlope = Number(dayParams.slope);
  const dayShift = Number(dayParams.shift);
  const nightSlope = Number(nightParams.slope);
  const nightShift = Number(nightParams.shift);
  const calcFlow = (tout, slope, shift) => clampFlow((20 - tout) * slope + 20 + shift) + offset;

  const plotCurve = (slope, shift, color) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i <= 60; i += 1) {
      const tout = minX + ((maxX - minX) * i) / 60;
      const flow = calcFlow(tout, slope, shift);
      const x = pad + (tout - minX) * scaleX;
      const y = canvas.height - pad - (flow - plotMinFlow) * scaleY;
      if (i === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
  };

  // Day (red) + Night (blue)
  plotCurve(daySlope, dayShift, "#ef4444");
  plotCurve(nightSlope, nightShift, "#2563eb");

  // Current point (active curve)
  const currentTout = state.status?.equitherm?.outdoorC;
  const isNight = Boolean(state.status?.equitherm?.night);
  const slope = isNight ? nightSlope : daySlope;
  const shift = isNight ? nightShift : dayShift;

  const noteEl = $("dashEqPointNote");
  if (noteEl) {
    noteEl.textContent = "";
  }

  if (Number.isFinite(currentTout)) {
    const flow = calcFlow(currentTout, slope, shift);
    const x = pad + (currentTout - minX) * scaleX;
    const y = canvas.height - pad - (flow - plotMinFlow) * scaleY;

    ctx.fillStyle = "#0f172a";
    ctx.beginPath();
    ctx.arc(x, y, 4, 0, Math.PI * 2);
    ctx.fill();

    // Label near point
    ctx.fillStyle = "#0f172a";
    ctx.font = "12px system-ui, -apple-system, Segoe UI, Roboto, sans-serif";
    ctx.fillText(`${flow.toFixed(1)}°C`, Math.min(canvas.width - 50, x + 6), Math.max(14, y - 6));

    if (noteEl) {
      noteEl.textContent = `${Number(currentTout).toFixed(1)}°C → ${flow.toFixed(1)}°C`;
    }
  }
}

const curveThrottleMs = 10000;

function getEquithermCurveSignature() {
  const eq = state.config?.equitherm || {};
  const refs = eq.refs || {};
  const statusEq = state.status?.equitherm || {};
  return JSON.stringify({
    minFlow: eq.minFlow,
    maxFlow: eq.maxFlow,
    curveOffsetC: eq.curveOffsetC,
    slopeDay: eq.slopeDay,
    shiftDay: eq.shiftDay,
    slopeNight: eq.slopeNight,
    shiftNight: eq.shiftNight,
    refsDay: refs.day || null,
    refsNight: refs.night || null,
    outdoorC: statusEq.outdoorC,
    night: statusEq.night,
  });
}

function shouldRedrawCurve(key, signature, force = false) {
  const entry = state.curveThrottle[key] || { lastAt: 0, signature: "" };
  const now = Date.now();
  if (force || signature !== entry.signature || now - entry.lastAt >= curveThrottleMs) {
    entry.lastAt = now;
    entry.signature = signature;
    state.curveThrottle[key] = entry;
    return true;
  }
  return false;
}

function drawEquithermCurveThrottled(force = false) {
  const signature = getEquithermCurveSignature();
  if (shouldRedrawCurve("equitherm", signature, force)) {
    drawEquithermCurve();
  }
}

function drawDashEquithermCurveThrottled(force = false) {
  const signature = getEquithermCurveSignature();
  if (shouldRedrawCurve("dashEquitherm", signature, force)) {
    drawDashEquithermCurve();
  }
}

function setActiveSection(hash) {
  const page = (hash || "#dashboard").replace("#", "");
  const sections = Array.from(document.querySelectorAll(".section"));
  const active = sections.some((section) => section.id === page) ? page : "dashboard";
  state.activePage = active;
  sections.forEach((section) => {
    section.classList.toggle("hidden", section.id !== active);
  });
  document.querySelectorAll(".nav a").forEach((link) => {
    link.classList.toggle("active", link.getAttribute("href") === `#${active}`);
  });
}

function formatTemp(value) {
  if (value === undefined || value === null) {
    return "—";
  }
  return `${Number(value).toFixed(1)} °C`;
}

async function loadAll() {
  state.caps = {};
  const capsPromise = fetchJson("/api/caps")
    .then((caps) => {
      state.caps = caps || {};
    })
    .catch(() => {});

  const blePromise = fetchJson("/api/ble/config")
    .then((config) => {
      state.bleConfig = config || {};
    })
    .catch(() => {});

  const buzzerPromise = fetchJson("/api/buzzer")
    .then((config) => {
      state.buzzerConfig = config || {};
    })
    .catch(() => {});

  try {
    const cfg = await fetchJson("/api/config", { timeoutMs: 15000 });
    if (!cfg) {
      throw new Error("config fetch returned null");
    }
    state.config = cfg;
  } catch (error) {
    state.config = {};
    toast("Nelze načíst config", "error");
  }
  ensureConfigDefaults();
  renderCaps();
  renderInputsConfig();
  renderRelayNames();
  renderRelayMap();
  renderIofuncRoles();
  renderValveConfig();
  initValveCalibrationForm();
  renderThermometers();
  renderThermometerRoles();
  renderEquithermSources();
  renderAkuSources();
  renderEquithermValveSelect();
  renderAkuHeaterWindows();
  renderRecircWindows();
  renderRecircReturnSource();
  // Bind static inputs in HTML sections
  bindInputs($("equitherm"), state.config);
  bindInputs($("tuv"), state.config);
  bindInputs($("dhwRecirc"), state.config);
  bindInputs($("aku"), state.config);
  bindInputs($("boiler"), state.config);
  initEquithermTooltips();
  initTuvTooltips();
  initTuvBindings();
  renderModeSelect();
  drawEquithermCurveThrottled(true);

  await Promise.allSettled([capsPromise, blePromise, buzzerPromise]);
  renderCaps();
  renderBleConfig();
  renderBuzzerConfig();

  renderAdvancedJsonEditors();
  await refreshStatus();
  await refreshDash();
}

async function refreshStatus() {
  // Avoid fighting with heavier endpoints (/api/dash, BLE)
  if (state.inFlight.status) {
    return;
  }
  state.inFlight.status = true;
  try {
    const result = await fetchJson("/api/status", { timeoutMs: API_TIMEOUT_STATUS_MS });

    // If timeout/abort -> fetchJson returns null. Do NOT wipe last known status.
    if (result) {
      state.status = result;
    }
    if (!state.status) state.status = {};
    const dumpEl = $("statusDump");
    if (dumpEl) dumpEl.textContent = JSON.stringify(state.status, null, 2);
    updateStatusUI();
  } finally {
    state.inFlight.status = false;
  }
}

async function refreshDash() {
  if (state.inFlight.dash) {
    return;
  }
  const now = Date.now();
  if (now < state.backoff.dashUntil) {
    return;
  }
  state.inFlight.dash = true;
  try {
    // /api/dash can occasionally be slower when ESP is busy (BLE, FS, MQTT)
    const result = await fetchJson("/api/dash", { timeoutMs: API_TIMEOUT_DASH_MS });
    if (!result) {
      // Temporary backoff to avoid hammering ESP32 when it is busy
      state.backoff.dashUntil = Date.now() + 8000;
      return;
    }
    state.dash = result || {};

    if (shouldPollBleStatus()) {
      await loadBleStatus();
    }

    updateDashUI();
  } finally {
    state.inFlight.dash = false;
  }
}

async function toggleRelay(relay, value) {
  if (!confirm("Přepnout relé? Tím se jednotka přepne do MANUAL.")) {
    return;
  }
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "relay", relay, value }),
    });
    toast(`Relé ${relay} změněno`);
    await refreshStatus();
  } catch (error) {
    toast("Chyba při přepnutí relé", "error");
  }
}

async function setControlMode(value) {
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "control_mode", value }),
    });
    toast(`Režim ${value}`);
    await refreshStatus();
  } catch (error) {
    toast("Chyba při změně režimu", "error");
  }
}

async function setManualMode(value) {
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "manual_mode", value }),
    });
    toast(`Manual mode ${value}`);
    await refreshStatus();
  } catch (error) {
    toast("Chyba při změně manuálního režimu", "error");
  }
}

async function autoRecompute() {
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "auto_recompute" }),
    });
    toast("Auto recompute odesláno");
  } catch (error) {
    toast("Chyba při recompute", "error");
  }
}

async function saveConfigSection(section) {
  const labelMap = {
    equitherm: "Ekviterm",
    tuv: "TUV / DHW",
    io: "I/O",
    valves: "Ventily",
    thermometers: "Teploměry",
    aku: "AKU nádrž",
    recirc: "Cirkulace TUV",
  };
  const label = labelMap[section] || "Konfigurace";

  try {
    // Backend očekává celé config JSON (ne wrapper). Filtruje pouze povolené klíče.
    const payload = buildConfigForSave(state.config);
    validateConfigPayload(payload, `save:${section}`);
    const result = await fetchJson("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      timeoutMs: 15000,
    });

    if (!result) {
      toast("Chyba při uložení konfigurace", "error");
      return;
    }

    toast(`${label} uloženo`);

    // Načti zpět (backend může config sanitizovat / přepočítat defaulty)
    const refreshed = await fetchJson("/api/config", { timeoutMs: 15000 });
    if (refreshed) {
      state.config = refreshed;
      ensureConfigDefaults();
      renderThermometers();
      renderThermometerRoles();
      renderEquithermSources();
    renderAkuSources();
      renderEquithermValveSelect();
      renderAkuSources();
      renderAkuHeaterWindows();
  renderRecircWindows();
  renderRecircReturnSource();
      drawEquithermCurve();
    }
  } catch (error) {
    toast("Chyba uložení", "error");
  }
}

async function saveBleConfig() {
  try {
    await fetchJson("/api/ble/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(state.bleConfig),
    });
    toast("BLE config uložen");
  } catch (error) {
    toast("Chyba při uložení BLE", "error");
  }
}

async function saveBuzzerConfig() {
  try {
    await fetchJson("/api/buzzer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "set_config", config: state.buzzerConfig }),
    });
    toast("Buzzer config uložen");
  } catch (error) {
    toast("Chyba při uložení buzzeru", "error");
  }
}

async function sendBuzzerTest(pattern) {
  try {
    await fetchJson("/api/buzzer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "test", pattern }),
    });
    toast(`Buzzer ${pattern}`);
  } catch (error) {
    toast("Chyba při testu", "error");
  }
}

async function stopBuzzer() {
  try {
    await fetchJson("/api/buzzer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "stop" }),
    });
    toast("Buzzer stop");
  } catch (error) {
    toast("Chyba při stop", "error");
  }
}

async function startBlePairing() {
  try {
    await fetchJson("/api/ble/pair", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        seconds: Number($("blePairSeconds").value || 120),
        role: $("blePairRole").value,
      }),
    });
    toast("BLE pairing spuštěn");
    await loadBlePaired();
  } catch (error) {
    toast("Chyba pairing", "error");
  }
}

async function stopBlePairing() {
  try {
    await fetchJson("/api/ble/pair/stop", { method: "POST" });
    toast("BLE pairing stop");
  } catch (error) {
    toast("Chyba při stop pairing", "error");
  }
}

async function loadBlePaired() {
  if (!shouldPollBlePaired()) {
    return;
  }
  if (state.inFlight.blePaired) {
    return;
  }
  const now = Date.now();
  if (now < state.backoff.bleUntil) {
    return;
  }
  state.inFlight.blePaired = true;
  try {
    const response = await fetchJson("/api/ble/paired", { timeoutMs: 7000 });
    if (!response) {
      state.backoff.bleUntil = Date.now() + 15000;
      return;
    }
    const paired = Array.isArray(response)
      ? response
      : Array.isArray(response?.paired)
        ? response.paired
        : Array.isArray(response?.devices)
          ? response.devices
          : [];
    state.blePaired = paired;
    const container = $("blePaired");
    container.innerHTML = "";
    paired.forEach((device) => {
      const row = document.createElement("div");
      row.className = "inline";
      row.innerHTML = `
        <span>${device.name || ""}</span>
        <span>${device.mac || ""}</span>
        <button class="btn secondary" data-mac="${device.mac}">Remove</button>
      `;
      row.querySelector("button").addEventListener("click", () => removeBleDevice(device.mac));
      container.appendChild(row);
    });
  } finally {
    state.inFlight.blePaired = false;
  }
}

async function removeBleDevice(mac) {
  try {
    await fetchJson("/api/ble/remove", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac }),
    });
    toast("BLE zařízení odebráno");
    await loadBlePaired();
  } catch (error) {
    toast("Chyba při odebrání BLE", "error");
  }
}

async function retryBleMeteo() {
  try {
    await fetchJson("/api/ble/meteo/retry", { method: "POST" });
    toast("Meteo retry odesláno");
  } catch (error) {
    toast("Chyba při meteo retry", "error");
  }
}

async function loadBleStatus() {
  if (!shouldPollBleStatus()) {
    return;
  }
  if (state.inFlight.bleStatus) {
    return;
  }
  const now = Date.now();
  if (now < state.backoff.bleUntil) {
    return;
  }
  state.inFlight.bleStatus = true;
  try {
    const result = await fetchJson("/api/ble/status", { timeoutMs: 7000 });
    if (!result) {
      state.backoff.bleUntil = Date.now() + 8000;
      return;
    }
    state.bleStatus = result;
    const el = $("bleStatus");
    if (el) {
      el.textContent = JSON.stringify(state.bleStatus, null, 2);
    }
    // Make sure BLE thermometer row auto-fills MAC + outdoor role when meteo is connected
    syncBleThermometerFromStatus();
    renderThermometers();
    updateStatusUI();
    updateDashUI();
  } finally {
    state.inFlight.bleStatus = false;
  }
}

async function exportConfig() {
  try {
    const config = await fetchJson("/api/config");
    downloadJson(config, "config.json");
  } catch (error) {
    toast("Chyba exportu", "error");
  }
}

async function exportBle() {
  try {
    const config = await fetchJson("/api/ble/config");
    downloadJson(config, "ble-config.json");
  } catch (error) {
    toast("Chyba exportu BLE", "error");
  }
}

async function exportBuzzer() {
  try {
    const config = await fetchJson("/api/buzzer");
    downloadJson(config, "buzzer-config.json");
  } catch (error) {
    toast("Chyba exportu buzzer", "error");
  }
}

function downloadJson(data, filename) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: "application/json" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = filename;
  link.click();
  URL.revokeObjectURL(link.href);
}

function handleImport(file) {
  if (!file) {
    return;
  }
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const json = JSON.parse(reader.result);
      $("configPreview").value = JSON.stringify(json, null, 2);
      $("configPreview").dataset.importPayload = reader.result;
      toast("Import načten");
    } catch (error) {
      toast("Neplatný JSON", "error");
    }
  };
  reader.readAsText(file);
}

async function applyImport() {
  const payload = $("configPreview").dataset.importPayload;
  if (!payload) {
    toast("Není načtený import", "error");
    return;
  }
  try {
    const json = JSON.parse(payload);
    await fetchJson("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(json),
    });
    toast("Import uložen");
    await loadAll();
  } catch (error) {
    toast("Chyba při importu", "error");
  }
}

async function rebootDevice() {
  if (!confirm("Opravdu rebootovat zařízení?")) {
    return;
  }
  try {
    await fetchJson("/api/reboot", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ reboot: true }),
    });
    toast("Reboot odeslán");
  } catch (error) {
    toast("Chyba při reboot", "error");
  }
}

async function testRelayRaw(relay, on) {
  if (!confirm("Raw relé test? Přepne do MANUAL.")) {
    return;
  }
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "relay_raw", relay, on }),
    });
    toast(`Relay ${relay} raw ${on ? "ON" : "OFF"}`);
  } catch (error) {
    toast("Chyba raw relé", "error");
  }
}

function applyCalibration() {
  const master = Number($("calMaster").value || 0);
  if (!master) {
    toast("Zadejte master relé", "error");
    return;
  }
  const idx = master - 1;
  const output = state.config.iofunc.outputs[idx] || { params: {} };
  output.params = output.params || {};

  const getNum = (id) => {
    const el = $(id);
    if (!el) return null;
    const v = parseInputValue(el);
    return typeof v === "number" && Number.isFinite(v) ? v : null;
  };

  // Peer může být 0 (single relay), proto nepoužíváme "truthy" kontrolu.
  const peer = getNum("calPeer");
  output.params.peerRel = peer === null ? 0 : peer;

  const invertEl = $("calInvert");
  if (invertEl) output.params.invertDir = !!invertEl.checked;

  const travel = getNum("calTravel");
  if (travel !== null) output.params.travelTime = travel;
  const pulse = getNum("calPulse");
  if (pulse !== null) output.params.pulseTime = pulse;
  const guard = getNum("calGuard");
  if (guard !== null) output.params.guardTime = guard;
  const minSw = getNum("calMinSwitch");
  if (minSw !== null) output.params.minSwitchS = minSw;

  const defEl = $("calDefault");
  if (defEl && defEl.value) output.params.defaultPos = defEl.value;

  state.config.iofunc.outputs[idx] = output;
  ensureConfigDefaults();
  renderValveConfig();
  toast("Kalibrace přenesena do configu");
}

function initValveCalibrationForm() {
  const masterEl = $("calMaster");
  if (!masterEl) return;

  // Default to the fixed mixing valve (R1+R2) and prefill from config params
  const mixOut = state.config?.iofunc?.outputs?.[0] || {};
  const p = mixOut.params || {};

  if (!String(masterEl.value || "").trim()) masterEl.value = "1";

  const peerEl = $("calPeer");
  if (peerEl && !String(peerEl.value || "").trim()) peerEl.value = String(p.peerRel || 2);

  const invEl = $("calInvert");
  if (invEl) invEl.checked = !!p.invertDir;

  const setIfEmpty = (id, val) => {
    const el = $(id);
    if (!el) return;
    if (!String(el.value || "").trim() && val !== undefined && val !== null && Number(val) !== 0) {
      el.value = String(val);
    }
  };
  setIfEmpty("calTravel", p.travelTime);
  setIfEmpty("calPulse", p.pulseTime);
  setIfEmpty("calGuard", p.guardTime);
  setIfEmpty("calMinSwitch", p.minSwitchS);

  const defEl = $("calDefault");
  if (defEl && !String(defEl.value || "").trim()) defEl.value = String(p.defaultPos || "A");
}

function buildCalibrationValvePayload() {
  const master = Number($("calMaster").value || 0);
  const peer = parseInputValue($("calPeer"));
  const invertDir = !!$("calInvert")?.checked;
  const travelTime = parseInputValue($("calTravel"));
  const pulseTime = parseInputValue($("calPulse"));
  const guardTime = parseInputValue($("calGuard"));
  const minSwitchS = parseInputValue($("calMinSwitch"));
  const defaultPos = String($("calDefault").value || "A");

  const payload = { master };
  const peerNum = typeof peer === "number" ? peer : 0;
  if (peerNum > 0) payload.peer = peerNum;
  payload.singleRelay = !(peerNum > 0);
  payload.invertDir = !!invertDir;

  if (typeof travelTime === "number" && travelTime > 0) payload.travelTime = travelTime;
  if (typeof pulseTime === "number" && pulseTime > 0) payload.pulseTime = pulseTime;
  if (typeof guardTime === "number" && guardTime >= 0) payload.guardTime = guardTime;
  if (typeof minSwitchS === "number" && minSwitchS >= 0) payload.minSwitchS = minSwitchS;
  payload.defaultPos = defaultPos;
  return payload;
}

async function valvePulse(dir) {
  const cfg = buildCalibrationValvePayload();
  if (!cfg.master) {
    toast("Zadejte master relé", "error");
    return;
  }
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "valve_pulse", dir, ...cfg }),
    });
    toast(`Ventil pulse ${dir < 0 ? "−" : "+"}`);
  } catch (error) {
    toast("Chyba pulse", "error");
  }
}

async function valveStop() {
  const cfg = buildCalibrationValvePayload();
  if (!cfg.master) {
    toast("Zadejte master relé", "error");
    return;
  }
  try {
    await fetchJson("/api/mode_ctrl", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "valve_stop", ...cfg }),
    });
    toast("Ventil STOP");
  } catch (error) {
    toast("Chyba stop", "error");
  }
}





function renderRecircWindows() {
  const container = $("recircWindows");
  if (!container) {
    return;
  }

  state.config.dhwRecirc = state.config.dhwRecirc || {};
  const rc = state.config.dhwRecirc;
  rc.windows = Array.isArray(rc.windows) ? rc.windows : [];

  container.innerHTML = "";

  const dayLabels = [
    { d: 1, l: "Po" },
    { d: 2, l: "Út" },
    { d: 3, l: "St" },
    { d: 4, l: "Čt" },
    { d: 5, l: "Pá" },
    { d: 6, l: "So" },
    { d: 7, l: "Ne" },
  ];

  const normTime = (t, fallback) => {
    const s = String(t || "").trim();
    if (/^\d{1,2}:\d{2}$/.test(s)) return s.padStart(5, "0");
    return fallback;
  };

  const renderRow = (w, idx) => {
    w.days = Array.isArray(w.days) ? w.days : [1, 2, 3, 4, 5, 6, 7];
    w.start = normTime(w.start, "06:00");
    w.end = normTime(w.end, "07:00");

    const row = document.createElement("div");
    row.className = "card compact";
    row.innerHTML = `
      <h3>Okno ${idx + 1}</h3>
      <p class="hint">Příklad: <span class="mono">06:00–07:00</span> Po–Pá; <span class="mono">10:00–15:00</span> Po–Ne.</p>
      <div class="form-grid compact">
        <label class="field" title="Začátek okna (např. 06:00)"><span>Start</span><input type="time" class="recirc-win-start" value="${w.start}" /></label>
        <label class="field" title="Konec okna (např. 07:00)"><span>End</span><input type="time" class="recirc-win-end" value="${w.end}" /></label>
        <div class="field" style="grid-column: 1 / -1;">
          <span>Dny</span>
          <div class="inline wrap recirc-days"></div>
        </div>
      </div>
      <div class="action-bar">
        <button class="btn danger small recirc-win-remove">Smazat</button>
      </div>
    `;

    const daysWrap = row.querySelector('.recirc-days');
    dayLabels.forEach(({ d, l }) => {
      const lab = document.createElement('label');
      lab.className = 'toggle';
      lab.innerHTML = `<input type="checkbox" ${w.days.includes(d) ? 'checked' : ''} /><span>${l}</span>`;
      const inp = lab.querySelector('input');
      inp.addEventListener('change', () => {
        const set = new Set(w.days);
        if (inp.checked) set.add(d); else set.delete(d);
        w.days = Array.from(set).sort((a, b) => a - b);
      });
      daysWrap.appendChild(lab);
    });

    row.querySelector('.recirc-win-start').addEventListener('change', (ev) => {
      w.start = normTime(ev.target.value, w.start);
    });
    row.querySelector('.recirc-win-end').addEventListener('change', (ev) => {
      w.end = normTime(ev.target.value, w.end);
    });
    row.querySelector('.recirc-win-remove').addEventListener('click', () => {
      rc.windows.splice(idx, 1);
      renderRecircWindows();
    });

    container.appendChild(row);
  };

  if (!rc.windows.length) {
    const empty = document.createElement('div');
    empty.className = 'hint';
    empty.textContent = 'Zatím nejsou definována žádná časová okna.';
    container.appendChild(empty);
  } else {
    rc.windows.forEach((w, idx) => renderRow(w, idx));
  }
}

function renderRecircReturnSource() {
  const container = $("recircReturnSource");
  if (!container) {
    return;
  }

  state.config.dhwRecirc = state.config.dhwRecirc || {};
  const rc = state.config.dhwRecirc;
  rc.tempReturnSource = rc.tempReturnSource || {};
  const cfg = rc.tempReturnSource;

  // Normalize old variants
  if (cfg.source === 'mqtt0') { cfg.source = 'mqtt'; cfg.mqttIdx = 1; }
  if (cfg.source === 'mqtt1') { cfg.source = 'mqtt'; cfg.mqttIdx = 2; }

  const makeSourceOptions = () => {
    const out = [];
    out.push('<option value="none">—</option>');
    out.push('<optgroup label="TEMP">');
    for (let i = 1; i <= 8; i += 1) out.push(`<option value="temp${i}">TEMP${i}</option>`);
    out.push('</optgroup>');
    out.push('<optgroup label="Dallas">');
    out.push('<option value="dallas">Dallas / DS18B20</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="MQTT">');
    out.push('<option value="mqtt">MQTT</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="BLE">');
    out.push('<option value="ble">BLE meteo</option>');
    out.push('</optgroup>');
    out.push('<optgroup label="OpenTherm">');
    out.push('<option value="opentherm_boiler">OpenTherm boiler</option>');
    out.push('<option value="opentherm_return">OpenTherm return</option>');
    out.push('</optgroup>');
    return out.join("\n");
  };

  const makeDallasBusOptions = () => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const out = [];
    const maxBus = Math.max(4, gpios.length);
    for (let i = 0; i < maxBus; i += 1) {
      const gpio = gpios[i];
      const suffix = gpio ? ` (GPIO${gpio})` : '';
      out.push(`<option value="${i}">Bus ${i}${suffix}</option>`);
    }
    return out.join("\n");
  };

  const normalizeSource = (s) => {
    const v = String(s || 'none').trim();
    return v.length ? v : 'none';
  };

  const normalizeDallasBus = (value) => {
    const gpios = Array.isArray(state.config.dallasGpios) ? state.config.dallasGpios : [];
    const v = Number(value || 0);
    if (Number.isFinite(v) && v >= 0 && v <= 3) return v;
    if (Number.isFinite(v) && v > 3 && gpios.length) {
      const idx = gpios.indexOf(v);
      if (idx >= 0) return idx;
    }
    return 0;
  };

  container.innerHTML = '';
  const card = document.createElement('div');
  card.className = 'card compact';
  card.innerHTML = `
    <div class="form-grid">
      <label class="field"><span>Source</span>
        <select class="rc-source">${makeSourceOptions()}</select>
      </label>

      <div class="rc-source-block dallas">
        <label class="field"><span>Dallas bus</span>
          <select class="rc-dallas-gpio">${makeDallasBusOptions()}</select>
        </label>
        <label class="field"><span>ROM (hex)</span>
          <input class="rc-dallas-rom" placeholder="28ff..." list="dallas-rc" />
          <datalist id="dallas-rc"></datalist>
        </label>
      </div>

      <div class="rc-source-block mqtt">
        <label class="field"><span>MQTT preset</span>
          <select class="rc-mqtt-preset">
            <option value="1">MQTT #1 (z Teploměry)</option>
            <option value="2">MQTT #2 (z Teploměry)</option>
            <option value="0">Custom topic</option>
          </select>
        </label>

        <div class="rc-mqtt-custom">
          <label class="field"><span>Topic</span>
            <input class="rc-mqtt-topic" placeholder="home/sensors/return" />
          </label>
          <label class="field"><span>JSON key</span>
            <input class="rc-mqtt-key" placeholder="temp" />
          </label>
        </div>
      </div>

      <div class="rc-source-block ble">
        <label class="field"><span>BLE ID (volitelné)</span>
          <input class="rc-ble-id" placeholder="meteo.tempC / (prázdné)" />
        </label>
        <div class="hint">Použije BLE meteo. Pokud ID necháš prázdné, použije se default (teplota).</div>
      </div>

      <label class="field"><span>Max age ms</span>
        <input type="number" min="0" class="rc-max-age" placeholder="900000" />
      </label>
    </div>
  `;
  container.appendChild(card);

  const sourceSelect = card.querySelector('.rc-source');
  const dallasBlock = card.querySelector('.rc-source-block.dallas');
  const mqttBlock = card.querySelector('.rc-source-block.mqtt');
  const bleBlock = card.querySelector('.rc-source-block.ble');

  const dallasBus = card.querySelector('.rc-dallas-gpio');
  const dallasRom = card.querySelector('.rc-dallas-rom');
  const mqttPreset = card.querySelector('.rc-mqtt-preset');
  const mqttCustom = card.querySelector('.rc-mqtt-custom');
  const mqttTopic = card.querySelector('.rc-mqtt-topic');
  const mqttKey = card.querySelector('.rc-mqtt-key');
  const bleId = card.querySelector('.rc-ble-id');
  const maxAge = card.querySelector('.rc-max-age');

  const syncDallasList = () => {
    const list = card.querySelector('datalist');
    const addrs = state.config?.dallasAddrs || [];
    list.innerHTML = '';
    addrs.forEach((addr) => {
      const opt = document.createElement('option');
      opt.value = addr;
      list.appendChild(opt);
    });
  };

  const showFor = (type) => {
    const t = normalizeSource(type);
    dallasBlock.style.display = t === 'dallas' ? 'block' : 'none';
    mqttBlock.style.display = t === 'mqtt' ? 'block' : 'none';
    bleBlock.style.display = t === 'ble' ? 'block' : 'none';
    if (mqttCustom) {
      mqttCustom.style.display = t === 'mqtt' && String(mqttPreset.value) === '0' ? 'block' : 'none';
    }
  };

  const fill = () => {
    const src = normalizeSource(cfg.source);
    sourceSelect.value = src;
    if (!Array.from(sourceSelect.options).some((o) => o.value === src)) {
      const opt = document.createElement('option');
      opt.value = src;
      opt.textContent = src;
      sourceSelect.appendChild(opt);
      sourceSelect.value = src;
    }

    const bus = normalizeDallasBus(cfg.gpio);
    dallasBus.value = String(bus);
    dallasRom.value = String(cfg.rom || cfg.addr || '');

    const idx = Number(cfg.mqttIdx || 0);
    mqttPreset.value = idx >= 1 && idx <= 2 ? String(idx) : '0';
    mqttTopic.value = String(cfg.topic || '');
    mqttKey.value = String(cfg.jsonKey || '');

    bleId.value = String(cfg.bleId || cfg.id || '');

    maxAge.value = cfg.maxAgeMs === undefined || cfg.maxAgeMs === null ? '' : String(cfg.maxAgeMs);

    syncDallasList();
    showFor(src);
  };

  fill();

  sourceSelect.addEventListener('change', () => {
    cfg.source = normalizeSource(sourceSelect.value);
    showFor(cfg.source);
  });

  dallasBus.addEventListener('change', () => {
    cfg.gpio = normalizeDallasBus(dallasBus.value);
  });

  dallasRom.addEventListener('input', () => {
    cfg.rom = String(dallasRom.value || '');
  });

  mqttPreset.addEventListener('change', () => {
    const v = Number(mqttPreset.value || 0);
    cfg.mqttIdx = v >= 1 && v <= 2 ? v : 0;
    showFor(cfg.source);
  });

  mqttTopic.addEventListener('input', () => {
    cfg.topic = String(mqttTopic.value || '');
    if (Number(mqttPreset.value || 0) !== 0) {
      mqttPreset.value = '0';
    }
    cfg.mqttIdx = 0;
    showFor(cfg.source);
  });

  mqttKey.addEventListener('input', () => {
    cfg.jsonKey = String(mqttKey.value || '');
    if (Number(mqttPreset.value || 0) !== 0) {
      mqttPreset.value = '0';
    }
    cfg.mqttIdx = 0;
    showFor(cfg.source);
  });

  bleId.addEventListener('input', () => {
    cfg.bleId = String(bleId.value || '');
  });

  maxAge.addEventListener('input', () => {
    const v = Number(maxAge.value);
    cfg.maxAgeMs = Number.isFinite(v) && v > 0 ? Math.floor(v) : 0;
  });
}
function renderAkuHeaterWindows() {
  const container = $("akuWindows");
  if (!container) {
    return;
  }

  state.config.akuHeater = state.config.akuHeater || {};
  const aku = state.config.akuHeater;
  aku.windows = Array.isArray(aku.windows) ? aku.windows : [];

  container.innerHTML = "";

  const dayLabels = [
    { d: 1, l: "Po" },
    { d: 2, l: "Út" },
    { d: 3, l: "St" },
    { d: 4, l: "Čt" },
    { d: 5, l: "Pá" },
    { d: 6, l: "So" },
    { d: 7, l: "Ne" },
  ];

  const normTime = (t, fallback) => {
    const s = String(t || "").trim();
    if (/^\d{1,2}:\d{2}$/.test(s)) return s.padStart(5, "0");
    return fallback;
  };

  const renderRow = (w, idx) => {
    w.days = Array.isArray(w.days) ? w.days : [1, 2, 3, 4, 5, 6, 7];
    w.start = normTime(w.start, "06:00");
    w.end = normTime(w.end, "07:00");

    const row = document.createElement("div");
    row.className = "card compact";
    row.innerHTML = `
      <h3>Okno ${idx + 1}</h3>
      <p class="hint">Příklad: <span class="mono">06:00–07:00</span> Po–Pá; <span class="mono">10:00–15:00</span> Po–Ne.</p>
      <div class="form-grid compact">
        <label class=\"field\" title=\"Začátek okna (např. 10:00)\"><span>Start</span><input type=\"time\" class=\"aku-win-start\" value=\"${w.start}\" /></label>
        <label class=\"field\" title=\"Konec okna (např. 15:00)\"><span>End</span><input type=\"time\" class=\"aku-win-end\" value=\"${w.end}\" /></label>
        <div class="field" style="grid-column: 1 / -1;">
          <span>Dny</span>
          <div class="inline wrap aku-days"></div>
        </div>
      </div>
      <div class="action-bar">
        <button class="btn danger small aku-win-remove">Smazat</button>
      </div>
    `;

    const daysWrap = row.querySelector('.aku-days');
    dayLabels.forEach(({ d, l }) => {
      const lab = document.createElement('label');
      lab.className = 'toggle';
      lab.innerHTML = `<input type="checkbox" ${w.days.includes(d) ? 'checked' : ''} /><span>${l}</span>`;
      const inp = lab.querySelector('input');
      inp.addEventListener('change', () => {
        const set = new Set(w.days);
        if (inp.checked) set.add(d); else set.delete(d);
        w.days = Array.from(set).sort((a, b) => a - b);
      });
      daysWrap.appendChild(lab);
    });

    row.querySelector('.aku-win-start').addEventListener('change', (ev) => {
      w.start = normTime(ev.target.value, w.start);
    });
    row.querySelector('.aku-win-end').addEventListener('change', (ev) => {
      w.end = normTime(ev.target.value, w.end);
    });
    row.querySelector('.aku-win-remove').addEventListener('click', () => {
      aku.windows.splice(idx, 1);
      renderAkuHeaterWindows();
    });

    container.appendChild(row);
  };

  if (!aku.windows.length) {
    const empty = document.createElement('div');
    empty.className = 'hint';
    empty.textContent = 'Zatím nejsou definována žádná časová okna.';
    container.appendChild(empty);
  } else {
    aku.windows.forEach((w, idx) => renderRow(w, idx));
  }
}
function setupEvents() {
  $("navToggle").addEventListener("click", () => {
    $("sidebar").classList.toggle("open");
  });
  $("refreshNow").addEventListener("click", async () => {
    await refreshStatus();
    await refreshDash();
    await loadBleStatus();
  });

  const runWebTest = $("runWebTest");
  if (runWebTest) {
    runWebTest.addEventListener("click", async () => {
      const dump = $("webTestDump");
      if (dump) dump.textContent = "Načítám...";
      try {
        const res = await fetchJson("/api/webtest", { timeoutMs: 15000 });
        if (dump) dump.textContent = JSON.stringify(res, null, 2);
        toast("Webtest OK");
      } catch (e) {
        if (dump) dump.textContent = `Chyba: ${e?.message || e}`;
        toast("Webtest selhal", "error");
      }
    });
  }

  $("pausePolling").addEventListener("click", () => {
    state.polling = !state.polling;
    $("pausePolling").textContent = state.polling ? "Pozastavit" : "Spustit";
  });
  $("setAuto").addEventListener("click", () => setControlMode("auto"));
  $("setManual").addEventListener("click", () => setControlMode("manual"));
  const manualModeSelect = $("manualModeSelect");
  if (manualModeSelect) {
    manualModeSelect.addEventListener("change", (event) => setManualMode(event.target.value));
  }

  $("inputsAllHigh").addEventListener("click", () => {
    state.config.inputActiveLevels = Array(8).fill(1);
    renderInputsConfig();
  });
  $("inputsAllLow").addEventListener("click", () => {
    state.config.inputActiveLevels = Array(8).fill(0);
    renderInputsConfig();
  });

  $("autoRecompute").addEventListener("click", autoRecompute);

  document.querySelectorAll("[data-save]").forEach((button) => {
    button.addEventListener("click", () => saveConfigSection(button.dataset.save));
  });

  $("saveBleConfig").addEventListener("click", saveBleConfig);
  $("saveBuzzer").addEventListener("click", saveBuzzerConfig);
  $("buzzerStop").addEventListener("click", stopBuzzer);

  $("blePairStart").addEventListener("click", startBlePairing);
  $("blePairStop").addEventListener("click", stopBlePairing);
  $("bleRetry").addEventListener("click", retryBleMeteo);

  $("exportConfig").addEventListener("click", exportConfig);
  $("exportBle").addEventListener("click", exportBle);
  $("exportBuzzer").addEventListener("click", exportBuzzer);
  $("importConfig").addEventListener("change", (event) => handleImport(event.target.files[0]));
  $("applyImport").addEventListener("click", applyImport);

  $("reboot").addEventListener("click", rebootDevice);

  const akuAdd = $("akuWinAdd");
  if (akuAdd) akuAdd.addEventListener("click", () => {
    state.config.akuHeater = state.config.akuHeater || {};
    const aku = state.config.akuHeater;
    aku.windows = Array.isArray(aku.windows) ? aku.windows : [];
    aku.windows.push({ days: [1,2,3,4,5,6,7], start: "06:00", end: "07:00" });
    renderAkuHeaterWindows();
  });

  const akuClear = $("akuWinClear");
  if (akuClear) akuClear.addEventListener("click", () => {
    state.config.akuHeater = state.config.akuHeater || {};
    state.config.akuHeater.windows = [];
    renderAkuHeaterWindows();
  });


  const recircAdd = $("recircWinAdd");
  if (recircAdd) recircAdd.addEventListener("click", () => {
    state.config.dhwRecirc = state.config.dhwRecirc || {};
    const rc = state.config.dhwRecirc;
    rc.windows = Array.isArray(rc.windows) ? rc.windows : [];
    rc.windows.push({ days: [1,2,3,4,5,6,7], start: "06:00", end: "07:00" });
    renderRecircWindows();
  });

  const recircClear = $("recircWinClear");
  if (recircClear) recircClear.addEventListener("click", () => {
    state.config.dhwRecirc = state.config.dhwRecirc || {};
    state.config.dhwRecirc.windows = [];
    renderRecircWindows();
  });

  $("testRelayA").addEventListener("click", () => {
    valvePulse(-1);
  });
  $("testRelayB").addEventListener("click", () => {
    valvePulse(1);
  });
  $("stopRelay").addEventListener("click", () => {
    valveStop();
  });
  $("saveCalibration").addEventListener("click", applyCalibration);
}

function startPolling() {
  // Use self-scheduling timers (instead of fixed setInterval) so we never stack
  // pending requests when ESP32 is busy.
  const statusPeriodMs = 2000;
  const dashPeriodMs = 8000;

  const schedule = (key, fn, periodMs) => {
    const run = async () => {
      if (!state.polling || document.hidden) {
        state.timers[key] = setTimeout(run, periodMs);
        return;
      }
      try {
        await fn();
      } catch (_) {
        // ignore
      } finally {
        state.timers[key] = setTimeout(run, periodMs);
      }
    };
    clearTimeout(state.timers[key]);
    state.timers[key] = setTimeout(run, 250);
  };

  schedule("status", refreshStatus, statusPeriodMs);

  schedule(
    "dash",
    async () => {
      // Avoid stacking requests on ESP32: poll only what the current page needs.
      if (shouldPollDash()) {
        await refreshDash();
      } else if (shouldPollBleStatus()) {
        await loadBleStatus();
      }
      if (shouldPollBlePaired()) {
        await loadBlePaired();
      }
    },
    dashPeriodMs
  );

  document.addEventListener("visibilitychange", () => {
    // When the tab becomes visible again, refresh quickly.
    if (!document.hidden && state.polling) {
      refreshStatus();
      if (shouldPollDash()) refreshDash();
      else if (shouldPollBleStatus()) loadBleStatus();
      if (shouldPollBlePaired()) loadBlePaired();
    }
  });
}

function initRoleLists() {
  const inputList = document.createElement("datalist");
  inputList.id = "inputRoles";
  inputRoleOptions.forEach((role) => {
    const option = document.createElement("option");
    option.value = role;
    inputList.appendChild(option);
  });
  document.body.appendChild(inputList);

  const outputList = document.createElement("datalist");
  outputList.id = "outputRoles";
  outputRoleOptions.forEach((role) => {
    const option = document.createElement("option");
    option.value = role;
    outputList.appendChild(option);
  });
  document.body.appendChild(outputList);

  const thermoList = document.createElement("datalist");
  thermoList.id = "thermoRoles";
  thermoRoleOptions.forEach((role) => {
    const option = document.createElement("option");
    option.value = role.value;
    option.textContent = role.label;
    thermoList.appendChild(option);
  });
  document.body.appendChild(thermoList);
}


// Debug helper: run from DevTools console:
//   await window.__runConfigRoundtripTest("thermometers")
window.__runConfigRoundtripTest = async function __runConfigRoundtripTest(section = "all") {
  const payload = buildConfigForSave(state.config);
  validateConfigPayload(payload, `test:${section}`);
  const before = JSON.parse(JSON.stringify(payload));
  const res = await fetchJson("/api/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
    timeoutMs: 15000,
  });
  const after = await fetchJson("/api/config", { timeoutMs: 15000 });
  const diffs = [];
  function walk(a, b, p) {
    if (a === b) return;
    if (a && b && typeof a === "object" && typeof b === "object" && !Array.isArray(a) && !Array.isArray(b)) {
      const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
      for (const k of keys) walk(a[k], b[k], p ? `${p}.${k}` : k);
      return;
    }
    if (Array.isArray(a) && Array.isArray(b)) {
      const n = Math.max(a.length, b.length);
      for (let i = 0; i < n; i += 1) walk(a[i], b[i], `${p}[${i}]`);
      return;
    }
    diffs.push({ path: p, before: a, after: b });
  }
  walk(before, after, "");
  if (diffs.length) console.warn("Config roundtrip diffs:", diffs);
  else console.log("Config roundtrip OK");
  return { ok: true, diffs, response: res };
};


document.addEventListener("DOMContentLoaded", async () => {
  initRoleLists();
  setupEvents();
  setActiveSection(window.location.hash);
  window.addEventListener("hashchange", () => setActiveSection(window.location.hash));
  await loadAll();
  // Only load what the current page needs to avoid extra load on ESP32 during startup.
  if (shouldPollBleStatus()) {
    await loadBleStatus();
  }
  if (shouldPollBlePaired()) {
    await loadBlePaired();
  }
  startPolling();
});
