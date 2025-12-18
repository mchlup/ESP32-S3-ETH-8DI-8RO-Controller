/* New UI (v2) – uses the same backend endpoints (/api/*) */
(() => {
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

  // ---------- navigation ----------
  const showPage = (name) => {
    $$(".navItem").forEach(b => b.classList.toggle("active", b.dataset.page === name));
    $$(".page").forEach(p => p.classList.toggle("active", p.id === `page-${name}`));
    const h = `#${name}`;
    if (location.hash !== h) history.replaceState(null, "", h);
  };

  const setCfgTab = (name) => {
    $$(".tab").forEach(b => b.classList.toggle("active", b.dataset.ctab === name));
    $$(".tabPage").forEach(p => p.classList.toggle("active", p.id === `cfg-${name}`));
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
    $("#topHint").textContent = `System: ${st.systemMode || st.mode || "—"} • Control: ${st.controlMode || "—"}`;

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
    const st = state.status || {};
    const relays = Array.isArray(st.relays) ? st.relays : [];
    const inputs = Array.isArray(st.inputs) ? st.inputs : [];

    const rg = $("#relayGrid");
    rg.innerHTML = "";
    for (let i=0;i<RELAY_COUNT;i++){
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
    return cfg;
  };

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
      const when = it.when?.items?.[0];
      const thenA = it.then?.[0];
      const whenTxt = when ? `${when.type||"input"}: IN${when.input||"?"}=${when.state||"ACTIVE"}` : "—";
      const thenTxt = thenA ? `${thenA.type||"relay_set"}: R${thenA.relay||"?"}=${String(thenA.value ?? "?")}` : "—";
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

  const fillRuleSelects = () => {
    const si = $("#ruleWhenInput");
    const sr = $("#ruleThenRelay");
    if (si){
      si.innerHTML = Array.from({length: INPUT_COUNT}, (_,i)=>`<option value="${i+1}">Vstup ${i+1}</option>`).join("");
    }
    if (sr){
      sr.innerHTML = Array.from({length: RELAY_COUNT}, (_,i)=>`<option value="${i+1}">Relé ${i+1}</option>`).join("");
    }
  };

  const openRuleModal = (ruleId) => {
    const r = rulesEnsureShape();
    const modal = $("#ruleModal");
    if (!modal) return;

    fillRuleSelects();

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

    const w0 = it.when?.items?.[0] || { input:1, state:"ACTIVE" };
    $("#ruleWhenInput").value = String(w0.input ?? 1);
    $("#ruleWhenState").value = String(w0.state ?? "ACTIVE");

    const a0 = it.then?.[0] || { relay:1, value:true };
    $("#ruleThenRelay").value = String(a0.relay ?? 1);
    $("#ruleThenValue").value = (a0.value ? "true" : "false");

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
    const it = {
      id,
      enabled: $("#ruleEnabled").value === "true",
      priority: Number($("#rulePriority").value || 50),
      stopOnMatch: $("#ruleStopOnMatch").value === "true",
      debounceMs: Number($("#ruleDebounceMs").value || 0),
      minOnMs: Number($("#ruleMinOnMs").value || 0),
      minOffMs: Number($("#ruleMinOffMs").value || 0),
      name: ($("#ruleName").value || "").trim(),
      desc: ($("#ruleDesc").value || "").trim(),
      when: { op: "AND", items: [ { type:"input", input: Number($("#ruleWhenInput").value || 1), state: $("#ruleWhenState").value } ] },
      then: [ { type:"relay_set", relay: Number($("#ruleThenRelay").value || 1), value: $("#ruleThenValue").value === "true" } ]
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
    toast("Restart…");
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
    if (nowUi > (state.ui?.lock?.systemUntil || 0) && document.activeElement !== selSM) {
      selSM.value = MODE_IDS.includes(sm) ? sm : MODE_IDS[0];
    }

    // keep config editor synced if open
    if (state.config) $("#cfgJson").value = prettyJson(state.config);
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
    $$(".navItem").forEach(b => b.addEventListener("click", () => showPage(b.dataset.page)));

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
        await setControlMode($("#selControlMode").value);
        await setSystemMode($("#selSystemMode").value);
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
          await setControlMode($("#selControlMode").value);
          await setSystemMode($("#selSystemMode").value);
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
    setCfgTab("io");
    const start = (location.hash || "").replace("#","") || "dash";
    showPage(start);
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
