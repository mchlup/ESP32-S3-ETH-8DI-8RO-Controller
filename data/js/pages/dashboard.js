(() => {
  const RELAY_COUNT = 8;
  const INPUT_COUNT = 8;

  const $id = (id) => document.getElementById(id);

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const isTempRole = (r) => r === "temp_dallas";
  const isValveMaster = (r) => r === "valve_3way_mix" || r === "valve_3way_2rel";

  const fmtTemp = (v) => (typeof v === "number" && Number.isFinite(v)) ? `${v.toFixed(1)} °C` : "—";

  const normFlow = (v, defV) => {
    if (typeof v !== "number" || !Number.isFinite(v)) return defV;
    if (v > 120 && v <= 2000) return v / 10.0;
    return v;
  };
  const normShift = (v, defV) => {
    if (typeof v !== "number" || !Number.isFinite(v)) return defV;
    if (Math.abs(v) > 60 && Math.abs(v) <= 600) return v / 10.0;
    return v;
  };
  const normSlope = (v, defV) => {
    if (typeof v !== "number" || !Number.isFinite(v)) return defV;
    if (v > 10 && v <= 500) return v / 100.0;
    return v;
  };

  const computeTargetFlow = (tout, slope, shift, minFlow, maxFlow, offset = 0) => {
    const t = (20 - tout) * slope + 20 + shift + offset;
    const cl = clamp(t, minFlow, maxFlow);
    return cl;
  };

  const setCardDisabled = (cardId, reason, actionHash) => {
    const card = $id(cardId);
    if (!card) return;
    const disabled = !!reason;
    card.classList.toggle("cardDisabled", disabled);
    let overlay = card.querySelector(".cardOverlay");
    if (!overlay) {
      overlay = document.createElement("div");
      overlay.className = "cardOverlay";
      overlay.innerHTML = `
        <div class="overlayContent">
          <div class="overlayTitle">Funkce není aktivní</div>
          <div class="overlayReason"></div>
          <a class="btn mini overlayBtn" href="#">Opravit</a>
        </div>
      `;
      card.appendChild(overlay);
    }
    const reasonEl = overlay.querySelector(".overlayReason");
    const actionEl = overlay.querySelector(".overlayBtn");
    if (reasonEl) reasonEl.textContent = reason || "";
    if (actionEl) {
      actionEl.setAttribute("href", actionHash || "#");
      actionEl.style.display = actionHash ? "" : "none";
    }
    overlay.style.display = disabled ? "flex" : "none";
  };

  const getEqCfg = (cfg) => {
    const e = (cfg && typeof cfg === "object" && cfg.equitherm && typeof cfg.equitherm === "object") ? cfg.equitherm : {};
    return {
      enabled: !!e.enabled,
      // sane defaults (match refs: day -10->55 / 15->30 ; night -10->50 / 15->25)
      minFlow: normFlow(e.minFlow, 25),
      maxFlow: normFlow(e.maxFlow, 55),
      slopeDay: normSlope(e.slopeDay, 1.0),
      shiftDay: normShift(e.shiftDay, 5.0),
      slopeNight: normSlope(e.slopeNight, 1.0),
      shiftNight: normShift(e.shiftNight, 0.0),
      curveOffsetC: (typeof e.curveOffsetC === "number") ? e.curveOffsetC : 0,
      outdoor: (e.outdoor && typeof e.outdoor === "object") ? e.outdoor : {},
      flow: (e.boilerIn && typeof e.boilerIn === "object") ? e.boilerIn : ((e.flow && typeof e.flow === "object") ? e.flow : {}),
      akuTop: (e.akuTop && typeof e.akuTop === "object") ? e.akuTop : {},
      akuMid: (e.akuMid && typeof e.akuMid === "object") ? e.akuMid : {},
      akuBottom: (e.akuBottom && typeof e.akuBottom === "object") ? e.akuBottom : {},
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
        name: (kind === "outdoor") ? "Venkovní teplota" : (kind === "boilerIn" ? "Teplota boiler_in" : "Teplota vody"),
        sub: `Dallas • GPIO${gpio}${rom ? ` • ${rom.toUpperCase()}` : ""}`,
        dallasGpio: gpio,
      };
    }
    if (s === "mqtt") {
      const idx = Number(srcCfg?.mqttIdx || srcCfg?.preset || 0);
      const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
      let topic = String(srcCfg?.topic || "").trim();
      let jsonKey = String(srcCfg?.jsonKey || "").trim();
      let nm = (kind === "outdoor") ? "Venkovní teplota" : "Teplota vody";
      if (Number.isFinite(idx) && idx >= 1 && idx <= 2) {
        const it = list[idx - 1] || {};
        const t = String(it.topic || "").trim();
        const k = String(it.jsonKey || "tempC").trim();
        if (t) topic = t;
        if (k) jsonKey = k;
        const n = String(it.name || "").trim();
        if (n) nm = n;
      } else {
        const mt = findMqttThermoByTopic(cfg, topic, jsonKey);
        if (mt && mt.name) nm = mt.name;
      }
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
        name: (kind === "outdoor") ? "Venkovní teplota" : (kind === "boilerIn" ? "Teplota boiler_in" : "Teplota vody"),
        sub: `BLE • ${id || "—"}`,
        bleId: id,
      };
    }
    if (s && s.startsWith("temp")) {
      // legacy TEMP1..8
      return {
        type: "temp",
        key: s,
        name: (kind === "outdoor") ? "Venkovní teplota" : (kind === "boilerIn" ? "Teplota boiler_in" : "Teplota vody"),
        sub: `${s.toUpperCase()} (legacy)`
      };
    }
    return {
      type: "none",
      key: "none",
      name: (kind === "outdoor") ? "Venkovní teplota" : (kind === "boilerIn" ? "Teplota boiler_in" : "Teplota vody"),
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

    const padL = 32, padR = 10, padT = 10, padB = 18;
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
    // popisky os + rozsahy
    ctx.save();
    ctx.fillStyle = fg;
    ctx.globalAlpha = 0.70;
    ctx.font = `${11*dpr}px system-ui, -apple-system, Segoe UI, Roboto, Arial`;
    // X label
    ctx.fillText(`T venku (°C)  ${xMin}...${xMax}`, padL, H - 4*dpr);
    // Y label (nahoře vlevo)
    ctx.fillText(`T vody (°C)  ${Math.round(yMin)}...${Math.round(yMax)}`, 6*dpr, padT + 12*dpr);
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

    const slopeDay = Number(eqCfg?.slopeDay ?? 1.25);
    const shiftDay = Number(eqCfg?.shiftDay ?? 30);
    const slopeNight = Number(eqCfg?.slopeNight ?? 1.0);
    const shiftNight = Number(eqCfg?.shiftNight ?? 25);
    const offsetC = Number(eqCfg?.curveOffsetC ?? 0);

    function plotLine(slope, shift, dashed){
      ctx.save();
      ctx.strokeStyle = fg;
      ctx.globalAlpha = dashed ? 0.35 : 0.9;
      ctx.lineWidth = 2;
      if (dashed) ctx.setLineDash([6*dpr, 5*dpr]); else ctx.setLineDash([]);
      ctx.beginPath();
      let first = true;
      for (let x = xMin; x <= xMax; x += 1) {
        const y = computeTargetFlow(x, slope, shift, yMin, yMax, offsetC);
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
      const y = computeTargetFlow(x, slope, shift, yMin, yMax, offsetC);
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
  let initialized = false;

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
  function getDash(){
    return (window.App && window.App.getDash) ? (window.App.getDash() || {}) : dash;
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
    dash = getDash() || dash;
    const cfg = getCfg();
    const st  = getStatus();
    const roleMap = window.App?.getRoleMap?.() || { inputs: {}, outputs: {}, temps: {} };
    const valves = Array.isArray(dash.valves) ? dash.valves : [];
    const vByMaster = new Map();
    for (const v of valves){
      const m = Number(v.master||0);
      if (m>=1 && m<=RELAY_COUNT) vByMaster.set(m-1, v);
    }

    // --- Ekviterm widget (ventil + venkovní teplota) ---
    const cardEq = $id("cardEquitherm");
    const eqGrid = $id("eqDashGrid");
    const eq = (st && st.equitherm) ? st.equitherm : {};
    const eqCfg = getEqCfg(cfg);
    const eqEnabled = !!eq.enabled;
    let eqValveMaster0 = -1;
    if (eqEnabled && eq){
      const vm = Number(eq.valveMaster || 0);
      if (vm >= 1 && vm <= RELAY_COUNT) eqValveMaster0 = vm-1;
    }
    if (cardEq && eqGrid){
      eqGrid.innerHTML = "";
      if (eqEnabled){
        const outdoorOk = Number.isFinite(eq.outdoorC);
        const flowVal   = Number.isFinite(eq.actualC) ? eq.actualC : eq.flowC;
        const flowOk    = Number.isFinite(flowVal);
        const targetVal = Number.isFinite(eq.targetC) ? eq.targetC : eq.targetFlowC;
        const targetOk  = Number.isFinite(targetVal);

        const outdoorTxt = outdoorOk ? fmtTemp(eq.outdoorC) : "—";
        const flowTxt    = flowOk ? fmtTemp(flowVal) : "—";
        const targetTxt  = targetOk ? fmtTemp(targetVal) : "—";

        const outDesc = describeEqSource(cfg, eqCfg.outdoor, "outdoor");
        const flowDesc = describeEqSource(cfg, eqCfg.flow, "boilerIn");

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

        // Curve ranges for popis os
        const xMinCurve = -20, xMaxCurve = 20;
        let yMinCurve = Number(eqCfg?.minFlow ?? 22);
        let yMaxCurve = Number(eqCfg?.maxFlow ?? 50);
        if (!Number.isFinite(yMinCurve)) yMinCurve = 22;
        if (!Number.isFinite(yMaxCurve)) yMaxCurve = 50;
        if (Math.abs(yMaxCurve - yMinCurve) < 0.1) yMaxCurve = yMinCurve + 1;
        const rangeTxt = `Osy: venku ${xMinCurve}...${xMaxCurve} °C, voda ${Math.round(yMinCurve)}...${Math.round(yMaxCurve)} °C`;

        // Curve tile (wide)
        const tCurve = document.createElement("div");
        tCurve.className = "ioTile eqCurveTile";
        tCurve.style.gridColumn = "1 / -1";
        tCurve.innerHTML = `
          <div>
            <div class="ioName">Ekvitermní křivka</div>
            <div class="ioSub">Denní (plná) + noční (čárkovaná). Tečka = aktuální venkovní teplota. <span class="muted">${rangeTxt}</span></div>
          </div>
          <div class="ioRight" style="width:100%">
            <canvas id="eqMiniCurve" class="eqMiniCurve" aria-label="Ekvitermní křivka"></canvas>
          </div>
        `;
        eqGrid.appendChild(tCurve);

        // draw after inserted
        const canvas = tCurve.querySelector("#eqMiniCurve");
        if (canvas) drawEqMiniCurve(canvas, cfg, eqCfg, eq);
      } else {
        eqGrid.innerHTML = `<div class="muted">Ekviterm je vypnutý.</div>`;
      }
    }

    // --- AKU widget ---
    const cardAku = $id("cardAku");
    const akuGrid = $id("akuDashGrid");
    const hasAkuCfg = ["akuTop", "akuMid", "akuBottom"].some((k) => String(eqCfg?.[k]?.source || "none") !== "none");
    if (cardAku && akuGrid) {
      akuGrid.innerHTML = "";
      if (hasAkuCfg || Number.isFinite(eq?.akuTopC)) {
        const topTxt = Number.isFinite(eq.akuTopC) ? fmtTemp(eq.akuTopC) : "—";
        const midTxt = Number.isFinite(eq.akuMidC) ? fmtTemp(eq.akuMidC) : "—";
        const bottomTxt = Number.isFinite(eq.akuBottomC) ? fmtTemp(eq.akuBottomC) : "—";
        const support = eq?.akuSupportActive ? "k dispozici" : "blokováno";
        const supportReason = eq?.akuSupportReason ? ` (${eq.akuSupportReason})` : "";
        akuGrid.innerHTML = `
          <div class="akuTank">
            <div class="akuRow"><div>Top</div><div class="tempValue">${escapeHtml(topTxt)}</div></div>
            <div class="akuRow"><div>Mid</div><div class="tempValue">${escapeHtml(midTxt)}</div></div>
            <div class="akuRow"><div>Bottom</div><div class="tempValue">${escapeHtml(bottomTxt)}</div></div>
            <div class="akuStatus"><span class="akuDot ${eq?.akuSupportActive ? "on" : ""}"></span>AKU support: ${escapeHtml(support)}${escapeHtml(supportReason)}</div>
          </div>
        `;
      } else {
        akuGrid.innerHTML = `<div class="muted">AKU senzory nejsou přiřazené.</div>`;
      }
    }

    // --- V2/V3 widget ---
    const cardValveMix = $id("cardValveMix");
    const valveMixGrid = $id("valveMixGrid");
    if (cardValveMix && valveMixGrid) {
      const v2Master = Number(eq?.valveMaster || 0);
      const v3Master = Number(st?.tuv?.valveMaster || 0);
      valveMixGrid.innerHTML = "";
      if (!v2Master && !v3Master) {
        valveMixGrid.innerHTML = `<div class="muted">Ventily nejsou nakonfigurované.</div>`;
      } else {

        if (v2Master >= 1 && v2Master <= RELAY_COUNT) {
          const pos = clamp(Number(eq?.valvePosPct ?? 0), 0, 100);
          const tgt = clamp(Number(eq?.valveTargetPct ?? pos), 0, 100);
          const moving = !!eq?.valveMoving;
          const tile = document.createElement("div");
          tile.className = "ioTile" + (moving ? " moving" : "");
          tile.innerHTML = `
            <div>
              <div class="ioName">V2 – AKU → kotel</div>
              <div class="ioSub">${escapeHtml(getNameRelays(cfg, v2Master - 1))} • ${pos}% → ${tgt}%${moving ? " • pohyb" : ""}</div>
            </div>
            <div class="ioRight">
              <div class="dial" style="--p:${pos}%"><div class="dialTxt">${pos}%</div></div>
            </div>
          `;
          valveMixGrid.appendChild(tile);
        }

        if (v3Master >= 1 && v3Master <= RELAY_COUNT) {
          const pos = clamp(Number(st?.tuv?.valvePosPct ?? st?.tuv?.valveTargetPct ?? 0), 0, 100);
          const tgt = clamp(Number(st?.tuv?.valveTargetPct ?? pos), 0, 100);
          const moving = (typeof st?.tuv?.valveMoving === "boolean")
            ? st.tuv.valveMoving
            : !!vByMaster.get(v3Master - 1)?.moving;
          const mode = String(st?.tuv?.valveMode || (st?.tuv?.modeActive ? "dhw" : "ch")).toUpperCase();
          const chPct = Number.isFinite(st?.tuv?.chPct) ? Math.round(st.tuv.chPct) : 100;
          const bpPct = Number.isFinite(st?.tuv?.bypassPct) ? Math.round(st.tuv.bypassPct) : 100;
          const invTxt = st?.tuv?.bypassInvert ? "invert" : "normal";
          const tile = document.createElement("div");
          tile.className = "ioTile" + (moving ? " moving" : "");
          tile.innerHTML = `
            <div>
              <div class="ioName">V3 – DHW bypass</div>
              <div class="ioSub">${escapeHtml(getNameRelays(cfg, v3Master - 1))} • ${mode} • ${pos}% → ${tgt}%${moving ? " • pohyb" : ""} • CH ${chPct}% / DHW ${bpPct}% • ${invTxt}</div>
            </div>
            <div class="ioRight">
              <div class="dial" style="--p:${pos}%"><div class="dialTxt">${pos}%</div></div>
            </div>
          `;
          valveMixGrid.appendChild(tile);
        }
      }
    }

    // --- TUV (ohřev bojleru) ---
    const cardTuv = $id("cardTuv");
    const tuvGrid = $id("tuvDashGrid");
    const tuvCfg = cfg?.tuv || {};
    const tuv = st?.tuv || {};
    const hasTuvCfg = !!tuvCfg?.enabled || (tuvCfg?.demandInput > 0) || (tuvCfg?.requestRelay > 0) || (tuvCfg?.valveMaster > 0);
    if (cardTuv && tuvGrid) {
      const active = (typeof tuv.active === "boolean") ? tuv.active : !!tuv.modeActive;
      tuvGrid.innerHTML = "";
      if (!hasTuvCfg && !active) {
        tuvGrid.innerHTML = `<div class="muted">TUV není nakonfigurováno.</div>`;
      } else {

        const relayRoles = getRoleRelays(cfg);
        const relays = Array.isArray(st?.relays) ? st.relays : [];
        const dhwRelayIdx = relayRoles.findIndex((r) => r === "boiler_enable_dhw");
        const dhwRelayOn = (typeof tuv.boilerRelayOn === "boolean")
          ? tuv.boilerRelayOn
          : ((dhwRelayIdx >= 0 && relays.length > dhwRelayIdx) ? !!relays[dhwRelayIdx] : null);
        const dhwRelayTxt = (dhwRelayOn === null) ? "relé: —" : (dhwRelayOn ? "relé: ON" : "relé: OFF");

        const modeTile = document.createElement("div");
        modeTile.className = "ioTile";
        const modeLabel = active ? "Aktivní" : "Neaktivní";
        const demandTxt = (typeof tuv.demandActive !== "undefined") ? (tuv.demandActive ? "požadavek: ANO" : "požadavek: ne") : "požadavek: —";
        const schedTxt = (typeof tuv.scheduleEnabled !== "undefined") ? (tuv.scheduleEnabled ? "plán: ON" : "plán: OFF") : "plán: —";
        const reasonTxt = (tuv.reason || tuv.source) ? `důvod: ${String(tuv.reason || tuv.source)}` : "důvod: —";
        modeTile.innerHTML = `
          <div>
            <div class="ioName">Ohřev TUV</div>
            <div class="ioSub">${escapeHtml(demandTxt)} • ${escapeHtml(schedTxt)} • ${escapeHtml(reasonTxt)} • ${escapeHtml(dhwRelayTxt)}</div>
          </div>
          <div class="ioRight">
            <div class="tempValue ${active ? "" : "muted"}">${modeLabel}</div>
          </div>
        `;
        tuvGrid.appendChild(modeTile);

        const eqMaster = Number(tuv.eqValveMaster || eqCfg?.valve?.master || 0);
        if (eqMaster >= 1 && eqMaster <= RELAY_COUNT) {
          const eqTile = document.createElement("div");
          eqTile.className = "ioTile";
          const tgt = Number.isFinite(tuv.eqValveTargetPct) ? Math.round(tuv.eqValveTargetPct) : 0;
          eqTile.innerHTML = `
            <div>
              <div class="ioName">Směšovací ventil</div>
              <div class="ioSub">${escapeHtml(getNameRelays(cfg, eqMaster - 1))} • cíl ${tgt}%</div>
            </div>
            <div class="ioRight">
              <div class="dial" style="--p:${tgt}%">
                <div class="dialTxt">${tgt}%</div>
              </div>
            </div>
          `;
          tuvGrid.appendChild(eqTile);
        }

        const tuvMaster = Number(tuv.valveMaster || tuvCfg?.valveMaster || 0);
        if (tuvMaster >= 1 && tuvMaster <= RELAY_COUNT) {
          const vTile = document.createElement("div");
          vTile.className = "ioTile";
          const pos = Number.isFinite(tuv.valvePosPct) ? Math.round(tuv.valvePosPct) : 0;
          const tgt = Number.isFinite(tuv.valveTargetPct) ? Math.round(tuv.valveTargetPct) : 0;
          const moving = !!tuv.valveMoving;
          vTile.innerHTML = `
            <div>
              <div class="ioName">V3 bypass</div>
              <div class="ioSub">${escapeHtml(getNameRelays(cfg, tuvMaster - 1))} • ${pos}% → ${tgt}%${moving ? " • pohyb" : ""}</div>
            </div>
            <div class="ioRight">
              <div class="dial" style="--p:${pos}%">
                <div class="dialTxt">${pos}%</div>
              </div>
            </div>
          `;
          tuvGrid.appendChild(vTile);
        }
      }
    }

    // --- Recirc widget ---
    const cardRecirc = $id("cardRecirc");
    const recircGrid = $id("recircDashGrid");
    const rec = st?.recirc || {};
    if (cardRecirc && recircGrid) {
      if (!rec?.enabled) {
        recircGrid.innerHTML = `<div class="muted">Cirkulace je vypnutá.</div>`;
      } else {
        const active = !!rec.active;
        const remaining = Number(rec.remainingMs || 0) ? Math.round(rec.remainingMs / 1000) : 0;
        const stopTxt = rec.stopReached ? "stop reached" : "stop: —";
        const tempTxt = Number.isFinite(rec.returnTempC) ? fmtTemp(rec.returnTempC) : "—";
        recircGrid.innerHTML = `
          <div class="ioTile">
            <div>
              <div class="ioName">Cirkulace</div>
              <div class="ioSub">Režim: ${escapeHtml(String(rec.mode || "—"))} • ${stopTxt}</div>
            </div>
            <div class="ioRight">
              <div class="tempValue ${active ? "" : "muted"}">${active ? "ON" : "OFF"}</div>
            </div>
          </div>
          <div class="ioTile">
            <div>
              <div class="ioName">Návrat</div>
              <div class="ioSub">${escapeHtml(rec.returnTempValid ? "čidlo OK" : "čidlo —")}</div>
            </div>
            <div class="ioRight">
              <div class="tempValue">${escapeHtml(tempTxt)}</div>
            </div>
          </div>
          <div class="ioTile">
            <div>
              <div class="ioName">Zbývá</div>
              <div class="ioSub">on-demand běh</div>
            </div>
            <div class="ioRight">
              <div class="tempValue">${remaining ? `${remaining}s` : "—"}</div>
            </div>
          </div>
        `;
      }
    }

    // --- AKU heater widget ---
    const cardAkuHeater = $id("cardAkuHeater");
    const akuHeaterGrid = $id("akuHeaterDashGrid");
    const heater = st?.akuHeater || {};
    const heaterCfg = cfg?.akuHeater || {};
    if (cardAkuHeater && akuHeaterGrid) {
      if (!heater?.enabled) {
        akuHeaterGrid.innerHTML = `<div class="muted">AKU heater je vypnutý.</div>`;
      } else {
        const active = !!heater.active;
        const mode = String(heater.mode || heaterCfg.mode || "—");
        const reason = String(heater.reason || "—");
        const target = Number.isFinite(heaterCfg.targetTopC) ? `${Number(heaterCfg.targetTopC).toFixed(1)} °C` : "—";
        const topTxt = heater.topValid ? fmtTemp(heater.topC) : "—";
        akuHeaterGrid.innerHTML = `
          <div class="ioTile">
            <div>
              <div class="ioName">Heater</div>
              <div class="ioSub">Režim: ${escapeHtml(mode)} • ${escapeHtml(reason)}</div>
            </div>
            <div class="ioRight">
              <div class="tempValue ${active ? "" : "muted"}">${active ? "ON" : "OFF"}</div>
            </div>
          </div>
          <div class="ioTile">
            <div>
              <div class="ioName">Cíl / AKU top</div>
              <div class="ioSub">Cíl: ${escapeHtml(target)}</div>
            </div>
            <div class="ioRight">
              <div class="tempValue">${escapeHtml(topTxt)}</div>
            </div>
          </div>
        `;
      }
    }

    // --- Reasons widget ---
    const cardReasons = $id("cardReasons");
    const reasonsGrid = $id("reasonsDashGrid");
    if (cardReasons && reasonsGrid) {
      cardReasons.style.display = "";
      const heatCallRaw = st?.heatCall?.raw;
      const heatCall = (typeof heatCallRaw === "boolean") ? (heatCallRaw ? "ON (den)" : "OFF (noc)") : "—";
      const dhwMode = (typeof st?.tuv?.active === "boolean") ? (st.tuv.active ? "ON" : "OFF") : (st?.tuv?.modeActive ? "ON" : "OFF");
      const dhwReason = st?.tuv?.reason || st?.tuv?.source || "—";
      const outdoorOk = st?.equitherm?.outdoorValid ? "OK" : "stale";
      const outdoorAge = Number.isFinite(st?.equitherm?.outdoorAgeMs) ? `${Math.round(st.equitherm.outdoorAgeMs / 1000)}s` : "—";
      const eqActive = st?.equitherm?.active ? "active" : "paused";
      const eqReason = st?.equitherm?.reason || "OK";
      const profile = cfg?.system?.profile || "standard";
      const relayRoles = getRoleRelays(cfg);
      const relays = Array.isArray(st?.relays) ? st.relays : [];
      const nmRelayIdx = relayRoles.findIndex((r) => r === "boiler_enable_nm");
      const nmRelayTxt = (nmRelayIdx >= 0 && relays.length > nmRelayIdx) ? (relays[nmRelayIdx] ? "ON" : "OFF") : "—";
      const dhwRelayIdx = relayRoles.findIndex((r) => r === "boiler_enable_dhw");
      const dhwRelayTxt = (typeof st?.tuv?.boilerRelayOn === "boolean")
        ? (st.tuv.boilerRelayOn ? "ON" : "OFF")
        : ((dhwRelayIdx >= 0 && relays.length > dhwRelayIdx) ? (relays[dhwRelayIdx] ? "ON" : "OFF") : "—");
      reasonsGrid.innerHTML = `
        <div class="ioTile"><div><div class="ioName">Heat call</div><div class="ioSub">${escapeHtml(heatCall)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">DHW režim</div><div class="ioSub">${escapeHtml(dhwMode)} • ${escapeHtml(dhwReason)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">Outdoor</div><div class="ioSub">${escapeHtml(outdoorOk)} • ${escapeHtml(outdoorAge)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">Equitherm</div><div class="ioSub">${escapeHtml(eqActive)} • ${escapeHtml(eqReason)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">Profil</div><div class="ioSub">${escapeHtml(profile)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">Noční relé</div><div class="ioSub">${escapeHtml(nmRelayTxt)}</div></div></div>
        <div class="ioTile"><div><div class="ioName">Relé TUV</div><div class="ioSub">${escapeHtml(dhwRelayTxt)}</div></div></div>
      `;
    }

    const inRoles = getRoleInputs(cfg);
    const reRoles = getRoleRelays(cfg);

    const cardValves = $id("cardValves");
    const valveGrid  = $id("valveGrid");

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

    if (valveGrid && !valveCount) {
      valveGrid.innerHTML = `<div class="muted">Žádné další ventily nejsou aktivní.</div>`;
    }
    const cardTemps = $id("cardTemps");
    const tempGrid  = $id("tempGrid");
    let tempCount = 0;
    if (tempGrid) tempGrid.innerHTML = "";

    const temps = Array.isArray(dash.temps) ? dash.temps : [];
    const vArr  = Array.isArray(dash.tempsValid) ? dash.tempsValid : [];

    // Determine which temperature tiles should be hidden because they are part of Ekviterm block
    const eqOut = describeEqSource(cfg, eqCfg.outdoor, "outdoor");
    const eqFlow = describeEqSource(cfg, eqCfg.flow, "boilerIn");
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
      const isTemp = role === "temp_dallas";
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
          <div class="ioSub">Dallas/DS18B20 • vstup ${i+1}</div>
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

    if (tempGrid && !tempCount) {
      tempGrid.innerHTML = `<div class="muted">Teploty nejsou dostupné.</div>`;
    }

    const missingTemp = (role) => (roleMap.temps?.[role]?.source || "none") === "none";
    const missingInput = (role) => !roleMap.inputs?.[role];
    const missingOutput = (role) => !roleMap.outputs?.[role];

    let eqReason = null;
    let eqAction = "#ekviterm";
    if (!eqCfg?.enabled) {
      eqReason = "Funkce je vypnutá v konfiguraci.";
    } else if (missingTemp("outdoor")) {
      eqReason = "Chybí role outdoor.";
      eqAction = "#temps";
    } else if (missingTemp("flow")) {
      eqReason = "Chybí role flow (boiler_in).";
      eqAction = "#temps";
    } else if (missingOutput("valve_3way_mix")) {
      eqReason = "Není přiřazen ventil valve_3way_mix.";
      eqAction = "#iofunc";
    }
    setCardDisabled("cardEquitherm", eqReason, eqReason ? eqAction : "");

    let akuReason = null;
    let akuAction = "#aku";
    if (!eqCfg?.akuSupportEnabled) {
      akuReason = "Podpora AKU je vypnutá.";
    } else if (missingTemp("tankTop")) {
      akuReason = "Chybí role tankTop.";
      akuAction = "#temps";
    } else if (missingTemp("tankMid")) {
      akuReason = "Chybí role tankMid.";
      akuAction = "#temps";
    } else if (missingTemp("tankBottom")) {
      akuReason = "Chybí role tankBottom.";
      akuAction = "#temps";
    }
    setCardDisabled("cardAku", akuReason, akuReason ? akuAction : "");

    let tuvReason = null;
    let tuvAction = "#tuv";
    if (!tuvCfg?.enabled) {
      tuvReason = "Ohřev TUV je vypnutý.";
    } else if (missingInput("dhw_enable")) {
      tuvReason = "Chybí role vstupu dhw_enable.";
      tuvAction = "#iofunc";
    } else if (missingOutput("boiler_enable_dhw")) {
      tuvReason = "Chybí role výstupu boiler_enable_dhw.";
      tuvAction = "#iofunc";
    } else if (missingOutput("valve_3way_tuv")) {
      tuvReason = "Chybí ventil valve_3way_tuv.";
      tuvAction = "#iofunc";
    }
    setCardDisabled("cardTuv", tuvReason, tuvReason ? tuvAction : "");

    let recReason = null;
    let recAction = "#recirc";
    if (!cfg?.dhwRecirc?.enabled) {
      recReason = "Cirkulace je vypnutá.";
    } else if (missingInput("recirc_demand")) {
      recReason = "Chybí role vstupu recirc_demand.";
      recAction = "#iofunc";
    } else if (missingOutput("dhw_recirc_pump")) {
      recReason = "Chybí role výstupu dhw_recirc_pump.";
      recAction = "#iofunc";
    } else if (missingTemp("return")) {
      recReason = "Chybí role teploty return.";
      recAction = "#temps";
    }
    setCardDisabled("cardRecirc", recReason, recReason ? recAction : "");

    let heaterReason = null;
    let heaterAction = "#aku_heater";
    if (!cfg?.akuHeater?.enabled) {
      heaterReason = "AKU heater je vypnutý.";
    } else if (missingOutput("heater_aku")) {
      heaterReason = "Chybí role výstupu heater_aku.";
      heaterAction = "#iofunc";
    } else if (missingTemp("tankTop")) {
      heaterReason = "Chybí role tankTop.";
      heaterAction = "#temps";
    }
    setCardDisabled("cardAkuHeater", heaterReason, heaterReason ? heaterAction : "");

    const anyValve = !!roleMap.outputs?.valve_3way_mix || !!roleMap.outputs?.valve_3way_tuv;
    setCardDisabled("cardValveMix", anyValve ? "" : "Ventily nejsou přiřazené.", anyValve ? "" : "#iofunc");
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

  const init = () => {
    if (initialized) return;
    initialized = true;
    dash = getDash() || dash;
    render();

    const vg = $id("valveGrid");
    vg && vg.addEventListener("click", async (e) => {
      const sw = e.target.closest(".sw[data-relay]");
      if (!sw) return;
      const idx = Number(sw.dataset.relay);
      const on = !sw.classList.contains("on");
      try {
        // /api/mode_ctrl expects action:"relay" (1..8, value:true/false)
        await apiPostJson("/api/mode_ctrl", { action: "relay", relay: idx+1, value: on });
        refreshDash();
      } catch {}
    });

    window.addEventListener("app:dashUpdated", (e) => {
      dash = e?.detail || getDash() || dash;
      render();
    });
  };

  const mount = () => {
    init();
    render();
    window.Pages?.schema?.mount?.();
  };

  window.Pages = window.Pages || {};
  window.Pages.dashboard = { id: "dashboard", mount, unmount() {} };
})();
