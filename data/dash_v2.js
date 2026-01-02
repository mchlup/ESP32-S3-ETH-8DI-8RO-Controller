(() => {
  const RELAY_COUNT = 8;
  const INPUT_COUNT = 8;

  const $id = (id) => document.getElementById(id);

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const isTempRole = (r) => r === "temp_ntc10k" || r === "temp_dallas";
  const isValveMaster = (r) => r === "valve_3way_2rel";

  const fmtTemp = (v) => (typeof v === "number" && Number.isFinite(v)) ? `${v.toFixed(1)} °C` : "—";

  let dash = { temps: [], valves: [] };

  async function apiGetJson(url){
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) throw new Error(`${r.status}`);
    return await r.json();
  }

  async function apiPostJson(url, body){
    const r = await fetch(url, {
      method: "POST",
      headers: { "Content-Type":"application/json" },
      body: JSON.stringify(body),
    });
    if (!r.ok) throw new Error(`${r.status}`);
    return await r.json().catch(()=> ({}));
  }

  function getCfg(){
    return (window.App && window.App.getConfig) ? (window.App.getConfig() || {}) : {};
  }
  function getStatus(){
    return (window.App && window.App.getStatus) ? (window.App.getStatus() || {}) : {};
  }

  function getRoleInputs(cfg){
    const arr = (((cfg.iofunc||{}).inputs)||[]);
    return Array.from({length: INPUT_COUNT}, (_,i) => (arr[i] && arr[i].role) ? arr[i].role : "none");
  }
  function getRoleRelays(cfg){
    // current schema uses iofunc.outputs; keep backward compatibility with older keys
    const iof = (cfg.iofunc || {});
    const arr = (iof.outputs || iof.relays || iof.relay || []);
    return Array.from({length: RELAY_COUNT}, (_,i) => (arr[i] && arr[i].role) ? arr[i].role : "none");
  }
  function getNameInputs(cfg, i){
    const names = cfg.inputNames || [];
    return names[i] || `Vstup ${i+1}`;
  }
  function getNameRelays(cfg, r){
    const names = cfg.relayNames || [];
    return names[r] || `Relé ${r+1}`;
  }

  function render() {
    const cfg = getCfg();
    const st  = getStatus();

    const inRoles = getRoleInputs(cfg);
    const reRoles = getRoleRelays(cfg);

    const cardValves = $id("cardValves");
    const valveGrid  = $id("valveGrid");

    const valves = Array.isArray(dash.valves) ? dash.valves : [];
    const vByMaster = new Map();
    for (const v of valves){
      const m = Number(v.master||0);
      if (m>=1 && m<=RELAY_COUNT) vByMaster.set(m-1, v);
    }

    let valveCount = 0;
    if (valveGrid) valveGrid.innerHTML = "";

    for (let r=0;r<RELAY_COUNT;r++){
      if (!isValveMaster(reRoles[r])) continue;
      valveCount++;

      const v = vByMaster.get(r) || {};
      const pct = clamp(Number(v.posPct ?? 0), 0, 100);
      const moving = !!v.moving;
      const targetB = !!v.targetB;

      const tile = document.createElement("div");
      tile.className = "ioTile" + (moving ? " moving" : "");
      tile.innerHTML = `
        <div>
          <div class="ioName">${getNameRelays(cfg, r)}</div>
          <div class="ioSub">Pozice: ${pct}% • Cíl: ${targetB ? "B(100%)" : "A(0%)"}${moving ? " • pohyb" : ""}</div>
        </div>
        <div class="ioRight">
          <div class="dial" style="--p:${pct}%">
            <div class="dialTxt">${pct}%</div>
          </div>
          <div class="sw ${targetB ? "on" : ""}" data-relay="${r}">
            <div class="knob"></div>
          </div>
        </div>
      `;
      valveGrid && valveGrid.appendChild(tile);
    }

    if (cardValves) cardValves.style.display = valveCount ? "" : "none";
    const cardTemps = $id("cardTemps");
    const tempGrid  = $id("tempGrid");
    let tempCount = 0;
    if (tempGrid) tempGrid.innerHTML = "";

    const temps = Array.isArray(dash.temps) ? dash.temps : [];
    const vArr  = Array.isArray(dash.tempsValid) ? dash.tempsValid : [];

    // --- Dallas header thermometers (GPIO0..3) ---
    const dNames = Array.isArray(cfg.dallasNames) ? cfg.dallasNames : [];
    const dDiag  = Array.isArray(dash.dallas) ? dash.dallas : [];
    const diagByGpio = new Map();
    for (const g of dDiag){
      const gpio = Number(g?.gpio ?? -1);
      if (gpio >= 0) diagByGpio.set(gpio, g);
    }

    for (let gpio=0; gpio<=3; gpio++){
      const t = temps[gpio];
      const isValid = !!vArr[gpio];
      const dg = diagByGpio.get(gpio) || {};
      const devs = Array.isArray(dg.devices) ? dg.devices : [];
      const devCount = devs.length;
      const show = devCount>0 || (typeof t === "number" && Number.isFinite(t)) || isValid;
      if (!show) continue;
      tempCount++;
      const name = String(dNames[gpio] || "").trim() || `Teploměr ${gpio+1}`;
      const sub = `Dallas/DS18B20 • GPIO${gpio}${devCount>1 ? ` • ${devCount} čidla` : ""}`;
      const tile = document.createElement("div");
      tile.className = "ioTile";
      tile.innerHTML = `
        <div>
          <div class="ioName">${name}</div>
          <div class="ioSub">${sub}</div>
        </div>
        <div class="ioRight">
          <div class="tempValue ${isValid ? "" : "muted"}" title="${isValid ? "" : "Neplatné / poslední známá hodnota"}">${fmtTemp(t)}</div>
        </div>
      `;
      tempGrid && tempGrid.appendChild(tile);
    }

    // --- Terminal inputs with explicit temperature role (kept for backward compatibility) ---
    for (let i=0;i<INPUT_COUNT;i++){
      const role = String(inRoles[i] || "none");
      const isTemp = role === "temp_ntc10k" || role === "temp_dallas";
      if (!isTemp) continue;
      // avoid duplicating header GPIO0..3 tiles
      if (role === "temp_dallas" && i <= 3) continue;

      tempCount++;
      const t = temps[i];
      const isValid = !!vArr[i];
      const tile = document.createElement("div");
      tile.className = "ioTile";
      tile.innerHTML = `
        <div>
          <div class="ioName">${getNameInputs(cfg, i)}</div>
          <div class="ioSub">${role === "temp_dallas" ? "Dallas/DS18B20" : "NTC"} • vstup ${i+1}</div>
        </div>
        <div class="ioRight">
          <div class="tempValue ${isValid ? "" : "muted"}" title="${isValid ? "" : "Neplatné / poslední známá hodnota"}">${fmtTemp(t)}</div>
        </div>
      `;
      tempGrid && tempGrid.appendChild(tile);
    }

    if (cardTemps) cardTemps.style.display = tempCount ? "" : "none";
  }

  async function refreshDash(){
    try {
      dash = await apiGetJson("/api/dash");
      render();
    } catch(e) {}
  }

  window.App = window.App || {};
  const prevCfg = window.App.onConfigLoaded;
  window.App.onConfigLoaded = (cfg) => {
    try { prevCfg && prevCfg(cfg); } catch(_) {}
    render();
  };

  const prevSt = window.App.onStatusLoaded;
  window.App.onStatusLoaded = (st) => {
    try { prevSt && prevSt(st); } catch(_) {}
    render();
  };

  document.addEventListener("DOMContentLoaded", () => {
    render();
    refreshDash();
    setInterval(refreshDash, 1200);

    const vg = $id("valveGrid");
    vg && vg.addEventListener("click", async (e) => {
      const sw = e.target.closest(".sw[data-relay]");
      if (!sw) return;
      const idx = Number(sw.dataset.relay);
      const on = !sw.classList.contains("on");
      try {
        await apiPostJson("/api/mode_ctrl", { action: "relay_set", relay: idx+1, value: on });
        refreshDash();
      } catch {}
    });
  });
})();
