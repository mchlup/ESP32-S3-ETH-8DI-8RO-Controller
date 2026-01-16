const state = {
  config: null,
  caps: {},
  status: null,
  dash: null,
  bleConfig: null,
  bleStatus: null,
  blePaired: [],
  buzzerConfig: null,
  polling: true,
  statusTimer: null,
  dashTimer: null,
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
];

const inputRoleOptions = [
  "none",
  "dhw_enable",
  "night_mode",
  "thermostat",
  "heat_call",
  "recirc_demand",
];

const $ = (id) => document.getElementById(id);

function toast(message, variant = "info") {
  const el = $("toast");
  el.textContent = message;
  el.className = `toast show ${variant}`;
  setTimeout(() => {
    el.className = "toast";
  }, 2800);
}

async function fetchJson(url, options) {
  const response = await fetch(url, options);
  if (!response.ok) {
    const text = await response.text();
    throw new Error(`${response.status} ${response.statusText}: ${text}`);
  }
  if (response.status === 204) {
    return null;
  }
  return response.json();
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
  if (input.type === "number") {
    const value = input.value;
    return value === "" ? null : Number(value);
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
  container.querySelectorAll("[data-path]").forEach((input) => {
    const path = input.dataset.path;
    setInputValue(input, getPath(data, path));
    input.addEventListener("change", () => {
      setPath(data, path, parseInputValue(input));
    });
  });
}

function ensureConfigDefaults() {
  state.config = state.config || {};
  state.config.inputNames = state.config.inputNames || Array(8).fill("");
  state.config.relayNames = state.config.relayNames || Array(8).fill("");
  state.config.inputActiveLevels = state.config.inputActiveLevels || Array(8).fill(1);
  state.config.relayMap = state.config.relayMap || Array.from({ length: 8 }, () => ({ input: 0, polarity: false }));
  state.config.iofunc = state.config.iofunc || { inputs: [], outputs: [] };
  state.config.iofunc.inputs = state.config.iofunc.inputs.length
    ? state.config.iofunc.inputs
    : Array.from({ length: 8 }, () => ({ role: "" }));
  state.config.iofunc.outputs = state.config.iofunc.outputs.length
    ? state.config.iofunc.outputs
    : Array.from({ length: 8 }, () => ({ role: "", params: {} }));
  state.config.thermometers = state.config.thermometers || { mqtt: [{}, {}], ble: {} };
  state.config.thermometers.mqtt = state.config.thermometers.mqtt || [{}, {}];
  if (state.config.thermometers.mqtt.length < 2) {
    state.config.thermometers.mqtt = [state.config.thermometers.mqtt[0] || {}, {}];
  }
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
}

function renderValveConfig() {
  const container = $("valveConfig");
  container.innerHTML = "";
  state.config.iofunc.outputs.forEach((output, index) => {
    const params = output.params || {};
    const row = document.createElement("div");
    row.className = "card";
    row.innerHTML = `
      <h3>Relé ${index + 1}</h3>
      <div class="form-grid">
        <label class="field"><span>Role</span><input list="outputRoles" data-path="iofunc.outputs.${index}.role" /></label>
        <label class="field"><span>Peer relé</span><input type="number" min="0" max="8" data-path="iofunc.outputs.${index}.params.peerRel" /></label>
        <label class="field"><span>Travel time (s)</span><input type="number" min="0" data-path="iofunc.outputs.${index}.params.travelTime" /></label>
        <label class="field"><span>Pulse time (s)</span><input type="number" min="0" data-path="iofunc.outputs.${index}.params.pulseTime" /></label>
        <label class="field"><span>Guard time (s)</span><input type="number" min="0" data-path="iofunc.outputs.${index}.params.guardTime" /></label>
        <label class="field"><span>Min switch (s)</span><input type="number" min="0" data-path="iofunc.outputs.${index}.params.minSwitchS" /></label>
        <label class="field"><span>Invert dir</span><input type="checkbox" data-path="iofunc.outputs.${index}.params.invertDir" /></label>
        <label class="field"><span>Default pos</span>
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
}

function renderThermometers() {
  const mqttContainer = $("mqttThermometers");
  mqttContainer.innerHTML = "";
  state.config.thermometers.mqtt.forEach((slot, index) => {
    const row = document.createElement("div");
    row.className = "card";
    row.innerHTML = `
      <h3>MQTT slot ${index + 1}</h3>
      <div class="form-grid">
        <label class="field"><span>Name</span><input data-path="thermometers.mqtt.${index}.name" /></label>
        <label class="field"><span>Topic</span><input data-path="thermometers.mqtt.${index}.topic" /></label>
        <label class="field"><span>JSON key</span><input data-path="thermometers.mqtt.${index}.jsonKey" /></label>
      </div>
      <div class="status-box" id="mqttLive${index}">—</div>
    `;
    mqttContainer.appendChild(row);
  });
  bindInputs(mqttContainer, state.config);

  const bleContainer = $("bleThermometer");
  bleContainer.innerHTML = `
    <label class="field"><span>Name</span><input data-path="thermometers.ble.name" /></label>
    <label class="field"><span>ID</span><input data-path="thermometers.ble.id" /></label>
    <div class="status-box" id="bleThermoLive">—</div>
  `;
  bindInputs(bleContainer, state.config);
}

function renderDallasDiag() {
  const container = $("dallasDiag");
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
  container.innerHTML = "";
  sourceKeys.forEach((item) => {
    const source = state.config?.equitherm?.[item.key] || {};
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.sourceKey = item.key;
    card.innerHTML = `
      <h3>${item.label}</h3>
      <div class="form-grid">
        <label class="field"><span>Source</span>
          <select class="source-select" data-source-type>
            <option value="">—</option>
            <option value="ds18b20">DS18B20</option>
            <option value="mqtt">MQTT</option>
            <option value="ble">BLE</option>
          </select>
        </label>
        <label class="field"><span>Max age (ms)</span><input type="number" min="0" data-source-field="maxAgeMs" /></label>
      </div>
      <div class="source-fields" data-source-fields>
        <div class="form-grid" data-source-block="ds18b20">
          <label class="field"><span>GPIO</span><input type="number" min="0" max="3" data-source-field="gpio" /></label>
          <label class="field"><span>ROM</span><input data-source-field="rom" list="dallas-${item.key}" /></label>
          <datalist id="dallas-${item.key}"></datalist>
        </div>
        <div class="form-grid" data-source-block="mqtt">
          <label class="field"><span>MQTT slot</span>
            <select data-mqtt-slot>
              <option value="">—</option>
              <option value="0">Slot 1</option>
              <option value="1">Slot 2</option>
            </select>
          </label>
          <label class="field"><span>Topic</span><input data-source-field="topic" /></label>
          <label class="field"><span>JSON key</span><input data-source-field="jsonKey" /></label>
        </div>
        <div class="form-grid" data-source-block="ble">
          <label class="field"><span>BLE ID</span><input data-source-field="bleId" /></label>
        </div>
      </div>
    `;
    container.appendChild(card);

    const select = card.querySelector("[data-source-type]");
    select.value = source.source || "";
    const fields = card.querySelectorAll("[data-source-field]");
    fields.forEach((field) => {
      field.value = source[field.dataset.sourceField] ?? "";
      field.addEventListener("change", () => {
        updateEquithermSource(item.key, field.dataset.sourceField, parseInputValue(field));
      });
    });
    select.addEventListener("change", () => {
      updateEquithermSource(item.key, "source", select.value || null);
      updateSourceVisibility(card, select.value);
    });
    const mqttSlot = card.querySelector("[data-mqtt-slot]");
    mqttSlot.addEventListener("change", () => {
      const idx = mqttSlot.value;
      if (idx === "") {
        return;
      }
      const slot = state.config.thermometers.mqtt[idx];
      if (slot) {
        updateEquithermSource(item.key, "topic", slot.topic || "");
        updateEquithermSource(item.key, "jsonKey", slot.jsonKey || "tempC");
        card.querySelector("[data-source-field=topic]").value = slot.topic || "";
        card.querySelector("[data-source-field=jsonKey]").value = slot.jsonKey || "tempC";
      }
    });
    const list = card.querySelector("datalist");
    const dallas = state.dash?.dallas || [];
    dallas.forEach((bus) => {
      (bus.devices || []).forEach((dev) => {
        const option = document.createElement("option");
        option.value = dev.rom;
        option.label = dev.tempC !== undefined ? `${dev.tempC.toFixed(2)} °C` : "";
        list.appendChild(option);
      });
    });
    updateSourceVisibility(card, select.value || "");
  });
}

function updateSourceVisibility(card, type) {
  card.querySelectorAll("[data-source-block]").forEach((block) => {
    block.style.display = block.dataset.sourceBlock === type ? "grid" : "none";
  });
}

function updateEquithermSource(key, field, value) {
  state.config.equitherm = state.config.equitherm || {};
  state.config.equitherm[key] = state.config.equitherm[key] || {};
  state.config.equitherm[key][field] = value;
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
  select.innerHTML = "";
  const modes = state.config?.modes || [];
  modes.forEach((mode) => {
    const option = document.createElement("option");
    option.value = mode.name || mode.id || "";
    option.textContent = mode.label || mode.name || mode.id || "(bez názvu)";
    select.appendChild(option);
  });
}

function updateStatusUI() {
  const status = state.status || {};
  const network = status.wifi?.connected ? `Wi-Fi ${status.wifi.ip || ""}` : status.eth?.connected ? `ETH ${status.eth.ip || ""}` : "offline";
  $("statusNetwork").textContent = `Síť: ${network}`;
  $("statusMqtt").textContent = `MQTT: ${status.mqtt?.connected ? "online" : "offline"}`;
  $("statusBle").textContent = `BLE: ${status.ble?.connected || status.ble?.meteoFix ? "online" : "offline"}`;
  $("statusMode").textContent = `Režim: ${status.systemMode || "?"} / ${status.controlMode || "?"}`;

  $("dashMode").textContent = `${status.systemMode || "?"} • ${status.controlMode || "?"}`;
  $("eqOutdoor").textContent = formatTemp(status.equitherm?.outdoorC);
  $("eqTarget").textContent = formatTemp(status.equitherm?.targetFlowC);
  $("eqFlow").textContent = formatTemp(status.equitherm?.flowC);
  $("eqValve").textContent = status.equitherm?.valve ? `${status.equitherm.valve.posPct ?? "?"}%` : "—";
  $("dashEquitherm").textContent = status.equitherm ? `${status.equitherm.enabled ? "ON" : "OFF"} / ${status.equitherm.active ? "active" : "idle"} / ${status.equitherm.reason || ""}` : "—";

  $("dashTuv").textContent = status.tuv ? `${status.tuv.enabled ? "ON" : "OFF"} / ${status.tuv.active ? "active" : "idle"} / ${status.tuv.reason || ""}` : "—";
  $("tuvDemand").textContent = status.tuv?.demandActive ? "active" : "inactive";
  $("tuvRelay").textContent = status.tuv?.boilerRelayOn ? "ON" : "OFF";
  $("tuvValve").textContent = status.tuv?.valve ? `${status.tuv.valve.posPct ?? "?"}%` : "—";

  $("heapFree").textContent = status.heap?.free ?? "—";
  $("heapMin").textContent = status.heap?.minFree ?? "—";
  $("heapLargest").textContent = status.heap?.largest ?? "—";

  renderRelayGrid(status.relays || []);
  renderInputGrid(status.inputs || []);
  renderTempGrid(status.temps || []);
  $("eqStatus").textContent = JSON.stringify(status.equitherm || {}, null, 2);
  $("calStatus").textContent = JSON.stringify(status.valves || status.valvesList || {}, null, 2);
}

function updateDashUI() {
  renderDallasDiag();
  const mqttTemps = state.dash?.mqttTemps || [];
  mqttTemps.forEach((entry, index) => {
    const el = $("mqttLive" + index);
    if (!el) {
      return;
    }
    el.textContent = entry ? `${entry.valid ? "valid" : "invalid"} • ${entry.tempC ?? "—"} °C • age ${entry.ageMs ?? "—"} ms` : "—";
  });
  const bleTemps = state.dash?.bleTemps || [];
  const bleEntry = bleTemps[0];
  const bleLive = $("bleThermoLive");
  if (bleLive) {
    bleLive.textContent = bleEntry ? `${bleEntry.valid ? "valid" : "invalid"} • ${bleEntry.tempC ?? "—"} °C` : "—";
  }
}

function renderRelayGrid(relays) {
  const container = $("relayGrid");
  container.innerHTML = "";
  for (let i = 0; i < 8; i += 1) {
    const active = Boolean(relays[i]);
    const chip = document.createElement("button");
    chip.className = `relay-chip ${active ? "active" : ""}`;
    chip.textContent = `R${i + 1} ${active ? "ON" : "OFF"}`;
    chip.addEventListener("click", () => toggleRelay(i + 1, !active));
    container.appendChild(chip);
  }
}

function renderInputGrid(inputs) {
  const container = $("inputGrid");
  container.innerHTML = "";
  for (let i = 0; i < 8; i += 1) {
    const active = Boolean(inputs[i]);
    const chip = document.createElement("div");
    chip.className = `input-chip ${active ? "active" : ""}`;
    chip.textContent = `IN${i + 1} ${active ? "ON" : "OFF"}`;
    container.appendChild(chip);
  }
}

function renderTempGrid(temps) {
  const container = $("tempGrid");
  container.innerHTML = "";
  for (let i = 0; i < 8; i += 1) {
    const entry = temps[i] || {};
    const valid = entry.valid !== false;
    const chip = document.createElement("div");
    chip.className = `temp-chip ${valid ? "" : "invalid"}`;
    chip.textContent = `T${i + 1} ${entry.tempC ?? "—"} °C`;
    container.appendChild(chip);
  }
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
  const points = [
    { x: day.tout1, y: day.tflow1, color: "#2563eb" },
    { x: day.tout2, y: day.tflow2, color: "#1d4ed8" },
    { x: night.tout1, y: night.tflow1, color: "#9333ea" },
    { x: night.tout2, y: night.tflow2, color: "#7e22ce" },
  ].filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));

  if (!points.length) {
    ctx.fillStyle = "#94a3b8";
    ctx.fillText("Není k dispozici", 20, 20);
    return;
  }

  const xs = points.map((p) => p.x);
  const ys = points.map((p) => p.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const pad = 20;
  const scaleX = (canvas.width - pad * 2) / (maxX - minX || 1);
  const scaleY = (canvas.height - pad * 2) / (maxY - minY || 1);

  ctx.strokeStyle = "#cbd5f5";
  ctx.beginPath();
  ctx.moveTo(pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, canvas.height - pad);
  ctx.lineTo(canvas.width - pad, pad);
  ctx.stroke();

  points.forEach((point) => {
    const x = pad + (point.x - minX) * scaleX;
    const y = canvas.height - pad - (point.y - minY) * scaleY;
    ctx.fillStyle = point.color;
    ctx.beginPath();
    ctx.arc(x, y, 4, 0, Math.PI * 2);
    ctx.fill();
  });
}

function formatTemp(value) {
  if (value === undefined || value === null) {
    return "—";
  }
  return `${Number(value).toFixed(1)} °C`;
}

async function loadAll() {
  try {
    state.caps = await fetchJson("/api/caps");
  } catch (error) {
    console.warn(error);
  }
  try {
    state.config = await fetchJson("/api/config");
  } catch (error) {
    toast("Nelze načíst config", "error");
  }
  ensureConfigDefaults();
  renderCaps();
  renderInputsConfig();
  renderRelayNames();
  renderRelayMap();
  renderIofuncRoles();
  renderValveConfig();
  renderThermometers();
  renderEquithermSources();
  renderModeSelect();
  drawEquithermCurve();

  try {
    state.bleConfig = await fetchJson("/api/ble/config");
    renderBleConfig();
  } catch (error) {
    console.warn(error);
  }

  try {
    state.buzzerConfig = await fetchJson("/api/buzzer");
    renderBuzzerConfig();
  } catch (error) {
    console.warn(error);
  }

  renderAdvancedJsonEditors();
  await refreshStatus();
  await refreshDash();
}

async function refreshStatus() {
  try {
    state.status = await fetchJson("/api/status");
    $("statusDump").textContent = JSON.stringify(state.status, null, 2);
    updateStatusUI();
  } catch (error) {
    console.warn(error);
  }
}

async function refreshDash() {
  try {
    state.dash = await fetchJson("/api/dash");
    renderEquithermSources();
    renderThermometers();
    updateDashUI();
  } catch (error) {
    console.warn(error);
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
  const payload = {};
  if (section === "equitherm") {
    payload.equitherm = state.config.equitherm || {};
  }
  if (section === "tuv") {
    payload.tuv = state.config.tuv || {};
  }
  if (section === "relay") {
    payload.relayMap = state.config.relayMap;
    payload.relayNames = state.config.relayNames;
    payload.inputNames = state.config.inputNames;
    payload.inputActiveLevels = state.config.inputActiveLevels;
    payload.autoDefaultOffUnmapped = state.config.autoDefaultOffUnmapped;
  }
  if (section === "iofunc") {
    payload.iofunc = state.config.iofunc;
  }
  if (section === "valves") {
    payload.iofunc = state.config.iofunc;
  }
  if (section === "thermometers") {
    payload.thermometers = state.config.thermometers;
    payload.mqtt = state.config.mqtt || {};
  }
  try {
    await fetchJson("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    toast("Config uložen");
  } catch (error) {
    toast("Chyba při uložení configu", "error");
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
  try {
    state.blePaired = await fetchJson("/api/ble/paired");
    const container = $("blePaired");
    container.innerHTML = "";
    (state.blePaired || []).forEach((device) => {
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
  } catch (error) {
    console.warn(error);
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
  try {
    state.bleStatus = await fetchJson("/api/ble/status");
    $("bleStatus").textContent = JSON.stringify(state.bleStatus, null, 2);
  } catch (error) {
    console.warn(error);
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
  output.params.peerRel = Number($("calPeer").value || 0);
  output.params.invertDir = $("calInvert").checked;
  output.params.travelTime = Number($("calTravel").value || 0);
  output.params.pulseTime = Number($("calPulse").value || 0);
  output.params.guardTime = Number($("calGuard").value || 0);
  output.params.minSwitchS = Number($("calMinSwitch").value || 0);
  output.params.defaultPos = $("calDefault").value;
  state.config.iofunc.outputs[idx] = output;
  renderValveConfig();
  toast("Kalibrace přenesena do configu");
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
  $("pausePolling").addEventListener("click", () => {
    state.polling = !state.polling;
    $("pausePolling").textContent = state.polling ? "Pozastavit" : "Spustit";
  });
  $("setAuto").addEventListener("click", () => setControlMode("auto"));
  $("setManual").addEventListener("click", () => setControlMode("manual"));
  $("manualModeSelect").addEventListener("change", (event) => setManualMode(event.target.value));

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

  $("testRelayA").addEventListener("click", () => {
    const master = Number($("calMaster").value || 0);
    if (master) {
      testRelayRaw(master, true);
    }
  });
  $("testRelayB").addEventListener("click", () => {
    const peer = Number($("calPeer").value || 0);
    if (peer) {
      testRelayRaw(peer, true);
    }
  });
  $("stopRelay").addEventListener("click", () => {
    const master = Number($("calMaster").value || 0);
    const peer = Number($("calPeer").value || 0);
    if (master) {
      testRelayRaw(master, false);
    }
    if (peer) {
      testRelayRaw(peer, false);
    }
  });
  $("saveCalibration").addEventListener("click", applyCalibration);
}

function startPolling() {
  state.statusTimer = setInterval(() => {
    if (state.polling) {
      refreshStatus();
    }
  }, 2000);
  state.dashTimer = setInterval(() => {
    if (state.polling) {
      refreshDash();
      loadBleStatus();
      loadBlePaired();
    }
  }, 8000);
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
}

document.addEventListener("DOMContentLoaded", async () => {
  initRoleLists();
  setupEvents();
  await loadAll();
  await loadBleStatus();
  await loadBlePaired();
  startPolling();
});
