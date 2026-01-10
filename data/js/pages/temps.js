/* Temps config tab (Dallas header / DS18B20) - GPIO0..3 on pin header
   Independent from terminal inputs (Funkce I/O).
*/
(() => {
  let initialized = false;
  let diagTimer = null;

  const init = () => {
    if (initialized) return;
    initialized = true;
    const App = window.App;
    if (!App) {
      initialized = false;
      return;
    }

    const $id = (id)=>document.getElementById(id);
    const GPIO_COUNT = 4; // GPIO0..3

  const isFiniteNum = (v)=> (typeof v === "number" && Number.isFinite(v));
  const fmt = (v)=> isFiniteNum(v) ? `${v.toFixed(1)} °C` : "—";

  const escapeHtml = (s)=>
    String(s ?? "").replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  function ensureShape(cfg){
    cfg = (cfg && typeof cfg === "object") ? cfg : {};
    cfg.dallasNames = Array.isArray(cfg.dallasNames) ? cfg.dallasNames : ["","","",""];
    cfg.dallasAddrs = Array.isArray(cfg.dallasAddrs) ? cfg.dallasAddrs : ["","","",""];
    cfg.dallasNames = cfg.dallasNames.slice(0, GPIO_COUNT);
    cfg.dallasAddrs = cfg.dallasAddrs.slice(0, GPIO_COUNT);
    while (cfg.dallasNames.length < GPIO_COUNT) cfg.dallasNames.push("");
    while (cfg.dallasAddrs.length < GPIO_COUNT) cfg.dallasAddrs.push("");
    cfg.dallasNames = cfg.dallasNames.map(v => String(v || ""));
    cfg.dallasAddrs = cfg.dallasAddrs.map(v => String(v || ""));

    // Extra thermometers (MQTT/BLE)
    cfg.thermometers = (cfg.thermometers && typeof cfg.thermometers === "object") ? cfg.thermometers : {};
    cfg.thermometers.mqtt = Array.isArray(cfg.thermometers.mqtt) ? cfg.thermometers.mqtt : [];
    cfg.thermometers.mqtt = cfg.thermometers.mqtt.slice(0,2);
    while (cfg.thermometers.mqtt.length < 2) cfg.thermometers.mqtt.push({ name: "", topic: "", jsonKey: "tempC" });
    cfg.thermometers.mqtt = cfg.thermometers.mqtt.map((o)=>{
      o = (o && typeof o === "object") ? o : {};
      return { name: String(o.name||""), topic: String(o.topic||""), jsonKey: String(o.jsonKey||"tempC") };
    });

    cfg.thermometers.ble = (cfg.thermometers.ble && typeof cfg.thermometers.ble === "object") ? cfg.thermometers.ble : {};
    cfg.thermometers.ble.name = String(cfg.thermometers.ble.name || "BLE Meteo");
    cfg.thermometers.ble.id   = String(cfg.thermometers.ble.id || "meteo.tempC");

    // System roles (meaning within heating system)
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
    return cfg;
  }

  function defaultName(gpio){
    return `Teploměr ${gpio+1}`;
  }

  function render(){
    const host = $id("tblTempsCfg");
    if (!host) return;

    const cfg = ensureShape(App.getConfig?.() || {});
    host.innerHTML = "";

    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = `
      <div class="col1">#</div>
      <div class="col2">Popisek</div>
      <div class="col3">GPIO</div>
      <div class="col4">Adresa (volitelné)</div>
      <div class="col5">Teplota</div>
    `;
    host.appendChild(head);

    for (let gpio=0; gpio<GPIO_COUNT; gpio++){
      const row = document.createElement("div");
      row.className = "trow";
      row.dataset.gpio = String(gpio);
      row.innerHTML = `
        <div class="col1">${gpio+1}</div>
        <div class="col2">
          <input class="pfield nameField" data-k="name" data-typ="s" type="text"
                 value="${escapeHtml(cfg.dallasNames[gpio] || "")}" placeholder="${escapeHtml(defaultName(gpio))}">
        </div>
        <div class="col3 mono">GPIO${gpio}</div>
        <div class="col4">
          <input class="pfield addrField" data-k="addr" data-typ="s" type="text"
                 placeholder="28FF... (16 hex)" value="${escapeHtml(cfg.dallasAddrs[gpio] || "")}">
          <div class="muted" style="margin-top:6px">Pokud je prázdné, použije se první senzor na sběrnici.</div>
        </div>
        <div class="col5 tempCell" id="tempCell${gpio}">—</div>
      `;
      host.appendChild(row);
    }

    renderExtraTempsCfg();
    renderTempRolesCfg();
  }

  let lastDash = null;

  const ROLE_LABELS = {
    outdoor: "Venkovní teplota",
    flow: "Teplota topné vody (výstup)",
    return: "Teplota vratky",
    dhw: "TUV / bojler",
    tankTop: "Akumulační nádrž – horní",
    tankMid: "Akumulační nádrž – střed",
    tankBottom: "Akumulační nádrž – spodní",
  };

  const filterState = {
    onlyAssigned: false,
    query: "",
  };

  const applyRoleFilters = () => {
    const q = filterState.query.trim().toLowerCase();
    const host = $id("tblTempRolesCfg");
    if (!host) return;
    host.querySelectorAll(".trow").forEach((row) => {
      if (row.classList.contains("head")) return;
      const roleKey = (row.dataset.role || "").toLowerCase();
      const label = (ROLE_LABELS[row.dataset.role] || "").toLowerCase();
      const src = (row.dataset.source || "").toLowerCase();
      if (filterState.onlyAssigned && (!src || src === "none")) {
        row.style.display = "none";
        return;
      }
      if (q && !roleKey.includes(q) && !label.includes(q) && !src.includes(q)) {
        row.style.display = "none";
        return;
      }
      row.style.display = "";
    });
  };

  function encodeRoleVal(cfg, role){
    const src = String(role?.source || "none");
    if (src === "dallas"){
      const gpio = Number(role?.gpio ?? 0);
      const rom = String(role?.rom || "").trim();
      return `dallas:${gpio}:${rom}`;
    }
    if (src === "mqtt"){
      const idx = Number(role?.mqttIdx || role?.preset || 0);
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2) return `mqtt:${idx}`;
      // try to match by topic
      const t = String(role?.topic || "").trim();
      const k = String(role?.jsonKey || "").trim();
      const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
      for (let i=0;i<Math.min(2,list.length);i++){
        const it = list[i] || {};
        if (String(it.topic||"").trim() === t && String(it.jsonKey||"tempC").trim() === k) return `mqtt:${i+1}`;
      }
      return "none";
    }
    if (src === "ble"){
      const id = String(role?.bleId || role?.id || "").trim();
      return `ble:${id}`;
    }
    if (src && src.startsWith("temp")) return src;
    return "none";
  }

  function applyRoleFromValue(cfg, roleObj, val){
    const v = String(val || "none");
    // reset
    roleObj.source = "none";
    roleObj.gpio = 0;
    roleObj.rom = "";
    roleObj.topic = "";
    roleObj.jsonKey = "";
    roleObj.bleId = "";
    roleObj.mqttIdx = 0;

    if (v === "none") return;
    if (v.startsWith("dallas:")){
      const parts = v.split(":");
      const gpio = parseInt(parts[1] || "0", 10);
      const rom = (parts[2] || "").trim();
      roleObj.source = "dallas";
      roleObj.gpio = Number.isFinite(gpio) ? gpio : 0;
      roleObj.rom = rom;
      return;
    }
    if (v.startsWith("mqtt:")){
      const idx = parseInt(v.split(":")[1] || "0", 10);
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2){
        const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
        const it = list[idx-1] || {};
        roleObj.source = "mqtt";
        roleObj.mqttIdx = idx;
        roleObj.topic = String(it.topic || "").trim();
        roleObj.jsonKey = String(it.jsonKey || "tempC").trim();
        return;
      }
      return;
    }
    if (v.startsWith("ble:")){
      roleObj.source = "ble";
      roleObj.bleId = v.substring(4);
      return;
    }
    if (/^temp[1-8]$/.test(v)){
      roleObj.source = v;
      return;
    }
  }

  function buildRoleOptionsHtml(cfg){
    const out = [];
    out.push(`<option value="none">Nepřiřazeno</option>`);

    // Dallas header GPIO0..3
    out.push(`<optgroup label="Dallas DS18B20 (GPIO0..3)">`);
    for (let gpio=0; gpio<GPIO_COUNT; gpio++){
      const nm = String(cfg.dallasNames[gpio] || "").trim() || defaultName(gpio);
      out.push(`<option value="dallas:${gpio}:">${escapeHtml(nm)} • GPIO${gpio} (auto)</option>`);
      // known devices (if available)
      const d = Array.isArray(lastDash?.dallas) ? lastDash.dallas[gpio] : null;
      const devs = Array.isArray(d?.devices) ? d.devices : [];
      for (const dv of devs){
        const rom = String(dv?.rom || "").trim();
        if (!rom) continue;
        out.push(`<option value="dallas:${gpio}:${escapeHtml(rom)}">${escapeHtml(nm)} • GPIO${gpio} • ${escapeHtml(rom.toUpperCase())}</option>`);
      }
    }
    out.push(`</optgroup>`);

    // MQTT presets (2×)
    out.push(`<optgroup label="MQTT teploměry (záložka Teploměry)">`);
    const ml = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
    for (let i=0;i<2;i++){
      const it = ml[i] || {};
      const name = String(it.name || "").trim() || `MQTT ${i+1}`;
      const topic = String(it.topic || "").trim();
      const key = String(it.jsonKey || "tempC").trim();
      const dis = topic ? "" : "disabled";
      const sub = topic ? `${topic}${key ? ` • ${key}` : ""}` : "(nenastaveno)";
      out.push(`<option value="mqtt:${i+1}" ${dis}>${escapeHtml(name)} • ${escapeHtml(sub)}</option>`);
    }
    out.push(`</optgroup>`);

    // BLE
    out.push(`<optgroup label="BLE">`);
    {
      const b = cfg?.thermometers?.ble || {};
      const name = String(b.name || "BLE").trim();
      const id = String(b.id || "meteo.tempC").trim();
      out.push(`<option value="ble:${escapeHtml(id)}">${escapeHtml(name)} • ${escapeHtml(id)}</option>`);
    }
    out.push(`</optgroup>`);

    // Legacy TEMP1..8 (terminal inputs)
    out.push(`<optgroup label="Vstupy TEMP (legacy)">`);
    for (let i=1;i<=8;i++) out.push(`<option value="temp${i}">Vstup TEMP${i}</option>`);
    out.push(`</optgroup>`);

    return out.join("");
  }

  function renderTempRolesCfg(){
    const host = $id("tblTempRolesCfg");
    if (!host) return;

    const cfg = ensureShape(App.getConfig?.() || {});
    host.innerHTML = "";

    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = `
      <div class="col1">Typ</div>
      <div class="col2">Zdroj</div>
      <div class="col3">Teplota</div>
      <div class="col4">Stav</div>
    `;
    host.appendChild(head);

    const optsHtml = buildRoleOptionsHtml(cfg);

    const roles = cfg.thermometers.roles || {};
    for (const key of Object.keys(ROLE_LABELS)){
      const role = roles[key] || { source: "none" };
      const row = document.createElement("div");
      row.className = "trow";
      row.dataset.role = key;
      row.dataset.source = String(role.source || "none");
      row.innerHTML = `
        <div class="col1">${escapeHtml(ROLE_LABELS[key])}</div>
        <div class="col2"><select class="pfield roleSel" data-role="${escapeHtml(key)}"></select></div>
        <div class="col3 tempCell" id="roleTempCell_${escapeHtml(key)}">—</div>
        <div class="col4 muted" id="roleTempState_${escapeHtml(key)}">—</div>
      `;
      host.appendChild(row);
      const sel = row.querySelector("select");
      sel.innerHTML = optsHtml;
      sel.value = encodeRoleVal(cfg, role);
    }

    updateRoleTempCells(cfg, lastDash);
    applyRoleFilters();
  }

  function tryGetRoleTemp(cfg, dash, role){
    const src = String(role?.source || "none");
    if (!dash) return { valid:false };
    if (src === "dallas"){
      const gpio = Number(role?.gpio ?? 0);
      const rom = String(role?.rom || "").trim().toUpperCase();
      const d = Array.isArray(dash.dallas) ? dash.dallas[gpio] : null;
      const devs = Array.isArray(d?.devices) ? d.devices : [];
      let best = null;
      for (const dv of devs){
        const r = String(dv?.rom || "").trim().toUpperCase();
        if (rom && r !== rom) continue;
        if (dv && dv.valid && isFiniteNum(dv.tempC)) { best = dv; break; }
      }
      if (!best && !rom){
        // first valid
        for (const dv of devs){
          if (dv && dv.valid && isFiniteNum(dv.tempC)) { best = dv; break; }
        }
      }
      if (best) return { valid:true, tempC: best.tempC };
      return { valid:false };
    }
    if (src === "mqtt"){
      const idx = Number(role?.mqttIdx || role?.preset || 0);
      const mt = Array.isArray(dash.mqttTemps) ? dash.mqttTemps.find(x => Number(x.idx) === idx) : null;
      const val = (mt && typeof mt.tempC === "number") ? mt.tempC : (mt ? mt.valueC : NaN);
      if (mt && mt.valid && isFiniteNum(val)) return { valid:true, tempC: val, ageMs: mt.ageMs };
      return { valid:false };
    }
    if (src === "ble"){
      const id = String(role?.bleId || role?.id || "").trim();
      const bt = Array.isArray(dash.bleTemps) ? dash.bleTemps.find(x => String(x.id||"") === id) : null;
      const val = (bt && typeof bt.tempC === "number") ? bt.tempC : (bt ? bt.valueC : NaN);
      if (bt && bt.valid && isFiniteNum(val)) return { valid:true, tempC: val, ageMs: bt.ageMs };
      return { valid:false };
    }
    if (src && src.startsWith("temp")){
      const i = parseInt(src.substring(4) || "0", 10) - 1;
      if (i>=0 && i<8){
        const v = dash.temps?.[i];
        const ok = dash.tempsValid?.[i];
        if (ok && isFiniteNum(v)) return { valid:true, tempC: v };
      }
      return { valid:false };
    }
    return { valid:false };
  }

  function updateRoleTempCells(cfg, dash){
    const roles = cfg?.thermometers?.roles || {};
    for (const key of Object.keys(ROLE_LABELS)){
      const role = roles[key] || {};
      const cell = $id(`roleTempCell_${key}`);
      const state = $id(`roleTempState_${key}`);
      if (!cell || !state) continue;
      const r = tryGetRoleTemp(cfg, dash, role);
      cell.textContent = (r.valid && isFiniteNum(r.tempC)) ? `${r.tempC.toFixed(1)} °C` : "—";
      state.textContent = r.valid ? "OK" : "Bez dat";
      state.className = r.valid ? "" : "muted";
    }
  }

  function renderExtraTempsCfg(){
    const host = $id("tblExtraTempsCfg");
    if (!host) return;

    const cfg = ensureShape(App.getConfig?.() || {});
    host.innerHTML = "";

    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = `
      <div class="col1">Typ</div>
      <div class="col2">Popisek</div>
      <div class="col3">Zdroj</div>
      <div class="col4">jsonKey</div>
      <div class="col5">Teplota</div>
    `;
    host.appendChild(head);

    // MQTT #1..2
    for (let i=0;i<2;i++){
      const m = cfg.thermometers.mqtt[i];
      const row = document.createElement("div");
      row.className = "trow";
      row.dataset.mqtt = String(i);
      row.innerHTML = `
        <div class="col1 mono">MQTT ${i+1}</div>
        <div class="col2"><input class="pfield mqttName" type="text" value="${escapeHtml(m.name)}" placeholder="Např. Venkovní"></div>
        <div class="col3"><input class="pfield mqttTopic" type="text" value="${escapeHtml(m.topic)}" placeholder="esp/senzor/1"></div>
        <div class="col4"><input class="pfield mqttKey" type="text" value="${escapeHtml(m.jsonKey)}" placeholder="tempC"></div>
        <div class="col5 tempCell" id="mqttTempCell${i}">—</div>
      `;
      host.appendChild(row);
    }

    // BLE
    {
      const b = cfg.thermometers.ble;
      const row = document.createElement("div");
      row.className = "trow";
      row.dataset.ble = "1";
      row.innerHTML = `
        <div class="col1 mono">BLE</div>
        <div class="col2"><input class="pfield bleName" type="text" value="${escapeHtml(b.name)}" placeholder="Např. BLE Meteo"></div>
        <div class="col3"><input class="pfield bleId" type="text" value="${escapeHtml(b.id)}" placeholder="meteo.tempC"></div>
        <div class="col4 muted">—</div>
        <div class="col5 tempCell" id="bleTempCellCfg">—</div>
      `;
      host.appendChild(row);
    }
  }

  function applyFromRow(row){
    const gpio = Number(row?.dataset?.gpio ?? -1);
    if (!(gpio>=0 && gpio<GPIO_COUNT)) return;

    const cfg = ensureShape(App.getConfig?.() || {});

    const nameVal = (row.querySelector("input.nameField")?.value || "").trim();
    const addrVal = (row.querySelector("input.addrField")?.value || "").trim();

    cfg.dallasNames[gpio] = nameVal;
    cfg.dallasAddrs[gpio] = addrVal;

    App.setConfig?.(cfg);
  }

  function applyFromExtraRow(row){
    const cfg = ensureShape(App.getConfig?.() || {});

    if (row?.dataset?.mqtt != null){
      const idx = Number(row.dataset.mqtt);
      if (!(idx>=0 && idx<2)) return;
      const name = (row.querySelector("input.mqttName")?.value || "").trim();
      const topic = (row.querySelector("input.mqttTopic")?.value || "").trim();
      const key = (row.querySelector("input.mqttKey")?.value || "").trim();
      cfg.thermometers.mqtt[idx] = { name, topic, jsonKey: key || "tempC" };
      App.setConfig?.(cfg);
      renderTempRolesCfg();
      return;
    }

    if (row?.dataset?.ble){
      const name = (row.querySelector("input.bleName")?.value || "").trim();
      const id = (row.querySelector("input.bleId")?.value || "").trim();
      cfg.thermometers.ble = { name: name || "BLE Meteo", id: id || "meteo.tempC" };
      App.setConfig?.(cfg);
      renderTempRolesCfg();
      return;
    }
  }

  function applyFromRoleRow(row){
    const key = String(row?.dataset?.role || "");
    if (!key || !(key in ROLE_LABELS)) return;
    const sel = row.querySelector("select.roleSel");
    if (!sel) return;

    const cfg = ensureShape(App.getConfig?.() || {});
    cfg.thermometers.roles = cfg.thermometers.roles || {};
    const r = (cfg.thermometers.roles[key] && typeof cfg.thermometers.roles[key] === "object") ? cfg.thermometers.roles[key] : {};
    applyRoleFromValue(cfg, r, sel.value);
    cfg.thermometers.roles[key] = r;
    App.setConfig?.(cfg);
    updateRoleTempCells(cfg, lastDash);
    row.dataset.source = String(r.source || "none");
    applyRoleFilters();
  }

  async function refreshTempsAndDiag(){
    try{
      const r = await fetch("/api/dash", {cache:"no-store"});
      if(!r.ok) return;
      const j = await r.json();
      lastDash = j;
      const temps = Array.isArray(j?.temps) ? j.temps : [];
      const vArr  = Array.isArray(j?.tempsValid) ? j.tempsValid : [];
      const mqttTemps = Array.isArray(j?.mqttTemps) ? j.mqttTemps : [];
      const bleTemps  = Array.isArray(j?.bleTemps) ? j.bleTemps : [];

      for (let gpio=0; gpio<GPIO_COUNT; gpio++){
        const el = $id(`tempCell${gpio}`);
        if (!el) continue;
        const t = temps[gpio];
        const valid = !!vArr[gpio];
        el.textContent = fmt(t);
        el.classList.toggle("muted", !valid);
        el.title = valid ? "" : "Neplatné / poslední známá hodnota";
      }

      const t = new Date();
      const lu = $id("tempsLastUpd");
      if (lu) lu.textContent = `${String(t.getHours()).padStart(2,"0")}:${String(t.getMinutes()).padStart(2,"0")}:${String(t.getSeconds()).padStart(2,"0")}`;

      // Dallas diagnostics table
      const arr = Array.isArray(j?.dallas) ? j.dallas : [];
      renderDallasDiag(arr);

      // MQTT/BLE diagnostics table
      renderExtraTempsDiag(j);

      // MQTT temps into config table
      for (let i=0;i<2;i++){
        const cell = $id(`mqttTempCell${i}`);
        if (!cell) continue;
        const it = mqttTemps.find(x => Number(x?.idx) === (i+1)) || mqttTemps[i];
        const t = it?.tempC;
        const valid = !!it?.valid;
        cell.textContent = fmt(t);
        cell.classList.toggle("muted", !valid);
        const ageMs = Number(it?.ageMs);
        cell.title = (valid && Number.isFinite(ageMs)) ? `Stáří: ${Math.round(ageMs/1000)} s` : (valid ? "" : "Neplatné / bez dat");
      }

      // BLE into config table
      {
        const cell = $id("bleTempCellCfg");
        if (cell){
          const it = bleTemps[0];
          const t = it?.tempC;
          const valid = !!it?.valid;
          cell.textContent = fmt(t);
          cell.classList.toggle("muted", !valid);
          cell.title = valid ? "" : "Neplatné / bez fix";
        }
      }

      // Update role temperature preview cells
      updateRoleTempCells(ensureShape(App.getConfig?.() || {}), lastDash);
    }catch(e){}
  }

  function statusLabel(s){
    switch(String(s||"")){
      case "ok": return "OK";
      case "no_sensor": return "Bez čidla";
      case "error": return "Chyba";
      case "disabled": return "Vypnuto";
      default: return String(s||"—");
    }
  }

  function renderDallasDiag(arr){
    const host = $id("tblDallasDiag");
    if (!host) return;

    host.innerHTML = "";

    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = `
      <div class="col1">GPIO</div>
      <div class="col2">Stav</div>
      <div class="col3">Čidla</div>
      <div class="col4">Detaily</div>
    `;
    host.appendChild(head);

    (Array.isArray(arr)?arr:[]).forEach((g)=>{
      const gpio = Number(g?.gpio ?? 0);
      const st = statusLabel(g?.status);
      const devs = Array.isArray(g?.devices) ? g.devices : [];

      // Role badges (from config: Teploměry -> Význam teploměrů)
      const cfg = ensureShape(App.getConfig?.() || {});
      const roles = (cfg.thermometers && cfg.thermometers.roles && typeof cfg.thermometers.roles === "object") ? cfg.thermometers.roles : {};

      const roleTagsFor = (gpio, rom, isFirstDevice) => {
        const tags = [];
        const romU = String(rom||"").trim().toUpperCase();
        for (const k of Object.keys(ROLE_LABELS)){
          const r = roles[k] || {};
          if (String(r.source||"") !== "dallas") continue;
          const rg = Number(r.gpio ?? 0);
          if (rg !== gpio) continue;
          const rr = String(r.rom || r.addr || "").trim().toUpperCase();
          if (rr){
            if (rr === romU) tags.push(ROLE_LABELS[k]);
          }else{
            // "auto" mapping (no ROM) -> annotate the first device on that GPIO
            if (isFirstDevice) tags.push(ROLE_LABELS[k] + " (auto)");
          }
        }
        if (!tags.length) return "";
        return tags.map(t=>`<span class="badge" style="padding:2px 8px; font-size:11px; margin-left:6px">${escapeHtml(t)}</span>`).join("");
      };

      const details = devs.map((d, idx)=>{
        const rom = String(d?.rom || "—");
        const romHtml = escapeHtml(rom);
        const t = isFiniteNum(d?.tempC) ? `${d.tempC.toFixed(1)} °C` : "—";
        const tags = roleTagsFor(gpio, rom, idx === 0);
        return `<div class="mono" style="font-size:12px">ROM ${romHtml} • ${t} ${d?.valid ? "" : "<span class=\"muted\">(invalid)</span>"}${tags}</div>`;
      }).join("");

      const row = document.createElement("div");
      row.className = "trow";
      row.innerHTML = `
        <div class="col1 mono">${gpio}</div>
        <div class="col2">${escapeHtml(st)}</div>
        <div class="col3 mono">${devs.length}</div>
        <div class="col4">${details || "<span class='muted'>—</span>"}</div>
      `;
      host.appendChild(row);
    });
  }

  function renderExtraTempsDiag(dash){
    const host = $id("tblExtraTempsDiag");
    if (!host) return;

    const mqttTemps = Array.isArray(dash?.mqttTemps) ? dash.mqttTemps : [];
    const bleTemps = Array.isArray(dash?.bleTemps) ? dash.bleTemps : [];

    host.innerHTML = "";

    const head = document.createElement("div");
    head.className = "trow head";
    head.innerHTML = `
      <div class="col1">Typ</div>
      <div class="col2">Zdroj</div>
      <div class="col3">Hodnota</div>
      <div class="col4">Stav</div>
    `;
    host.appendChild(head);

    // MQTT 1..2
    for (let i=0;i<2;i++){
      const it = mqttTemps.find(x => Number(x?.idx) === (i+1)) || mqttTemps[i] || {};
      const topic = String(it?.topic || "").trim();
      const key = String(it?.jsonKey || "").trim();
      if (!topic) continue;
      const name = String(it?.name || "").trim() || `MQTT teploměr ${i+1}`;
      const valid = !!it?.valid && Number.isFinite(it?.tempC);
      const val = valid ? `${Number(it.tempC).toFixed(1)} °C` : "—";
      const ageMs = Number(it?.ageMs);
      const ageTxt = (valid && Number.isFinite(ageMs)) ? `${Math.round(ageMs/1000)} s` : "—";
      const st = valid ? `OK (${ageTxt})` : "Bez dat";

      const row = document.createElement("div");
      row.className = "trow";
      row.innerHTML = `
        <div class="col1 mono">MQTT</div>
        <div class="col2">
          <div>${escapeHtml(name)}</div>
          <div class="muted" style="font-size:12px">${escapeHtml(topic)}${key ? ` • ${escapeHtml(key)}` : ""}</div>
        </div>
        <div class="col3 mono">${escapeHtml(val)}</div>
        <div class="col4 ${valid ? "" : "muted"}">${escapeHtml(st)}</div>
      `;
      host.appendChild(row);
    }

    // BLE (1)
    const bt = bleTemps[0];
    if (bt){
      const id = String(bt?.id || "").trim();
      const label = String(bt?.label || "").trim() || "BLE teploměr";
      const valid = !!bt?.valid && Number.isFinite(bt?.tempC);
      const val = valid ? `${Number(bt.tempC).toFixed(1)} °C` : "—";
      const row = document.createElement("div");
      row.className = "trow";
      row.innerHTML = `
        <div class="col1 mono">BLE</div>
        <div class="col2">
          <div>${escapeHtml(label)}</div>
          <div class="muted" style="font-size:12px">${escapeHtml(id || "—")}</div>
        </div>
        <div class="col3 mono">${escapeHtml(val)}</div>
        <div class="col4 ${valid ? "" : "muted"}">${valid ? "OK" : "Bez dat"}</div>
      `;
      host.appendChild(row);
    }
  }

  function bind(){
    const host = $id("tblTempsCfg");
    if (host){
      host.addEventListener("input", (ev)=>{
        const row = ev.target.closest(".trow");
        if (!row) return;
        if (ev.target.matches(".pfield")) applyFromRow(row);
      });
      host.addEventListener("change", (ev)=>{
        const row = ev.target.closest(".trow");
        if (!row) return;
        if (ev.target.matches(".pfield")) applyFromRow(row);
      });
    }

    const host2 = $id("tblExtraTempsCfg");
    if (host2){
      host2.addEventListener("input", (ev)=>{
        const row = ev.target.closest(".trow");
        if (!row) return;
        if (ev.target.matches(".pfield")) applyFromExtraRow(row);
      });
      host2.addEventListener("change", (ev)=>{
        const row = ev.target.closest(".trow");
        if (!row) return;
        if (ev.target.matches(".pfield")) applyFromExtraRow(row);
      });
    }

    const host3 = $id("tblTempRolesCfg");
    if (host3){
      host3.addEventListener("change", (ev)=>{
        const row = ev.target.closest(".trow");
        if (!row) return;
        if (ev.target.matches("select.roleSel")) applyFromRoleRow(row);
      });
    }

    const updateFilters = () => {
      filterState.onlyAssigned = $id("tempsOnlyAssigned")?.checked ?? false;
      filterState.query = $id("tempsSearch")?.value || "";
      applyRoleFilters();
    };
    $id("tempsOnlyAssigned")?.addEventListener("change", updateFilters);
    $id("tempsSearch")?.addEventListener("input", updateFilters);
    updateFilters();

    const doSave = async ()=>{
      try{
        // Migration: header thermometers are independent; clear old temp roles on terminal inputs 1..4
        const cfg = App.getConfig?.();
        if (cfg && cfg.iofunc && Array.isArray(cfg.iofunc.inputs)) {
          for (let i=0;i<4 && i<cfg.iofunc.inputs.length;i++){
            const role = String(cfg.iofunc.inputs[i]?.role || "none");
            if (role === "temp_dallas" || role === "temp_ntc10k") {
              cfg.iofunc.inputs[i] = { role: "none", params: {} };
            }
          }
        }

        await App.saveConfig?.();
        App.toast?.("Uloženo (Teploměry)." );
      }catch(e){
        App.toast?.("Chyba při ukládání.");
      }
    };

    $id("btnSaveTempsCfg")?.addEventListener("click", doSave);
    $id("btnSaveTempRoles")?.addEventListener("click", doSave);
  }

  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg)=>{
    try { prev && prev(cfg); } catch(e){}
    render();
  };

    bind();
    render();
    refreshTempsAndDiag();
    diagTimer = setInterval(refreshTempsAndDiag, 1500);

    window.TempsTab = { render };
  };

  const unmount = () => {};

  window.Pages = window.Pages || {};
  window.Pages.temps = { id: "temps", mount: init, unmount };
})();
