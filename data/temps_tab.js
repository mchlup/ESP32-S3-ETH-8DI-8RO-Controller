/* Temps config tab (Dallas header / DS18B20) - GPIO0..3 on pin header
   Independent from terminal inputs (Funkce I/O).
*/
(() => {
  const App = window.App;
  if (!App) return;

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
      return;
    }

    if (row?.dataset?.ble){
      const name = (row.querySelector("input.bleName")?.value || "").trim();
      const id = (row.querySelector("input.bleId")?.value || "").trim();
      cfg.thermometers.ble = { name: name || "BLE Meteo", id: id || "meteo.tempC" };
      App.setConfig?.(cfg);
      return;
    }
  }

  async function refreshTempsAndDiag(){
    try{
      const r = await fetch("/api/dash", {cache:"no-store"});
      if(!r.ok) return;
      const j = await r.json();
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
      const details = devs.map(d=>{
        const rom = escapeHtml(d?.rom || "—");
        const t = isFiniteNum(d?.tempC) ? `${d.tempC.toFixed(1)} °C` : "—";
        return `<div class="mono" style="font-size:12px">ROM ${rom} • ${t} ${d?.valid ? "" : "<span class='muted'>(invalid)</span>"}</div>`;
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

    $id("btnSaveTempsCfg")?.addEventListener("click", async ()=>{
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
    });
  }

  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg)=>{
    try { prev && prev(cfg); } catch(e){}
    render();
  };

  window.addEventListener("DOMContentLoaded", ()=>{
    bind();
    render();
    refreshTempsAndDiag();
    setInterval(refreshTempsAndDiag, 1500);
  });

  window.TempsTab = { render };
})();
