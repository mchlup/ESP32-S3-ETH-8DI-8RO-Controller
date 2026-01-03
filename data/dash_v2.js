(() => {
  const RELAY_COUNT = 8;
  const INPUT_COUNT = 8;

  const $id = (id) => document.getElementById(id);

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const isTempRole = (r) => r === "temp_ntc10k" || r === "temp_dallas";
  const isValveMaster = (r) => r === "valve_3way_2rel";

  const fmtTemp = (v) => (typeof v === "number" && Number.isFinite(v)) ? `${v.toFixed(1)} °C` : "—";

  const computeTargetFlow = (tout, slope, shift, minFlow, maxFlow) => {
    const t = (20 - tout) * slope + 20 + shift;
    const cl = clamp(t, minFlow, maxFlow);
    return cl;
  };

  const getEqCfg = (cfg) => {
    const e = (cfg && typeof cfg === "object" && cfg.equitherm && typeof cfg.equitherm === "object") ? cfg.equitherm : {};
    return {
      enabled: !!e.enabled,
      minFlow: (typeof e.minFlow === "number") ? e.minFlow : 22,
      maxFlow: (typeof e.maxFlow === "number") ? e.maxFlow : 50,
      slopeDay: (typeof e.slopeDay === "number") ? e.slopeDay : 1.25,
      shiftDay: (typeof e.shiftDay === "number") ? e.shiftDay : 30,
      slopeNight: (typeof e.slopeNight === "number") ? e.slopeNight : 1.0,
      shiftNight: (typeof e.shiftNight === "number") ? e.shiftNight : 25,
      outdoor: (e.outdoor && typeof e.outdoor === "object") ? e.outdoor : {},
      flow: (e.flow && typeof e.flow === "object") ? e.flow : {},
      valve: (e.valve && typeof e.valve === "object") ? e.valve : {},
    };
  };

  function dallasLabel(cfg, gpio, rom) {
    const names = Array.isArray(cfg?.dallasNames) ? cfg.dallasNames : [];
    const n = String(names[gpio] || "").trim();
    const head = n || `Teploměr ${gpio+1}`;
    const tail = rom ? ` • ${String(rom).toUpperCase()}` : "";
    return `${head}${tail}`;
  }

  function findMqttThermoByTopic(cfg, topic, jsonKey) {
    const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
    const t = String(topic || "").trim();
    const k = String(jsonKey || "").trim();
    if (!t) return null;
    for (let i=0;i<list.length;i++){
      const it = list[i] || {};
      const tt = String(it.topic || "").trim();
      const kk = String(it.jsonKey || "tempC").trim();
      if (tt === t && kk === k) return { idx: i+1, name: String(it.name || "").trim() };
    }
    return null;
  }

  function describeEqSource(cfg, srcCfg, kind) {
    const s = String(srcCfg?.source || "dallas");
    if (s === "dallas") {
      const gpio = Number(srcCfg?.gpio ?? 0);
      const rom = String(srcCfg?.rom || "").trim();
      return {
        type: "dallas",
        key: `dallas:${gpio}`,
        name: (kind === "outdoor") ? "Venkovní teplota" : "Teplota vody",
        sub: `Dallas • GPIO${gpio}${rom ? ` • ${rom.toUpperCase()}` : ""}`,
        dallasGpio: gpio,
      };
    }
    if (s === "mqtt") {
      const topic = String(srcCfg?.topic || "").trim();
      const jsonKey = String(srcCfg?.jsonKey || "").trim();
      const mt = findMqttThermoByTopic(cfg, topic, jsonKey);
      const nm = (mt && mt.name) ? mt.name : ((kind === "outdoor") ? "Venkovní teplota" : "Teplota vody");
      return {
        type: "mqtt",
        key: `mqtt:${topic}:${jsonKey}`,
        name: nm,
        sub: `MQTT • ${topic || "—"}${jsonKey ? ` • ${jsonKey}` : ""}`,
        mqttTopic: topic,
        mqttKey: jsonKey,
      };
    }
    if (s === "ble") {
      const id = String(srcCfg?.bleId || srcCfg?.id || "").trim();
      return {
        type: "ble",
        key: `ble:${id}`,
        name: (kind === "outdoor") ? "Venkovní teplota" : "Teplota vody",
        sub: `BLE • ${id || "—"}`,
        bleId: id,
      };
    }
    if (s && s.startsWith("temp")) {
      // legacy TEMP1..8
      return {
        type: "temp",
        key: s,
        name: (kind === "outdoor") ? "Venkovní teplota" : "Teplota vody",
        sub: `${s.toUpperCase()} (legacy)`
      };
    }
    return {
      type: "none",
      key: "none",
      name: (kind === "outdoor") ? "Venkovní teplota" : "Teplota vody",
      sub: "Nepoužito"
    };
  }

  function drawEqMiniCurve(canvas, cfg, eqCfg, eqStatus) {
    if (!canvas || !canvas.getContext) return;

    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth || 360;
    const cssH = canvas.clientHeight || 140;
    const W = Math.max(140, Math.round(cssW * dpr));
    const H = Math.max(90, Math.round(cssH * dpr));
    if (canvas.width !== W) canvas.width = W;
    if (canvas.height !== H) canvas.height = H;

    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, W, H);

    const xMin = -20, xMax = 20;
    let yMin = Number(eqCfg?.minFlow ?? 22);
    let yMax = Number(eqCfg?.maxFlow ?? 50);
    if (!Number.isFinite(yMin)) yMin = 22;
    if (!Number.isFinite(yMax)) yMax = 50;
    if (Math.abs(yMax - yMin) < 0.1) yMax = yMin + 1;

    const padL = 32, padR = 10, padT = 10, padB = 26;
    const x2px = (x) => padL + ((x - xMin) / (xMax - xMin)) * (W - padL - padR);
    const y2px = (y) => (H - padB) - ((y - yMin) / (yMax - yMin)) * (H - padT - padB);

    // colors (use current text color)
    const fg = (window.getComputedStyle ? getComputedStyle(canvas).color : "#fff") || "#fff";

    // grid
    ctx.save();
    ctx.globalAlpha = 0.22;
    ctx.strokeStyle = fg;
    ctx.lineWidth = 1;
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
    ctx.restore();

    // axes
    ctx.save();
    ctx.globalAlpha = 0.7;
    ctx.strokeStyle = fg;
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(padL, padT);
    ctx.lineTo(padL, H - padB);
    ctx.lineTo(W - padR, H - padB);
    ctx.stroke();
    ctx.restore();


// axis labels
ctx.save();
ctx.fillStyle = fg;
ctx.globalAlpha = 0.65;
ctx.font = `${11*dpr}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial`;
ctx.textAlign = "center";
ctx.textBaseline = "alphabetic";
ctx.fillText("Tout (°C)", padL + (W - padL - padR) / 2, H - 6 * dpr);

// Y label (rotated)
ctx.translate(10 * dpr, padT + (H - padT - padB) / 2);
ctx.rotate(-Math.PI / 2);
ctx.textAlign = "center";
ctx.textBaseline = "middle";
ctx.fillText("Tflow (°C)", 0, 0);
ctx.restore();

    // axis ranges (min/max)
    ctx.save();
    ctx.fillStyle = fg;
    ctx.globalAlpha = 0.55;
    ctx.font = `${10*dpr}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial`;
    // X min/max
    ctx.textBaseline = "alphabetic";
    ctx.textAlign = "left";
    ctx.fillText(`${xMin}`, padL, H - 6 * dpr);
    ctx.textAlign = "right";
    ctx.fillText(`${xMax}`, W - padR, H - 6 * dpr);
    // Y min/max
    const yMinLbl = Math.round(yMin * 10) / 10;
    const yMaxLbl = Math.round(yMax * 10) / 10;
    ctx.textBaseline = "middle";
    ctx.textAlign = "left";
    ctx.fillText(`${yMaxLbl}`, 4 * dpr, y2px(yMax));
    ctx.fillText(`${yMinLbl}`, 4 * dpr, y2px(yMin));
    ctx.restore();


    const slopeDay = Number(eqCfg?.slopeDay ?? 1.25);
    const shiftDay = Number(eqCfg?.shiftDay ?? 30);
    const slopeNight = Number(eqCfg?.slopeNight ?? 1.0);
    const shiftNight = Number(eqCfg?.shiftNight ?? 25);

    function plotLine(slope, shift, dashed){
      ctx.save();
      ctx.strokeStyle = fg;
      ctx.globalAlpha = dashed ? 0.35 : 0.9;
      ctx.lineWidth = 2;
      if (dashed) ctx.setLineDash([6*dpr, 5*dpr]); else ctx.setLineDash([]);
      ctx.beginPath();
      let first = true;
      for (let x = xMin; x <= xMax; x += 1) {
        const y = computeTargetFlow(x, slope, shift, yMin, yMax);
        const px = x2px(x);
        const py = y2px(y);
        if (first) { ctx.moveTo(px, py); first = false; }
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
      ctx.restore();
    }

    plotLine(slopeDay, shiftDay, false);
    plotLine(slopeNight, shiftNight, true);

    // current marker
    const tout = Number(eqStatus?.outdoorC);
    if (Number.isFinite(tout)) {
      const isNight = !!eqStatus?.night;
      const slope = isNight ? slopeNight : slopeDay;
      const shift = isNight ? shiftNight : shiftDay;
      const x = clamp(tout, xMin, xMax);
      const y = computeTargetFlow(x, slope, shift, yMin, yMax);
      const px = x2px(x);
      const py = y2px(y);
      ctx.save();
      ctx.fillStyle = fg;
      ctx.globalAlpha = 0.95;
      ctx.beginPath();
      ctx.arc(px, py, 4*dpr, 0, Math.PI*2);
      ctx.fill();
      ctx.restore();
    }
  }

  const escapeHtml = (s) => String(s ?? "").replace(/[&<>"']/g, (ch) => ({
    "&":"&amp;",
    "<":"&lt;",
    ">":"&gt;",
    '"':"&quot;",
    "'":"&#39;",
  }[ch]));

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

    // --- Ekviterm widget (ventil + venkovní teplota) ---
    const cardEq = $id("cardEquitherm");
    const eqGrid = $id("eqDashGrid");
    const eq = (st && st.equitherm) ? st.equitherm : {};
    const eqCfg = getEqCfg(cfg);
    const eqEnabled = !!eq.enabled;
    const cardModes = $id("cardModes");
    if (cardModes) cardModes.classList.toggle("span2", !eqEnabled);
    let eqValveMaster0 = -1;
    if (eqEnabled && eq){
      const vm = Number(eq.valveMaster || 0);
      if (vm >= 1 && vm <= RELAY_COUNT) eqValveMaster0 = vm-1;
    }
    if (cardEq && eqGrid){
      if (!eqEnabled){
        cardEq.style.display = "none";
        eqGrid.innerHTML = "";
      } else {
        cardEq.style.display = "";

        const outdoorOk = Number.isFinite(eq.outdoorC);
        const flowOk    = Number.isFinite(eq.flowC);
        const targetOk  = Number.isFinite(eq.targetFlowC);

        const outdoorTxt = outdoorOk ? fmtTemp(eq.outdoorC) : "—";
        const flowTxt    = flowOk ? fmtTemp(eq.flowC) : "—";
        const targetTxt  = targetOk ? fmtTemp(eq.targetFlowC) : "—";

        const outDesc = describeEqSource(cfg, eqCfg.outdoor, "outdoor");
        const flowDesc = describeEqSource(cfg, eqCfg.flow, "flow");

        let valveTitle = "Trojcestný ventil";
        if (eqValveMaster0 >= 0) valveTitle = `${getNameRelays(cfg, eqValveMaster0)}`;

        const pct = clamp(Number(eq.valvePosPct ?? 0), 0, 100);
        const moving = !!eq.valveMoving;
        const targetPct = (typeof eq.valveTargetPct === "number" && isFinite(eq.valveTargetPct)) ? Math.round(eq.valveTargetPct) : undefined;

        // Build tiles: Outdoor + Flow + Valve + mini curve
        eqGrid.innerHTML = "";

        const tOutdoor = document.createElement("div");
        tOutdoor.className = "ioTile";
        tOutdoor.innerHTML = `
          <div>
            <div class="ioName">${escapeHtml(outDesc.name)}</div>
            <div class="ioSub">${escapeHtml(outDesc.sub)}</div>
          </div>
          <div class="ioRight">
            <div class="tempValue ${outdoorOk ? "" : "muted"}">${escapeHtml(outdoorTxt)}</div>
          </div>
        `;
        eqGrid.appendChild(tOutdoor);

        const tFlow = document.createElement("div");
        tFlow.className = "ioTile";
        tFlow.innerHTML = `
          <div>
            <div class="ioName">${escapeHtml(flowDesc.name)}</div>
            <div class="ioSub">${escapeHtml(flowDesc.sub)}</div>
          </div>
          <div class="ioRight">
            <div class="tempValue ${flowOk ? "" : "muted"}">${escapeHtml(flowTxt)}</div>
          </div>
        `;
        eqGrid.appendChild(tFlow);

        const tValve = document.createElement("div");
        tValve.className = "ioTile" + (moving ? " moving" : "");
        tValve.innerHTML = `
          <div>
            <div class="ioName">${escapeHtml(valveTitle)}</div>
            <div class="ioSub">Cíl vody: ${escapeHtml(targetTxt)}${eq.night ? " • noc" : ""}${(typeof targetPct === "number") ? ` • Ventil: ${pct}% → ${targetPct}%` : ` • Ventil: ${pct}%`}${moving ? " • pohyb" : ""}</div>
          </div>
          <div class="ioRight">
            <div class="dial" style="--p:${pct}%">
              <div class="dialTxt">${pct}%</div>
            </div>
          </div>
        `;
        eqGrid.appendChild(tValve);

        // Curve tile (wide)
        const tCurve = document.createElement("div");
        tCurve.className = "ioTile eqCurveTile";
        tCurve.style.gridColumn = "1 / -1";
        tCurve.innerHTML = `
          <div>
            <div class="ioName">Ekvitermní křivka</div>
            <div class="ioSub">Denní (plná) + noční (čárkovaná). Tečka = aktuální venkovní teplota.</div>
          </div>
          <div class="ioRight" style="width:100%">
            <canvas id="eqMiniCurve" class="eqMiniCurve" aria-label="Ekvitermní křivka"></canvas>
          </div>
        `;
        eqGrid.appendChild(tCurve);

        // draw after inserted
        const canvas = tCurve.querySelector("#eqMiniCurve");
        if (canvas) drawEqMiniCurve(canvas, cfg, eqCfg, eq);
      }
    }

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
      // ekvitermní ventil zobrazujeme v samostatném widgetu
      if (r === eqValveMaster0) continue;
      valveCount++;

      const v = vByMaster.get(r) || {};
      const pct = clamp(Number(v.posPct ?? 0), 0, 100);
      const moving = !!v.moving;
      const targetB = !!v.targetB;
      const targetPct = (typeof v.targetPct === "number" && isFinite(v.targetPct)) ? Math.round(v.targetPct) : (targetB ? 100 : 0);

      const tile = document.createElement("div");
      tile.className = "ioTile" + (moving ? " moving" : "");
      tile.innerHTML = `
        <div>
          <div class="ioName">${getNameRelays(cfg, r)}</div>
          <div class="ioSub">Pozice: ${pct}% • Cíl: ${targetPct}%${moving ? " • pohyb" : ""}</div>
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

    // Determine which temperature tiles should be hidden because they are part of Ekviterm block
    const eqOut = describeEqSource(cfg, eqCfg.outdoor, "outdoor");
    const eqFlow = describeEqSource(cfg, eqCfg.flow, "flow");
    const skip = {
      dallasGpio: new Set(),
      mqtt: new Set(),
      ble: new Set(),
      tempIdx: new Set(),
    };
    if (eqEnabled) {
      if (eqOut.type === "dallas" && Number.isFinite(eqOut.dallasGpio)) skip.dallasGpio.add(eqOut.dallasGpio);
      if (eqFlow.type === "dallas" && Number.isFinite(eqFlow.dallasGpio)) skip.dallasGpio.add(eqFlow.dallasGpio);
      if (eqOut.type === "mqtt" && eqOut.mqttTopic) skip.mqtt.add(`${eqOut.mqttTopic}|${eqOut.mqttKey||""}`);
      if (eqFlow.type === "mqtt" && eqFlow.mqttTopic) skip.mqtt.add(`${eqFlow.mqttTopic}|${eqFlow.mqttKey||""}`);
      if (eqOut.type === "ble" && eqOut.bleId) skip.ble.add(eqOut.bleId);
      if (eqFlow.type === "ble" && eqFlow.bleId) skip.ble.add(eqFlow.bleId);

      const parseTempIdx = (k) => {
        const m = /^temp(\d+)$/.exec(String(k||""));
        if (!m) return null;
        const n = Number(m[1]);
        if (!Number.isFinite(n) || n < 1 || n > 8) return null;
        return n - 1;
      };
      if (eqOut.type === "temp") {
        const ix = parseTempIdx(eqOut.key);
        if (ix != null) skip.tempIdx.add(ix);
      }
      if (eqFlow.type === "temp") {
        const ix = parseTempIdx(eqFlow.key);
        if (ix != null) skip.tempIdx.add(ix);
      }
    }

    // --- Dallas header thermometers (GPIO0..3) ---
    const dNames = Array.isArray(cfg.dallasNames) ? cfg.dallasNames : [];
    const dDiag  = Array.isArray(dash.dallas) ? dash.dallas : [];
    const diagByGpio = new Map();
    for (const g of dDiag){
      const gpio = Number(g?.gpio ?? -1);
      if (gpio >= 0) diagByGpio.set(gpio, g);
    }

    for (let gpio=0; gpio<=3; gpio++){
      if (eqEnabled && skip.dallasGpio.has(gpio)) continue;
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
      if (eqEnabled && skip.tempIdx.has(i)) continue;
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

    // --- MQTT thermometers (configured in "Teploměry") ---
    const mqttTemps = Array.isArray(dash.mqttTemps) ? dash.mqttTemps : [];
    for (const mt of mqttTemps){
      const topic = String(mt?.topic || "").trim();
      if (!topic) continue;
      const jsonKey = String(mt?.jsonKey || "").trim();
      if (eqEnabled && skip.mqtt.has(`${topic}|${jsonKey}`)) continue;
      tempCount++;
      const name = String(mt?.name || "").trim() || `MQTT teploměr ${Number(mt?.idx||0) || ""}`.trim();
      const isValid = !!mt?.valid;
      const val = (typeof mt?.tempC === "number") ? mt.tempC : NaN;
      const sub = `MQTT • ${topic}${jsonKey ? ` • ${jsonKey}` : ""}`;
      const tile = document.createElement("div");
      tile.className = "ioTile";
      tile.innerHTML = `
        <div>
          <div class="ioName">${escapeHtml(name)}</div>
          <div class="ioSub">${escapeHtml(sub)}</div>
        </div>
        <div class="ioRight">
          <div class="tempValue ${isValid ? "" : "muted"}" title="${isValid ? "" : "Neplatné / poslední známá hodnota"}">${fmtTemp(val)}</div>
        </div>
      `;
      tempGrid && tempGrid.appendChild(tile);
    }

    // --- BLE thermometers (configured in "Teploměry") ---
    const bleTemps = Array.isArray(dash.bleTemps) ? dash.bleTemps : [];
    for (const bt of bleTemps){
      const id = String(bt?.id || "").trim();
      if (!id) continue;
      // Pokud příjem BLE teplot není aktivní nebo data nejsou validní, na dashboardu ho nezobrazujeme.
      if (!bt?.valid || !Number.isFinite(bt?.tempC)) continue;
      if (eqEnabled && skip.ble.has(id)) continue;
      tempCount++;
      const name = String(bt?.label || "").trim() || "BLE teploměr";
      const isValid = !!bt?.valid;
      const val = (typeof bt?.tempC === "number") ? bt.tempC : NaN;
      const sub = `BLE • ${id}`;
      const tile = document.createElement("div");
      tile.className = "ioTile";
      tile.innerHTML = `
        <div>
          <div class="ioName">${escapeHtml(name)}</div>
          <div class="ioSub">${escapeHtml(sub)}</div>
        </div>
        <div class="ioRight">
          <div class="tempValue ${isValid ? "" : "muted"}" title="${isValid ? "" : "Neplatné / poslední známá hodnota"}">${fmtTemp(val)}</div>
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
