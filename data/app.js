    // ============================================================
    // UI 2026
    // - embedded device web UI
    // - fetch from /api/...
    // - single-file, no external libraries
    // ============================================================

    const $ = (sel, root=document) => root.querySelector(sel);
    const $$ = (sel, root=document) => Array.from(root.querySelectorAll(sel));

    const state = {
      theme: localStorage.getItem("ui2026_theme") || (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark"),
      source: "device", // fixed (device)
      apiBase: localStorage.getItem("ui2026_apiBase") || "",
      dev: {},
      fast: {},
      eqFast: {},
      dhwFast: {},
      bleFast: {},
      mqtt: { loaded:false, načítání:false, status:null, config:null },
      ws: { sock:null, připojeno:false, retryTimer:null, reconnectMs:2000, manualClose:false, failCount:0, disabledUntilMs:0 },
      last: null,
      history: {
        out: [],
        ch: [],
        dhw: [],
        pr: [],
      },
      io: {
        relays: [false,false,false,false,false,false,false,false],
        inputs: [false,false,false],
      }
    ,
      accu: {
        top: NaN,
        mid: NaN,
        bot: NaN,
        valve: 0,     // %
        after: NaN,   // °C (temp after mixing valve)
        mode: "auto"
      },
      ot: {
        comm: true,
        enabled: false,
        ready: false,
        fault: false,
        present: false,
        chSet: 45.0,
        chTemp: 44.2,
        dhwTemp: 49.3,
        outsideTempC: NaN,
        returnTempC: NaN,
        pressure: 1.58,
        modulationPct: NaN,
        maxCapacityKw: NaN,
        currentPowerKw: NaN,
        reqWaterTempC: NaN,
        reqDhwSetpointC: NaN,
        maxChSetpointC: NaN,
        maxChBoundMinC: NaN,
        maxChBoundMaxC: NaN,
        dhwSetpointC: NaN,
        dhwBoundMinC: NaN,
        dhwBoundMaxC: NaN,
        faultFlags: 0,
        oemFaultCode: 0,
        reason: "",
        lastCmd: "",
        statusRaw: 0,
        cfg: { enable: true, pollMs: 1000, failMode: "hold", log: false }
      },
      otMeta: {
        capacityFetchMs: 0,
        capacityFetching: false,
      },
      alerts: {
        pressure: {
          enabled: true,
          minBar: 0.8,
          maxBar: 2.8,
          hysteresisBar: 0.05,
          active: false,
          sensorValid: false,
          pressureBar: NaN,
          state: "init"
        }
      },
      circPulse: { enable: true, onMin: 5, vypnutoMin: 15 },
      net: {
        failCount: 0,
        lastErrorToastMs: 0,
        nextPollMs: 10000,
        extrasDueMs: 0,
        apiBaseFallbackUsed: false,
      },
      ui: {
        eqConfigDirty: false,
      },
      diag: {
        heap: {},
        adminActions: []
      },
      render: {
        pendingSample: null,
        rafId: 0,
      },
      service: loadServiceStats()
    };

    // OpenTherm


    // ----- Minimal toast
    function toast(title, msg, icon="ℹ"){
      const host = $("#toast");
      const el = document.createElement("div");
      el.className = "t";
      el.innerHTML = `
        <div class="ic">${icon}</div>
        <div class="tx"><strong>${escapeHtml(title)}</strong><span>${escapeHtml(msg || "")}</span></div>
        <button aria-label="Zavřít">✕</button>
      `;
      el.querySelector("button").addEventListener("click", () => el.remove());
      host.prepend(el);
      setTimeout(() => { if(el.isConnected) el.remove(); }, 5200);
    }

    function escapeHtml(s){
      return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    function numOrNaN(v){
      if(v === null || v === undefined) return NaN;
      if(typeof v === "boolean") return NaN;
      if(typeof v === "string" && !v.trim().length) return NaN;
      const n = Number(v);
      return Number.isFinite(n) ? n : NaN;
    }

    function firstFinite(){
      for(const v of arguments){
        const n = numOrNaN(v);
        if(Number.isFinite(n)) return n;
      }
      return NaN;
    }

    function loadServiceStats(){
      const blank = {
        current:{ mixKey:"", dhwKey:"" },
        counters:{},
        lastChangeMs:0,
      };
      try{
        const raw = localStorage.getItem("ui2026_service_stats");
        if(!raw) return blank;
        const parsed = JSON.parse(raw);
        const counters = (parsed && typeof parsed.counters === "object" && parsed.counters) ? parsed.counters : {};
        return {
          current:{
            mixKey: String(parsed?.current?.mixKey || ""),
            dhwKey: String(parsed?.current?.dhwKey || ""),
          },
          counters,
          lastChangeMs: Number(parsed?.lastChangeMs) || 0,
        };
      }catch(_e){
        return blank;
      }
    }

    function saveServiceStats(){
      try{
        localStorage.setItem("ui2026_service_stats", JSON.stringify(state.service || loadServiceStats()));
      }catch(_e){}
    }

    function hasOwn(obj, key){
      return !!obj && Object.prototype.hasOwnProperty.call(obj, key);
    }

    function readMaybeNumber(obj, key, fallback=NaN){
      if(!hasOwn(obj, key)) return fallback;
      return numOrNaN(obj[key]);
    }

    function readMaybeString(obj, key, fallback=""){
      if(!hasOwn(obj, key)) return fallback;
      const v = obj[key];
      return (v === null || v === undefined) ? "" : String(v);
    }

    function readMaybeBool(obj, key, fallback=false){
      return hasOwn(obj, key) ? !!obj[key] : fallback;
    }

    function fmtNum(v, digits=1){
      const n = Number(v);
      return Number.isFinite(n) ? n.toFixed(digits) : "--";
    }

    function fmtBytes(v){
      const n = Number(v);
      if(!Number.isFinite(n) || n < 0) return "--";
      if(n < 1024) return `${Math.round(n)} B`;
      if(n < 1024*1024) return `${(n/1024).toFixed(n >= 10*1024 ? 0 : 1)} kB`;
      return `${(n/(1024*1024)).toFixed(n >= 10*1024*1024 ? 0 : 1)} MB`;
    }

    async function parseApiReply(r, path){
      const txt = await r.text();
      let body = null;
      if(txt){
        try{ body = JSON.parse(txt); }catch{}
      }
      if(!r.ok){
        const msg = body?.msg || body?.error || body?.message || txt || `HTTP ${r.status} for ${path}`;
        const retryAfterMs = Number(body?.retryAfterMs);
        if(r.status === 429){
          const extra = Number.isFinite(retryAfterMs) && retryAfterMs > 0 ? ` Zkus to znovu za ${Math.ceil(retryAfterMs/1000)} s.` : " Zkus to znovu za chvíli.";
          throw new Error(`HTTP 429 for ${path}: ${msg}.${extra}`);
        }
        throw new Error(`HTTP ${r.status} for ${path}: ${msg}`);
      }
      if(body !== null) return body;
      return txt ? { raw: txt } : {};
    }

    function setTextNum(id, v, digits=1){
      const el = $(id);
      if(!el) return;
      el.textContent = fmtNum(v, digits);
    }

    function setApiHealth(kind, text){
      setBadge("#bApi", kind || "", text || "API: --");
      setText("#apiState", text || "API: --");
    }

    function debounce(fn, wait=120){
      let t = null;
      return function(){
        const ctx = this;
        const args = arguments;
        clearTimeout(t);
        t = setTimeout(() => fn.apply(ctx, args), wait);
      };
    }

    function setBusy(btn, busy, busyText="Pracuji…"){
      if(!btn) return;
      if(busy){
        if(btn.dataset.busy === "1") return;
        btn.dataset.busy = "1";
        btn.dataset.label = btn.innerHTML;
        btn.disabled = true;
        btn.innerHTML = escapeHtml(busyText);
      }else{
        btn.disabled = false;
        if(btn.dataset.label) btn.innerHTML = btn.dataset.label;
        delete btn.dataset.label;
        delete btn.dataset.busy;
      }
    }

    async function withButtonBusy(btn, busyText, task){
      if(!btn) return await task();
      if(btn.dataset.busy === "1") return;
      setBusy(btn, true, busyText);
      try{
        return await task();
      }finally{
        setBusy(btn, false);
      }
    }

    function isEditableTarget(el){
      if(!el || !(el instanceof Element)) return false;
      return !!el.closest('input, textarea, select, [contenteditable="true"]');
    }

    const redrawEquithermViewsDebounced = debounce(() => redrawEquithermViews(), 90);

    // ----- Theme
    function applyTheme(){
      document.documentElement.setAttribute("data-theme", state.theme);
      $("#btnTheme").innerHTML = state.theme === "light" ? "☾" : "☀";
      localStorage.setItem("ui2026_theme", state.theme);
    }

    // ----- Navigation
    const titles = {
      overview: ["Přehled", "Kompaktní status + rychlé akce"],
      heating:  ["Topení", "Ekviterm • křivky komfort / útlum • limity • plán"],
      dhw:      ["TUV", "Ohřev teplé užitkové vody • cirkulace • plánovače"],
      accu:     ["Akumulační nádrž", "Teploty nádrže • směšovací ventil"],
      opentherm:["OpenTherm", "Komunikace • hodnoty kotle"],
      thermometers:["Teploměry", "Mapování zdrojů teplot na role"],
      io:       ["I/O", "Relé • vstupy • rychlé přepínače"],
      diag:     ["Diagnostika", "API • MQTT/HA • logy • export"],
    };

    function getUiSample(){
      const last = state.last || {};
      const now = Date.now();
      const temps = (state.fast && typeof state.fast.temps === "object") ? state.fast.temps : {};
      const out = firstFinite(roleValueFromFast("outside", temps), state?.ot?.outsideTempC, state?.bleFast?.t, last?.out, 0);
      const ch = firstFinite(roleValueFromFast("flow", temps), state?.ot?.chTemp, last?.ch, 0);
      const dhw = firstFinite(roleValueFromFast("dhw_tank", temps), state?.ot?.dhwTemp, last?.dhw, 0);
      const pr = firstFinite(state?.ot?.pressure, last?.pr, 0);
      const afterMix = firstFinite(getAfterMixTempFromTemps(temps), last?.mixAfter, state?.accu?.after, state?.ot?.returnTempC, ch);
      return {
        now,
        out: Number.isFinite(out) ? out : 0,
        ch: Number.isFinite(ch) ? ch : 0,
        dhw: Number.isFinite(dhw) ? dhw : 0,
        pr: Number.isFinite(pr) ? pr : 0,
        in1: !!state?.io?.inputs?.[0],
        in2: !!state?.io?.inputs?.[1],
        in3: !!state?.io?.inputs?.[2],
        accTop: firstFinite(roleValueFromFast("tank_top", temps), last?.accTop, state?.accu?.top),
        accMid: firstFinite(roleValueFromFast("tank_mid", temps), last?.accMid, state?.accu?.mid),
        accBot: firstFinite(roleValueFromFast("tank_bottom", temps), last?.accBot, state?.accu?.bot),
        mixValve: firstFinite(state?.eqFast?.mix?.pct, last?.mixValve, state?.accu?.valve),
        mixAfter: afterMix,
        mixTarget: firstFinite(state?.eqFast?.tf, document.getElementById("hTarget")?.value, document.getElementById("eqSet")?.value, last?.mixTarget, 45),
        mixReturn: firstFinite(roleValueFromFast("return", temps), afterMix, state?.ot?.returnTempC, last?.mixReturn),
        eqTarget: firstFinite(state?.eqFast?.tf, last?.eqTarget),
      };
    }

    function waitForFirstFastSnapshot(timeoutMs=1200){
      if(state.fast || state.last) return Promise.resolve(true);
      return new Promise((resolve) => {
        let done = false;
        const finish = (ok) => {
          if(done) return;
          done = true;
          clearTimeout(timer);
          document.removeEventListener("ui:first-fast", onFast);
          resolve(!!ok);
        };
        const onFast = () => finish(true);
        const timer = setTimeout(() => finish(!!(state.fast || state.last)), Math.max(150, Number(timeoutMs) || 1200));
        document.addEventListener("ui:first-fast", onFast, { once:true });
      });
    }

    function queueRenderSample(sample){
      state.render = state.render || { pendingSample:null, rafId:0 };
      state.render.pendingSample = sample || getUiSample();
      if(state.render.rafId) return;
      const flush = () => {
        const pending = state.render?.pendingSample || getUiSample();
        state.render.pendingSample = null;
        state.render.rafId = 0;
        renderSample(pending);
      };
      if(typeof window !== "undefined" && typeof window.requestAnimationFrame === "function"){
        state.render.rafId = window.requestAnimationFrame(flush);
      }else{
        flush();
      }
    }

    function redrawEquithermViews(){
      queueRenderSample(getUiSample());
    }

    function setView(view){
      const currentView = getActiveView();
      const changing = currentView !== view;
      $$(".section").forEach(s => s.classList.remove("active"));
      $(`#view-${view}`).classList.add("active");

      const [t, st] = titles[view] || ["", ""];
      $("#tbTitle").textContent = t;
      $("#tbSubtitle").textContent = st;

      // desktop nav
      $$("#sideNav a").forEach(a => a.setAttribute("aria-current", a.dataset.view === view ? "page" : "false"));
      // mobile nav
      $$("#bottomNav button").forEach(b => b.setAttribute("aria-current", b.dataset.view === view ? "page" : "false"));
      // topbar actions (only on overview)
      const oa = document.getElementById("tbOverviewActions");
      if(oa) oa.style.display = (view === "overview") ? "flex" : "none";

      if(location.hash !== `#${view}`) location.hash = view;

      if(changing) log(`view -> ${view}`);
      if(view === "opentherm") { void otScanRefresh(); void otProfileRefresh(); }
      if(view === "diag") { void mqttLoad({ silent:true }); }
      if(view === "thermometers" && !state.th?.loaded && !state.th?.načítání) { void thermoLoad({ silent:true }); }
      if(view === "overview" || view === "heating") {
        redrawEquithermViews();
      }

    }

    // ----- Source selector
    // ----- Device mode (fixed)
function setSource(){
  state.source = "device";
  const el = document.getElementById("apiState");
  if(el) el.textContent = "API: zařízení";
}

function pageOriginBase(){
  return (/^https?:$/i.test(window.location.protocol || "")) ? window.location.origin : "";
}

function normalizedApiBase(){
  return String(state.apiBase || "").trim();
}

function syncApiBaseUi(){
  const baseEl = document.getElementById("apiBase");
  const baseLbl = document.getElementById("apiBaseLabel");
  const value = normalizedApiBase();
  if(baseEl && document.activeElement !== baseEl) baseEl.value = value;
  if(baseLbl) baseLbl.textContent = value || "/";
}

function maybeAdoptPageOriginBase(reason="runtime", silent=false){
  const pageBase = pageOriginBase();
  const current = normalizedApiBase();
  if(!pageBase || !current) return false;
  let currentOrigin = "";
  try{ currentOrigin = new URL(current, window.location.href).origin; }catch{ return false; }
  if(!currentOrigin || currentOrigin === pageBase) return false;
  state.apiBase = "";
  try{ localStorage.setItem("ui2026_apiBase", ""); }catch{}
  syncApiBaseUi();
  if(state.net) state.net.apiBaseFallbackUsed = true;
  log(`apiBase fallback -> same origin (${reason})`);
  if(!silent) toast("API", "Používám stejné origin zařízení místo uložené IP adresy.", "🔁");
  return true;
}


function computeOfflinePollMs(){
  const base = document.hidden ? 30000 : 10000;
  const backvypnuto = Number(state?.net?.nextPollMs ?? 10000);
  return Math.max(base, backvypnuto);
}

function ensureWsState(){
  state.ws = state.ws || {};
  if(!Object.prototype.hasOwnProperty.call(state.ws, "sock")) state.ws.sock = null;
  if(typeof state.ws.připojeno !== "boolean") state.ws.připojeno = false;
  if(!Object.prototype.hasOwnProperty.call(state.ws, "retryTimer")) state.ws.retryTimer = null;
  if(!Number.isFinite(Number(state.ws.reconnectMs))) state.ws.reconnectMs = 2000;
  if(typeof state.ws.manualClose !== "boolean") state.ws.manualClose = false;
  if(!Number.isFinite(Number(state.ws.failCount))) state.ws.failCount = 0;
  if(!Number.isFinite(Number(state.ws.disabledUntilMs))) state.ws.disabledUntilMs = 0;
  return state.ws;
}

function wsReconnectPaused(){
  const ws = ensureWsState();
  return Number(ws.disabledUntilMs || 0) > Date.now();
}

function wsPauseReconnect(ms=300000, reason=""){
  const ws = ensureWsState();
  ws.disabledUntilMs = Date.now() + Math.max(10000, Number(ms) || 300000);
  if(ws.retryTimer){
    clearTimeout(ws.retryTimer);
    ws.retryTimer = null;
  }
  startFallbackPolling(computeOfflinePollMs());
  setApiHealth("warn", "API: polling (WS pauza)");
  if(reason) log(`ws paused: ${reason}`);
}

function updateRefreshCadence(){
  if(state.ws?.připojeno){
    stopFallbackPolling();
    return;
  }
  startFallbackPolling(computeOfflinePollMs());
}

function wsIsAlive(){
  const sock = state.ws?.sock;
  if(!sock) return false;
  return sock.readyState === WebSocket.OPEN || sock.readyState === WebSocket.CONNECTING;
}

function handleVisibilityChange(){
  updateRefreshCadence();
  if(document.hidden) return;
  if(!wsIsAlive()) connectWs();
  refresh(false);
}


    // ----- Logging
    function log(msg){
      const ts = new Date().toLocaleTimeString("cs-CZ");
      const pre = $("#log");
      pre.textContent = `[${ts}] ${msg}\n` + pre.textContent;
    }

    // ----- Sparklines
    function drawSpark(canvas, arr){
      const ctx = canvas.getContext("2d");
      const w = canvas.width, h = canvas.height;
      ctx.clearRect(0,0,w,h);

      // background grid
      ctx.globalAlpha = 0.35;
      ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--border");
      ctx.lineWidth = 1;
      for(let i=1;i<4;i++){
        const y = (h/4)*i;
        ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke();
      }
      ctx.globalAlpha = 1;

      if(!arr.length) return;
      const min = Math.min(...arr), max = Math.max(...arr);
      const span = (max - min) || 1;

      ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--fg0");
      ctx.globalAlpha = 0.85;
      ctx.lineWidth = 2;
      ctx.beginPath();
      arr.forEach((v,i) => {
        const x = (i/(arr.length-1 || 1)) * (w-6) + 3;
        const y = h - (((v-min)/span) * (h-10) + 5);
        if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
      });
      ctx.stroke();

      // endpoint dot
      const last = arr[arr.length-1];
      const x = (w-6)+3;
      const y = h - (((last-min)/span) * (h-10) + 5);
      ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue("--brand");
      ctx.globalAlpha = 0.95;
      ctx.beginPath(); ctx.arc(x,y,3.2,0,Math.PI*2); ctx.fill();
      ctx.globalAlpha = 1;
    }

    
        // ----- Equitherm curves (chart)
    // Firmware model (this repo):
    // Zařízení interně používá dva body křivky, ale UI nyní pracuje
    // s běžným modelem ekvitermu: sklon + posun.
    // Přepočet: Tflow = (20 - Tout) * slope + 20 + shift, následně clamp na Min/Max.
    const eq = {
      xMin: -20, xMax: 20, // outdoor range
      yPad: 2,
      fitY: false,
    };

    function eqCurveFromPoints(pts){
      const x1 = Number(pts?.outColdC), y1 = Number(pts?.flowColdC);
      const x2 = Number(pts?.outWarmC), y2 = Number(pts?.flowWarmC);
      if(!Number.isFinite(x1) || !Number.isFinite(y1) || !Number.isFinite(x2) || !Number.isFinite(y2)){
        return { slope: NaN, shift: NaN };
      }
      const dx = x2 - x1;
      const lineSlope = (Math.abs(dx) < 1e-6) ? 0 : (y2 - y1) / dx;
      const slope = -lineSlope;
      const yAt20 = y1 + lineSlope * (20 - x1);
      const shift = yAt20 - 20;
      return { slope, shift };
    }

    function normalizeEqCurve(curveLike, fallback = null){
      if(curveLike && Number.isFinite(Number(curveLike.slope)) && Number.isFinite(Number(curveLike.shift))){
        return {
          slope: Math.abs(Number(curveLike.slope)),
          shift: Number(curveLike.shift),
        };
      }
      const derived = eqCurveFromPoints(curveLike);
      if(Number.isFinite(derived.slope) && Number.isFinite(derived.shift)){
        return {
          slope: Math.abs(Number(derived.slope)),
          shift: Number(derived.shift),
        };
      }
      if(fallback && Number.isFinite(Number(fallback.slope)) && Number.isFinite(Number(fallback.shift))){
        return {
          slope: Math.abs(Number(fallback.slope)),
          shift: Number(fallback.shift),
        };
      }
      return { slope: NaN, shift: NaN };
    }

    function normalizeEqLimits(limitsLike, fallback = null){
      const minCandidate = Number(limitsLike?.minFlowC);
      const maxCandidate = Number(limitsLike?.maxFlowC);
      const fbMin = Number(fallback?.minFlowC);
      const fbMax = Number(fallback?.maxFlowC);
      const minFlowC = Number.isFinite(minCandidate) ? minCandidate : (Number.isFinite(fbMin) ? fbMin : 25);
      const maxFlowC = Number.isFinite(maxCandidate) ? maxCandidate : (Number.isFinite(fbMax) ? fbMax : 60);
      return {
        minFlowC: Math.min(minFlowC, maxFlowC),
        maxFlowC: Math.max(minFlowC, maxFlowC),
      };
    }

    function buildEqChartConfigFromSource(cfg, fallback = null){
      if(!cfg) return null;
      const dayCurve = normalizeEqCurve(cfg?.dayCurve || cfg?.day, fallback?.dayCurve);
      const nightCurve = normalizeEqCurve(cfg?.nightCurve || cfg?.night, fallback?.nightCurve);
      const limits = normalizeEqLimits(cfg?.limits || cfg, fallback);
      if(!Number.isFinite(dayCurve.slope) || !Number.isFinite(dayCurve.shift)
        || !Number.isFinite(nightCurve.slope) || !Number.isFinite(nightCurve.shift)){
        return null;
      }
      return {
        dayCurve,
        nightCurve,
        minFlowC: limits.minFlowC,
        maxFlowC: limits.maxFlowC,
      };
    }

    function eqPointsFromCurve(curve){
      const slope = Number(curve?.slope);
      const shift = Number(curve?.shift);
      const safeSlope = Number.isFinite(slope) ? slope : 0;
      const safeShift = Number.isFinite(shift) ? shift : 0;
      return {
        outColdC: -20,
        flowColdC: (20 - (-20)) * safeSlope + 20 + safeShift,
        outWarmC: 20,
        flowWarmC: 20 + safeShift,
      };
    }

    function eqRawFromCurve(Tout, curve){
      const slope = Number(curve?.slope);
      const shift = Number(curve?.shift);
      if(!Number.isFinite(slope) || !Number.isFinite(shift)) return NaN;
      return (20 - Tout) * slope + 20 + shift;
    }

    function eqChFromCurve(Tout, curve, minFlowC, maxFlowC){
      const raw = eqRawFromCurve(Tout, curve);
      if(!Number.isFinite(raw)) return NaN;
      return clamp(raw, minFlowC, maxFlowC);
    }

    function roundRect(ctx, x, y, w, h, r){
      const rr = Math.max(0, Math.min(Number(r)||0, (w||0)/2, (h||0)/2));
      if(ctx.roundRect){
        ctx.beginPath();
        ctx.roundRect(x, y, w, h, rr);
        return;
      }
      ctx.beginPath();
      ctx.moveTo(x+rr, y);
      ctx.arcTo(x+w, y, x+w, y+h, rr);
      ctx.arcTo(x+w, y+h, x, y+h, rr);
      ctx.arcTo(x, y+h, x, y, rr);
      ctx.arcTo(x, y, x+w, y, rr);
      ctx.closePath();
    }


    function drawEquithermChart(canvas, opts){
      if(!canvas) return;
      const dpr = Math.max(1, Math.min(2, window.devicePixelRatio || 1));
      const cssW = canvas.clientWidth || 900;
      const cssH = canvas.clientHeight || 260;
      canvas.width = Math.round(cssW * dpr);
      canvas.height = Math.round(cssH * dpr);

      const ctx = canvas.getContext("2d");
      ctx.setTransform(dpr,0,0,dpr,0,0);

      const w = cssW, h = cssH;
      ctx.clearRect(0,0,w,h);

      const styles = getComputedStyle(document.documentElement);
      const border = styles.getPropertyValue("--border").trim() || "rgba(255,255,255,.14)";
      const fg0 = styles.getPropertyValue("--fg0").trim() || "rgba(255,255,255,.92)";
      const fg2 = styles.getPropertyValue("--fg2").trim() || "rgba(255,255,255,.55)";
      const bg1 = styles.getPropertyValue("--bg1").trim() || "rgba(255,255,255,.06)";
      const brand = styles.getPropertyValue("--brand").trim() || "#8b5cf6";
      const info = styles.getPropertyValue("--info").trim() || "#38bdf8";

      const padL = 46, padR = 12, padT = 10, padB = 34;
      const pw = w - padL - padR;
      const ph = h - padT - padB;

      const xMin = opts?.xMin ?? eq.xMin;
      const xMax = opts?.xMax ?? eq.xMax;

      const minFlowC = Number(opts.minFlowC);
      const maxFlowC = Number(opts.maxFlowC);
      const dayCurve = opts.dayCurve || null;
      const nightCurve = opts.nightCurve || null;

      let yMin = minFlowC, yMax = maxFlowC;
      if(eq.fitY || opts.fitY){
        const samples = [minFlowC, maxFlowC];
        for(let i=0;i<=40;i++){
          const x = xMin + (i/40)*(xMax-xMin);
          samples.push(eqRawFromCurve(x, dayCurve));
          samples.push(eqRawFromCurve(x, nightCurve));
        }
        const finite = samples.filter(Number.isFinite);
        yMin = (finite.length ? Math.min(...finite) : minFlowC) - eq.yPad;
        yMax = (finite.length ? Math.max(...finite) : maxFlowC) + eq.yPad;
      }else{
        yMin = minFlowC - eq.yPad;
        yMax = maxFlowC + eq.yPad;
      }
      const ySpan = (yMax - yMin) || 1;

      const xToPx = x => padL + ((x - xMin)/(xMax - xMin)) * pw;
      const yToPx = y => padT + (1 - (y - yMin)/ySpan) * ph;

      // background
      ctx.fillStyle = bg1;
      ctx.strokeStyle = border;
      ctx.lineWidth = 1;
      roundRect(ctx, 0.5, 0.5, w-1, h-1, 14);
      ctx.fill();
      ctx.stroke();

      // grid
      ctx.save();
      ctx.strokeStyle = border;
      ctx.globalAlpha = 0.55;
      ctx.lineWidth = 1;
      for(let x = Math.ceil(xMin/5)*5; x<=xMax; x+=5){
        const px = xToPx(x);
        ctx.beginPath(); ctx.moveTo(px, padT); ctx.lineTo(px, padT+ph); ctx.stroke();
      }
      for(let y = Math.ceil(yMin/5)*5; y<=yMax; y+=5){
        const py = yToPx(y);
        ctx.beginPath(); ctx.moveTo(padL, py); ctx.lineTo(padL+pw, py); ctx.stroke();
      }
      ctx.restore();

      // axes
      ctx.strokeStyle = border;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(padL, padT);
      ctx.lineTo(padL, padT+ph);
      ctx.lineTo(padL+pw, padT+ph);
      ctx.stroke();

      // ticks + labels
      ctx.fillStyle = fg2;
      ctx.font = "12px " + (styles.getPropertyValue("--mono").trim() || "ui-monospace");
      for(let x = Math.ceil(xMin/10)*10; x<=xMax; x+=10){
        const px = xToPx(x);
        ctx.fillText(`${x}`, px-6, padT+ph+18);
      }
      for(let y = Math.ceil(yMin/10)*10; y<=yMax; y+=10){
        const py = yToPx(y);
        ctx.fillText(`${y}`, 8, py+4);
      }
      ctx.fillStyle = fg2;
      ctx.font = "12px " + (styles.getPropertyValue("--sans").trim() || "system-ui");
      ctx.fillText("Venkovní teplota (°C)", padL + pw/2 - 70, h-8);
      ctx.save();
      ctx.translate(14, padT + ph/2 + 60);
      ctx.rotate(-Math.PI/2);
      ctx.fillText("Požadovaná teplota topné vody (°C)", 0, 0);
      ctx.restore();

      function strokeCurve(curve, color, alpha=0.85, mode="raw"){
        ctx.save();
        ctx.strokeStyle = color;
        ctx.globalAlpha = alpha;
        ctx.lineWidth = 2.2;
        ctx.beginPath();
        for(let i=0;i<=80;i++){
          const x = xMin + (i/80)*(xMax-xMin);
          const y = mode === "clamped"
            ? eqChFromCurve(x, curve, minFlowC, maxFlowC)
            : eqRawFromCurve(x, curve);
          const px = xToPx(x);
          const py = yToPx(y);
          if(i===0) ctx.moveTo(px,py); else ctx.lineTo(px,py);
        }
        ctx.stroke();
        ctx.restore();
      }

      function strokeLimit(y, alpha=0.45){
        if(!Number.isFinite(y)) return;
        const py = yToPx(y);
        ctx.save();
        ctx.strokeStyle = border;
        ctx.globalAlpha = alpha;
        ctx.setLineDash([6,6]);
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(padL, py);
        ctx.lineTo(padL + pw, py);
        ctx.stroke();
        ctx.restore();
      }

      strokeLimit(minFlowC, 0.35);
      strokeLimit(maxFlowC, 0.45);
      strokeCurve(dayCurve, fg0, 0.9, "raw");
      strokeCurve(nightCurve, info, 0.9, "raw");

      if(Number.isFinite(opts.outdoorNow)){
        const px = xToPx(opts.outdoorNow);
        ctx.save();
        ctx.strokeStyle = border;
        ctx.setLineDash([5,5]);
        ctx.globalAlpha = 0.9;
        ctx.beginPath(); ctx.moveTo(px, padT); ctx.lineTo(px, padT+ph); ctx.stroke();
        ctx.restore();
      }

      if(Number.isFinite(opts.pointX) && Number.isFinite(opts.pointY)){
        const px = xToPx(opts.pointX);
        const py = yToPx(opts.pointY);
        ctx.save();
        ctx.fillStyle = brand;
        ctx.globalAlpha = 0.18;
        ctx.beginPath(); ctx.arc(px, py, 10, 0, Math.PI*2); ctx.fill();
        ctx.globalAlpha = 1;
        ctx.beginPath(); ctx.arc(px, py, 4.2, 0, Math.PI*2); ctx.fill();
        ctx.fillStyle = fg0;
        ctx.font = "12px " + (styles.getPropertyValue("--mono").trim() || "ui-monospace");
        const label = `(${opts.pointX.toFixed(1)}°C, ${opts.pointY.toFixed(1)}°C)`;
        ctx.fillText(label, clamp(px+10, padL+6, padL+pw-120), clamp(py-10, padT+14, padT+ph-6));
        ctx.restore();
      }

      if(opts.swDay) opts.swDay.style.background = fg0;
      if(opts.swNight) opts.swNight.style.background = info;
      if(opts.swNow) opts.swNow.style.background = border;
    }

// ----- Schedules (multiple intervals per day)
    const daysCZ = ["Pondělí","Úterý","Středa","Čtvrtek","Pátek","Sobota","Neděle"];
const HEATING_MAX_INTERVALS_PER_DAY = 6;
const DHW_MAX_INTERVALS_PER_DAY = 8;

    function defaultSchedules(){
      return {
        heatingDay: [
          [{start:"06:00", end:"22:00"}],
          [{start:"06:00", end:"22:00"}],
          [{start:"06:00", end:"22:00"}],
          [{start:"06:00", end:"22:00"}],
          [{start:"06:00", end:"22:00"}],
          [{start:"08:00", end:"23:00"}],
          [{start:"08:00", end:"23:00"}],
        ],
        dhwHeat: [
          [{start:"05:30", end:"06:30"}],
          [{start:"05:30", end:"06:30"}],
          [{start:"05:30", end:"06:30"}],
          [{start:"05:30", end:"06:30"}],
          [{start:"05:30", end:"06:30"}],
          [{start:"07:00", end:"08:00"}],
          [{start:"07:00", end:"08:00"}],
        ],
        dhwCirc: [
          [{start:"06:00", end:"06:10"},{start:"18:00", end:"18:10"}],
          [{start:"06:00", end:"06:10"},{start:"18:00", end:"18:10"}],
          [{start:"06:00", end:"06:10"},{start:"18:00", end:"18:10"}],
          [{start:"06:00", end:"06:10"},{start:"18:00", end:"18:10"}],
          [{start:"06:00", end:"06:10"},{start:"18:00", end:"18:10"}],
          [{start:"08:00", end:"08:10"},{start:"20:00", end:"20:10"}],
          [{start:"08:00", end:"08:10"},{start:"20:00", end:"20:10"}],
        ],
      };
    }

    state.schedules = (() => {
      try{
        const raw = localStorage.getItem("ui2026_schedules");
        if(raw){
          const obj = JSON.parse(raw);
          if(obj && obj.heatingDay && obj.dhwHeat && obj.dhwCirc) return obj;
        }
      }catch{}
      return defaultSchedules();
    })()// Circulation pulse config (ON/OFF cycling during active schedule)
state.circPulse = (() => {
  try{
    const raw = localStorage.getItem("ui2026_circPulse");
    if(raw){
      const o = JSON.parse(raw);
      if(o && typeof o === "object"){
        return {
          enable: !!o.enable,
          onMin: Math.max(0, Number(o.onMin ?? 5)),
          vypnutoMin: Math.max(0, Number(o.vypnutoMin ?? 15)),
        };
      }
    }
  }catch{}
  return state.circPulse;
})();

function saveCircPulse(){
  localStorage.setItem("ui2026_circPulse", JSON.stringify(state.circPulse));
}

function circPulseIsOn(nowMs, intervalStartMin){
  if(!state.circPulse?.enable) return true;
  const on = Math.max(0, Number(state.circPulse.onMin ?? 0));
  const vypnuto = Math.max(0, Number(state.circPulse.vypnutoMin ?? 0));
  const cycle = on + vypnuto;
  if(cycle <= 0) return true;
  if(on <= 0) return false;
  const d = new Date(nowMs);
  const minsNow = d.getHours()*60 + d.getMinutes();
  let elapsed = minsNow - Number(intervalStartMin ?? minsNow);
  if(!Number.isFinite(elapsed)) elapsed = 0;
  if(elapsed < 0) elapsed += 1440;
  return (elapsed % cycle) < on;
};

    function saveSchedules(){
      localStorage.setItem("ui2026_schedules", JSON.stringify(state.schedules));
    }

    function timeToMin(hhmm){
      const m = /^(\d\d):(\d\d)$/.exec(hhmm || "");
      if(!m) return null;
      const hh = Number(m[1]), mm = Number(m[2]);
      if(hh<0||hh>23||mm<0||mm>59) return null;
      return hh*60+mm;
    }

    function isInIntervals(intervals, minutes){
      for(const it of (intervals||[])){
        const a = timeToMin(it.start), b = timeToMin(it.end);
        if(a===null || b===null) continue;
        if(a<=b){
          if(minutes>=a && minutes<b) return true;
        }else{
          if(minutes>=a || minutes<b) return true;
        }
      }
      return false;
    }

    function nowDayIndex(){
      const d = new Date();
      const js = d.getDay(); // 0=Sun
      return (js + 6) % 7;
    }

    function scheduleNow(){
      const d = new Date();
      const mins = d.getHours()*60 + d.getMinutes();
      const di = nowDayIndex();

      const dayActive = isInIntervals(state.schedules.heatingDay[di], mins);
      const dhwActive = isInIntervals(state.schedules.dhwHeat[di], mins);
      const circIntervals = state.schedules.dhwCirc[di] || [];
      const circPlanActive = isInIntervals(circIntervals, mins);
      let circIntervalStart = null;
      for(const it of circIntervals){
        const a = timeToMin(it.start), b = timeToMin(it.end);
        if(a===null || b===null) continue;
        if((a <= b && mins >= a && mins < b) || (a > b && (mins >= a || mins < b))){ circIntervalStart = a; break; }
      }
      const nowMs = d.getTime();
      const circInputActive = !!state.io.inputs?.[2];
      const circRequested = circPlanActive || circInputActive;
      const pulseAnchorStart = circPlanActive ? circIntervalStart : 0;
      const circPulseOn = circRequested ? (state.circPulse?.enable ? circPulseIsOn(nowMs, pulseAnchorStart) : true) : false;
      const circActive = circRequested && (state.circPulse?.enable ? circPulseOn : true);
      return { dayActive, dhwActive, circActive, circRequested, circInputActive, circPlanActive, circPulseOn, circIntervalStart, mins, di };
    }

    
    // Nest-like planner UI (multiple intervals/day)
    state.uiPlannerDay = state.uiPlannerDay || {};

    function summarizeIntervals(intervals){
      const arr = (intervals||[]).filter(it => it?.start && it?.end);
      if(!arr.length) return "bez intervalů";
      return arr.map(it => `${it.start}–${it.end}`).join(" • ");
    }

    function normIntervals(list){
      // Keep valid HH:MM, drop invalid
      const out = [];
      for(const it of (list||[])){
        const a = timeToMin(it.start);
        const b = timeToMin(it.end);
        if(a===null || b===null) continue;
        out.push({start: it.start, end: it.end});
      }
      return out;
    }

    function splitOvernight(it){
      const a = timeToMin(it.start), b = timeToMin(it.end);
      if(a===null || b===null) return [];
      if(a<=b) return [{a,b,label:`${it.start}–${it.end}`}];
      // overnight -> split
      return [
        {a, b: 1440, label:`${it.start}–24:00`},
        {a: 0, b, label:`00:00–${it.end}`},
      ];
    }

    function plannerTypeClass(key){
      if(key==="heatingDay") return "heat";
      if(key==="dhwHeat") return "dhw";
      if(key==="dhwCirc") return "circ";
      return "heat";
    }

    function plannerTitle(key){
      if(key==="heatingDay") return "Plán Komfort (mimo interval = Útlum)";
      if(key==="dhwHeat") return "Plán ohřevu TUV";
      if(key==="dhwCirc") return "Plán cirkulace TUV";
      return "Plán";
    }

    function plannerAllowsMultiple(key){
      return true;
    }

    function plannerAllowsOvernight(key){
      return key !== "heatingDay";
    }

    function plannerValidateInterval(key, start, end){
      const a = timeToMin(start), b = timeToMin(end);
      if(a===null || b===null) return { ok:false, msg:"Neplatný čas." };
      if(a===b) return { ok:false, msg:"Začátek a konec nesmí být stejné." };
      if(!plannerAllowsOvernight(key) && a > b) return { ok:false, msg:"Topení nepodporuje interval přes půlnoc." };
      return { ok:true };
    }

    function serializeDhwWeek(key, maxIntervals=DHW_MAX_INTERVALS_PER_DAY){
      return ["mon","tue","wed","thu","fri","sat","sun"].map((day, i) => {
        const arr = normIntervals(state.schedules?.[key]?.[i] || []);
        if(arr.length > maxIntervals) throw new Error(`${key} den ${i+1}: maximum je ${maxIntervals} intervalů.`);
        if(intervalsOverlap(arr)) throw new Error(`${key} den ${i+1}: intervaly se překrývají.`);
        return {
          day,
          intervals: arr.map(iv => ({ startMin: timeToMin(iv.start), endMin: timeToMin(iv.end) }))
            .filter(iv => Number.isFinite(iv.startMin) && Number.isFinite(iv.endMin) && iv.startMin !== iv.endMin)
        };
      });
    }

    function updatePlannerStateBadges(){
      try{
        const s = scheduleNow();
        const dhwf = state.dhwFast || {};
        const eqf = state.eqFast || {};

        const heatDayActive = String(eqf.me || state.eqStatus?.mode?.eff || "day").toLowerCase() !== "night";
        const heatSource = eqf.i1 ? "IN1" : (eqf.su ? "plán" : (String(eqf.m || state.eqStatus?.mode?.req || "auto").toLowerCase() === "auto" ? "auto" : "ručně"));

        const dhwScheduleActive = !!(dhwf.hs ?? state.dhwStatus?.heatScheduleActive ?? s.dhwActive);
        const dhwInputActive = !!(dhwf.hi ?? state.dhwStatus?.heatInputActive ?? state.io.inputs?.[1]);
        const dhwRequested = !!(dhwf.hr ?? state.dhwStatus?.heatRequested ?? (dhwScheduleActive || dhwInputActive));

        const circRequested = !!(dhwf.cr ?? state.dhwStatus?.circRequested ?? s.circRequested);
        const circActive = !!(dhwf.ca ?? state.dhwStatus?.circActive ?? s.circActive);
        const circPlanActive = !!(dhwf.cs ?? state.dhwStatus?.circScheduleActive ?? s.circPlanActive);
        const circInputActive = !!(dhwf.ci ?? state.dhwStatus?.circInputActive ?? s.circInputActive);
        const circPulseOn = !!(dhwf.cp ?? state.dhwStatus?.circPulseOn ?? s.circPulseOn);
        const pulseEnabled = !!(state.dhwCfg?.circ?.pulseEnabled ?? state.circPulse?.enable);

        setBadge("#plStateHeat", heatDayActive ? "good" : "warn", "teď: " + (heatDayActive ? "KOMFORT" : "ÚTLUM") + ` • ${heatSource}`);
        const dhwTxt = dhwRequested
          ? (dhwScheduleActive ? "ohřev: aktivní plán" : (dhwInputActive ? "ohřev: aktivní vstup IN2" : "ohřev: požadavek aktivní"))
          : "ohřev: neaktivní";
        setBadge("#plStateDhw", dhwRequested ? "good" : "", dhwTxt);
        setBadge("#plStateCirc", circRequested ? "good" : "", "cirkulace: " + (circRequested ? "požadavek aktivní" : "bez požadavku"));
        setBadge("#dhwPlanNow", dhwScheduleActive ? "good" : (dhwInputActive ? "warn" : ""), dhwScheduleActive ? "ohřev: aktivní plán" : (dhwInputActive ? "ohřev: aktivní vstup IN2" : "ohřev: neaktivní"));
        setBadge("#circPlanNow", circPlanActive ? "good" : (circInputActive ? "warn" : ""), circPlanActive ? "cirkulace: aktivní plán" : (circInputActive ? "cirkulace: aktivní vstup IN3" : "cirkulace: neaktivní"));

        let pulseTxt = "cyklus: bez požadavku";
        let pulseCls = "";
        if(circRequested){
          if(pulseEnabled){
            pulseTxt = "cyklus: " + (circPulseOn ? "ON" : "OFF");
            pulseCls = circPulseOn ? "good" : "warn";
          }else{
            pulseTxt = "cyklus: kontinuálně";
            pulseCls = circActive ? "good" : "";
          }
        }
        setBadge("#circPulseNow", pulseCls, pulseTxt);
      }catch(e){
        console.warn("planner badge update failed", e);
      }
    }


    function intervalsOverlap(list){
      const parts = [];
      for(const it of (list||[])) parts.push(...splitOvernight(it));
      parts.sort((x,y)=>x.a-y.a || x.b-y.b);
      for(let i=1;i<parts.length;i++){ if(parts[i].a < parts[i-1].b) return true; }
      return false;
    }

    function renderPlanner(key){
      const host = document.getElementById("planner-" + key);
      if(!host) return;

      const typeCls = plannerTypeClass(key);
      const sched = state.schedules[key];

      const today = nowDayIndex();
      const selected = (state.uiPlannerDay[key] ?? today);
      state.uiPlannerDay[key] = selected;

      host.innerHTML = "";

      const wrap = document.createElement("div");
      wrap.className = "plannerN";

      // Left: day list
      const days = document.createElement("div");
      days.className = "pDays";
      for(let d=0; d<7; d++){
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "pDayBtn";
        btn.setAttribute("aria-selected", d===selected ? "true" : "false");
        const sum = summarizeIntervals(sched[d]);
        btn.innerHTML = `
          <span class="l">
            <strong>${escapeHtml(daysCZ[d])}</strong>
            <span>${escapeHtml(sum)}</span>
          </span>
          <span class="r">${(sched[d]||[]).length}×</span>
        `;
        btn.addEventListener("click", () => {
          state.uiPlannerDay[key] = d;
          renderPlanner(key);
        });
        days.appendChild(btn);
      }

      // Right: main timeline + editor
      const main = document.createElement("div");
      main.className = "pMain";

      const top = document.createElement("div");
      top.className = "pTop";
      top.innerHTML = `
        <div class="t">
          <strong>${escapeHtml(plannerTitle(key))} — ${escapeHtml(daysCZ[selected])}</strong>
          <span>Časová osa 0–24 h • klikni na blok pro úpravu</span>
        </div>
        <div class="a">
          <button class="btn" type="button" id="pAdd_${key}" ${(!plannerAllowsMultiple(key) && (sched[selected]||[]).length >= 1) ? "disabled" : ""}>＋ Přidat</button>
          <span class="pHint">${escapeHtml(key==="heatingDay" ? "až 6 intervalů/den, bez přesahu přes půlnoc" : "Intervaly → požadavek aktivní")}</span>
        </div>
      `;
      main.appendChild(top);

      // timeline
      const tl = document.createElement("div");
      tl.className = "pTimeline";
      const ticks = document.createElement("div");
      ticks.className = "pTicks";
      // 0, 6, 12, 18, 24
      [0,6,12,18,24].forEach(h => {
        const t = document.createElement("div");
        t.className = "pTick";
        t.style.left = `calc(${(h/24)*100}% )`;
        t.textContent = (h===24 ? "24" : String(h));
        ticks.appendChild(t);
      });
      const blocks = document.createElement("div");
      blocks.className = "pBlocks";
      tl.appendChild(ticks);
      tl.appendChild(blocks);
      main.appendChild(tl);

      const editor = document.createElement("div");
      editor.className = "pEditor";
      editor.style.display = "none";
      main.appendChild(editor);

      const list = document.createElement("div");
      list.className = "pList";
      main.appendChild(list);

      wrap.appendChild(days);
      wrap.appendChild(main);
      host.appendChild(wrap);

      function refreshDay(){
        // normalize
        sched[selected] = normIntervals(sched[selected]);
        saveSchedules();

        // blocks
        blocks.innerHTML = "";
        const dayInts = sched[selected] || [];
        dayInts.forEach((it, idx) => {
          const parts = splitOvernight(it);
          parts.forEach(p => {
            const left = (p.a/1440)*100;
            const width = ((p.b - p.a)/1440)*100;
            const b = document.createElement("div");
            b.className = `pBlock ${typeCls}`;
            b.style.left = `calc(${left}% + 0px)`;
            b.style.width = `calc(${width}% - 2px)`;
            b.innerHTML = `<span class="txt">${escapeHtml(p.label)}</span><span class="ic">✎</span>`;
            b.title = `${it.start}–${it.end}`;
            b.addEventListener("click", () => openEditor(idx));
            blocks.appendChild(b);
          });
        });

        // list
        list.innerHTML = "";
        dayInts.forEach((it, idx) => {
          const row = document.createElement("div");
          row.className = "pItem";
          row.innerHTML = `
            <div class="l">
              <strong>${escapeHtml(it.start)}–${escapeHtml(it.end)}</strong>
              <span>${escapeHtml(key==="heatingDay" ? "Komfort" : "Aktivní")}</span>
            </div>
            <div class="r">
              <button class="btn" type="button" data-edit="${idx}">Upravit</button>
              <button class="btn danger" type="button" data-del="${idx}">Smazat</button>
            </div>
          `;
          row.querySelector("[data-edit]").addEventListener("click", () => openEditor(idx));
          row.querySelector("[data-del]").addEventListener("click", () => {
            sched[selected].splice(idx,1);
            saveSchedules();
            updatePlannerStateBadges();
            refreshDay();
            toast("Plán", `${daysCZ[selected]}: interval smazán.`, "🗑");
          });
          list.appendChild(row);
        });

        // update day summaries on left
        Array.from(days.children).forEach((btn, d) => {
          btn.setAttribute("aria-selected", d===selected ? "true":"false");
          const sum = summarizeIntervals(sched[d]);
          btn.querySelector(".l span").textContent = sum;
          btn.querySelector(".r").textContent = `${(sched[d]||[]).length}×`;
        });

        updatePlannerStateBadges();
      }

      function openEditor(idx){
        const it = sched[selected][idx];
        editor.style.display = "grid";
        editor.innerHTML = `
          <div class="row">
            <strong>Upravit interval</strong>
            <span class="badge"><span class="b"></span>${escapeHtml(daysCZ[selected])}</span>
          </div>
          <div class="pRowMini">
            <div class="field">
              <label>Od</label>
              <input type="time" id="pStart_${key}" value="${escapeHtml(it.start)}">
            </div>
            <div class="field">
              <label>Do</label>
              <input type="time" id="pEnd_${key}" value="${escapeHtml(it.end)}">
            </div>
            <button class="btn" type="button" id="pSave_${key}">Uložit</button>
            <button class="btn" type="button" id="pClose_${key}">Zavřít</button>
            <button class="btn danger" type="button" id="pDelete_${key}">Smazat</button>
          </div>
          <div class="muted">${escapeHtml(plannerAllowsOvernight(key) ? "Pozn.: Interval přes půlnoc se automaticky rozkreslí na dvě části." : "Pozn.: U topení je povoleno až 6 intervalů/den bez přesahu přes půlnoc.")}</div>
        `;

        const sEl = document.getElementById(`pStart_${key}`);
        const eEl = document.getElementById(`pEnd_${key}`);

        document.getElementById(`pSave_${key}`).addEventListener("click", () => {
          const ns = sEl.value, ne = eEl.value;
          const chk = plannerValidateInterval(key, ns, ne);
          if(!chk.ok){
            toast("Plán", chk.msg, "⚠");
            return;
          }
          const next = [...sched[selected]];
          next[idx] = {start: ns, end: ne};
          if(key === "heatingDay" && next.length > HEATING_MAX_INTERVALS_PER_DAY){ toast("Plán topení", `Maximálně ${HEATING_MAX_INTERVALS_PER_DAY} intervalů za den.`, "⚠"); return; }
          if(intervalsOverlap(next)){ toast("Plán", "Intervaly se překrývají.", "⚠"); return; }
          sched[selected] = next;
          saveSchedules();
          refreshDay();
          toast("Plán", "Uloženo.", "✅");
          log(`planner edit: ${key} day=${selected} idx=${idx}`);
        });
        document.getElementById(`pClose_${key}`).addEventListener("click", () => {
          editor.style.display = "none";
        });
        document.getElementById(`pDelete_${key}`).addEventListener("click", () => {
          sched[selected].splice(idx,1);
          saveSchedules();
          editor.style.display = "none";
          refreshDay();
          toast("Plán", "Interval smazán.", "🗑");
        });
      }

      document.getElementById(`pAdd_${key}`).addEventListener("click", () => {
        sched[selected] = sched[selected] || [];
        if(!plannerAllowsMultiple(key) && sched[selected].length >= 1){
          toast("Plán topení", `Maximálně ${HEATING_MAX_INTERVALS_PER_DAY} intervalů za den.`, "⚠");
          return;
        }
        if(key === "heatingDay" && sched[selected].length >= HEATING_MAX_INTERVALS_PER_DAY){ toast("Plán topení", `Maximálně ${HEATING_MAX_INTERVALS_PER_DAY} intervalů za den.`, "⚠"); return; }
        const next = [...sched[selected], {start:"06:00", end:"07:00"}];
        if(intervalsOverlap(next)){ toast("Plán", "Nový interval se překrývá se stávajícím.", "⚠"); return; }
        sched[selected] = next;
        saveSchedules();
        refreshDay();
        toast("Plán", `${daysCZ[selected]}: přidán interval.`, "🗓");
        log(`planner add: ${key} day=${selected}`);
      });

      refreshDay();
    }


        // ----- Thermometers (DEVICE)
const dallasRoleMetaDefault = [
  { key:"outside",     name:"Venkovní",        note:"ekviterm • bod X" },
  { key:"dhw_tank",    name:"Zásobník TUV",    note:"náhradní režim při neplatné OT TUV" },
  { key:"tank_top",    name:"AKU nahoře",      note:"nádrž" },
  { key:"tank_mid",    name:"AKU uprostřed",   note:"nádrž" },
  { key:"tank_bottom", name:"AKU dole",        note:"nádrž" },
  { key:"return",      name:"Zpátečka",        note:"GPIO2 • ReturnTempC / Return.flow / Return" },
  { key:"dhw_return",  name:"Zpátečka TUV",      note:"okruh TUV" },
];

state.th = { loaded:false, načítání:false, cfgLoaded:false, dallasEnabled:false, roles:{}, roleGpio:{}, roleMeta:[...dallasRoleMetaDefault], roleState:{}, ds:[], ble:null, lastError:"" };

function thBadge(kind, text){
  const b = document.getElementById("thCfgState");
  if(!b) return;
  b.classList.remove("good","warn","bad");
  if(kind) b.classList.add(kind);
  b.childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
  b.appendChild(document.createTextNode(" " + text));
}

function flattenDallas(dallasStatus){
  const out = { ds: [], roleGpio: {}, roleState: {} };
  const roles = (dallasStatus && typeof dallasStatus === "object" && dallasStatus.dallas && typeof dallasStatus.dallas.roles === "object")
    ? dallasStatus.dallas.roles
    : {};
  Object.keys(roles).forEach(k => {
    const role = roles?.[k] || {};
    const g = numOrNaN(role?.gpio);
    if(Number.isFinite(g)) out.roleGpio[k] = Number(g);
    out.roleState[k] = {
      currentC: numOrNaN(role?.currentC),
      currentSrc: readMaybeString(role, "currentSrc", ""),
      resolvedRom: readMaybeString(role, "resolvedRom", ""),
      resolvedGpio: Number.isFinite(numOrNaN(role?.resolvedGpio)) ? Number(role.resolvedGpio) : (Number.isFinite(g) ? Number(g) : NaN),
      ageMs: Number(role?.ageMs || 0),
    };
  });

  const buses = Array.isArray(dallasStatus?.dallas?.buses) ? dallasStatus.dallas.buses : [];
  for(const b of buses){
    const gpio = Number(b?.gpio);
    const devs = Array.isArray(b?.devs) ? b.devs : [];
    for(const d of devs){
      out.ds.push({ gpio, rom:String(d?.rom||""), c:(d?.c==null?null:Number(d.c)), ok:!!d?.ok });
    }
  }
  return out;
}

function normalizeDallasRoleMeta(availableRoles){
  if(!Array.isArray(availableRoles) || !availableRoles.length) return [...dallasRoleMetaDefault];
  const defaults = Object.fromEntries(dallasRoleMetaDefault.map(item => [item.key, item]));
  const out = [];
  for(const r of availableRoles){
    const key = String(r?.key || "").trim();
    if(!key) continue;
    const base = defaults[key] || {};
    const label = String(r?.label || base.name || key).trim() || key;
    const gpio = Number(r?.gpio);
    let note = String(base.note || "").trim();
    if(Number.isFinite(gpio) && !new RegExp(`\bGPIO${gpio}\b`, "i").test(note)){
      note = [note, `GPIO${gpio}`].filter(Boolean).join(" • ");
    }
    out.push({ key, name: label, note });
  }
  return out.length ? out : [...dallasRoleMetaDefault];
}

function roleTempKey(role){
  return role === "dhw_tank" ? "dhw" : role;
}

function roleValueFromFast(role, temps = state.fast?.temps){
  if(!temps || typeof temps !== "object") return NaN;
  if(role === "return") return getAfterMixTempFromTemps(temps);
  if(role === "dhw_tank") return firstFinite(temps.dhw_tank, temps.dhw);
  return numOrNaN(temps[roleTempKey(role)]);
}

function roleSourceFromFast(role, temps = state.fast?.temps){
  if(!temps || typeof temps !== "object") return "";
  if(role === "return"){
    return readMaybeString(temps, "returnSrc",
      readMaybeString(temps, "returnTempSrc",
        readMaybeString(temps, "afterMixSrc", "")));
  }
  if(role === "dhw_tank") return readMaybeString(temps, "dhw_tankSrc", readMaybeString(temps, "dhwSrc", ""));
  const key = roleTempKey(role);
  return readMaybeString(temps, key + "Src", "");
}

function roleResolvedRomFromFast(role, temps = state.fast?.temps){
  const roms = temps?.rom;
  if(!roms || typeof roms !== "object") return "";
  if(role === "dhw_tank") return String(roms.dhw_tank || roms.dhw || "");
  return String(roms[roleTempKey(role)] || "");
}

function getRoleUiState(role){
  const roleState = state.th?.roleState?.[role] || {};
  const currentC = firstFinite(roleValueFromFast(role), roleState.currentC);
  const currentSrc = roleSourceFromFast(role) || String(roleState.currentSrc || "");
  const resolvedRom = roleResolvedRomFromFast(role) || String(roleState.resolvedRom || "");
  const resolvedGpio = firstFinite(roleState.resolvedGpio, state.th?.roleGpio?.[role]);
  const ageMs = Number(roleState.ageMs || 0);
  return { currentC, currentSrc, resolvedRom, resolvedGpio, ageMs };
}

function formatRoleUiState(role){
  const s = getRoleUiState(role);
  const parts = [Number.isFinite(s.currentC) ? `${s.currentC.toFixed(1)} °C` : "--"];
  if(s.currentSrc) parts.push(s.currentSrc);
  if(s.resolvedRom) parts.push(s.resolvedRom);
  return parts.join(" • ");
}

function optRom(rom, label){
  const r = String(rom || "");
  const l = label || r;
  return `<option value="${escapeHtml(r)}">${escapeHtml(l)}</option>`;
}

function normalizeDallasRolesMap(rawRoles){
  const out = { outside:"", dhw_tank:"", tank_top:"", tank_mid:"", tank_bottom:"", return:"", dhw_return:"" };
  const src = (rawRoles && typeof rawRoles === "object") ? rawRoles : {};
  for(const key of Object.keys(out)){
    const raw = src[key];
    if(raw && typeof raw === "object"){
      const rom = String(raw.rom || raw.resolvedRom || "").trim();
      out[key] = rom;
    }else{
      out[key] = String(raw || "").trim();
    }
  }
  return out;
}

function renderThermometersDevice(){
  const tbl = document.getElementById("thMapTbl");
  if(!tbl) return;

  if(!state.th.loaded){
    thBadge(state.th.lastError ? "bad" : "warn", state.th.lastError ? "chyba" : "nenačteno");
    tbl.innerHTML = `<tr><td colspan="4" class="muted">${escapeHtml(state.th.lastError || 'Klikni na „Načíst“.')}</td></tr>`;
    return;
  }

  thBadge("good","OK");
  const en = document.getElementById("dallasEnable");
  if(en) en.checked = !!state.th.dallasEnabled;

  const dsList = Array.isArray(state.th.ds) ? state.th.ds : [];
  const dsCnt = document.getElementById("thDsCnt");
  if(dsCnt) dsCnt.textContent = `${dsList.length} ks`;

  tbl.innerHTML = "";
  for(const meta of (state.th.roleMeta?.length ? state.th.roleMeta : dallasRoleMetaDefault)){
    const role = meta.key;
    const cur = String(state.th.roles?.[role] || "");
    const gpio = firstFinite(state.th.roleGpio?.[role], getRoleUiState(role).resolvedGpio);
    const tr = document.createElement("tr");

    const sel = document.createElement("select");
    const roleDevices = dsList
      .filter(x => !Number.isFinite(gpio) || Number(x.gpio) === Number(gpio))
      .sort((a,b) => String(a.rom).localeCompare(String(b.rom)));
    let html = `<option value="">(auto)</option>`;
    for(const dev of roleDevices){
      const t = (dev && dev.c!=null && Number.isFinite(dev.c)) ? `${dev.c.toFixed(1)}°C` : "--";
      const g = (dev && Number.isFinite(dev.gpio)) ? `GPIO${dev.gpio}` : "";
      html += optRom(dev.rom, `${dev.rom} • ${t} • ${g}`);
    }
    if(cur && !roleDevices.some(x => x.rom === cur)){
      const fallbackDev = dsList.find(x => x.rom === cur);
      const t = (fallbackDev && fallbackDev.c!=null && Number.isFinite(fallbackDev.c)) ? `${fallbackDev.c.toFixed(1)}°C` : "--";
      const g = (fallbackDev && Number.isFinite(fallbackDev.gpio)) ? `GPIO${fallbackDev.gpio}` : "jiný GPIO";
      html += optRom(cur, `${cur} • ${t} • ${g} • mimo roli`);
    }
    sel.innerHTML = html;
    sel.value = cur;

    const valTd = document.createElement("td");
    valTd.className = "mono";
    valTd.textContent = formatRoleUiState(role);

    sel.addEventListener("change", () => {
      state.th.roles[role] = sel.value || "";
      valTd.textContent = formatRoleUiState(role);
    });

    tr.innerHTML = `
      <td><strong>${escapeHtml(meta.name)}</strong><div class="muted">${escapeHtml(meta.note||"")}</div></td>
      <td class="mono">${Number.isFinite(gpio) ? `GPIO${gpio}` : "--"}</td>
      <td></td><td></td>
    `;
    tr.children[2].appendChild(sel);
    tr.replaceChild(valTd, tr.children[3]);
    tbl.appendChild(tr);
  }

  const dsTb = document.getElementById("thDsTbl");
  if(dsTb){
    dsTb.innerHTML = dsList.map(d => {
      const t = (d.c!=null && Number.isFinite(d.c)) ? `${d.c.toFixed(1)} °C` : "--";
      const ok = d.ok ? '<span class="badge good"><span class="b"></span>OK</span>' : '<span class="badge bad"><span class="b"></span>ERR</span>';
      return `<tr><td class="mono">GPIO${escapeHtml(d.gpio)}</td><td class="mono">${escapeHtml(d.rom)}</td><td class="mono">${escapeHtml(t)}</td><td>${ok}</td></tr>`;
    }).join("");
  }

  const bleState = document.getElementById("thBleState");
  const bleTb = document.getElementById("thBleTbl");
  const b = state.th.ble;
  if(bleState){
    const en2 = !!b?.en;
    const v = !!b?.meteo?.v;
    bleState.textContent = (!en2) ? "vypnuto" : (v ? "ok" : "bez dat");
  }
  if(bleTb){
    const rows = [];
    rows.push(`<tr><td><strong>povoleno</strong></td><td class="mono">${escapeHtml(String(!!b?.en))}</td></tr>`);
    const t2 = b?.meteo?.t;
    const h2 = b?.meteo?.h;
    const p2 = b?.meteo?.p;
    rows.push(`<tr><td><strong>teplota</strong></td><td class="mono">${(t2==null) ? "--" : Number(t2).toFixed(1) + " °C"}</td></tr>`);
    rows.push(`<tr><td><strong>vlhkost</strong></td><td class="mono">${(h2==null) ? "--" : Number(h2).toFixed(1) + " %"}</td></tr>`);
    rows.push(`<tr><td><strong>tlak</strong></td><td class="mono">${(p2==null) ? "--" : Number(p2).toFixed(1) + " hPa"}</td></tr>`);
    bleTb.innerHTML = rows.join("");
  }

  const otState = document.getElementById("thOtState");
  const otTb = document.getElementById("thOtTbl");
  if(otState) otState.textContent = state.ot?.comm ? "ok" : "err";
  if(otTb){
    otTb.innerHTML = [
      `<tr><td><strong>CH setpoint</strong></td><td class="mono">${fmtMaybeNumber(state.ot?.chSet, 1, " °C")}</td></tr>`,
      `<tr><td><strong>CH teplota</strong></td><td class="mono">${fmtMaybeNumber(state.ot?.chTemp, 1, " °C")}</td></tr>`,
      `<tr><td><strong>TUV teplota</strong></td><td class="mono">${fmtMaybeNumber(state.ot?.dhwTemp, 1, " °C")}</td></tr>`,
      `<tr><td><strong>Tlak</strong></td><td class="mono">${fmtMaybeNumber(state.ot?.pressure, 2, " bar")}</td></tr>`,
    ].join("");
  }
}

async function thermoLoad(options={}){
  if(state.th.načítání) return;
  const silent = !!options.silent;
  const forceConfig = !!options.forceConfig;
  state.th.načítání = true;
  state.th.lastError = "";
  try{
    thBadge("warn","čtu…");
    let cfg = null;
    if(!forceConfig && state.th?.cfgLoaded){
      cfg = {
        enabled: !!state.th.dallasEnabled,
        roles: state.th.roles || {},
        availableRoles: state.th.roleMeta || []
      };
    }else{
      cfg = await api.fetchConfigSection("dallas");
    }
    let ds = null;
    let ble = null;
    try{ ds = await api.getJson("/api/dallas/status", 3500); }catch(_e){}
    try{ ble = await api.getJson("/api/ble/status", 2000); }catch(_e){}
    const flat = flattenDallas(ds);

    state.th.loaded = true;
    state.th.cfgLoaded = true;
    state.th.dallasEnabled = !!cfg?.enabled;
    state.th.roles = normalizeDallasRolesMap(cfg?.roles);
    state.th.roleMeta = normalizeDallasRoleMeta(cfg?.availableRoles);
    state.th.roleGpio = flat.roleGpio || {};
    state.th.roleState = flat.roleState || {};
    state.th.ds = Array.isArray(flat.ds) ? flat.ds : [];
    state.th.ble = ble;

    renderThermometersDevice();
    setApiHealth(ds ? "good" : "warn", ds ? "API: zařízení" : "API: Dallas status chyba");
    if(ds){
      if(!silent) toast("Teploměry", "Načteno ze zařízení.", "✅");
    }else{
      if(!silent) toast("Teploměry", "Konfigurace načtena, ale Dallas status se nepodařilo přečíst.", "⚠");
      log("thermo dallas status warning: status unavailable");
    }
  }catch(e){
    state.th.loaded = false;
    state.th.lastError = e?.message || String(e);
    renderThermometersDevice();
    thBadge("bad","chyba");
    setApiHealth("bad", "API: chyba");
    if(!silent) toast("Chyba", state.th.lastError, "⚠");
    log("thermo load error: " + state.th.lastError);
  }finally{
    state.th.načítání = false;
  }
}

async function thermoSave(){
  try{
    state.th.dallasEnabled = !!document.getElementById("dallasEnable")?.checked;
    thBadge("warn","odesílám…");
    await api.postConfigSection("dallas", { enabled: state.th.dallasEnabled, roles: normalizeDallasRolesMap(state.th.roles) });
    toast("Teploměry", "Uloženo do zařízení.", "✅");
    await thermoLoad();
  }catch(e){
    thBadge("bad","chyba");
    toast("Chyba", e.message || String(e), "⚠");
    log("thermo save error: " + (e.message || e));
  }
}
// ----- Shared UI helpers
    function clamp(v,a,b){ return Math.max(a, Math.min(b, v)); }

    function setTankFill(id, top, mid, bot){
      const el = $(id);
      if(!el) return;
      const vals = [top, mid, bot].map(numOrNaN).filter(Number.isFinite);
      if(!vals.length){
        el.style.transform = "scaleY(0)";
        return;
      }
      const avg = vals.reduce((a,b) => a + b, 0) / vals.length;
      // Map 10..80°C -> 0..1
      const k = clamp((avg - 10) / 70, 0, 1);
      el.style.transform = `scaleY(${k.toFixed(3)})`;
    }
    function rnd(n=1){ return (Math.random()-0.5)*n; }

    // ----- Device API mapping
    // NOTE: Without knowing your exact API schema, this is designed as an adapter.
    // Update endpoints/fields to match your firmware.
      const api = {
      async getJson(path, timeoutMs=6000){
        const base = normalizedApiBase();
        const url = base + path;
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(new Error(`Timeout ${timeoutMs} ms: ${path}`)), timeoutMs);
        try{
          const r = await fetch(url, { signal: ctrl.signal, cache: "no-store" });
          return await parseApiReply(r, path);
        }catch(e){
          if(e?.name === "AbortError") throw new Error(`Request timeout: ${path}`);
          throw e;
        }finally{
          clearTimeout(t);
        }
      },

      async postJson(path, obj, timeoutMs=12000){
        const base = normalizedApiBase();
        const url = base + path;
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(new Error(`Timeout ${timeoutMs} ms: ${path}`)), timeoutMs);
        try{
          const r = await fetch(url, {
            method: "POST",
            signal: ctrl.signal,
            cache: "no-store",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(obj ?? {})
          });
          return await parseApiReply(r, path);
        }catch(e){
          if(e?.name === "AbortError") throw new Error(`Request timeout: ${path}`);
          throw e;
        }finally{
          clearTimeout(t);
        }
      },

      async fetchStatusExtras(activeView){
        const out = {};
        const view = String(activeView || "").toLowerCase();
        const fetchAll = !view || !["heating","dhw","opentherm"].includes(view);
        const wantEq = fetchAll || view === "heating";
        const wantOt = fetchAll || view === "heating" || view === "dhw" || view === "opentherm";
        const wantDhw = fetchAll || view === "dhw";

        if(wantEq){
          try{ out.equitherm = await api.getJson("/api/equitherm/status", 5000); }catch(_e){}
        }
        if(wantOt){
          try{ out.opentherm = await api.getJson("/api/opentherm/status", 5000); }catch(_e){}
        }
        if(wantDhw){
          try{ out.dhw = await api.getJson("/api/dhw/status", 5000); }catch(_e){}
        }
        return out;
      },
      async fetchConfigSection(section){
        return await api.getJson(`/api/config/${encodeURIComponent(section)}`, 8000);
      },
      async fetchBootstrap(){
        return await api.getJson("/api/bootstrap", 5000);
      },

      async postConfigSection(section, obj){
        return await api.postJson(`/api/config/${encodeURIComponent(section)}`, obj ?? {}, 10000);
      },

      async setRelay(id, on=null){
        const payload = { id: Number(id) };
        if(on === null || on === undefined){
          payload.toggle = true;
        }else{
          payload.toggle = false;
          payload.on = !!on;
        }
        return await api.postJson("/api/relay", payload, 6000);
      },

      async setRelayMask(mask){
        return await api.postJson("/api/relay", { mask: Number(mask) & 0xFF }, 6000);
      },
      async eqCmd(obj){
        return await api.postJson("/api/equitherm/cmd", obj ?? {}, 6000);
      },

      async otCmd(obj){
        return await api.postJson("/api/opentherm/cmd", obj ?? {}, 6000);
      },

      async dhwCmd(obj){
        return await api.postJson("/api/dhw/cmd", obj ?? {}, 6000);
      },

      async reboot(){
        return await api.postJson("/api/reboot", {}, 6000);
      }
    };

    function mqttSetBadge(kind, text){
      setBadge("#mqttState", kind, text);
    }

    function mqttApplyConfigToForm(cfgLike, statusLike=null){
      const cfg = (cfgLike && cfgLike.mqtt) ? cfgLike.mqtt : (cfgLike || {});
      const status = (statusLike && statusLike.mqtt) ? statusLike.mqtt : (statusLike || {});
      const ha = cfg.homeAssistant || status.homeAssistant || {};
      const mqttEnable = document.getElementById("mqttEnable");
      const mqttHost = document.getElementById("mqttHost");
      const mqttPort = document.getElementById("mqttPort");
      const mqttUser = document.getElementById("mqttUser");
      const mqttPassword = document.getElementById("mqttPassword");
      const mqttClientId = document.getElementById("mqttClientId");
      const mqttBaseTopic = document.getElementById("mqttBaseTopic");
      const mqttPublish = document.getElementById("mqttPublishIntervalMs");
      const mqttHaEnable = document.getElementById("mqttHaEnable");
      const mqttHaDiscovery = document.getElementById("mqttHaDiscovery");
      const mqttDiscoveryPrefix = document.getElementById("mqttDiscoveryPrefix");
      const mqttNodeId = document.getElementById("mqttNodeId");
      const passwordSetLbl = document.getElementById("mqttPasswordSet");
      const clearPassword = document.getElementById("mqttClearPassword");

      if(mqttEnable) mqttEnable.checked = !!cfg.enabled;
      if(mqttHost) mqttHost.value = String(cfg.host || status.host || "");
      if(mqttPort) mqttPort.value = String(Number(cfg.port || status.port || 1883));
      if(mqttUser) mqttUser.value = String(cfg.username || status.username || "");
      if(mqttPassword && document.activeElement !== mqttPassword) mqttPassword.value = "";
      if(mqttClientId) mqttClientId.value = String(cfg.clientId || status.clientId || "esp32-controller");
      if(mqttBaseTopic) mqttBaseTopic.value = String(cfg.baseTopic || status.baseTopic || "esp32-controller");
      if(mqttPublish) mqttPublish.value = String(Number(cfg.publishIntervalMs || status.publishIntervalMs || 10000));
      if(mqttHaEnable) mqttHaEnable.checked = !!ha.enabled;
      if(mqttHaDiscovery) mqttHaDiscovery.checked = !!ha.discovery;
      if(mqttDiscoveryPrefix) mqttDiscoveryPrefix.value = String(ha.discoveryPrefix || "homeassistant");
      if(mqttNodeId) mqttNodeId.value = String(ha.nodeId || "esp32_controller");
      if(passwordSetLbl) passwordSetLbl.textContent = (cfg.passwordSet || status.passwordSet) ? "heslo uloženo" : "heslo není uloženo";
      if(clearPassword) clearPassword.checked = false;
    }

    function mqttRenderStatus(statusLike){
      const status = (statusLike && statusLike.mqtt) ? statusLike.mqtt : (statusLike || {});
      const preview = status.preview || {};
      const runtime = String(status.runtime || "jen konfigurace");
      const připojeno = !!status.připojeno;
      const enabled = !!status.enabled;
      const runtimeLbl = document.getElementById("mqttRuntimeText");
      const previewEl = document.getElementById("mqttPreview");
      if(runtimeLbl) runtimeLbl.textContent = `${runtime}${připojeno ? " • připojeno" : " • dispřipojeno"}`;
      setBadge("#haState", (!!status.homeAssistant?.enabled && !!status.homeAssistant?.discovery) ? "good" : "", `HA: ${status.homeAssistant?.enabled ? (status.homeAssistant?.discovery ? "discovery" : "povoleno") : "vypnuto"}`);
      mqttSetBadge(enabled ? (připojeno ? "good" : "warn") : "", `MQTT: ${enabled ? (připojeno ? "připojeno" : runtime) : "vypnuto"}`);
      if(previewEl){
        previewEl.textContent = JSON.stringify(preview, null, 2);
      }
    }

    async function mqttLoad(options={}){
      const silent = !!options.silent;
      state.mqtt = state.mqtt || { loaded:false, načítání:false, status:null, config:null };
      if(state.mqtt.načítání) return;
      state.mqtt.načítání = true;
      try{
        if(!silent) mqttSetBadge("warn", "MQTT: načítám…");
        const [cfgRes, statusRes] = await Promise.allSettled([
          api.fetchConfigSection("mqtt"),
          api.getJson("/api/mqtt/status", 4000),
        ]);
        if(cfgRes.status === "fulfilled"){
          state.mqtt.config = cfgRes.value?.mqtt || cfgRes.value || {};
          mqttApplyConfigToForm(cfgRes.value, statusRes.status === "fulfilled" ? statusRes.value : null);
        }
        if(statusRes.status === "fulfilled"){
          state.mqtt.status = statusRes.value?.mqtt || statusRes.value || {};
          if(cfgRes.status !== "fulfilled") mqttApplyConfigToForm(statusRes.value, statusRes.value);
          mqttRenderStatus(statusRes.value);
        } else if(cfgRes.status === "fulfilled"){
          mqttRenderStatus(state.mqtt.config || {});
        }
        state.mqtt.loaded = true;
        if(!silent) toast("MQTT", "Načteno.", "✅");
      }catch(e){
        mqttSetBadge("bad", "MQTT: chyba");
        if(!silent) toast("MQTT", e.message || String(e), "⚠");
        log("mqtt load error: " + (e.message || e));
      }finally{
        state.mqtt.načítání = false;
      }
    }

    async function mqttSave(){
      const payload = {
        mqtt: {
          enabled: !!document.getElementById("mqttEnable")?.checked,
          host: String(document.getElementById("mqttHost")?.value || "").trim(),
          port: clamp(Number(document.getElementById("mqttPort")?.value || 1883), 1, 65535),
          username: String(document.getElementById("mqttUser")?.value || "").trim(),
          clientId: String(document.getElementById("mqttClientId")?.value || "").trim(),
          baseTopic: String(document.getElementById("mqttBaseTopic")?.value || "").trim(),
          publishIntervalMs: clamp(Number(document.getElementById("mqttPublishIntervalMs")?.value || 10000), 1000, 600000),
          clearPassword: !!document.getElementById("mqttClearPassword")?.checked,
          homeAssistant: {
            enabled: !!document.getElementById("mqttHaEnable")?.checked,
            discovery: !!document.getElementById("mqttHaDiscovery")?.checked,
            discoveryPrefix: String(document.getElementById("mqttDiscoveryPrefix")?.value || "").trim(),
            nodeId: String(document.getElementById("mqttNodeId")?.value || "").trim(),
          }
        }
      };
      const pw = String(document.getElementById("mqttPassword")?.value || "");
      if(pw.trim().length) payload.mqtt.password = pw;
      mqttSetBadge("warn", "MQTT: ukládám…");
      await api.postConfigSection("mqtt", payload.mqtt);
      state.mqtt.loaded = false;
      await mqttLoad({ silent:true });
      toast("MQTT", "Nastavení uloženo. MQTT runtime byl znovu načten.", "✅");
    }

    function getMixPulseMsFromForm(){
      return clamp(Number(document.getElementById("hMixPulseMs")?.value || 300), 50, 60000);
    }

    async function mixManualPulse(direction){
      const pulseMs = getMixPulseMsFromForm();
      const dir = String(direction || "").toLowerCase();
      await api.eqCmd({ mixPulse: dir, pulseMs });
      state.dev = state.dev || {};
      state.dev.eqCfgLoaded = false;
      if(state.net) state.net.extrasDueMs = 0;
      await refresh(false);
      toast("Směšovací ventil", `Manuální puls ${dir === "a" ? "směr A (otevření ventilu)" : "směr B (zavření ventilu)"} (${pulseMs} ms).`, "🧪");
    }

    async function mixManualMoveToEnd(direction){
      const dir = String(direction || "").toLowerCase();
      await api.eqCmd({ mixMove: dir === "a" ? "a_end" : "b_end" });
      state.dev = state.dev || {};
      state.dev.eqCfgLoaded = false;
      if(state.net) state.net.extrasDueMs = 0;
      await refresh(false);
      toast("Směšovací ventil", `Přejezd do krajní polohy ${dir === "a" ? "A (ventil otevřený)" : "B (ventil zavřený)"}.`, "🎯");
    }

    async function mixManualStop(){
      await api.eqCmd({ mixPulse: "stop" });
      if(state.net) state.net.extrasDueMs = 0;
      await refresh(false);
      toast("Směšovací ventil", "Pohyb ventilu zastaven.", "⛔");
    }

    function mixStateMeta(rawState){
      const s = String(rawState || "idle").toLowerCase();
      const map = {
        idle: ["klid", ""],
        in_deadband: ["v regulačním pásmu", ""],
        open: ["otevírá ventil", "good"],
        close: ["zavírá ventil", "warn"],
        open_pending: ["čeká na otevření", "warn"],
        close_pending: ["čeká na zavření", "warn"],
        hold_min_interval_open: ["čeká na min. interval před otevřením", "warn"],
        hold_min_interval_close: ["čeká na min. interval před zavřením", "warn"],
        settling_open: ["dobíhá k cíli po otevření", "warn"],
        settling_close: ["dobíhá k cíli po zavření", "warn"],
        no_feedback: ["bez zpětné vazby", "warn"],
        no_feedback_wait: ["čeká na návrat zpětné vazby", "warn"],
        fault_no_feedback: ["porucha: chybí zpětná vazba", "bad"],
        fault_actuator_suspect: ["porucha: podezření na pohon/ventil", "bad"],
        blocked_summer: ["blokováno letním režimem", "warn"],
        limit_a: ["na krajní poloze A", "warn"],
        limit_b: ["na krajní poloze B", "warn"],
        calibrating_a: ["kalibrace do polohy A", "warn"],
        calibrating_b: ["kalibrace do polohy B", "warn"],
        manual_open: ["ruční otevírání", "good"],
        manual_close: ["ruční zavírání", "warn"],
      };
      const hit = map[s];
      return { raw:s, label: hit ? hit[0] : s, kind: hit ? hit[1] : "", fault: /^fault_/.test(s), blocked: /^blocked_/.test(s) || /^limit_/.test(s) };
    }

    function mixCalibrationLabel(raw){
      const s = String(raw || "").toLowerCase();
      const map = {
        trusted: "kalibrace platná",
        trusted_a: "kalibrace potvrzena v poloze A",
        trusted_b: "kalibrace potvrzena v poloze B",
        untrusted: "poloha není důvěryhodná",
        stale: "kalibrace je zastaralá",
        feedback_missing: "bez zpětné vazby teploty",
        actuator_suspect: "podezření na pohon nebo ventil",
      };
      return map[s] || (s || "--");
    }

    function dhwPhaseMeta(rawPhase){
      const s = String(rawPhase || "idle").toLowerCase();
      const map = {
        idle: ["klid", ""],
        switching_to_dhw: ["přepínání na TUV", "warn"],
        heating: ["ohřev TUV", "good"],
        switching_back_to_ch: ["návrat na CH", "warn"],
      };
      const hit = map[s];
      return { raw:s, label: hit ? hit[0] : s, kind: hit ? hit[1] : "" };
    }

    function dhwReasonMeta(rawReason){
      const s = String(rawReason || "").toLowerCase();
      const map = {
        disabled: ["funkce TUV je vypnutá", ""],
        idle: ["bez požadavku", ""],
        switching_to_dhw: ["čeká po přepnutí na TUV", "warn"],
        switching_back: ["dobíhá návrat na CH", "warn"],
        heating: ["probíhá ohřev", "good"],
        anti_legionella: ["probíhá anti-legionella ohřev", "warn"],
        target_reached: ["cílová teplota dosažena", "good"],
        request_blocked_no_valid_temp: ["požadavek blokován: chybí platná teplota zásobníku", "bad"],
        boiler_dhw_active_ot: ["kotel hlásí aktivní TUV přes OpenTherm", "warn"],
      };
      const hit = map[s];
      return { raw:s, label: hit ? hit[0] : (s || "--"), kind: hit ? hit[1] : "", fault: /blocked_no_valid_temp/.test(s) };
    }

    function serviceIssueMeta(domain, key){
      const raw = String(key || "").toLowerCase();
      const none = { domain, key:"", label:"bez aktivní servisní události", kind:"", recommendation:"Bez zásahu.", counterLabel:"Bez událostí" };
      if(!raw) return none;
      const map = {
        mix_fault_no_feedback: { label:"Mix: chybí zpětná vazba", kind:"bad", recommendation:"Zkontrolovat čidlo zpátečky / teplotu za směšovačem, kabeláž a dostupnost hodnoty v diagnostice.", counterLabel:"Výpadek feedbacku mixu" },
        mix_fault_actuator_suspect: { label:"Mix: podezření na pohon nebo ventil", kind:"bad", recommendation:"Ověřit směr A/B, mechanický chod ventilu, relé R1/R2 a reálnou změnu teploty po pulzu.", counterLabel:"Podezření na pohon mixu" },
        mix_no_feedback_wait: { label:"Mix: čeká na návrat zpětné vazby", kind:"warn", recommendation:"Pokud stav trvá déle, zkontrolovat teplotní čidlo a mapování role Return.", counterLabel:"Čekání na feedback mixu" },
        mix_blocked_summer: { label:"Mix: blokováno letním režimem", kind:"warn", recommendation:"Běžný stav. Zkontrolovat prahy summer mode jen pokud blokace neodpovídá venkovní teplotě.", counterLabel:"Blokace summer mode" },
        mix_limit_a: { label:"Mix: dosažena krajní poloha A", kind:"warn", recommendation:"Pokud nedochází k dosažení cíle, ověřit hydrauliku a kalibraci krajních poloh.", counterLabel:"Mix na limitu A" },
        mix_limit_b: { label:"Mix: dosažena krajní poloha B", kind:"warn", recommendation:"Pokud nedochází k dosažení cíle, ověřit hydrauliku a kalibraci krajních poloh.", counterLabel:"Mix na limitu B" },
        dhw_request_blocked_no_valid_temp: { label:"TUV: chybí platná teplota zásobníku", kind:"bad", recommendation:"Zkontrolovat čidlo TUV, zdroj teploty a stáří hodnoty. Bez validní teploty je ohřev záměrně blokován.", counterLabel:"Blokace TUV bez teploty" },
        dhw_switching_to_dhw: { label:"TUV: probíhá přepnutí na ohřev", kind:"warn", recommendation:"Běžný přechodový stav. Zásah je potřeba jen pokud trvá neobvykle dlouho.", counterLabel:"Přechod na TUV" },
        dhw_switching_back_to_ch: { label:"TUV: návrat zpět na CH", kind:"warn", recommendation:"Běžný doběh po ohřevu. Zásah je potřeba jen pokud stav visí příliš dlouho.", counterLabel:"Návrat TUV na CH" },
        dhw_boiler_dhw_active_ot: { label:"TUV: kotel hlásí aktivní TUV přes OT", kind:"warn", recommendation:"Ověřit, zda kotel skutečně dokončuje TUV. Pokud stav neodpovídá realitě, prověřit interpretaci OT stavů.", counterLabel:"OT aktivní TUV" },
      };
      const meta = map[raw];
      return meta ? { domain, key:raw, ...meta } : { domain, key:raw, label: raw, kind:"warn", recommendation:"Prověřit detailní diagnostiku firmware a odpovídající akční členy/čidla.", counterLabel: raw };
    }

    function evaluateServiceIssues(){
      const mixState = String(state.eqStatus?.mix?.state || state.eqFast?.mix?.state || "idle").toLowerCase();
      const dhwReason = String(state.dhwStatus?.heatReason || "").toLowerCase();
      const dhwPhase = String(state.dhwStatus?.heatPhase || state.dhwFast?.hp || "idle").toLowerCase();

      let mixKey = "";
      if(["fault_no_feedback","fault_actuator_suspect","no_feedback_wait","blocked_summer","limit_a","limit_b"].includes(mixState)) mixKey = `mix_${mixState}`;

      let dhwKey = "";
      if(dhwReason === "request_blocked_no_valid_temp") dhwKey = `dhw_${dhwReason}`;
      else if(dhwReason === "boiler_dhw_active_ot") dhwKey = `dhw_${dhwReason}`;
      else if(["switching_to_dhw","switching_back_to_ch"].includes(dhwPhase)) dhwKey = `dhw_${dhwPhase}`;

      return { mixKey, dhwKey };
    }

    function observeServiceIssues(){
      state.service = state.service || loadServiceStats();
      const issues = evaluateServiceIssues();
      let changed = false;
      ["mixKey","dhwKey"].forEach((field) => {
        const nextKey = String(issues[field] || "");
        const prevKey = String(state.service.current?.[field] || "");
        if(nextKey && nextKey !== prevKey){
          state.service.counters[nextKey] = (Number(state.service.counters[nextKey]) || 0) + 1;
          state.service.lastChangeMs = Date.now();
          changed = true;
        }
        if(nextKey !== prevKey){
          state.service.current[field] = nextKey;
          changed = true;
        }
      });
      if(changed) saveServiceStats();
    }

    function serviceCounterValue(key){
      return Number(state.service?.counters?.[key]) || 0;
    }

    function renderServicePanel(){
      state.service = state.service || loadServiceStats();
      const issues = evaluateServiceIssues();
      const mixMeta = serviceIssueMeta("mix", issues.mixKey);
      const dhwMeta = serviceIssueMeta("dhw", issues.dhwKey);
      const overall = issues.mixKey ? mixMeta : (issues.dhwKey ? dhwMeta : serviceIssueMeta("all", ""));

      const mixBadgeText = issues.mixKey ? `servis mix: ${mixMeta.label}` : "servis mix: bez aktivní události";
      const dhwBadgeText = issues.dhwKey ? `servis TUV: ${dhwMeta.label}` : "servis TUV: bez aktivní události";
      setBadge("#mixServiceNow", mixMeta.kind, mixBadgeText);
      setBadge("#dhwServiceNow", dhwMeta.kind, dhwBadgeText);
      setBadge("#serviceOverviewBadge", overall.kind, issues.mixKey || issues.dhwKey ? `servis: ${overall.label}` : "servis: bez aktivní události");

      setText("#mixServiceAdvice", mixMeta.recommendation);
      setText("#dhwServiceAdvice", dhwMeta.recommendation);
      setText("#serviceLastChange", state.service.lastChangeMs ? new Date(state.service.lastChangeMs).toLocaleString("cs-CZ") : "--");

      const rows = [
        ["mix_fault_no_feedback", serviceIssueMeta("mix", "mix_fault_no_feedback")],
        ["mix_fault_actuator_suspect", serviceIssueMeta("mix", "mix_fault_actuator_suspect")],
        ["mix_no_feedback_wait", serviceIssueMeta("mix", "mix_no_feedback_wait")],
        ["dhw_request_blocked_no_valid_temp", serviceIssueMeta("dhw", "dhw_request_blocked_no_valid_temp")],
        ["dhw_switching_to_dhw", serviceIssueMeta("dhw", "dhw_switching_to_dhw")],
        ["dhw_switching_back_to_ch", serviceIssueMeta("dhw", "dhw_switching_back_to_ch")],
      ];
      const tbody = document.getElementById("serviceCounterTbl");
      if(tbody){
        tbody.innerHTML = "";
        rows.forEach(([key, meta]) => {
          const tr = document.createElement("tr");
          tr.innerHTML = `<td>${escapeHtml(meta.counterLabel)}</td><td>${serviceCounterValue(key)}</td><td>${escapeHtml(meta.recommendation)}</td>`;
          tbody.appendChild(tr);
        });
      }
    }

    function resetServiceCounters(){
      state.service = { current:{ mixKey:"", dhwKey:"" }, counters:{}, lastChangeMs:0 };
      saveServiceStats();
      renderServicePanel();
      toast("Servisní počitadla", "Počitadla fault stavů byla vynulována pouze v prohlížeči.", "🧹");
    }

    function renderMixBadge(){
      const mix = state.eqStatus?.mix || state.eqFast?.mix || {};
      const meta = mixStateMeta(mix.state || state.eqFast?.mix?.state || "idle");
      const manual = !!mix.manual || !!state.eqFast?.mix?.manual;
      const remainingMs = firstFinite(mix.pulseRemainingMs, state.eqFast?.mix?.prm);
      const elapsedMs = firstFinite(mix.pulseElapsedMs, state.eqFast?.mix?.elp);
      let label = `mix: ${meta.label}`;
      if(manual && meta.raw.indexOf("manual") < 0) label += " • ruční zásah";
      if(Number.isFinite(remainingMs) && remainingMs > 0) label += ` • zbývá ${Math.round(remainingMs)} ms`;
      else if(Number.isFinite(elapsedMs) && elapsedMs > 0 && /manual|open|close/.test(meta.raw)) label += ` • ${Math.round(elapsedMs)} ms`;
      setBadge("#hMixState", meta.kind, label);
      const warn = $("#hMixWarn");
      if(warn){
        if(meta.fault) setBadge("#hMixWarn", "bad", `varování: ${meta.label}`);
        else if(meta.blocked) setBadge("#hMixWarn", "warn", `omezení: ${meta.label}`);
        else setBadge("#hMixWarn", "", "varování: žádné");
      }
      renderMixCalibrationInfo();
    }

    function renderMixCalibrationInfo(){
      const el = document.getElementById("hMixDiag");
      if(!el) return;
      const mix = state.eqStatus?.mix || state.eqFast?.mix || {};
      const meta = mixStateMeta(mix.state || state.eqFast?.mix?.state || "idle");
      const parts = [`stav ${meta.label}`];
      const pct = firstFinite(mix.pct, state.eqFast?.mix?.pct, state.accu?.valve);
      const feedbackC = firstFinite(state.eqStatus?.temps?.mixFeedbackC, state.eqFast?.mf, state.accu?.after, state.ot?.returnTempC);
      const lastPulseMs = firstFinite(mix.lastPulseMs, state.eqStatus?.mix?.lastPulseMs);
      const remainingMs = firstFinite(mix.pulseRemainingMs, state.eqFast?.mix?.prm);
      const elapsedMs = firstFinite(mix.pulseElapsedMs, state.eqFast?.mix?.elp);
      const calibration = String(mix.calibration || state.eqStatus?.mix?.calibration || "");
      if(Number.isFinite(pct)) parts.push(`odhad ${fmtNum(pct, 0)} %`);
      if(Number.isFinite(feedbackC)) parts.push(`feedback ${fmtNum(feedbackC, 1)} °C`);
      if(Number.isFinite(lastPulseMs) && lastPulseMs > 0) parts.push(`poslední puls ${Math.round(lastPulseMs)} ms`);
      if(Number.isFinite(remainingMs) && remainingMs > 0) parts.push(`zbývá ${Math.round(remainingMs)} ms`);
      else if(Number.isFinite(elapsedMs) && elapsedMs > 0 && /manual|open|close/.test(meta.raw)) parts.push(`běží ${Math.round(elapsedMs)} ms`);
      if(calibration) parts.push(mixCalibrationLabel(calibration));
      if(meta.raw && meta.raw !== meta.label) parts.push(`raw ${meta.raw}`);
      el.textContent = `Stav ventilu: ${parts.join(" • ")}`;
    }

    function getHeatingConfigFromRoot(cfg){
      if(!cfg || typeof cfg !== "object") return null;
      return cfg.equitherm || null;
    }

    function clampHeatingLimitsToOt(limitsLike){
      const rawMin = Number(limitsLike?.minFlowC);
      const rawMax = Number(limitsLike?.maxFlowC);
      let minFlowC = Number.isFinite(rawMin) ? rawMin : 25;
      let maxFlowC = Number.isFinite(rawMax) ? rawMax : 60;
      const otBoundLo = Number(state.ot?.maxChBoundMinC);
      const otBoundHi = Number(state.ot?.maxChBoundMaxC);
      if(Number.isFinite(otBoundLo)) minFlowC = Math.max(minFlowC, otBoundLo);
      if(Number.isFinite(otBoundHi)) minFlowC = Math.min(minFlowC, otBoundHi);
      if(Number.isFinite(otBoundLo)) maxFlowC = Math.max(maxFlowC, otBoundLo);
      if(Number.isFinite(otBoundHi)) maxFlowC = Math.min(maxFlowC, otBoundHi);
      if(maxFlowC < minFlowC) maxFlowC = minFlowC;
      return { minFlowC, maxFlowC };
    }

    async function heatingReloadConfigFromDevice(){
      // Heavier config handlers are more reliable sequentially on the device web server.
      const eqCfg = await api.fetchConfigSection("equitherm");
      const otCfg = await api.fetchConfigSection("opentherm");
      if(eqCfg){
        state.dev = state.dev || {};
        state.dev.eqCfgLoaded = true;
        applyEqConfigToForm(eqCfg, { force:true });
        setEqConfigDirty(false);
      }
      if(otCfg){
        state.dev = state.dev || {};
        state.dev.otCfgLoaded = true;
        applyOtConfigToForm(otCfg);
      }
      await refresh(false);
    }

    function renderHeatingOtInfo(){
      const maxEl = document.getElementById("hOtMaxBounds");
      const dhwEl = document.getElementById("hOtDhwBounds");
      const reqEl = document.getElementById("hOtReqInfo");
      const chBoundLo = Number(state.ot?.maxChBoundMinC);
      const chBoundHi = Number(state.ot?.maxChBoundMaxC);
      const dhwBoundLo = Number(state.ot?.dhwBoundMinC);
      const dhwBoundHi = Number(state.ot?.dhwBoundMaxC);
      const chCurrent = Number(state.ot?.maxChSetpointC);
      const dhwCurrent = Number(state.ot?.dhwSetpointC);
      const boilerMaxInput = document.getElementById("hBoilerMax");
      const minFlowInput = document.getElementById("hMin");
      const maxFlowInput = document.getElementById("hMax");

      if(maxEl){
        if(Number.isFinite(chBoundLo) || Number.isFinite(chBoundHi) || Number.isFinite(chCurrent)){
          const boundsTxt = (Number.isFinite(chBoundLo) || Number.isFinite(chBoundHi))
            ? `${fmtNum(chBoundLo, 1)}–${fmtNum(chBoundHi, 1)} °C`
            : "--";
          let txt = `OT Max CH (ID57): ${boundsTxt}`;
          if(Number.isFinite(chCurrent)) txt += ` • aktuálně ${fmtNum(chCurrent, 1)} °C`;
          maxEl.textContent = txt;
        }else{
          maxEl.textContent = "OT Max CH (ID57): --";
        }
      }

      if(dhwEl){
        if(Number.isFinite(dhwBoundLo) || Number.isFinite(dhwBoundHi) || Number.isFinite(dhwCurrent)){
          const boundsTxt = (Number.isFinite(dhwBoundLo) || Number.isFinite(dhwBoundHi))
            ? `${fmtNum(dhwBoundLo, 1)}–${fmtNum(dhwBoundHi, 1)} °C`
            : "--";
          let txt = `OT TUV (ID56): ${boundsTxt}`;
          if(Number.isFinite(dhwCurrent)) txt += ` • aktuálně ${fmtNum(dhwCurrent, 1)} °C`;
          dhwEl.textContent = txt;
        }else{
          dhwEl.textContent = "OT TUV (ID56): --";
        }
      }

      if(reqEl){
        const reqParts = [];
        const reqCh = firstFinite(state.ot?.reqWaterTempC, state.ot?.chSet);
        const reqDhw = firstFinite(state.ot?.reqDhwSetpointC, state.ot?.dhwSetpointC);
        if(Number.isFinite(reqCh)) reqParts.push(`CH ${fmtNum(reqCh, 1)} °C`);
        if(Number.isFinite(reqDhw)) reqParts.push(`TUV ${fmtNum(reqDhw, 1)} °C`);
        if(Number.isFinite(Number(state.ot?.modulationPct))) reqParts.push(`mod ${fmtNum(state.ot.modulationPct, 0)} %`);
        if(state.ot?.reason) reqParts.push(`stav ${state.ot.reason}`);
        reqEl.textContent = reqParts.length ? `OT požadavky: ${reqParts.join(" • ")}` : "OT požadavky: --";
      }

      if(boilerMaxInput){
        if(Number.isFinite(chBoundLo)) boilerMaxInput.min = String(chBoundLo);
        else boilerMaxInput.removeAttribute("min");
        if(Number.isFinite(chBoundHi)) boilerMaxInput.max = String(chBoundHi);
        else boilerMaxInput.removeAttribute("max");
        const titleParts = [];
        if(Number.isFinite(chBoundLo) || Number.isFinite(chBoundHi)) titleParts.push(`Povolený rozsah kotle: ${fmtNum(chBoundLo, 1)} až ${fmtNum(chBoundHi, 1)} °C`);
        if(Number.isFinite(chCurrent)) titleParts.push(`Aktuální ID57: ${fmtNum(chCurrent, 1)} °C`);
        boilerMaxInput.title = titleParts.join(" • ");
      }

      [minFlowInput, maxFlowInput].forEach((inputEl) => {
        if(!inputEl) return;
        if(Number.isFinite(chBoundLo)) inputEl.min = String(chBoundLo);
        else inputEl.removeAttribute("min");
        if(Number.isFinite(chBoundHi)) inputEl.max = String(chBoundHi);
        else inputEl.removeAttribute("max");
        const titleParts = [];
        if(Number.isFinite(chBoundLo) || Number.isFinite(chBoundHi)) titleParts.push(`OT rozsah topné vody: ${fmtNum(chBoundLo, 1)} až ${fmtNum(chBoundHi, 1)} °C`);
        if(Number.isFinite(chCurrent)) titleParts.push(`Aktuální Max CH kotle: ${fmtNum(chCurrent, 1)} °C`);
        inputEl.title = titleParts.join(" • ");
      });
    }

    function buildWsUrl(){
      try{
        const base = normalizedApiBase();
        const u = new URL(base || window.location.href, window.location.href);
        u.protocol = (u.protocol === "https:") ? "wss:" : "ws:";
        // Firmware uses WebSocketsServer on dedicated port 81, not HTTP path /ws.
        u.port = "81";
        u.pathname = "/";
        u.search = "";
        u.hash = "";
        return u.toString();
      }catch(e){
        log("ws base parse error: " + (e.message || e));
      }
      const proto = (window.location.protocol === "https:") ? "wss:" : "ws:";
      const host = window.location.hostname || window.location.host;
      return `${proto}//${host}:81/`;
    }

    function stopFallbackPolling(){
      if(timer){
        clearInterval(timer);
        timer = null;
      }
    }

    function startFallbackPolling(intervalMs=10000){
      const safeMs = Math.max(3000, Number(intervalMs) || 10000);
      stopFallbackPolling();
      timer = setInterval(() => refresh(false), safeMs);
    }

    function closeWs(){
      const ws = ensureWsState();
      if(ws.retryTimer){
        clearTimeout(ws.retryTimer);
      }
      const sock = ws.sock;
      if(sock){
        try{
          ws.manualClose = true;
          sock.onopen = null;
          sock.onmessage = null;
          sock.onerror = null;
          sock.onclose = null;
          if(sock.readyState === WebSocket.OPEN || sock.readyState === WebSocket.CONNECTING){
            sock.close();
          }
        }catch(e){}
      }
      state.ws = {
        sock:null,
        připojeno:false,
        retryTimer:null,
        reconnectMs:Number(ws.reconnectMs || 2000),
        manualClose:false,
        failCount:Number(ws.failCount || 0),
        disabledUntilMs:Number(ws.disabledUntilMs || 0),
      };
    }

    function scheduleWsReconnect(){
      const ws = ensureWsState();
      if(ws.manualClose) return;
      if(wsReconnectPaused()) return;
      if(ws.retryTimer) return;
      const delay = Math.max(1000, Math.min(15000, Number(ws.reconnectMs || 2000)));
      ws.retryTimer = setTimeout(() => {
        ws.retryTimer = null;
        connectWs();
      }, delay);
      ws.reconnectMs = Math.min(delay * 2, 15000);
    }

    function connectWs(){
      const ws = ensureWsState();
      const existing = ws.sock;
      if(existing){
        if(existing.readyState === WebSocket.OPEN || existing.readyState === WebSocket.CONNECTING) return;
        if(existing.readyState === WebSocket.CLOSING) return;
      }
      if(wsReconnectPaused()){
        setApiHealth("warn", "API: polling (WS pauza)");
        updateRefreshCadence();
        return;
      }
      ws.manualClose = false;
      let sock = null;
      try{
        sock = new WebSocket(buildWsUrl());
      }catch(e){
        log("ws open error: " + (e.message || e));
        setApiHealth("warn", "API: WS chyba");
        startFallbackPolling(computeOfflinePollMs());
        ws.failCount = Math.min(10, Number(ws.failCount || 0) + 1);
        if(ws.failCount >= 6) wsPauseReconnect(300000, "too_many_open_failures");
        else scheduleWsReconnect();
        return;
      }

      ws.sock = sock;
      ws.připojeno = false;
      setApiHealth("warn", "API: WS připojuji…");

      sock.onopen = () => {
        const live = ensureWsState();
        live.připojeno = true;
        live.reconnectMs = 2000;
        live.failCount = 0;
        live.disabledUntilMs = 0;
        updateRefreshCadence();
        setApiHealth("good", "API: WebSocket");
        log("ws připojeno");
      };

      sock.onmessage = (ev) => {
        try{
          const msg = JSON.parse(ev.data);
          const fast = normalizeFastMessage(msg);
          if(!fast || typeof fast !== "object") return;
          applyFastSnapshot(fast);
          setApiHealth("good", "API: WebSocket");
        }catch(e){
          log("ws message error: " + (e.message || e));
        }
      };

      sock.onerror = () => {
        setApiHealth("warn", "API: WS chyba");
        if(maybeAdoptPageOriginBase("ws", true)){
          try{ sock.close(); }catch(e){}
        }
      };

      sock.onclose = () => {
        const live = ensureWsState();
        const manual = !!live.manualClose;
        live.připojeno = false;
        live.sock = null;
        if(manual) return;
        live.failCount = Math.min(10, Number(live.failCount || 0) + 1);
        setApiHealth("warn", "API: polling");
        log(`ws dispřipojeno (${live.failCount})`);
        startFallbackPolling(computeOfflinePollMs());
        if(live.failCount >= 6) wsPauseReconnect(300000, "too_many_failures");
        else scheduleWsReconnect();
      };
    }

  // ----- OpenTherm advanced tools (scan + Data-ID RW)
state.otAdv = state.otAdv || { scan: null, profile: null, last: null, loading:false, lastRefreshMs:0 };

function otSetBadge(id, kind, text){
  const el = document.getElementById(id);
  if(!el) return;
  el.classList.remove("good","warn","bad");
  if(kind) el.classList.add(kind);
  el.childNodes.forEach(n => { if(n.nodeType===3) n.remove(); });
  el.appendChild(document.createTextNode(" " + text));
}

function otFmtVal(it){
  if(!it) return "--";
  if(it.f88 != null && isFinite(Number(it.f88))) return Number(it.f88).toFixed(2) + (it.unit ? " " + it.unit : "");
  if(it.s16 != null && isFinite(Number(it.s16))) return String(it.s16) + (it.unit ? " " + it.unit : "");
  if(it.upper != null || it.lower != null) return `${it.upper ?? "--"}/${it.lower ?? "--"}${it.unit ? " " + it.unit : ""}`;
  if(it.val != null) return "raw=" + it.val;
  return "--";
}

function otFmtRW(it){
  // very light hint based on meta info
  const id = Number(it?.id);
  if(!Number.isFinite(id)) return "--";
  // heuristics: common read-only IDs are most of them; keep simple
  // If scan has rwMask (ID6) we could decode, but that's beyond scope here.
  return (id === 1 || id === 56 || id === 57) ? "R/W" : "R";
}

function otProfileSupportedSet(profileJson){
  const items = Array.isArray(profileJson?.scan?.items) ? profileJson.scan.items : [];
  const set = new Set();
  items.forEach(it => { if(it && it.supported) set.add(Number(it.id)); });
  return set;
}

function otApplyProfileVisibility(profileJson){
  const hasProfile = !!profileJson?.exists && Array.isArray(profileJson?.scan?.items);
  otSetBadge("otProfileBadge", hasProfile ? "good" : "", hasProfile ? `profil: ${Number(profileJson?.scan?.supportedCount ?? 0)} ID` : "profil: není");
  const supported = otProfileSupportedSet(profileJson);
  document.querySelectorAll("[data-ot-id]").forEach(el => {
    const id = Number(el.getAttribute("data-ot-id"));
    if(!hasProfile || !Number.isFinite(id)){
      el.hidden = false;
      return;
    }
    el.hidden = !(supported.has(id) || id === 0 || id === 3);
  });
}

async function otProfileRefresh(){
  try{
    const j = await api.getJson("/api/opentherm/scan/profile");
    state.otAdv.profile = j;
    otApplyProfileVisibility(j);
  }catch(e){
    state.otAdv.profile = { ok:false, exists:false };
    otApplyProfileVisibility(state.otAdv.profile);
    log("otProfileRefresh error: " + (e.message || e));
  }
}

function otRenderScan(scanJson){
  const badge = document.getElementById("otScanBadge");
  const tbl = document.getElementById("otScanTbl");
  if(!tbl) return;

  const scan = scanJson?.scan || {};
  const active = !!scan.active;
  const done = !!scan.done;
  const cur = Number(scan.curId ?? 0);
  const end = Number(scan.endId ?? 127);
  const sup = Number(scan.supportedCount ?? 0);
  const stateTxt = active ? `běží (${cur}/${end})` : (done ? `hotovo (supported: ${sup})` : "neaktivní");
  otSetBadge("otScanBadge", active ? "warn" : (done ? "good" : ""), "scan: " + stateTxt);

  const showAll = !!document.getElementById("otScanShowAll")?.checked;
  const items = Array.isArray(scan.items) ? scan.items : [];
  const view = showAll ? items : items.filter(x => !!x.supported);

  if(!view.length){
    tbl.innerHTML = `<tr><td colspan="7" class="muted">Žádná data. Klikni „Start“ nebo „Načíst stav“.</td></tr>`;
    return;
  }

  tbl.innerHTML = view.map(it => {
    const sup2 = it.supported ? '<span class="badge good"><span class="b"></span>ANO</span>' : '<span class="badge"><span class="b"></span>NE</span>';
    const rs = escapeHtml(String(it.rs ?? "--"));
    const typ = it.isTemp ? "temp" : (it.unit ? "val" : "--");
    return `<tr>
      <td class="mono">${escapeHtml(it.id)}</td>
      <td><strong>${escapeHtml(it.name || ("ID"+it.id))}</strong><div class="muted">${escapeHtml(it.desc || "")}</div></td>
      <td class="mono">${escapeHtml(otFmtRW(it))}</td>
      <td class="mono">${escapeHtml(typ)}</td>
      <td class="mono">${escapeHtml(otFmtVal(it))}</td>
      <td class="mono">${rs}</td>
      <td>${sup2}</td>
    </tr>`;
  }).join("");
}

async function otScanRefresh(force=false){
  const now = Date.now();
  if(state.otAdv?.loading) return;
  if(!force && (now - Number(state.otAdv?.lastRefreshMs || 0)) < 4000) return;
  state.otAdv.loading = true;
  try{
    const j = await api.getJson("/api/opentherm/scan/status", 4500);
    state.otAdv.scan = j;
    state.otAdv.lastRefreshMs = Date.now();
    otRenderScan(j);
    if(j?.scan?.profileSaved || j?.scan?.done) await otProfileRefresh();
  }catch(e){
    otSetBadge("otScanBadge", "bad", "scan: chyba");
    toast("OpenTherm scan", e.message || String(e), "⚠");
    log("otScanRefresh error: " + (e.message || e));
  }finally{
    state.otAdv.loading = false;
  }
}

async function otScanStart(){
  try{
    const includeAll = !!document.getElementById("otScanIncludeAll")?.checked;
    const delayMs = clamp(Number(document.getElementById("otScanDelay")?.value ?? 60), 10, 500);
    await api.postJson("/api/opentherm/scan/start", { includeAll, delayMs });
    toast("OpenTherm scan", "Start.", "✅");
    await otScanRefresh();
  }catch(e){
    toast("OpenTherm scan", e.message || String(e), "⚠");
    log("otScanStart error: " + (e.message || e));
  }
}

async function otScanStop(){
  try{
    await api.postJson("/api/opentherm/scan/stop", {});
    toast("OpenTherm scan", "Stop.", "🛑");
    await otScanRefresh();
  }catch(e){
    toast("OpenTherm scan", e.message || String(e), "⚠");
    log("otScanStop error: " + (e.message || e));
  }
}

function otRwOut(obj){
  const pre = document.getElementById("otRwOut");
  if(pre) pre.textContent = JSON.stringify(obj ?? {}, null, 2);
}

function parseIntAny(s){
  const t = String(s ?? "").trim();
  if(!t) return NaN;
  if(/^0x/i.test(t)) return parseInt(t, 16);
  return parseInt(t, 10);
}

async function otRwRead(){
  try{
    const id = clamp(Number(document.getElementById("otRwId")?.value ?? 0), 0, 127);
    const reqValue = clamp(Number(document.getElementById("otRwReq")?.value ?? 0), 0, 65535);
    otSetBadge("otRwBadge", "warn", "RW: čtu…");
    const j = await api.postJson("/api/opentherm/dataid/read", { id, reqValue });
    otRwOut(j);
    otSetBadge("otRwBadge", (j?.ok ? "good" : "bad"), "RW: čtení " + (j?.ok ? "OK" : "ERR"));
  }catch(e){
    otSetBadge("otRwBadge", "bad", "RW: chyba");
    toast("OpenTherm read", e.message || String(e), "⚠");
    log("otRwRead error: " + (e.message || e));
  }
}

async function otRwWrite(){
  try{
    const id = clamp(Number(document.getElementById("otRwId")?.value ?? 0), 0, 127);
    const fmt = document.getElementById("otRwFmt")?.value || "f88";
    const valRaw = String(document.getElementById("otRwVal")?.value ?? "").trim();
    const payload = { id };
    if(fmt === "f88"){
      const v = Number(valRaw);
      if(!Number.isFinite(v)) throw new Error("Neplatná hodnota (float).");
      payload.valueF88 = v;
    }else if(fmt === "raw"){
      const v = parseIntAny(valRaw);
      if(!Number.isFinite(v)) throw new Error("Neplatná hodnota (raw).");
      payload.valueRaw = clamp(v, 0, 65535);
    }else{
      // hb/lb: accept "HB,LB" or "HB LB"
      const parts = valRaw.split(/[\s,;]+/).filter(Boolean);
      if(parts.length < 2) throw new Error("Zadej HB a LB (např. 0x3C 0x00).");
      const hb = clamp(parseIntAny(parts[0]), 0, 255);
      const lb = clamp(parseIntAny(parts[1]), 0, 255);
      payload.hb = hb; payload.lb = lb;
    }

    otSetBadge("otRwBadge", "warn", "RW: zapisuji…");
    const j = await api.postJson("/api/opentherm/dataid/write", payload);
    otRwOut(j);
    otSetBadge("otRwBadge", (j?.ok ? "good" : "bad"), "RW: zápis " + (j?.ok ? "OK" : "ERR"));
    // refresh fast snapshot soon
    refresh(false);
  }catch(e){
    otSetBadge("otRwBadge", "bad", "RW: chyba");
    toast("OpenTherm write", e.message || String(e), "⚠");
    log("otRwWrite error: " + (e.message || e));
  }
}



    // ----- Rendering
    function fmtTs(ts){ return new Date(ts).toLocaleTimeString("cs-CZ"); }
    function fmtMaybeNumber(v, digits=1, suffix=""){
      const n = Number(v);
      return Number.isFinite(n) ? n.toFixed(digits) + suffix : "--";
    }

    function mergeFastSnapshot(base, patch){
      const out = Object.assign({}, base || {});
      if(!patch || typeof patch !== "object") return out;
      for(const [k, v] of Object.entries(patch)){
        if(v && typeof v === "object" && !Array.isArray(v)){
          out[k] = Object.assign({}, (out[k] && typeof out[k] === "object") ? out[k] : {}, v);
        }else{
          out[k] = v;
        }
      }
      return out;
    }

    function normalizeFastMessage(msg){
      if(!msg || typeof msg !== "object") return null;
      if(msg.fast && typeof msg.fast === "object") return msg.fast;
      if(msg.type === "fast_full" && msg.data && typeof msg.data === "object") return msg.data;
      if(msg.type === "fast_patch" && msg.changed && typeof msg.changed === "object"){
        state.fast = mergeFastSnapshot(state.fast, msg.changed);
        return state.fast;
      }
      if(msg.ot || msg.eq || msg.dhw || msg.temps || msg.rel || msg.in) return msg;
      if(msg.data && typeof msg.data === "object") return msg.data;
      return null;
    }

    function syncOtDerived(){
      if(Number.isFinite(Number(state.ot.maxCapacityKw)) && Number.isFinite(Number(state.ot.modulationPct))){
        state.ot.currentPowerKw = Number(state.ot.maxCapacityKw) * Number(state.ot.modulationPct) / 100;
      }else if(!Number.isFinite(Number(state.ot.currentPowerKw))){
        state.ot.currentPowerKw = NaN;
      }
      if(!Number.isFinite(Number(state.ot.reqWaterTempC)) && Number.isFinite(Number(state.eqFast?.tf))){
        state.ot.reqWaterTempC = Number(state.eqFast.tf);
      }
    }

    function otMergeFieldLines(fields){
      const order = [
        ["chEnable", "CH enable"],
        ["dhwEnable", "TUV enable"],
        ["chSetpointC", "CH setpoint"],
        ["dhwSetpointC", "TUV setpoint"],
        ["maxModulationPct", "Max modulace"],
      ];
      const out = [];
      for(const [key, label] of order){
        const f = fields?.[key] || {};
        if(!f.set) continue;
        let v = f.value;
        if(v === null || v === undefined) v = "null";
        else if(typeof v === "boolean") v = v ? "true" : "false";
        else if(typeof v === "number") v = Number.isFinite(v) ? String(Number(v)) : "NaN";
        else v = String(v);
        out.push(`${label}: ${v}`);
      }
      return out.length ? out.join("\n") : "(žádné přepsání)";
    }

    function otEffectiveLines(eff){
      if(!eff || typeof eff !== "object") return "--";
      const out = [
        `Zdroj: ${String(eff.activeSource || "--")}`,
        `CH enable: ${eff.chEnable ? "true" : "false"}`,
        `TUV enable: ${eff.dhwEnable ? "true" : "false"}`,
      ];
      if(eff.chSetpointC != null) out.push(`CH setpoint: ${Number(eff.chSetpointC)} °C`);
      if(eff.dhwSetpointC != null) out.push(`TUV setpoint: ${Number(eff.dhwSetpointC)} °C`);
      if(eff.maxModulationPct != null) out.push(`Max modulace: ${Number(eff.maxModulationPct)} %`);
      return out.join("\n");
    }

    function renderOtMerge(merge){
      state.ot = state.ot || {};
      state.ot.merge = merge || null;
      const manual = merge?.manual || null;
      const eq = merge?.equitherm || null;
      const dhw = merge?.dhw || null;
      const eff = merge?.effective || null;

      setText("#otMergeManualState", manual ? (manual.active ? "active" : "neaktivní") : "--");
      setText("#otMergeEqState", eq ? (eq.active ? "active" : "neaktivní") : "--");
      setText("#otMergeDhwState", dhw ? (dhw.active ? "active" : "neaktivní") : "--");
      setText("#otMergeEffectiveState", eff ? String((eff.activeSource === "dhw" ? "tuv" : eff.activeSource === "equitherm" ? "ekviterm" : eff.activeSource) || "--") : "--");
      setText("#otMergeManualOut", manual ? otMergeFieldLines(manual.fields) : "--");
      setText("#otMergeEqOut", eq ? otMergeFieldLines(eq.fields) : "--");
      setText("#otMergeDhwOut", dhw ? otMergeFieldLines(dhw.fields) : "--");
      setText("#otMergeEffectiveOut", eff ? otEffectiveLines(eff) : "--");

      let active = "--";
      let cls = "";
      if(eff?.activeSource){
        active = String(eff.activeSource);
        cls = active === "dhw" ? "warn" : (active === "equitherm" || active === "manual") ? "good" : "";
      }
      setBadge("#otMergeBadge", cls, `merge: ${active === "dhw" ? "tuv" : active === "equitherm" ? "ekviterm" : active}`);
    }

    function applyOtStatus(status){
      if(!status || typeof status !== "object") return;
      state.ot.present = readMaybeBool(status, "present", state.ot.present);
      const enabledFromStatus = readMaybeBool(status, "enabled", state.ot.enabled);
      state.ot.enabled = enabledFromStatus || !!state.ot.cfg?.enabled || !!state.ot.cfg?.enable;
      state.ot.ready = readMaybeBool(status, "ready", state.ot.ready);
      state.ot.fault = readMaybeBool(status, "fault", state.ot.fault);
      state.ot.comm = !!state.ot.present && !!state.ot.ready && !state.ot.fault;
      state.ot.chSet = readMaybeNumber(status, "reqChSetpointC", state.ot.chSet);
      state.ot.reqWaterTempC = hasOwn(status, "reqChSetpointC")
        ? numOrNaN(status.reqChSetpointC)
        : state.ot.reqWaterTempC;
      if(!Number.isFinite(state.ot.reqWaterTempC)) state.ot.reqWaterTempC = firstFinite(state.eqFast?.tf, state.ot.reqWaterTempC);
      state.ot.chTemp = readMaybeNumber(status, "boilerTempC", state.ot.chTemp);
      state.ot.returnTempC = readMaybeNumber(status, "returnTempC", state.ot.returnTempC);
      state.ot.dhwTemp = readMaybeNumber(status, "dhwTempC", state.ot.dhwTemp);
      state.ot.outsideTempC = readMaybeNumber(status, "outsideTempC", state.ot.outsideTempC);
      state.ot.pressure = readMaybeNumber(status, "pressureBar", state.ot.pressure);
      state.ot.modulationPct = readMaybeNumber(status, "modulationPct", state.ot.modulationPct);
      state.ot.maxChSetpointC = readMaybeNumber(status, "maxChSetpointC", state.ot.maxChSetpointC);
      state.ot.maxChBoundMinC = readMaybeNumber(status, "maxChBoundMinC", state.ot.maxChBoundMinC);
      state.ot.maxChBoundMaxC = readMaybeNumber(status, "maxChBoundMaxC", state.ot.maxChBoundMaxC);
      state.ot.dhwSetpointC = readMaybeNumber(status, "dhwSetpointC", state.ot.dhwSetpointC);
      state.ot.dhwBoundMinC = readMaybeNumber(status, "dhwBoundMinC", state.ot.dhwBoundMinC);
      state.ot.dhwBoundMaxC = readMaybeNumber(status, "dhwBoundMaxC", state.ot.dhwBoundMaxC);
      state.ot.reqDhwSetpointC = readMaybeNumber(status, "reqDhwSetpointC", state.ot.reqDhwSetpointC);
      if(hasOwn(status, "faultFlags")) state.ot.faultFlags = Number(status.faultFlags) || 0;
      if(hasOwn(status, "oemFaultCode")) state.ot.oemFaultCode = Number(status.oemFaultCode) || 0;
      state.ot.reason = readMaybeString(status, "reason", state.ot.reason);
      state.ot.lastCmd = readMaybeString(status, "lastCmd", state.ot.lastCmd);
      state.ot.statusRaw = Number(status.statusRaw || state.ot.statusRaw || 0);
      state.ot.dhwActive = readMaybeBool(status, "dhwActive", !!state.ot.dhwActive);
      state.ot.flameOn = readMaybeBool(status, "flameOn", !!state.ot.flameOn);
      state.ot.chActive = readMaybeBool(status, "chActive", !!state.ot.chActive);
      renderOtMerge(status.merge || null);
      syncOtDerived();
      renderHeatingOtInfo();
    }

    function setText(id, v){ const el = $(id); if(el) el.textContent = v; }
    function setBadge(id, kind, text){
      const el = $(id);
      if(!el) return;
      el.classList.remove("good","warn","bad");
      if(kind) el.classList.add(kind);
      el.querySelector?.(".b"); // keep dot
      el.lastChild && (el.lastChild.textContent = "");
      el.childNodes.forEach(n => {
        if(n.nodeType === 3) n.remove();
      });
      // ensure text node at end
      el.appendChild(document.createTextNode(" " + text));
    }


    // ----- Firmware adapter helpers (this repo)
    function setInputNumber(id, v, digits=1){
      const el = document.getElementById(id);
      if(!el) return;
      const n = Number(v);
      if(!Number.isFinite(n)) return;
      el.value = n.toFixed(digits);
    }
    function setInputBool(id, v){
      const el = document.getElementById(id);
      if(!el) return;
      el.checked = !!v;
    }

    function getEqChartConfig(){
      const deviceCfg = buildEqChartConfigFromSource(state.dev?.eqCfgRaw || state.dev?.eqCfg || null, {
        dayCurve: { slope: 1.0, shift: 0 },
        nightCurve: { slope: 0.7, shift: -5 },
        minFlowC: 25,
        maxFlowC: 60,
      });

      const formCfg = buildEqChartConfigFromSource({
        day: {
          slope: Number($("#hDaySlope")?.value),
          shift: Number($("#hDayShift")?.value),
        },
        night: {
          slope: Number($("#hNightSlope")?.value),
          shift: Number($("#hNightShift")?.value),
        },
        limits: {
          minFlowC: Number($("#hMin")?.value),
          maxFlowC: Number($("#hMax")?.value),
        },
      }, deviceCfg || {
        dayCurve: { slope: 1.0, shift: 0 },
        nightCurve: { slope: 0.7, shift: -5 },
        minFlowC: 25,
        maxFlowC: 60,
      });

      return formCfg || deviceCfg || {
        dayCurve: { slope: 1.0, shift: 0 },
        nightCurve: { slope: 0.7, shift: -5 },
        minFlowC: 25,
        maxFlowC: 60,
      };
    }

    function eqConfigInputIds(){
      return [
        "hDaySlope","hDayShift","hNightSlope","hNightShift","hMin","hMax",
        "hWrite57","hBoilerMax","hMixEnabled","hMixDeadband","hMixTargetOffsetC",
        "hMixPulseMs","hMixMinIntervalMs","hMixTravelMs","hEqModeCfg","hUseIn1NightOverride",
        "hSummerModeEnabled","hSummerOffAboveC","hSummerOnBelowC","hDriveNightRelay","hNightRelay",
        "hNightRelayOnWhenNight","hBoilerAssistEnabled","hBoilerAssistDeltaC","hBoilerAssistForceChEnable"
      ];
    }

    function setEqConfigDirty(v){
      state.ui = state.ui || {};
      state.ui.eqConfigDirty = !!v;
    }

    function applyEqConfigToForm(cfg, options={}){
      if(!cfg) return;
      const force = !!options.force;
      if(state.ui?.eqConfigDirty && !force) return;
      state.dev = state.dev || {};
      state.dev.eqCfgRaw = cfg;
      state.dev.eqUsesOpenTherm = !!(cfg?.output?.useOpenTherm ?? state.dev.eqUsesOpenTherm ?? true);
      state.dev.eqCfg = buildEqChartConfigFromSource(cfg, {
        dayCurve: { slope: 1.0, shift: 0 },
        nightCurve: { slope: 0.7, shift: -5 },
        minFlowC: 25,
        maxFlowC: 60,
      }) || state.dev.eqCfg || null;
      // curve params (slope + shift), derived from firmware point config or read directly
      const dayCurve = normalizeEqCurve(cfg?.day, state.dev.eqCfg?.dayCurve);
      const nightCurve = normalizeEqCurve(cfg?.night, state.dev.eqCfg?.nightCurve);
      const limits = normalizeEqLimits(cfg?.limits, state.dev.eqCfg);
      setInputNumber("hDaySlope", dayCurve.slope, 2);
      setInputNumber("hDayShift", dayCurve.shift, 1);
      setInputNumber("hNightSlope", nightCurve.slope, 2);
      setInputNumber("hNightShift", nightCurve.shift, 1);

      // limits (flow temp clamp)
      const displayLimits = clampHeatingLimitsToOt(limits);
      setInputNumber("hMin", displayLimits.minFlowC, 1);
      setInputNumber("hMax", displayLimits.maxFlowC, 1);

      // boiler max CH (ID57)
      setInputBool("hWrite57", cfg?.output?.applyBoilerMaxCh);
      setInputNumber("hBoilerMax", cfg?.output?.boilerMaxChC, 1);
      setInputBool("hDriveNightRelay", cfg?.output?.driveNightRelay);
      if(document.getElementById("hNightRelay") && Number.isFinite(Number(cfg?.output?.nightRelay))) document.getElementById("hNightRelay").value = String(Number(cfg.output.nightRelay));
      setInputBool("hNightRelayOnWhenNight", cfg?.output?.nightRelayOnWhenNight);
      setInputBool("hBoilerAssistEnabled", cfg?.boilerAssist?.enabled);
      setInputNumber("hBoilerAssistDeltaC", cfg?.boilerAssist?.deltaC, 1);
      setInputBool("hBoilerAssistForceChEnable", cfg?.boilerAssist?.forceChEnable);

      if(document.getElementById("eqMode") && cfg?.mode) document.getElementById("eqMode").value = String(cfg.mode);
      if(document.getElementById("hEqModeCfg") && cfg?.mode) document.getElementById("hEqModeCfg").value = String(cfg.mode);
      setInputBool("hUseIn1NightOverride", cfg?.useIn1NightOverride);
      setInputBool("hSummerModeEnabled", cfg?.summerModeEnabled);
      setInputNumber("hSummerOffAboveC", cfg?.summerOffAboveC, 1);
      setInputNumber("hSummerOnBelowC", cfg?.summerOnBelowC, 1);
      if(cfg?.schedule?.week){
        state.schedules.heatingDay = Array.from({length:7}, () => []);
        for(const day of (cfg.schedule.week || [])){
          const di = ["mon","tue","wed","thu","fri","sat","sun"].indexOf(String(day?.day || "").toLowerCase());
          if(di < 0) continue;

          const out = [];
          if(Array.isArray(day?.intervals)){
            for(const iv of day.intervals){
              const s = Number(iv?.startMin);
              const e = Number(iv?.endMin);
              if(Number.isFinite(s) && Number.isFinite(e) && s !== e){
                out.push({ start: minsToTime(s), end: minsToTime(e) });
              }
              if(out.length >= HEATING_MAX_INTERVALS_PER_DAY) break;
            }
          }

          if(!out.length){
            const s = Number(day?.dayStartMin);
            const e = Number(day?.nightStartMin);
            if(Number.isFinite(s) && Number.isFinite(e) && s !== e){
              out.push({ start: minsToTime(s), end: minsToTime(e) });
            }
          }

          state.schedules.heatingDay[di] = out;
        }
        saveSchedules();
      }
      const mx = cfg?.mixing || {};
      setInputBool("hMixEnabled", mx.enabled);
      if(document.getElementById("hMixOpenRelay")) document.getElementById("hMixOpenRelay").value = "Relé1 (pevně)";
      if(document.getElementById("hMixCloseRelay")) document.getElementById("hMixCloseRelay").value = "Relé2 (pevně)";
      setInputNumber("hMixDeadband", mx.deadbandC, 1);
      if(document.getElementById("hMixTargetOffsetC") && mx.targetOffsetC != null) document.getElementById("hMixTargetOffsetC").value = String(Number(mx.targetOffsetC));
      if(document.getElementById("hMixPulseMs") && mx.pulseMs != null) document.getElementById("hMixPulseMs").value = String(Number(mx.pulseMs));
      if(document.getElementById("hMixMinIntervalMs") && mx.minIntervalMs != null) document.getElementById("hMixMinIntervalMs").value = String(Number(mx.minIntervalMs));
      if(document.getElementById("hMixTravelMs") && mx.travelMs != null) document.getElementById("hMixTravelMs").value = String(Number(mx.travelMs));
      redrawEquithermViews();
      renderHeatingOtInfo();
      renderMixCalibrationInfo();
    }

    function applyOtConfigToForm(cfg){
      if(!cfg) return;
      state.ot.cfg = state.ot.cfg || {};
      state.ot.cfg.enabled = !!cfg.enabled;
      state.ot.cfg.enable = state.ot.cfg.enabled;
      state.ot.enabled = !!cfg.enabled || !!state.ot.present;
      state.ot.cfg.pollMs = Number(cfg.pollMs ?? 2000);
      state.ot.cfg.mode = String(cfg.mode ?? "control");
      state.ot.cfg.boilerControl = String(cfg.boilerControl ?? state.ot.cfg.boilerControl ?? (state.ot.cfg.mode === "control" ? "opentherm" : "relay"));
      state.ot.cfg.allowRawWrite = !!cfg.allowRawWrite;

      const en = document.getElementById("otEnable");
      if(en) en.checked = !!cfg.enabled;
      const poll = document.getElementById("otPoll");
      if(poll) poll.value = String(Number(cfg.pollMs ?? 2000));
      const mode = document.getElementById("otFailMode");
      if(mode) mode.value = String(cfg.mode ?? "control");
      const raw = document.getElementById("otLog");
      if(raw) raw.checked = !!cfg.allowRawWrite;
    }

function applyAlertsConfigToForm(cfg){
  const src = cfg?.alerts?.pressure || cfg?.pressure || {};
  state.alerts = state.alerts || {};
  state.alerts.pressure = Object.assign({}, state.alerts.pressure || {}, {
    enabled: ("enabled" in src) ? !!src.enabled : !!state.alerts?.pressure?.enabled,
    minBar: Number(src.minBar ?? state.alerts?.pressure?.minBar ?? 0.8),
    maxBar: Number(src.maxBar ?? state.alerts?.pressure?.maxBar ?? 2.8),
    hysteresisBar: Number(src.hysteresisBar ?? state.alerts?.pressure?.hysteresisBar ?? 0.05),
    active: ("active" in src) ? !!src.active : !!state.alerts?.pressure?.active,
    sensorValid: ("sensorValid" in src) ? !!src.sensorValid : !!state.alerts?.pressure?.sensorValid,
    pressureBar: numOrNaN(src.pressureBar ?? state.alerts?.pressure?.pressureBar),
    state: String(src.state ?? state.alerts?.pressure?.state ?? "init"),
  });

  const en = document.getElementById("pressAlarmEnable");
  const minEl = document.getElementById("pressAlarmMin");
  const maxEl = document.getElementById("pressAlarmMax");
  const hysEl = document.getElementById("pressAlarmHys");
  if(en) en.checked = !!state.alerts.pressure.enabled;
  if(minEl && Number.isFinite(state.alerts.pressure.minBar)) minEl.value = Number(state.alerts.pressure.minBar).toFixed(2);
  if(maxEl && Number.isFinite(state.alerts.pressure.maxBar)) maxEl.value = Number(state.alerts.pressure.maxBar).toFixed(2);
  if(hysEl && Number.isFinite(state.alerts.pressure.hysteresisBar)) hysEl.value = Number(state.alerts.pressure.hysteresisBar).toFixed(2);
}

function applyTimeConfigToForm(cfg){
  const src = cfg?.time || cfg || {};
  const ntp = Array.isArray(src.ntp) ? src.ntp : [];
  const en = document.getElementById("timeEnable");
  const tz = document.getElementById("timeTz");
  const n1 = document.getElementById("timeNtp1");
  const n2 = document.getElementById("timeNtp2");
  const n3 = document.getElementById("timeNtp3");
  const st = document.getElementById("timeStatusText");
  if(en) en.checked = !!src.enabled;
  if(tz) tz.value = String(src.tz || "");
  if(n1) n1.value = String(ntp[0] || "");
  if(n2) n2.value = String(ntp[1] || "");
  if(n3) n3.value = String(ntp[2] || "");
  if(st) st.textContent = src.valid ? `${src.iso || "čas platný"} • ${src.src || "SNTP"}` : `čas neplatný${src.src ? ` • ${src.src}` : ""}`;
  setBadge("#timeCfgBadge", src.valid ? "good" : (src.enabled ? "warn" : ""), `čas: ${src.valid ? "platný" : (src.enabled ? "čeká" : "vypnuto")}`);
}

async function timeLoad(options={}){
  const silent = !!options.silent;
  try{
    const cfg = await api.fetchConfigSection("time");
    applyTimeConfigToForm(cfg);
    if(!silent) toast("Čas", "Načteno.", "✅");
  }catch(e){
    setBadge("#timeCfgBadge", "bad", "čas: chyba");
    if(!silent) toast("Čas", e.message || String(e), "⚠");
    log("time load error: " + (e.message || e));
  }
}

async function timeSave(){
  const payload = {
    enabled: !!document.getElementById("timeEnable")?.checked,
    tz: String(document.getElementById("timeTz")?.value || "").trim(),
    ntp: [
      String(document.getElementById("timeNtp1")?.value || "").trim(),
      String(document.getElementById("timeNtp2")?.value || "").trim(),
      String(document.getElementById("timeNtp3")?.value || "").trim(),
    ].filter(Boolean)
  };
  await api.postConfigSection("time", payload);
  await timeLoad({ silent:true });
  await refresh(false);
  toast("Čas", "Nastavení uloženo.", "✅");
}

async function eventsLoad(options={}){
  const silent = !!options.silent;
  try{
    const j = await api.getJson("/api/events", 5000);
    const out = document.getElementById("eventsOut");
    const items = j?.events || j?.items || j || [];
    if(out) out.textContent = JSON.stringify(items, null, 2);
    setBadge("#eventsBadge", Array.isArray(items) && items.length ? "good" : "", `events: ${Array.isArray(items) ? items.length : 0}`);
    if(!silent) toast("Event log", "Načteno.", "✅");
  }catch(e){
    setBadge("#eventsBadge", "bad", "events: chyba");
    if(!silent) toast("Event log", e.message || String(e), "⚠");
    log("events load error: " + (e.message || e));
  }
}

async function historyLoad(options={}){
  const silent = !!options.silent;
  try{
    const j = await api.getJson("/api/history", 5000);
    const out = document.getElementById("historyOut");
    const items = j?.history || j?.items || j || [];
    if(out) out.textContent = JSON.stringify(items, null, 2);
    const count = Array.isArray(items) ? items.length : (Array.isArray(j?.samples) ? j.samples.length : 0);
    setBadge("#historyBadge", count ? "good" : "", `history: ${count}`);
    if(!silent) toast("Historie", "Načteno.", "✅");
  }catch(e){
    setBadge("#historyBadge", "bad", "history: chyba");
    if(!silent) toast("Historie", e.message || String(e), "⚠");
    log("history load error: " + (e.message || e));
  }
}

async function serviceIoCall(payload){
  await api.postJson("/api/service/io", payload, 5000);
  setBadge("#serviceIoBadge", "good", "service: odesláno");
}

    function minsToTime(v){
      const n = Math.max(0, Math.min(1439, Number(v || 0)));
      const hh = String(Math.floor(n / 60)).padStart(2, "0");
      const mm = String(n % 60).padStart(2, "0");
      return `${hh}:${mm}`;
    }

    function scheduleWeekToUi(week){
      const out = Array.from({length:7}, () => []);
      for(const day of (week || [])){
        const name = String(day?.day || "").toLowerCase();
        const di = ["mon","tue","wed","thu","fri","sat","sun"].indexOf(name);
        if(di < 0) continue;
        const arr = [];
        for(const iv of (day?.intervals || [])){
          const s = Number(iv?.startMin);
          const e = Number(iv?.endMin);
          if(!Number.isFinite(s) || !Number.isFinite(e) || s === e) continue;
          arr.push({ start: minsToTime(s), end: minsToTime(e) });
        }
        out[di] = arr;
      }
      return out;
    }

    function applyDhwConfigToForm(cfg){
      if(!cfg) return;
      state.dev = state.dev || {};
      state.dev.dhwCfgRaw = cfg;
      const heat = cfg.heat || {};
      const circ = cfg.circ || {};
      if(document.getElementById("dhwEnable")) document.getElementById("dhwEnable").checked = !!cfg.enabled;
      if(document.getElementById("dhwDisableEqDuringHeat")) document.getElementById("dhwDisableEqDuringHeat").checked = !!cfg.disableEquithermDuringHeat;
      if(document.getElementById("dhw2Target") && heat.targetTempC != null) document.getElementById("dhw2Target").value = String(Number(heat.targetTempC));
      if(document.getElementById("dhwRequestMode") && heat.requestMode) document.getElementById("dhwRequestMode").value = String(heat.requestMode);
      if(document.getElementById("dhwHysteresis") && heat.hysteresisC != null) document.getElementById("dhwHysteresis").value = String(Number(heat.hysteresisC));
      if(document.getElementById("dhwHeatUseInput")) document.getElementById("dhwHeatUseInput").checked = !!(heat.useInput ?? true);
      if(document.getElementById("dhwHeatUseSchedule")) document.getElementById("dhwHeatUseSchedule").checked = !!(heat.useSchedule ?? true);
      if(document.getElementById("dhwRelayRequest")) document.getElementById("dhwRelayRequest").checked = !!(heat.relayRequest ?? true);
      if(document.getElementById("dhwDriveValveRelay")) document.getElementById("dhwDriveValveRelay").checked = !!(heat.driveValveRelay ?? true);
      if(document.getElementById("dhwValveRelay") && heat.valveRelay != null) document.getElementById("dhwValveRelay").value = String(Number(heat.valveRelay));
      if(document.getElementById("dhwBoilerRelay") && heat.boilerRelay != null) document.getElementById("dhwBoilerRelay").value = String(Number(heat.boilerRelay));
      if(document.getElementById("dhwValveLeadMs") && heat.valveLeadMs != null) document.getElementById("dhwValveLeadMs").value = String(Number(heat.valveLeadMs));
      if(document.getElementById("dhwValveSwitchBackMs") && heat.valveSwitchBackMs != null) document.getElementById("dhwValveSwitchBackMs").value = String(Number(heat.valveSwitchBackMs));
      if(document.getElementById("dhwBoilerOffHoldMs") && heat.boilerOffHoldMs != null) document.getElementById("dhwBoilerOffHoldMs").value = String(Number(heat.boilerOffHoldMs));
      if(heat.schedule?.week) state.schedules.dhwHeat = scheduleWeekToUi(heat.schedule.week);
      if(circ.schedule?.week) state.schedules.dhwCirc = scheduleWeekToUi(circ.schedule.week);
      if(document.getElementById("dhwCircUseInput")) document.getElementById("dhwCircUseInput").checked = !!(circ.useInput ?? true);
      if(document.getElementById("dhwCircUseSchedule")) document.getElementById("dhwCircUseSchedule").checked = !!(circ.useSchedule ?? true);
      if(document.getElementById("dhwCircRelay") && circ.relay != null) document.getElementById("dhwCircRelay").value = String(Number(circ.relay));
      if(circ.pulseEnabled != null) state.circPulse.enable = !!circ.pulseEnabled;
      if(circ.pulseOnMin != null) state.circPulse.onMin = Number(circ.pulseOnMin);
      if(circ.pulseOffMin != null) state.circPulse.vypnutoMin = Number(circ.pulseOffMin);
      const al = cfg.antiLegionella || {};
      if(document.getElementById("dhwAntiLegEnabled")) document.getElementById("dhwAntiLegEnabled").checked = !!al.enabled;
      if(document.getElementById("dhwAntiLegWeekday") && al.weekday != null) document.getElementById("dhwAntiLegWeekday").value = String(Number(al.weekday));
      if(document.getElementById("dhwAntiLegStart") && al.startMin != null) document.getElementById("dhwAntiLegStart").value = minsToTime(al.startMin);
      if(document.getElementById("dhwAntiLegTarget") && al.targetTempC != null) document.getElementById("dhwAntiLegTarget").value = String(Number(al.targetTempC));
      if(document.getElementById("dhwAntiLegHold") && al.holdMin != null) document.getElementById("dhwAntiLegHold").value = String(Number(al.holdMin));
      setBadge("#dhwAntiLegBadge", al.enabled ? "good" : "", `AL: ${al.enabled ? "povoleno" : "vypnuto"}`);
      saveSchedules();
      saveCircPulse();
      ["dhwHeat","dhwCirc","heatingDay"].forEach(k => renderPlanner(k));
      const elPulse = document.getElementById("circPulseEnable");
      const elOn = document.getElementById("circPulseOn");
      const elOff = document.getElementById("circPulseOff");
      if(elPulse) elPulse.checked = !!state.circPulse.enable;
      if(elOn) elOn.value = String(Math.max(0, Number(state.circPulse.onMin ?? 5)));
      if(elOff) elOff.value = String(Math.max(0, Number(state.circPulse.vypnutoMin ?? 15)));
      updatePlannerStateBadges();
    }

    function applyEqStatus(status){
      if(!status || typeof status !== "object") return;
      state.eqStatus = status;
      state.eqFast = state.eqFast || {};
      const mode = status.mode || {};
      const time = status.time || {};
      const out = status.out || {};
      const mix = status.mix || {};
      if(mode.req != null) state.eqFast.m = String(mode.req);
      if(mode.eff != null) state.eqFast.me = String(mode.eff);
      if(mode.scheduleUsed != null) state.eqFast.su = !!mode.scheduleUsed;
      if(mode.in1ForcingNight != null) state.eqFast.i1 = !!mode.in1ForcingNight;
      if(time.valid != null) state.eqFast.tv = !!time.valid;
      if(status.active != null) state.eqFast.ac = !!status.active;
      if(status.reason != null) state.eqFast.rs = String(status.reason || "");
      if(Number.isFinite(Number(out.targetFlowC))) state.eqFast.tf = Number(out.targetFlowC);
      state.eqFast.mix = state.eqFast.mix || {};
      if(mix.state != null) state.eqFast.mix.state = String(mix.state);
      if(mix.pulsing != null) state.eqFast.mix.pulsing = !!mix.pulsing;
      if(mix.manual != null) state.eqFast.mix.manual = !!mix.manual;
      if(Number.isFinite(Number(mix.pulseRemainingMs))) state.eqFast.mix.prm = Number(mix.pulseRemainingMs);
      if(Number.isFinite(Number(mix.pulseElapsedMs))) state.eqFast.mix.elp = Number(mix.pulseElapsedMs);
      if(mix.pct != null) state.eqFast.mix.pct = Number(mix.pct);
      if(Number.isFinite(Number(state.eqFast.mix.pct))) state.accu.valve = numOrNaN(state.eqFast.mix.pct);
      renderMixBadge();
      renderServicePanel();
      renderMixCalibrationInfo();
    }

    function applyDhwStatus(status){
      if(!status || typeof status !== "object") return;
      state.dhwStatus = status;
      state.dhwFast = state.dhwFast || {};
      if(status.enabled != null) state.dhwFast.en = !!status.enabled;
      if(status.heatRequested != null) state.dhwFast.hr = !!status.heatRequested;
      if(status.heatActive != null) state.dhwFast.ha = !!status.heatActive;
      if(status.heatScheduleActive != null) state.dhwFast.hs = !!status.heatScheduleActive;
      if(status.heatInputActive != null) state.dhwFast.hi = !!status.heatInputActive;
      if(status.circRequested != null) state.dhwFast.cr = !!status.circRequested;
      if(status.circActive != null) state.dhwFast.ca = !!status.circActive;
      if(status.circScheduleActive != null) state.dhwFast.cs = !!status.circScheduleActive;
      if(status.circInputActive != null) state.dhwFast.ci = !!status.circInputActive;
      if(status.circPulseOn != null) state.dhwFast.cp = !!status.circPulseOn;
      if(status.boilerDhwMode != null) state.dhwFast.bm = !!status.boilerDhwMode;
      if(status.requestMode != null) state.dhwFast.rm = String(status.requestMode);
      if(status.heatPhase != null) state.dhwFast.hp = String(status.heatPhase);
      if(status.heatReason != null) state.dhwFast.hrs = String(status.heatReason);
      if(status.heatSequenceActive != null) state.dhwFast.hsq = !!status.heatSequenceActive;
      if(status.antiLegionellaDone != null) state.dhwFast.ald = !!status.antiLegionellaDone;
      if(status.tankTempC != null) state.dhwFast.tt = Number(status.tankTempC);
      if(status.targetTempC != null) state.dhwFast.tg = Number(status.targetTempC);
      if(status.valveRelayOn != null) state.dhwFast.vr = !!status.valveRelayOn;
      if(status.boilerRelayOn != null) state.dhwFast.br = !!status.boilerRelayOn;
      if(status.circRelayOn != null) state.dhwFast.rr = !!status.circRelayOn;
      if(status.otDhwEnable != null) state.dhwFast.ode = !!status.otDhwEnable;
      if(status.timeValid != null) state.dhwFast.tv = !!status.timeValid;
    }

    async function refreshOtCapacity(force=false){
      const now = Date.now();
      const activeView = getActiveView();
      const otViewActive = activeView === "opentherm" || activeView === "heating";
      if(!state.ot?.enabled || !state.ot?.ready) return state.ot.maxCapacityKw;
      if(!force && !otViewActive) return state.ot.maxCapacityKw;
      if(state.otMeta?.capacityFetching) return state.ot.maxCapacityKw;
      const successAge = now - Number(state.otMeta?.capacityFetchMs || 0);
      const failAge = now - Number(state.otMeta?.capacityFailMs || 0);
      if(!force && Number.isFinite(Number(state.ot.maxCapacityKw)) && successAge < 5 * 60 * 1000) return state.ot.maxCapacityKw;
      if(!force && failAge < 5 * 60 * 1000) return state.ot.maxCapacityKw;
      state.otMeta.capacityFetching = true;
      try{
        const j = await api.postJson("/api/opentherm/dataid/read", { id: 15, reqValue: 0 }, 5000);
        const kw = Number(j?.maxCapacityKw ?? j?.val?.maxCapacityKw);
        if(Number.isFinite(kw) && kw > 0){
          state.ot.maxCapacityKw = kw;
          state.otMeta.capacityFetchMs = now;
          state.otMeta.capacityFailMs = 0;
          const mod = Number(state.ot.modulationPct);
          state.ot.currentPowerKw = Number.isFinite(mod) ? (kw * mod / 100) : NaN;
          if(state.last) renderSample(state.last);
        }else{
          state.otMeta.capacityFailMs = now;
          log("ot capacity read: missing maxCapacityKw in response");
        }
      }catch(e){
        state.otMeta.capacityFailMs = now;
        log("ot capacity read failed: " + (e.message || e));
      }finally{
        state.otMeta.capacityFetching = false;
      }
      return state.ot.maxCapacityKw;
    }

    function applyFastSnapshot(fast){
      if(!fast || typeof fast !== "object") return;
      const firstFast = !(state.fast || state.last);
      state.fast = mergeFastSnapshot(state.fast, fast);
      applyFastToState(state.fast);
      state.net = state.net || {};
      state.net.lastFastOkMs = Date.now();
      const sample = getUiSample();
      queueRenderSample(sample);
      if(firstFast){
        try{ document.dispatchEvent(new CustomEvent("ui:first-fast")); }catch(_e){}
      }
      if(state.th?.loaded && getActiveView() === "thermometers") renderThermometersDevice();
    }

    function getAfterMixTempFromTemps(temps){
      if(!temps || typeof temps !== "object") return NaN;
      return numOrNaN(firstFinite(
        temps.afterMixC,
        temps.flowReturnC,
        temps.returnFlowC,
        temps["return.flow"],
        temps.returnTempC,
        temps.return
      ));
    }

    function applyFastToState(fast){
      if(!fast) return;

      // Fast root/system snapshot
      state.dev = state.dev || {};
      if(fast.ip != null) state.dev.ip = String(fast.ip);
      if(fast.rel && typeof fast.rel === "object" && hasOwn(fast.rel, "mask")){
        const mask = Number(fast.rel.mask) & 0xFF;
        state.dev.relMask = mask;
        state.io.relays = Array.from({length:8}, (_,k) => ((mask >> k) & 1) === 1);
      }
      if(fast.in && typeof fast.in === "object" && hasOwn(fast.in, "actMask")){
        const actMask = Number(fast.in.actMask) & 0xFF;
        state.io.inputs = Array.from({length:3}, (_,k) => ((actMask >> k) & 1) === 1);
      }
      if(fast.temps && typeof fast.temps === "object"){
        const temps = fast.temps;
        if(hasOwn(temps, "tank_top")) state.accu.top = readMaybeNumber(temps, "tank_top", state.accu.top);
        if(hasOwn(temps, "tank_mid")) state.accu.mid = readMaybeNumber(temps, "tank_mid", state.accu.mid);
        if(hasOwn(temps, "tank_bottom")) state.accu.bot = readMaybeNumber(temps, "tank_bottom", state.accu.bot);
        const hasReturnAlias = ["afterMixC", "flowReturnC", "returnFlowC", "return.flow", "returnTempC", "return"].some(k => hasOwn(temps, k));
        if(hasReturnAlias) state.accu.after = getAfterMixTempFromTemps(temps);
      }
      if(fast.heap && typeof fast.heap === "object"){
        state.diag = state.diag || { heap:{}, adminActions:[] };
        state.diag.heap = {
          free: readMaybeNumber(fast.heap, "free", state.diag.heap?.free),
          minFree: readMaybeNumber(fast.heap, "minFree", state.diag.heap?.minFree),
          maxAlloc: readMaybeNumber(fast.heap, "maxAlloc", state.diag.heap?.maxAlloc),
          psramFree: readMaybeNumber(fast.heap, "psramFree", state.diag.heap?.psramFree),
        };
      }
      if(Array.isArray(fast.adminActions)){
        state.diag = state.diag || { heap:{}, adminActions:[] };
        state.diag.adminActions = fast.adminActions.slice(0, 12);
      }

      // OpenTherm (compact snapshot)
      const ot = (fast.ot && typeof fast.ot === "object") ? fast.ot : {};
      state.ot.enabled = readMaybeBool(ot, "en", state.ot.enabled);
      if(hasOwn(ot, "bc")){
        state.ot.cfg = state.ot.cfg || {};
        state.ot.cfg.boilerControl = readMaybeString(ot, "bc", state.ot.cfg.boilerControl || "");
      }
      state.ot.ready = readMaybeBool(ot, "rd", state.ot.ready);
      state.ot.fault = readMaybeBool(ot, "fl", state.ot.fault);
      state.ot.present = state.ot.enabled;
      state.ot.comm = state.ot.enabled && state.ot.ready && !state.ot.fault;
      state.ot.chSet = readMaybeNumber(ot, "cs", state.ot.chSet);
      state.ot.chTemp = readMaybeNumber(ot, "bt", state.ot.chTemp);
      state.ot.returnTempC = readMaybeNumber(ot, "rt", state.ot.returnTempC);
      state.ot.dhwTemp = readMaybeNumber(ot, "dt", state.ot.dhwTemp);
      state.ot.outsideTempC = readMaybeNumber(ot, "ot", state.ot.outsideTempC);
      state.ot.pressure = readMaybeNumber(ot, "pr", state.ot.pressure);
      state.ot.modulationPct = readMaybeNumber(ot, "mt", state.ot.modulationPct);
      state.ot.reqWaterTempC = hasOwn(ot, "cs") ? numOrNaN(ot.cs) : state.ot.reqWaterTempC;
      if(!Number.isFinite(state.ot.reqWaterTempC)) state.ot.reqWaterTempC = firstFinite(state.eqFast?.tf, state.ot.reqWaterTempC);
      state.ot.maxChSetpointC = readMaybeNumber(ot, "mx", state.ot.maxChSetpointC);
      state.ot.maxChBoundMinC = readMaybeNumber(ot, "mxl", state.ot.maxChBoundMinC);
      state.ot.maxChBoundMaxC = readMaybeNumber(ot, "mxu", state.ot.maxChBoundMaxC);
      state.ot.dhwSetpointC = readMaybeNumber(ot, "dw", state.ot.dhwSetpointC);
      state.ot.dhwBoundMinC = readMaybeNumber(ot, "dwl", state.ot.dhwBoundMinC);
      state.ot.dhwBoundMaxC = readMaybeNumber(ot, "dwu", state.ot.dhwBoundMaxC);
      state.ot.reqDhwSetpointC = readMaybeNumber(ot, "ds", state.ot.reqDhwSetpointC);
      if(hasOwn(ot, "ff")) state.ot.faultFlags = Number(ot.ff) || 0;
      if(hasOwn(ot, "oc")) state.ot.oemFaultCode = Number(ot.oc) || 0;
      state.ot.reason = readMaybeString(ot, "rs", state.ot.reason);
      state.ot.lastCmd = readMaybeString(ot, "cmd", state.ot.lastCmd);
      if(hasOwn(ot, "sr")) state.ot.statusRaw = Number(ot.sr) || 0;
      if(hasOwn(ot, "da")) state.ot.dhwActive = !!ot.da;
      if(hasOwn(ot, "fo")) state.ot.flameOn = !!ot.fo;
      if(hasOwn(ot, "ca")) state.ot.chActive = !!ot.ca;
      syncOtDerived();
      renderHeatingOtInfo();

      // BLE compact
      state.bleFast = (fast.ble && typeof fast.ble === "object") ? fast.ble : (state.bleFast || {});

      // Equitherm compact
      state.eqFast = (fast.eq && typeof fast.eq === "object") ? fast.eq : (state.eqFast || {});
      if(state.eqFast?.mix && typeof state.eqFast.mix === "object" && hasOwn(state.eqFast.mix, "pct")){
        state.accu.valve = numOrNaN(state.eqFast.mix.pct);
      }
      if(!(fast.temps && Number.isFinite(getAfterMixTempFromTemps(fast.temps)))){
        state.accu.after = numOrNaN(firstFinite(state.eqFast?.mf, state.eqFast?.mixFeedbackC, state.eqFast?.fc, state.accu.after));
      }

      // DHW compact
      state.dhwFast = (fast.dhw && typeof fast.dhw === "object") ? fast.dhw : (state.dhwFast || {});
      if(state.dhwStatus && typeof state.dhwStatus === "object"){
        if(!Object.prototype.hasOwnProperty.call(state.dhwFast, "hp") && state.dhwStatus.heatPhase != null) state.dhwFast.hp = String(state.dhwStatus.heatPhase);
        if(!Object.prototype.hasOwnProperty.call(state.dhwFast, "hsq") && state.dhwStatus.heatSequenceActive != null) state.dhwFast.hsq = !!state.dhwStatus.heatSequenceActive;
        if(!Object.prototype.hasOwnProperty.call(state.dhwFast, "ald") && state.dhwStatus.antiLegionellaDone != null) state.dhwFast.ald = !!state.dhwStatus.antiLegionellaDone;
      }

      // Alerts compact
      if(fast.alerts && typeof fast.alerts === "object"){
        applyAlertsConfigToForm({ pressure: {
          enabled: ("en" in fast.alerts) ? !!fast.alerts.en : state.alerts?.pressure?.enabled,
          sensorValid: ("sv" in fast.alerts) ? !!fast.alerts.sv : state.alerts?.pressure?.sensorValid,
          pressureBar: ("p" in fast.alerts) ? fast.alerts.p : state.alerts?.pressure?.pressureBar,
          active: ("act" in fast.alerts) ? !!fast.alerts.act : state.alerts?.pressure?.active,
          state: ("st" in fast.alerts) ? fast.alerts.st : state.alerts?.pressure?.state,
        }});
      }
    }

    function renderIO(){
      const relayNames = [
        "Směšovací ventil - směr A (otevření)",
        "Směšovací ventil - směr B (zavření)",
        "Ventil 3c TUV/CH",
        "Cirkulace TUV",
        "Požadavek kotli TUV",
        "Den/Noc křivka na kotli",
        "Omezení výkonu kotle",
        "Stykač topné tyče AKU",
      ];

      const tbody = $("#relayTbl");
      tbody.innerHTML = "";
      for(let i=0;i<8;i++){
        const on = !!state.io.relays[i];
        const managedByDhw = [2,3,4].includes(i);
        const tr = document.createElement("tr");
        tr.innerHTML = `
          <td class="mono">R${i+1}</td>
          <td><strong>${escapeHtml(relayNames[i])}</strong>${managedByDhw ? '<div class="muted" style="margin-top:4px">řízeno funkcemi TUV / cirkulace</div>' : ''}</td>
          <td>${on ? '<span class="badge good"><span class="b"></span>ON</span>' : '<span class="badge"><span class="b"></span>OFF</span>'}</td>
          <td>
            <button class="btn" data-relay="${i}" ${managedByDhw ? 'disabled title="Použijte ovládání TUV/Cirkulace"' : ''}>${managedByDhw ? "Řízeno" : (on ? "Vypnout" : "Zapnout")}</button>
          </td>
        `;
        const relayBtn = tr.querySelector("button");
        if(!managedByDhw) relayBtn.addEventListener("click", async () => {
          try{
            const resp = await api.setRelay(i+1, null);
            const mask = Number(resp?.rel?.mask ?? resp?.fast?.rel?.mask ?? state.dev?.relMask ?? 0) & 0xFF;
            state.dev = state.dev || {};
            state.dev.relMask = mask;
            state.io.relays = Array.from({length:8}, (_,k) => ((mask >> k) & 1) === 1);
            toast("Relé", `R${i+1} -> ${state.io.relays[i] ? "ON" : "OFF"}`, state.io.relays[i] ? "✅" : "⭕");
            log(`relay R${i+1} toggled`);
            renderIO();
            renderOverviewBadges();
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("relay error: " + (e.message || e));
          }
        });
        tbody.appendChild(tr);
      }

      const inNames = [
        "Den/Noc křivka (aktivní = noc)",
        "Požadavek TUV (aktivní)",
        "Požadavek cirkulace (aktivní)",
      ];
      const inTb = $("#inTbl");
      inTb.innerHTML = "";
      for(let i=0;i<3;i++){
        const on = !!state.io.inputs[i];
        const tr = document.createElement("tr");
        tr.innerHTML = `
          <td class="mono">IN${i+1}</td>
          <td><strong>${escapeHtml(inNames[i])}</strong></td>
          <td>${on ? '<span class="badge good"><span class="b"></span>1</span>' : '<span class="badge"><span class="b"></span>0</span>'}</td>
        `;
        inTb.appendChild(tr);
      }

      // dhw table binds
      const dhwf = state.dhwFast || {};
      setText("#dhwReq", (dhwf.hr ?? state.io.inputs[1]) ? "aktivní" : "neaktivní");
      setText("#circReq", (dhwf.cr ?? state.io.inputs[2]) ? "aktivní" : "neaktivní");
      setText("#rValve", (dhwf.vr ?? state.io.relays[2]) ? "ON" : "OFF");
      setText("#rCirc", (dhwf.rr ?? state.io.relays[3]) ? "ON" : "OFF");
      setText("#rBoiler", (dhwf.br ?? state.io.relays[4]) ? "ON" : ((dhwf.ode) ? "OT" : "OFF"));
      const dhwPhaseMetaNow = dhwPhaseMeta(dhwf.hp || state.dhwStatus?.heatPhase || "idle");
      const dhwReasonMetaNow = dhwReasonMeta(state.dhwStatus?.heatReason || "");
      setText("#dhwHeatPhase", `${dhwPhaseMetaNow.label} (${dhwPhaseMetaNow.raw})`);
      setText("#dhwHeatSeq", (dhwf.hsq ?? state.dhwStatus?.heatSequenceActive) ? "ANO" : "NE");
      setText("#dhwAntiLegDone", (dhwf.ald ?? state.dhwStatus?.antiLegionellaDone) ? "ANO" : "NE");
      setText("#dhwHeatReason", dhwReasonMetaNow.raw ? `${dhwReasonMetaNow.label} (${dhwReasonMetaNow.raw})` : "--");
    }


    function heatingModeLabel(mode){
      const m = String(mode || "").toLowerCase();
      if(m === "day") return "komfort";
      if(m === "night") return "útlum";
      if(m === "auto") return "auto";
      return m || "--";
    }

    function heatingModePairLabel(modeEff){
      return String(modeEff || "").toLowerCase() === "night" ? "komfort/útlum: útlum" : "komfort/útlum: komfort";
    }

    function heatingModeSourceLabel(source, timeValid){
      switch(source){
        case "manual-day": return "ručně komfort";
        case "manual-night": return "ručně útlum";
        case "in1": return "IN1";
        case "schedule": return "plán";
        default: return timeValid === false ? "náhradní režim bez času" : "náhradní režim";
      }
    }

    function getEquithermAuditState(){
      const fast = state.eqFast || {};
      const modeReq = String(fast.m || fast.mode || document.getElementById("eqMode")?.value || "auto").toLowerCase();
      const modeEff = String(fast.me || "").toLowerCase();
      const scheduleUsed = !!(fast.su ?? fast.scheduleUsed);
      const in1ForcingNight = !!(fast.i1 ?? fast.in1ForcingNight);
      const timeValid = (fast.tv ?? fast.timeValid);
      const normalizedTimeValid = (timeValid == null) ? null : !!timeValid;
      let source = "fallback";
      if(modeReq === "day") source = "manual-day";
      else if(modeReq === "night") source = "manual-night";
      else if(in1ForcingNight) source = "in1";
      else if(scheduleUsed) source = "schedule";
      return { modeReq, modeEff, scheduleUsed, in1ForcingNight, timeValid: normalizedTimeValid, source };
    }

    function describeEquithermAudit(audit){
      const modeLabel = heatingModePairLabel(audit.modeEff);
      const src = heatingModeSourceLabel(audit.source, audit.timeValid);
      return `${modeLabel} • zdroj: ${src}`;
    }

    function isBoilerDhwModeActive(){
      if(state.dhwStatus && typeof state.dhwStatus.boilerDhwMode === "boolean") return !!state.dhwStatus.boilerDhwMode;
      if(state.dhwFast && Object.prototype.hasOwnProperty.call(state.dhwFast, "bm")) return !!state.dhwFast.bm;
      if(state.ot && typeof state.ot.dhwActive === "boolean") return !!(state.ot.enabled && state.ot.ready && state.ot.dhwActive);
      const sr = Number(state.ot?.statusRaw || 0);
      return !!(sr & (1 << 2));
    }

    function renderDhwBoilerMode(){
      const active = isBoilerDhwModeActive();
      setText("#dhwBoilerMode", active ? "ANO" : "NE");
      setBadge("#bDhwOtMode", active ? "warn" : "", `Kotel TUV přes OT: ${active ? "ANO" : "NE"}`);
    }

    function renderOverviewBadges(){
      const audit = getEquithermAuditState();
      const dn = heatingModeLabel(audit.modeEff || "day");

      setText("#pillUptime", "uptime: " + $("#dUp").textContent);
      $("#bSchedule").childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
      $("#bSchedule").appendChild(document.createTextNode(" " + describeEquithermAudit(audit)));

      // NET pill
      const ip = state.dev?.ip || $("#dIp")?.textContent || "--";
      $("#pillNet").textContent = "LAN: " + ip;

      // BLE pill
      const b = state.bleFast || {};
      if(!b.en) $("#pillBle").textContent = "BLE: vypnuto";
      else if(b.cn) $("#pillBle").textContent = "BLE: ok";
      else if(b.sc) $("#pillBle").textContent = "BLE: scan";
      else $("#pillBle").textContent = "BLE: neaktivní";

      // OT pill
      if(!state.ot.enabled) $("#pillOT").textContent = "OT: vypnuto";
      else if(state.ot.fault) $("#pillOT").textContent = "OT: chyba";
      else if(state.ot.comm) $("#pillOT").textContent = "OT: ok";
      else if(state.ot.ready) $("#pillOT").textContent = "OT: připraveno";
      else $("#pillOT").textContent = "OT: inicializace";

      // function summaries
      const eqTarget = Number.isFinite(Number(state.eqFast?.tf)) ? Number(state.eqFast.tf) : Number($("#eqSet")?.value || 0);
      setText("#sumEq", `Křivka: ${dn} • cíl: ${Number.isFinite(eqTarget) ? eqTarget.toFixed(1) : "--"} °C • ${describeEquithermAudit(audit).replace(/^komfort\/útlum: [^•]+ • /i,"")}`);
      const dhwf = state.dhwFast || {};
      const dhwReq = !!(dhwf.hr ?? state.io.inputs[1]);
      const boilerDhwMode = isBoilerDhwModeActive();
      const dhwActive = !!(dhwf.ha ?? boilerDhwMode ?? state.io.relays[4]);
      const circReq = !!(dhwf.cr ?? state.io.inputs[2]);
      const circOn = !!(dhwf.rr ?? state.io.relays[3]);
      const circPulseOn = !!(dhwf.cp ?? false);
      const requestMode = String(dhwf.rm || document.getElementById("dhwRequestMode")?.value || "relay");
      const dhwPhaseMetaSum = dhwPhaseMeta(dhwf.hp || state.dhwStatus?.heatPhase || "idle");
      const dhwReasonMetaSum = dhwReasonMeta(state.dhwStatus?.heatReason || "");
      setText("#sumDhw", `Požadavek: ${dhwReq ? "ano" : "ne"} • ohřev: ${dhwActive ? "běží" : "neaktivní"} • fáze: ${dhwPhaseMetaSum.label} • důvod: ${dhwReasonMetaSum.label} • režim: ${requestMode === "opentherm" ? "OT" : "relé"}`);
      setText("#sumCirc", `Požadavek: ${circReq ? "ano" : "ne"} • relé: ${circOn ? "ON" : "OFF"}${state.circPulse.enable ? ` • pulz: ${circPulseOn ? "ON" : "OFF"}` : ""}`);

      // overall badge
      const ok = true;
      $("#bOverall").classList.toggle("good", ok);
      $("#bOverall").classList.toggle("warn", !ok);
      $("#bOverall").childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
      $("#bOverall").appendChild(document.createTextNode(" " + (ok ? "OK" : "POZOR")));
    }

    function pushHistory(key, value){
      const arr = state.history[key];
      const n = Number(value);
      if(!Number.isFinite(n)) return;
      arr.push(n);
      while(arr.length > 30) arr.shift();
    }

    function getActiveView(){
      const active = document.querySelector('.section.active');
      if(!active || !active.id) return 'overview';
      return String(active.id).replace(/^view-/, '') || 'overview';
    }

    function renderSample(sample){
      sample = Object.assign({}, sample || {});
      sample.out = numOrNaN(sample.out);
      sample.ch = numOrNaN(sample.ch);
      sample.dhw = numOrNaN(sample.dhw);
      sample.pr = numOrNaN(sample.pr);
      sample.accTop = numOrNaN(sample.accTop);
      sample.accMid = numOrNaN(sample.accMid);
      sample.accBot = numOrNaN(sample.accBot);
      sample.mixValve = numOrNaN(sample.mixValve);
      sample.mixAfter = numOrNaN(sample.mixAfter);
      sample.mixTarget = numOrNaN(sample.mixTarget);
      sample.mixReturn = numOrNaN(sample.mixReturn);
      sample.eqTarget = numOrNaN(sample.eqTarget);
      state.last = sample;
      observeServiceIssues();

      // tiles
      setTextNum("#kpiOut", sample.out, 1);
      setTextNum("#kpiCH",  sample.ch, 1);
      setTextNum("#kpiDHW", sample.dhw, 1);
      setTextNum("#kpiPr",  sample.pr, 2);

      const ts = fmtTs(sample.now);
      setText("#kpiOutTs", ts);
      setText("#kpiChTs", ts);
      setText("#kpiDhwTs", ts);
      setText("#kpiPrTs", ts);
      setText("#kpiPowerTs", ts);
      setText("#kpiReqWaterTs", ts);

      const modPct = Number(state.ot?.modulationPct);
      const maxKw = Number(state.ot?.maxCapacityKw);
      const currentKw = (Number.isFinite(Number(state.ot?.currentPowerKw))
        ? Number(state.ot.currentPowerKw)
        : (Number.isFinite(modPct) && Number.isFinite(maxKw) ? (maxKw * modPct / 100) : NaN));
      const reqWaterC = Number.isFinite(Number(state.ot?.reqWaterTempC))
        ? Number(state.ot.reqWaterTempC)
        : (Number.isFinite(Number(state.eqFast?.tf)) ? Number(state.eqFast.tf) : Number(state.ot?.chSet));
      setText("#kpiPower", Number.isFinite(currentKw) ? currentKw.toFixed(1) : "--");
      setText("#kpiPowerMeta", Number.isFinite(modPct) ? `${modPct.toFixed(0)} %` : "-- %");
      setText("#kpiReqWater", Number.isFinite(reqWaterC) ? reqWaterC.toFixed(1) : "--");
      setText("#kpiReqWaterMeta", Number.isFinite(Number(state.ot?.chSet)) ? `OT req ${Number(state.ot.chSet).toFixed(1)} °C` : "--");

      setText("#ovUpdated", "Aktualizace: " + new Date(sample.now).toLocaleString("cs-CZ"));

      // diag
      setText("#dTime", new Date(sample.now).toLocaleString("cs-CZ"));
      setText("#dUp",  uptimeString());
      setText("#pillUptime", "uptime: " + $("#dUp").textContent);
      const heap = state?.diag?.heap || {};
      const buildParts = ["UI 2026"];
      if(Number.isFinite(Number(heap.free))) buildParts.push(`heap ${fmtBytes(heap.free)}`);
      if(Number.isFinite(Number(heap.minFree))) buildParts.push(`min ${fmtBytes(heap.minFree)}`);
      if(Number.isFinite(Number(heap.maxAlloc))) buildParts.push(`max ${fmtBytes(heap.maxAlloc)}`);
      setText("#dBuild", buildParts.join(" • "));

      // history for sparklines
      pushHistory("out", sample.out);
      pushHistory("ch", sample.ch);
      pushHistory("dhw", sample.dhw);
      pushHistory("pr", sample.pr);

      const activeView = getActiveView();
      const overviewVisible = activeView === "overview";
      const heatingVisible = activeView === "heating";
      const accuVisible = activeView === "accu";

      if(overviewVisible){
        drawSpark($("#sparkOut"), state.history.out);
        drawSpark($("#sparkCH"),  state.history.ch);
        drawSpark($("#sparkDHW"), state.history.dhw);
        drawSpark($("#sparkPr"),  state.history.pr);
      }


      // Equitherm charts (overview + heating)
      const { dayCurve, nightCurve, minFlowC, maxFlowC } = getEqChartConfig();

      const pointX = sample.out;

      function getEqEffectiveCurve(){
        const modeEff = String(state?.eqFast?.me || "").toLowerCase();
        if(modeEff === "night") return nightCurve;
        if(modeEff === "day") return dayCurve;

        const uiMode = String($("#eqMode")?.value || "auto").toLowerCase();
        if(uiMode === "night") return nightCurve;
        if(uiMode === "day") return dayCurve;

        return sample.in1 ? nightCurve : dayCurve;
      }

      const activeCurve = getEqEffectiveCurve();
      const calcTarget = eqChFromCurve(pointX, activeCurve, minFlowC, maxFlowC);
      const tgt = Number.isFinite(sample?.eqTarget)
        ? Number(sample.eqTarget)
        : (Number.isFinite(calcTarget) ? calcTarget : Number($("#hTarget")?.value ?? $("#eqSet")?.value ?? 45));
      const pointY = clamp(tgt, minFlowC, maxFlowC);

      // In device mode, eqSet is informative (target comes from firmware)
      const eqSetEl = $("#eqSet");
      if(eqSetEl){
        if(Number.isFinite(sample?.eqTarget)){
          eqSetEl.value = Number(sample.eqTarget).toFixed(1);
        }
        eqSetEl.disabled = true;
      }


      if(overviewVisible){
        drawEquithermChart($("#eqChartOverview"), {
          dayCurve, nightCurve, minFlowC, maxFlowC,
          pointX, pointY,
          outdoorNow: sample.out,
          swDay: $("#swDay"), swNight: $("#swNight"), swNow: $("#swNow"),
          fitY: true,
        });
      }
      if(heatingVisible){
        drawEquithermChart($("#eqChartHeating"), {
          dayCurve, nightCurve, minFlowC, maxFlowC,
          pointX, pointY,
          outdoorNow: sample.out,
          swDay: $("#swDay2"), swNight: $("#swNight2"), swNow: $("#swNow2"),
          fitY: true,
        });
      }

// Accumulator tank + mixing valve
      setText("#accTop", fmtNum(sample.accTop, 1));
      setText("#accMid", fmtNum(sample.accMid, 1));
      setText("#accBot", fmtNum(sample.accBot, 1));

      // Tank widget (overview)
      const aTop = firstFinite(sample.accTop, state.accu.top);
      const aMid = firstFinite(sample.accMid, state.accu.mid);
      const aBot = firstFinite(sample.accBot, state.accu.bot);
      const mixPct = Number.isFinite(sample.mixValve) ? sample.mixValve : (state.accu.valve ?? 0);
      const vTgt = Number.isFinite(sample.mixTarget) ? sample.mixTarget : Number($("#hTarget")?.value ?? $("#eqSet")?.value ?? 45);
      const vAfter = Number.isFinite(sample.mixAfter) ? sample.mixAfter : (state.accu.after ?? sample.ch);
      const vRet = Number.isFinite(sample.mixReturn) ? sample.mixReturn : (sample.ch - 8);
      const helping = (Number(sample.accTop ?? state.accu.top) > (clamp(vTgt, 0, 99) + 0.2)) && (mixPct > 0.5);
      if([aTop,aMid,aBot].some(v => Number.isFinite(v))) setTankFill("#tankFill", aTop, aMid, aBot);
      setText("#mixPctLbl", Math.round(mixPct) + " %");
      const mb = $("#mixBar");
      if(mb){
        const pct = clamp(mixPct,0,100);
        mb.style.width = pct + "%";
        mb.parentElement?.setAttribute("aria-valuenow", String(Math.round(pct)));
      }
      const assist = $("#accAssist");
      if(assist){ assist.textContent = helping ? "ANO" : "NE"; }


      updatePlannerStateBadges();

      if(activeView === "io" || activeView === "dhw") renderIO();
      renderOverviewBadges();
      renderDhwBoilerMode();
  renderServicePanel();
      renderMixBadge();
      renderServicePanel();

      // modes
      const eqAudit = getEquithermAuditState();
      const dn = heatingModeLabel(eqAudit.modeEff || "day");
      $("#bMode").childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
      $("#bMode").appendChild(document.createTextNode(" Režim: " + heatingModeLabel(eqAudit.modeReq || "auto")));
      $("#bSchedule").childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
      $("#bSchedule").appendChild(document.createTextNode(" " + describeEquithermAudit(eqAudit)));

      const eqSourceLabel = heatingModeSourceLabel(eqAudit.source, eqAudit.timeValid);
      setBadge("#eqAuditSource", eqAudit.source.startsWith("manual") ? "good" : (eqAudit.source === "fallback" ? "warn" : ""), "zdroj: " + eqSourceLabel);
      setBadge("#eqAuditInput", eqAudit.in1ForcingNight ? "warn" : null, "IN1: " + (eqAudit.in1ForcingNight ? "nutí útlum" : ((state.io.inputs[0]) ? "aktivní" : "neaktivní")));
      setBadge("#eqAuditSchedule", eqAudit.scheduleUsed ? "good" : null, "plán: " + (eqAudit.scheduleUsed ? "použit" : "nepoužit"));
      setBadge("#eqAuditTime", eqAudit.timeValid === false ? "warn" : (eqAudit.timeValid ? "good" : null), "čas: " + (eqAudit.timeValid === false ? "neplatný" : (eqAudit.timeValid ? "platný" : "neznámý")));

      // DHW state badge
      const dhwf2 = state.dhwFast || {};
      const boilerDhwMode2 = isBoilerDhwModeActive();
      const dhwActive = !!(dhwf2.ha ?? boilerDhwMode2 ?? (state.io.inputs[1] || state.io.relays[2] || state.io.relays[4]));
      const dhwMode = String(dhwf2.rm || document.getElementById("dhwRequestMode")?.value || "relay");
      const dhwPhaseMetaBadge = dhwPhaseMeta(dhwf2.hp || state.dhwStatus?.heatPhase || "idle");
      const dhwReasonMetaBadge = dhwReasonMeta(state.dhwStatus?.heatReason || "");
      const dhwSeq = !!(dhwf2.hsq ?? state.dhwStatus?.heatSequenceActive);
      const dhwStateTxt = dhwActive ? `běží (${dhwMode === "opentherm" ? "OT" : "relé"})` : (dhwSeq ? "přechod sekvence" : "neaktivní");
      const dhwBadgeKind = dhwReasonMetaBadge.fault ? "bad" : ((dhwActive || boilerDhwMode2 || dhwSeq) ? (dhwPhaseMetaBadge.kind || "warn") : "");
      setBadge("#dhwState", dhwBadgeKind, "stav: " + dhwStateTxt + ` • fáze: ${dhwPhaseMetaBadge.label} • důvod: ${dhwReasonMetaBadge.label}` + ` • kotel TUV přes OT: ${boilerDhwMode2 ? "ANO" : "NE"}`);
      const dhwWarn = $("#dhwWarn");
      if(dhwWarn){
        if(dhwReasonMetaBadge.fault) setBadge("#dhwWarn", "bad", `varování: ${dhwReasonMetaBadge.label}`);
        else if(dhwPhaseMetaBadge.kind === "warn" || boilerDhwMode2) setBadge("#dhwWarn", "warn", `upozornění: ${dhwPhaseMetaBadge.label}` + (boilerDhwMode2 ? " • kotel TUV aktivní přes OT" : ""));
        else setBadge("#dhwWarn", "", "varování: žádné");
      }


      // OpenTherm pill + page
      const otPillText = !state.ot.enabled ? "OT: vypnuto" : state.ot.fault ? "OT: chyba" : state.ot.comm ? "OT: ok" : state.ot.ready ? "OT: připraveno" : "OT: inicializace";
      setText("#pillOT", otPillText);
      if($("#pillOT")){
        const p = $("#pillOT").closest(".pill");
        if(p){
          p.classList.toggle("good", !!state.ot.comm);
          p.classList.toggle("bad", !!state.ot.enabled && !state.ot.comm);
        }
      }
      setText("#otComm", !state.ot.enabled ? "VYPNUTO" : state.ot.comm ? "OK" : state.ot.fault ? "FAULT" : state.ot.ready ? "READY" : "INIT");
      setText("#otChSet", fmtMaybeNumber(state.ot.chSet, 1));
      setText("#otChTemp", fmtMaybeNumber(state.ot.chTemp, 1));
      setText("#otDhwTemp", fmtMaybeNumber(state.ot.dhwTemp, 1));
      setText("#otPress", fmtMaybeNumber(state.ot.pressure, 2));
      setText("#otOutside", fmtMaybeNumber(state.ot.outsideTempC, 1));
      setText("#otReturn", fmtMaybeNumber(state.ot.returnTempC, 1));
      setText("#otMod", fmtMaybeNumber(state.ot.modulationPct, 1));
      setText("#otPower", fmtMaybeNumber(state.ot.currentPowerKw, 2));
      setText("#otMaxPower", fmtMaybeNumber(state.ot.maxCapacityKw, 1));
      setText("#otMaxCh", fmtMaybeNumber(state.ot.maxChSetpointC, 1));
      setText("#otDhwSet", fmtMaybeNumber(state.ot.dhwSetpointC, 1));
      setText("#otReqDhw", fmtMaybeNumber(state.ot.reqDhwSetpointC, 1));
      setText("#otReason", state.ot.reason || "--");
      setText("#otFaultFlags", Number(state.ot.faultFlags || 0).toString());
      setText("#otOemFault", Number(state.ot.oemFaultCode || 0).toString());
      setText("#otCfgState", (state.ot.cfg?.enabled ?? state.ot.cfg?.enable) ? "povoleno" : "vypnuto");
      if($("#otEnable")){
        $("#otEnable").checked = !!(state.ot.cfg?.enabled ?? state.ot.cfg?.enable);
        $("#otPoll").value = Number(state.ot.cfg?.pollMs ?? 1000);
        $("#otFailMode").value = state.ot.cfg?.mode ?? state.ot.cfg?.failMode ?? "control";
        $("#otLog").checked = !!(state.ot.cfg?.allowRawWrite ?? state.ot.cfg?.log);
      }
      const otModeTxt = String(state.ot.cfg?.mode || "control");
      setBadge("#otBadge", state.ot.comm ? "good" : "bad", state.ot.comm ? `stav: OK • ${otModeTxt}` : `stav: ${state.ot.enabled ? "chyba" : "vypnuto"} • ${otModeTxt}`);

      const pAlert = state.alerts?.pressure || {};
      let pText = "alarm: vypnuto";
      let pKind = "";
      if(pAlert.enabled){
        if(!pAlert.sensorValid) { pText = "alarm: bez dat tlaku"; pKind = "warn"; }
        else if(pAlert.active) { pText = `alarm: ${pAlert.state === "high" ? "MAX" : "MIN"} • ${fmtMaybeNumber(pAlert.pressureBar, 2)} bar`; pKind = "bad"; }
        else { pText = `alarm: OK • ${fmtMaybeNumber(pAlert.pressureBar, 2)} bar`; pKind = "good"; }
      }
      setBadge("#pressAlarmBadge", pKind, pText);
      applyAlertsConfigToForm({ pressure: pAlert });

      // Accu dedicated page
      if(accuVisible){
        const aTopA = Number.isFinite(sample.accTop) ? sample.accTop : state.accu.top;
        const aMidA = Number.isFinite(sample.accMid) ? sample.accMid : state.accu.mid;
        const aBotA = Number.isFinite(sample.accBot) ? sample.accBot : state.accu.bot;
        setText("#accTopA", fmtNum(aTopA, 1));
        setText("#accMidA", fmtNum(aMidA, 1));
        setText("#accBotA", fmtNum(aBotA, 1));
        setText("#mixPctLblA", Math.round(mixPct) + " %");
        const mbA = $("#mixBarA"); if(mbA){ const pctA = clamp(mixPct,0,100); mbA.style.width = pctA + "%"; mbA.parentElement?.setAttribute("aria-valuenow", String(Math.round(pctA))); }
        setText("#mixAfterA", fmtNum(vAfter, 1));
        setText("#mixTargetA", fmtNum(clamp(vTgt,0,99), 1));
        setText("#mixReturnA", fmtNum(clamp(vRet,0,99), 1));
        if([aTopA,aMidA,aBotA].some(v => Number.isFinite(v))) setTankFill("#tankFillA", aTopA, aMidA, aBotA);
        setBadge("#accuHelpBadge", helping ? "good" : "", helping ? "pomoc: ANO" : "pomoc: NE");
      }
      // update sidebar device label
      const ip = state.dev?.ip || $("#dIp")?.textContent || (state.apiBase ? state.apiBase : "(origin)");
      $("#sbDevice").textContent = ip + " • device";
    }

    // Uptime (client session)
    const startMs = Date.now();
    function uptimeString(){
      const s = Math.floor((Date.now() - startMs)/1000);
      const d = Math.floor(s/86400);
      const h = Math.floor((s%86400)/3600);
      const m = Math.floor((s%3600)/60);
      const ss = s%60;
      return `${d}d ${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;
    }

    
    function updateUploadProgress(prefix, receivedBytes, expectedBytes, message, active){
      const bar = document.getElementById(prefix + "Bar");
      const text = document.getElementById(prefix + "Text");
      const pct = (expectedBytes > 0) ? Math.max(0, Math.min(100, Math.round((receivedBytes * 100) / expectedBytes))) : 0;
      if(bar) bar.style.width = pct + "%";
      if(text) text.textContent = `${prefix === "otaFw" ? "Firmware" : "Filesystem"}: ${message || (active ? "upnačítání" : "neaktivní")} • ${receivedBytes||0}/${expectedBytes||0} B (${pct}%)`;
      const live = document.getElementById("otaLiveState");
      if(live){
        live.classList.remove("good","warn","bad");
        if(active) live.classList.add("warn");
        else if(message === "uploaded") live.classList.add("good");
        else if(message && message !== "neaktivní") live.classList.add("bad");
        live.childNodes.forEach(n=>{ if(n.nodeType===3) n.remove(); });
        live.appendChild(document.createTextNode(` OTA: ${active ? "upload" : (message || "neaktivní")}`));
      }
    }


    function getHeatingOtUsageSummary(){
      const eqUsesOt = !!(state.dev?.eqCfgRaw?.output?.useOpenTherm ?? state.dev?.eqUsesOpenTherm ?? true);
      const dhwMode = String(document.getElementById("dhwRequestMode")?.value || state.dev?.dhwCfgRaw?.heat?.requestMode || "relay").toLowerCase();
      return {
        eqUsesOt,
        dhwUsesOt: dhwMode === "opentherm"
      };
    }

    function ensureOtConfigCompatible(enabled, mode){
      const usage = getHeatingOtUsageSummary();
      const wantsReadOnly = String(mode || "control") === "readOnly";
      if((usage.eqUsesOt || usage.dhwUsesOt) && (!enabled || wantsReadOnly)) {
        const parts = [];
        if(usage.eqUsesOt) parts.push("topení/ekviterm");
        if(usage.dhwUsesOt) parts.push("TUV přes OpenTherm");
        throw new Error(`OpenTherm musí zůstat v režimu control, protože ho právě používá ${parts.join(" + ")}.`);
      }
    }

    function ensureWritableOtForDhw(requestMode){
      if(String(requestMode || "relay") !== "opentherm") return;
      const otMode = String(state.ot?.cfg?.mode || "control");
      const boilerControl = String(state.dev?.fast?.ot?.bc || state.ot?.cfg?.boilerControl || "");
      if(otMode === "readOnly" || boilerControl === "relay") throw new Error("OpenTherm je v režimu jen pro čtení. Pro TUV přepněte OT do control, nebo použijte režim relay.");
    }

    async function dhwStartFromUi(targetTempC, requestMode){
      ensureWritableOtForDhw(requestMode);
      await api.postConfigSection("dhw", { heat: { requestMode, targetTempC, otEnableDhw: requestMode === "opentherm", otDhwSetpointC: targetTempC } });
      state.dev = state.dev || {};
      state.dev.dhwCfgLoaded = false;
      state.dev.dhwCfgRaw = Object.assign({}, state.dev.dhwCfgRaw || {}, { heat: Object.assign({}, state.dev?.dhwCfgRaw?.heat || {}, { requestMode, targetTempC, otEnableDhw: requestMode === "opentherm", otDhwSetpointC: targetTempC }) });
      await api.dhwCmd({ heatActive: true });
    }

    async function dhwStopFromUi(){
      await api.dhwCmd({ heatActive: false });
    }

    async function dhwBoostFromUi(targetTempC, requestMode, boostMin=15){
      ensureWritableOtForDhw(requestMode);
      await api.postConfigSection("dhw", { heat: { requestMode, targetTempC, otEnableDhw: requestMode === "opentherm", otDhwSetpointC: targetTempC } });
      state.dev = state.dev || {};
      state.dev.dhwCfgLoaded = false;
      state.dev.dhwCfgRaw = Object.assign({}, state.dev.dhwCfgRaw || {}, { heat: Object.assign({}, state.dev?.dhwCfgRaw?.heat || {}, { requestMode, targetTempC, otEnableDhw: requestMode === "opentherm", otDhwSetpointC: targetTempC }) });
      await api.dhwCmd({ boostMin });
    }

    async function dhwCircSetFromUi(on){
      await api.dhwCmd({ circActive: !!on });
    }

    async function dhwReloadConfigFromDevice(){
      const cfg = await api.fetchConfigSection("dhw");
      if(cfg){
        state.dhwCfg = cfg;
        state.dev = state.dev || {};
        state.dev.dhwCfgRaw = cfg;
        state.dev.dhwCfgLoaded = true;
        applyDhwConfigToForm(cfg);
      }
      await refresh(false);
    }

    async function dhwSaveConfigFromUi(){
      const heatCfg = state.dev?.dhwCfgRaw?.heat || state.dhwCfg?.heat || {};
      const circCfg = state.dev?.dhwCfgRaw?.circ || state.dhwCfg?.circ || {};
      const antiCfg = state.dev?.dhwCfgRaw?.antiLegionella || state.dhwCfg?.antiLegionella || {};
      const requestMode = String(document.getElementById("dhwRequestMode")?.value || heatCfg.requestMode || "relay");
      const targetTempC = Number(document.getElementById("dhw2Target")?.value || 50);
      const antiStart = timeToMin(String(document.getElementById("dhwAntiLegStart")?.value || minsToTime(Number(antiCfg.startMin ?? 120))));
      const payload = { dhw: {
        enabled: !!document.getElementById("dhwEnable")?.checked,
        disableEquithermDuringHeat: !!document.getElementById("dhwDisableEqDuringHeat")?.checked,
        heat: {
          useInput: !!document.getElementById("dhwHeatUseInput")?.checked,
          useSchedule: !!document.getElementById("dhwHeatUseSchedule")?.checked,
          scheduleEnabled: !!(heatCfg.scheduleEnabled ?? true),
          targetTempC,
          hysteresisC: Number(document.getElementById("dhwHysteresis")?.value || heatCfg.hysteresisC || 2),
          requestMode,
          otEnableDhw: requestMode === "opentherm",
          otDhwSetpointC: targetTempC,
          relayRequest: !!document.getElementById("dhwRelayRequest")?.checked,
          driveValveRelay: !!document.getElementById("dhwDriveValveRelay")?.checked,
          valveRelay: Number(document.getElementById("dhwValveRelay")?.value || heatCfg.valveRelay || 3),
          boilerRelay: Number(document.getElementById("dhwBoilerRelay")?.value || heatCfg.boilerRelay || 5),
          valveLeadMs: Math.max(0, Number(document.getElementById("dhwValveLeadMs")?.value || heatCfg.valveLeadMs || 3000)),
          valveSwitchBackMs: Math.max(0, Number(document.getElementById("dhwValveSwitchBackMs")?.value || heatCfg.valveSwitchBackMs || 1500)),
          boilerOffHoldMs: Math.max(0, Number(document.getElementById("dhwBoilerOffHoldMs")?.value || heatCfg.boilerOffHoldMs || 2000)),
          schedule: { week: serializeDhwWeek("dhwHeat") }
        },
        circ: {
          useInput: !!document.getElementById("dhwCircUseInput")?.checked,
          useSchedule: !!document.getElementById("dhwCircUseSchedule")?.checked,
          scheduleEnabled: !!(circCfg.scheduleEnabled ?? true),
          pulseEnabled: !!state.circPulse.enable,
          pulseOnMin: Math.max(0, Number(state.circPulse.onMin ?? 5)),
          pulseOffMin: Math.max(0, Number(state.circPulse.vypnutoMin ?? 15)),
          relay: Number(document.getElementById("dhwCircRelay")?.value || circCfg.relay || 4),
          schedule: { week: serializeDhwWeek("dhwCirc") }
        },
        antiLegionella: {
          enabled: !!document.getElementById("dhwAntiLegEnabled")?.checked,
          weekday: Number(document.getElementById("dhwAntiLegWeekday")?.value || antiCfg.weekday || 0),
          startMin: Number.isFinite(antiStart) ? antiStart : Number(antiCfg.startMin || 120),
          targetTempC: Number(document.getElementById("dhwAntiLegTarget")?.value || antiCfg.targetTempC || 60),
          holdMin: Number(document.getElementById("dhwAntiLegHold")?.value || antiCfg.holdMin || 30),
        }
      }};
      await api.postConfigSection("dhw", payload.dhw);
      state.dev = state.dev || {};
      state.dev.dhwCfgRaw = payload.dhw;
      state.dhwCfg = payload.dhw;
      applyDhwConfigToForm(payload.dhw);
      await refresh(false);
    }

    function getManagedRelayMaskFromUi(){
      const dhwCfg = state.dev?.dhwCfgRaw || state.dhwCfg || {};
      const heatCfg = dhwCfg.heat || {};
      const circCfg = dhwCfg.circ || {};
      const valveRelay = Math.max(1, Math.min(8, Number(heatCfg.valveRelay || 3)));
      const boilerRelay = Math.max(1, Math.min(8, Number(heatCfg.boilerRelay || 5)));
      const circRelay = Math.max(1, Math.min(8, Number(circCfg.relay || 4)));
      let mask = 0;
      if(heatCfg.driveValveRelay !== false) mask |= (1 << (valveRelay - 1));
      if(heatCfg.relayRequest !== false) mask |= (1 << (boilerRelay - 1));
      mask |= (1 << (circRelay - 1));
      return mask & 0xFF;
    }

    async function setUnmanagedRelayMask(mask){
      const managedMask = getManagedRelayMaskFromUi();
      const currentMask = Number(state.dev?.relMask ?? state.dev?.fast?.rel?.mask ?? 0) & 0xFF;
      const safeMask = ((Number(mask) & 0xFF) & ~managedMask) | (currentMask & managedMask);
      const resp = await api.setRelayMask(safeMask);
      const nextMask = Number(resp?.rel?.mask ?? resp?.fast?.rel?.mask ?? safeMask) & 0xFF;
      state.dev = state.dev || {};
      state.dev.relMask = nextMask;
      state.io.relays = Array.from({length:8}, (_,k) => ((nextMask >> k) & 1) === 1);
      return { mask: nextMask, managedSkipped: (safeMask !== (Number(mask) & 0xFF)) || resp?.warn === "managed_relays_ignored" };
    }

    function uploadWithProgress(url, fileInputId, prefix){
      return new Promise((resolve, reject) => {
        const inp = document.getElementById(fileInputId);
        const file = inp?.files?.[0];
        if(!file){ reject(new Error("Vyber soubor.")); return; }
        const xhr = new XMLHttpRequest();
        const fd = new FormData();
        fd.append("file", file);
        xhr.open("POST", normalizedApiBase() + url, true);
        xhr.upload.onprogress = (e) => updateUploadProgress(prefix, e.loaded || 0, e.total || file.size || 0, "upnačítání", true);
        xhr.onerror = () => reject(new Error("Network error"));
        xhr.onabort = () => reject(new Error("Upload aborted"));
        xhr.onload = () => {
          let j = {};
          try{ j = JSON.parse(xhr.responseText || "{}"); }catch{}
          if(xhr.status >= 200 && xhr.status < 300){
            updateUploadProgress(prefix, Number(j.receivedBytes || file.size || 0), Number(j.partitionBytes || file.size || 0), j.msg || "uploaded", false);
            resolve(j);
          }else{
            reject(new Error(j.msg || j.err || xhr.responseText || `HTTP ${xhr.status}`));
          }
        };
        updateUploadProgress(prefix, 0, file.size || 0, "starting", true);
        xhr.send(fd);
      });
    }

    // Legacy WebSocket/polling code removed. Current runtime uses connectWs() + applyFastSnapshot().

function applyBootstrapPayload(payload){
  if(!payload || typeof payload !== "object") return false;
  let applied = false;
  if(payload.fast && typeof payload.fast === "object") {
    applyFastSnapshot(payload.fast);
    applied = true;
  }
  state.dev = state.dev || {};
  if(payload.equitherm){
    state.dev.eqCfgLoaded = true;
    applyEqConfigToForm(payload.equitherm, { force:true });
    applied = true;
  }
  if(payload.opentherm){
    state.dev.otCfgLoaded = true;
    applyOtConfigToForm(payload.opentherm);
    applied = true;
  }
  if(payload.dhw){
    state.dev.dhwCfgLoaded = true;
    state.dhwCfg = payload.dhw;
    applyDhwConfigToForm(payload.dhw);
    applied = true;
  }
  if(payload.alerts){
    state.dev.alertsCfgLoaded = true;
    applyAlertsConfigToForm(payload.alerts);
    applied = true;
  }
  if(payload.time){
    state.dev.timeCfgLoaded = true;
    applyTimeConfigToForm(payload.time);
    applied = true;
  }
  if(payload.dallas){
    state.th = state.th || {};
    state.th.cfgLoaded = true;
    state.th.dallasEnabled = !!payload.dallas?.enabled;
    state.th.roles = normalizeDallasRolesMap(payload.dallas?.roles);
    state.th.roleMeta = normalizeDallasRoleMeta(payload.dallas?.availableRoles);
    applied = true;
  }
  return applied;
}

// ----- Refresh
let timer = null;
let refreshing = false;
ensureWsState();

async function refresh(forceToast=false){
  if(refreshing) return;
  refreshing = true;
  try{
    setApiHealth("warn", state.ws?.připojeno ? "API: WS sync…" : "API: čtu…");

	    const shouldFetchFast = forceToast || !state.ws?.připojeno || !state.fast;
	    if(shouldFetchFast){
	      const fast = await api.getJson("/api/fast", 4000);
	      applyFastSnapshot(fast);
	    }

    if(state.net){
      state.net.failCount = 0;
      state.net.nextPollMs = document.hidden ? 30000 : 10000;
    }
    setApiHealth("good", state.ws?.připojeno ? "API: WebSocket" : "API: zařízení");

    const nowMs = Date.now();
    const activeView = getActiveView();
    const extrasRelevant = forceToast || activeView === "heating" || activeView === "dhw" || activeView === "opentherm";
    const extrasDue = extrasRelevant && (forceToast || !state.net || nowMs >= Number(state.net.extrasDueMs || 0));
    if(extrasDue){
      const data = await api.fetchStatusExtras(activeView);

      let needRender = false;
      if(data?.equitherm?.config){
        state.dev = state.dev || {};
        state.dev.eqCfgLoaded = true;
        applyEqConfigToForm(data.equitherm.config);
      }
      if(data?.equitherm?.status){
        applyEqStatus(data.equitherm.status);
        needRender = true;
      }
      if(data?.opentherm?.status){
        applyOtStatus(data.opentherm.status);
        needRender = true;
      }
      if(data?.opentherm?.config){
        state.dev = state.dev || {};
        state.dev.otCfgLoaded = true;
        applyOtConfigToForm(data.opentherm.config);
      }
      if(data?.dhw?.status){
        applyDhwStatus(data.dhw.status);
        needRender = true;
      }
      if(data?.dhw?.config){
        state.dev = state.dev || {};
        state.dev.dhwCfgLoaded = true;
        state.dhwCfg = data.dhw.config;
        applyDhwConfigToForm(data.dhw.config);
      }
      if(needRender && state.last) queueRenderSample(getUiSample());
      if(state.net) state.net.extrasDueMs = nowMs + (state.ws?.připojeno ? 30000 : 45000);
    }

    void refreshOtCapacity(false);

    setApiHealth("good", state.ws?.připojeno ? "API: WebSocket" : "API: zařízení");
    if(forceToast) toast("Aktualizováno", state.ws?.připojeno ? "Zařízení odpovědělo, WebSocket aktivní." : "Zařízení odpovědělo.", "🔌");
  }catch(e){
    const msg = e?.message || String(e);
    if(msg && /\/api\/(fast|config)\b/.test(msg) && maybeAdoptPageOriginBase("refresh", true)){
      log("refresh fallback retry -> same origin");
      refreshing = false;
      return await refresh(forceToast);
    }
    setApiHealth("bad", state.ws?.připojeno ? "API: WS chyba" : "API: chyba");
    if(state.net){
      state.net.failCount = Math.min(8, Number(state.net.failCount || 0) + 1);
      const backvypnutoMs = Math.min(120000, 10000 * Math.pow(2, Math.max(0, state.net.failCount - 1)));
      state.net.nextPollMs = Math.max(document.hidden ? 30000 : 10000, backvypnutoMs);
    }
    updateRefreshCadence();
    const nowMs = Date.now();
    const shouldToast = !!forceToast || !state.net || (nowMs - Number(state.net.lastErrorToastMs || 0) >= 60000);
    if(shouldToast){
      if(state.net) state.net.lastErrorToastMs = nowMs;
      toast("Chyba", msg, "⚠");
    }
    log("refresh error: " + msg);
  }finally{
    refreshing = false;
  }
}

    async function pushHeatingPlannerToDevice(){
      const week = [];
      let any = false;
      for(let i=0;i<7;i++){
        const arr = normIntervals(state.schedules?.heatingDay?.[i] || []);
        if(arr.length > HEATING_MAX_INTERVALS_PER_DAY) throw new Error(`Den ${i+1}: maximum je ${HEATING_MAX_INTERVALS_PER_DAY} intervalů.`);
        if(intervalsOverlap(arr)) throw new Error(`Den ${i+1}: intervaly se překrývají.`);
        const intervals = [];
        for(const iv of arr){
          const s = timeToMin(iv.start);
          const e = timeToMin(iv.end);
          if(Number.isFinite(s) && Number.isFinite(e) && s >= 0 && e >= 0 && s !== e){
            intervals.push({ startMin: s, endMin: e, start: iv.start, end: iv.end });
            any = true;
          }
        }
        const o = { day: ["mon","tue","wed","thu","fri","sat","sun"][i], intervals };
        if(intervals.length){
          o.dayStartMin = intervals[0].startMin;
          o.nightStartMin = intervals[0].endMin;
        }
        week.push(o);
      }
      if(!any) throw new Error("Plán topení je prázdný nebo neplatný.");
      await api.postConfigSection("equitherm", {
        schedule: {
          enabled: true,
          week,
        }
      });
      state.dev = state.dev || {};
      state.dev.eqCfgLoaded = false;
    }

    async function pushDhwPlannerToDevice(){
      const heatCfg = state.dev?.dhwCfgRaw?.heat || {};
      const circCfg = state.dev?.dhwCfgRaw?.circ || {};
      await api.postConfigSection("dhw", {
        heat: {
          useSchedule: true,
          scheduleEnabled: !!(heatCfg.scheduleEnabled ?? true),
          schedule: { week: serializeDhwWeek("dhwHeat") }
        },
        circ: {
          useSchedule: true,
          scheduleEnabled: !!(circCfg.scheduleEnabled ?? true),
          pulseEnabled: !!state.circPulse.enable,
          pulseOnMin: Math.max(0, Number(state.circPulse.onMin ?? 5)),
          pulseOffMin: Math.max(0, Number(state.circPulse.vypnutoMin ?? 15)),
          schedule: { week: serializeDhwWeek("dhwCirc") }
        }
      });
    }

    async function syncAllPlannersToDevice(){
      await pushHeatingPlannerToDevice();
      await pushDhwPlannerToDevice();
      await refresh(false);
    }

    async function diagExportConfig(){
      const cfg = await api.getJson("/api/config", 12000);
      const stamp = new Date().toISOString().replace(/[:.]/g, "-");
      const blob = new Blob([JSON.stringify(cfg, null, 2)], { type:"application/json" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = `esp32-config-${stamp}.json`;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1500);
      return cfg;
    }

    async function diagImportConfigFile(file){
      if(!file) throw new Error("Vyber JSON soubor s konfigurací.");
      const txt = await file.text();
      let payload = null;
      try{
        payload = JSON.parse(txt);
      }catch(_e){
        throw new Error("Soubor není validní JSON.");
      }
      const resp = await api.postJson("/api/config/import", payload, 20000);
      state.dev = state.dev || {};
      state.dev.eqCfgLoaded = false;
      state.dev.dhwCfgLoaded = false;
      state.dev.otCfgLoaded = false;
      state.th.loaded = false;
      state.th.cfgLoaded = false;
      await heatingReloadConfigFromDevice().catch(() => null);
      await dhwReloadConfigFromDevice().catch(() => null);
      await mqttLoad({ silent:true }).catch(() => null);
      await timeLoad({ silent:true }).catch(() => null);
      await thermoLoad({ silent:true, forceConfig:true }).catch(() => null);
      await refresh(false);
      return resp;
    }

    // ----- Actions
    function wire(){
      // theme
      $("#btnTheme").addEventListener("click", () => {
        state.theme = state.theme === "light" ? "dark" : "light";
        applyTheme();
    renderHeatingOtInfo();
    renderMixCalibrationInfo();
        toast("Téma", state.theme === "light" ? "Světlý režim." : "Tmavý režim.", "🌓");
        // redraw sparklines for contrast
        ["sparkOut","sparkCH","sparkDHW","sparkPr"].forEach(id => {
          const key = id === "sparkOut" ? "out" : id === "sparkCH" ? "ch" : id === "sparkDHW" ? "dhw" : "pr";
          drawSpark($("#"+id), state.history[key]);
        });
        if(state.last) renderSample(state.last);
      });

      // refresh buttons
      $("#btnRefresh").addEventListener("click", () => withButtonBusy($("#btnRefresh"), "Obnovuji…", () => refresh(true)));
      $("#btnForceRefresh").addEventListener("click", () => withButtonBusy($("#btnForceRefresh"), "Aktualizuji…", () => refresh(true)));
      $("#btnQuickReboot")?.addEventListener("click", () => withButtonBusy($("#btnQuickReboot"), "Restartuji…", async () => {
        const ok = window.confirm("Opravdu restartovat zařízení?");
        if(!ok) return;
        await api.reboot();
        toast("Zařízení", "Restart odeslán.", "↻");
      }));

      // nav desktop + mobile
      $$("#sideNav a").forEach(a => a.addEventListener("click", (e) => {
        e.preventDefault();
        setView(a.dataset.view);
      }));
      $$("#bottomNav button").forEach(b => b.addEventListener("click", () => setView(b.dataset.view)));

      // quick action
      $("#btnQuick").addEventListener("click", async () => {
        const quickBtn = $("#btnQuick");
        return withButtonBusy(quickBtn, "Provádím…", async () => {
          try{
            const active = !!state.dhwFast?.ha || !!state.io.relays[4];
            if(active){
              await dhwStopFromUi();
              toast("Rychlá akce", "Zastavuji TUV.", "⛔");
              log("quick -> dhw stop");
            }else{
              await dhwBoostFromUi(Number($("#dhw2Target")?.value || $("#dhwTarget")?.value || 50), String(document.getElementById("dhwRequestMode")?.value || "relay"), 15);
              toast("Rychlá akce", "Boost TUV 15 min.", "⚡");
              log("quick -> dhw boost 15 min");
            }
            await refresh(false);
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("quick action error: " + (e.message || e));
          }
        });
      });

      // overview function buttons
      $$("[data-act]").forEach(btn => btn.addEventListener("click", async () => {
        const act = btn.dataset.act;
        try{
          if(act === "eqToggle"){
            const cur = String($("#eqMode")?.value || state.eqFast?.me || "auto");
            const next = (cur === "auto") ? ((state.eqFast?.me || "day") === "day" ? "night" : "day") : (cur === "day" ? "night" : "day");
            await api.eqCmd({ mode: next });
            if($("#eqMode")) $("#eqMode").value = next;
            toast("Ekviterm", `Mode -> ${next}`, "♨");
            log(`eqToggle -> ${next}`);
            await refresh(false);
            return;
          }
          if(act === "circToggle"){
            await dhwCircSetFromUi(!(state.dhwFast?.ca ?? state.io.relays[3]));
            toast("Cirkulace", "Přepnuto (zařízení).", "🔁");
            log("circToggle -> dhw circ");
            await refresh(false);
            return;
          }
          if(act === "dhwBoost"){
            await dhwBoostFromUi(Number($("#dhw2Target")?.value || $("#dhwTarget")?.value || 50), String(document.getElementById("dhwRequestMode")?.value || "relay"), 15);
            toast("TUV", "Boost 15 min (zařízení).", "⚡");
            log("dhwBoost -> /api/dhw/cmd boostMin=15");
            await refresh(false);
            return;
          }
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("quick action error: " + (e.message || e));
          return;
        }
      }));

      // Apply buttons
      $("#btnEqApply").addEventListener("click", async () => {
        try{
          const mode = $("#eqMode")?.value || "auto";
          await api.eqCmd({ mode });
          toast("Ekviterm", "Uloženo do zařízení.", "✅");
          log(`eq cmd -> /api/equitherm/cmd mode=${mode}`);
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("eq apply error: " + (e.message || e));
        }
      });

      $("#btnDhwApply").addEventListener("click", async () => {
        try{
          await dhwStartFromUi(Number($("#dhw2Target")?.value || $("#dhwTarget")?.value || 50), String(document.getElementById("dhwRequestMode")?.value || "relay"));
          toast("TUV", "Ohřev spuštěn (zařízení).", "🚿");
          log("dhw start -> /api/dhw/cmd heatActive=true");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhw start error: " + (e.message || e));
        }
      });
      $("#btnDhwStop").addEventListener("click", async () => {
        try{
          await dhwStopFromUi();
          toast("TUV", "Zastaveno.", "⛔");
          log("dhw stop -> /api/dhw/cmd heatActive=false");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhw stop error: " + (e.message || e));
        }
      });

      $("#btnCircOn").addEventListener("click", async () => {
        try{
          await dhwCircSetFromUi(true);
          toast("Cirkulace", "Zapnuto.", "✅");
          log("circ on -> /api/dhw/cmd circActive=true");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("circ on error: " + (e.message || e));
        }
      });
      $("#btnCircOff").addEventListener("click", async () => {
        try{
          await dhwCircSetFromUi(false);
          toast("Cirkulace", "Vypnuto.", "⭕");
          log("circ vypnuto -> /api/dhw/cmd circActive=false");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("circ vypnuto error: " + (e.message || e));
        }
      });

      $("#btnAllOff").addEventListener("click", async () => {
        try{
          const res = await setUnmanagedRelayMask(0x00);
          toast("Vše", res.managedSkipped ? "Ruční relé vypnuta. TUV/cirkulace zůstaly řízené funkcemi." : "Relé vypnuta (zařízení).", "⛔");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("all vypnuto error: " + (e.message || e));
        }
      });


      // live update equitherm chart when heating inputs change
      ["hDaySlope","hDayShift","hNightSlope","hNightShift","hMin","hMax","hBoilerMax","hTarget","eqSet","hMixDeadband","hMixTargetOffsetC","hMixPulseMs","hMixMinIntervalMs","hMixTravelMs","hMixEnabled","hMixOpenRelay","hMixCloseRelay","hEqModeCfg","hUseIn1NightOverride","hSummerModeEnabled","hSummerOffAboveC","hSummerOnBelowC","hDriveNightRelay","hNightRelay","hNightRelayOnWhenNight","hBoilerAssistEnabled","hBoilerAssistDeltaC","hBoilerAssistForceChEnable"].forEach(id => {
        const el = document.getElementById(id);
        if(!el) return;
        el.addEventListener("input", redrawEquithermViewsDebounced);
        el.addEventListener("change", redrawEquithermViewsDebounced);
      });
      eqConfigInputIds().forEach(id => {
        const el = document.getElementById(id);
        if(!el) return;
        const markDirty = () => setEqConfigDirty(true);
        el.addEventListener("input", markDirty);
        el.addEventListener("change", markDirty);
      });

      // Planner render + save buttons
      ["heatingDay","dhwHeat","dhwCirc"].forEach(k => renderPlanner(k));
      // Circulation pulse controls (DHW circulation)
const elPulse = document.getElementById("circPulseEnable");
const elOn = document.getElementById("circPulseOn");
const elOff = document.getElementById("circPulseOff");
if(elPulse && elOn && elOff){
  elPulse.checked = !!state.circPulse.enable;
  elOn.value = String(Math.max(0, Number(state.circPulse.onMin ?? 5)));
  elOff.value = String(Math.max(0, Number(state.circPulse.vypnutoMin ?? 15)));

  const applyPulse = debounce(() => {
    state.circPulse.enable = !!elPulse.checked;
    state.circPulse.onMin = Math.max(0, Number(elOn.value || 0));
    state.circPulse.vypnutoMin = Math.max(0, Number(elOff.value || 0));
    saveCircPulse();
    updatePlannerStateBadges();
    toast("Cirkulace TUV", state.circPulse.enable ? `Pulzní režim: ${state.circPulse.onMin} min ON / ${state.circPulse.vypnutoMin} min OFF` : "Pulzní režim vypnut (běží kontinuálně v rámci plánu).", "🟠");
    log(`circ pulse: enable=${state.circPulse.enable} on=${state.circPulse.onMin} vypnuto=${state.circPulse.vypnutoMin}`);
  }, 180);
  ["change","input"].forEach(ev => { elPulse.addEventListener(ev, applyPulse); elOn.addEventListener(ev, applyPulse); elOff.addEventListener(ev, applyPulse); });
}

updatePlannerStateBadges();
      setInterval(updatePlannerStateBadges, 10000);

      document.querySelectorAll("[data-pl-save]").forEach(btn => btn.addEventListener("click", async () => {
        saveSchedules();
        toast("Plán", "Uloženo.", "✅");
        log(`planner saved (${btn.dataset.plSave})`);

        if(btn.dataset.plSave === "heatingDay"){
          try{
            await pushHeatingPlannerToDevice();
            toast("Ekviterm plán", "Odesláno do zařízení.", "✅");
            log("schedule -> /api/config/equitherm");
            await refresh(false);
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("schedule error: " + (e.message || e));
          }
        }

        if(btn.dataset.plSave === "dhwHeat"){
          try{
            await pushDhwPlannerToDevice();
            toast("TUV plán", "Odesláno do zařízení.", "✅");
            log("schedule -> /api/config/dhw");
            await refresh(false);
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("dhw schedule error: " + (e.message || e));
          }
        }

        if(state.last) renderSample(state.last);
      }));

      $("#plExport")?.addEventListener("click", () => {
        const blob = new Blob([JSON.stringify(state.schedules,null,2)], {type:"application/json"});
        const a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "ui2026_schedules.json";
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
        toast("Plán", "Export stažen.", "⬇");
      });

      $("#plImport")?.addEventListener("click", async () => {
        const inp = document.createElement("input");
        inp.type = "file";
        inp.accept = "application/json";
        inp.addEventListener("change", async () => {
          const f = inp.files?.[0];
          if(!f) return;
          try{
            const txt = await f.text();
            const obj = JSON.parse(txt);
            if(!obj?.heatingDay || !obj?.dhwHeat || !obj?.dhwCirc) throw new Error("Neplatný formát.");
            state.schedules = obj;
            saveSchedules();
            ["heatingDay","dhwHeat","dhwCirc"].forEach(k => renderPlanner(k));
            updatePlannerStateBadges();
            if(state.source === "device"){
              await syncAllPlannersToDevice();
              toast("Plán", "Import hotový a odeslaný do zařízení.", "✅");
              log("planner import ok + device sync");
            }else{
              toast("Plán", "Import hotový.", "✅");
              log("planner import ok");
            }
            if(state.last) renderSample(state.last);
          }catch(e){
            toast("Import selhal", e.message || String(e), "⚠");
            log("planner import error: " + (e.message || e));
          }
        });
        inp.click();
      });


      // DHW planner export/import buttons (reuse same logic)
      $("#plExport2")?.addEventListener("click", () => $("#plExport")?.click());
      $("#plImport2")?.addEventListener("click", () => $("#plImport")?.click());


      $("#otPing")?.addEventListener("click", () => refresh(true));
      $("#otMergeRefresh")?.addEventListener("click", () => refresh(true));

      // Accu refresh
      $("#accuRefresh")?.addEventListener("click", () => refresh(true));

      // OpenTherm advanced
      $("#otScanRefresh")?.addEventListener("click", otScanRefresh);
      $("#otScanStart")?.addEventListener("click", otScanStart);
      $("#otScanStop")?.addEventListener("click", otScanStop);
      $("#otScanShowAll")?.addEventListener("change", () => otRenderScan(state.otAdv.scan || {}));
      if(getActiveView() === "opentherm") void otProfileRefresh();

      $("#otRwRead")?.addEventListener("click", otRwRead);
      $("#otRwWrite")?.addEventListener("click", otRwWrite);
      $("#otRwCopy")?.addEventListener("click", async () => {
        const t = document.getElementById("otRwOut")?.textContent || "";
        try{ await navigator.clipboard.writeText(t); toast("OpenTherm", "Zkopírováno.", "📋"); }catch{ toast("OpenTherm", "Clipboard nelze.", "⚠"); }
      });


      $("#hFit")?.addEventListener("click", () => {
        eq.fitY = !eq.fitY;
        toast("Graf", eq.fitY ? "Auto Y rozsah: zapnuto" : "Auto Y rozsah: vypnuto", "📈");
        if(state.last) renderSample(state.last);
      });
      // Heating view apply
      $("#hApply").addEventListener("click", async () => withButtonBusy($("#hApply"), "Ukládám…", async () => {
        try{
          const dayCurve = {
            slope: Number($("#hDaySlope")?.value),
            shift: Number($("#hDayShift")?.value),
          };
          const nightCurve = {
            slope: Number($("#hNightSlope")?.value),
            shift: Number($("#hNightShift")?.value),
          };
          const day = eqPointsFromCurve(dayCurve);
          const night = eqPointsFromCurve(nightCurve);
          const rawLimits = {
            minFlowC: Number($("#hMin")?.value),
            maxFlowC: Number($("#hMax")?.value),
          };
          const limits = clampHeatingLimitsToOt(rawLimits);
          const minEl = $("#hMin");
          const maxEl = $("#hMax");
          if(minEl && Number.isFinite(limits.minFlowC) && Math.abs(limits.minFlowC - Number(rawLimits.minFlowC)) > 0.01) minEl.value = String(limits.minFlowC);
          if(maxEl && Number.isFinite(limits.maxFlowC) && Math.abs(limits.maxFlowC - Number(rawLimits.maxFlowC)) > 0.01) maxEl.value = String(limits.maxFlowC);
          const rawBoilerMaxChC = Number($("#hBoilerMax")?.value);
          let boilerMaxChC = rawBoilerMaxChC;
          const otBoundLo = Number(state.ot?.maxChBoundMinC);
          const otBoundHi = Number(state.ot?.maxChBoundMaxC);
          if(Number.isFinite(boilerMaxChC)){
            if(Number.isFinite(otBoundLo)) boilerMaxChC = Math.max(boilerMaxChC, otBoundLo);
            if(Number.isFinite(otBoundHi)) boilerMaxChC = Math.min(boilerMaxChC, otBoundHi);
            const boilerMaxEl = $("#hBoilerMax");
            if(boilerMaxEl && Math.abs(boilerMaxChC - rawBoilerMaxChC) > 0.01) boilerMaxEl.value = String(boilerMaxChC);
          }
          const output = {
            applyBoilerMaxCh: !!$("#hWrite57")?.checked,
            boilerMaxChC,
            driveNightRelay: !!$("#hDriveNightRelay")?.checked,
            nightRelay: Number($("#hNightRelay")?.value || 6),
            nightRelayOnWhenNight: !!$("#hNightRelayOnWhenNight")?.checked,
          };
          const mixing = {
            enabled: !!$("#hMixEnabled")?.checked,
            openRelay: 1,
            closeRelay: 2,
            deadbandC: Number($("#hMixDeadband")?.value || 0.5),
            targetOffsetC: Number($("#hMixTargetOffsetC")?.value || 0),
            pulseMs: Number($("#hMixPulseMs")?.value || 300),
            minIntervalMs: Number($("#hMixMinIntervalMs")?.value || 30000),
            travelMs: Number($("#hMixTravelMs")?.value || 6000),
          };
          const boilerAssist = {
            enabled: !!$("#hBoilerAssistEnabled")?.checked,
            deltaC: Number($("#hBoilerAssistDeltaC")?.value || 0),
            forceChEnable: !!$("#hBoilerAssistForceChEnable")?.checked,
          };
          const mode = String($("#hEqModeCfg")?.value || $("#eqMode")?.value || "auto");
          if(document.getElementById("eqMode")) document.getElementById("eqMode").value = mode;

          await api.postConfigSection("equitherm", {
            mode,
            useIn1NightOverride: !!$("#hUseIn1NightOverride")?.checked,
            summerModeEnabled: !!$("#hSummerModeEnabled")?.checked,
            summerOffAboveC: Number($("#hSummerOffAboveC")?.value || 18),
            summerOnBelowC: Number($("#hSummerOnBelowC")?.value || 16),
            day, night, limits, output, mixing, boilerAssist
          });
          setEqConfigDirty(false);
          toast("Topení", "Nastavení uloženo do zařízení.", "✅");
          log("heating apply -> /api/config/equitherm");
          // allow refresh to hydrate updated values
          state.dev = state.dev || {};
          state.dev.eqCfgLoaded = false;
          await refresh(false);
          redrawEquithermViews();
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("heating apply error: " + (e.message || e));
        }
      }));

      document.getElementById("hReloadConfig")?.addEventListener("click", () => withButtonBusy(document.getElementById("hReloadConfig"), "Načítám…", async () => {
        try{
          await heatingReloadConfigFromDevice();
          toast("Topení", "Konfigurace načtena ze zařízení.", "✅");
          log("heating reload config");
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("heating reload config error: " + (e.message || e));
        }
      }));

      const mixPulseABtn = document.getElementById("hMixPulseA");
      if(mixPulseABtn){
        mixPulseABtn.addEventListener("click", () => withButtonBusy(mixPulseABtn, "Pulzuji A…", () => mixManualPulse("a")));
      }
      const mixPulseBBtn = document.getElementById("hMixPulseB");
      if(mixPulseBBtn){
        mixPulseBBtn.addEventListener("click", () => withButtonBusy(mixPulseBBtn, "Pulzuji B…", () => mixManualPulse("b")));
      }
      const mixEndABtn = document.getElementById("hMixEndA");
      if(mixEndABtn){
        mixEndABtn.addEventListener("click", () => withButtonBusy(mixEndABtn, "Přejíždím A…", () => mixManualMoveToEnd("a")));
      }
      const mixEndBBtn = document.getElementById("hMixEndB");
      if(mixEndBBtn){
        mixEndBBtn.addEventListener("click", () => withButtonBusy(mixEndBBtn, "Přejíždím B…", () => mixManualMoveToEnd("b")));
      }
      const mixStopBtn = document.getElementById("hMixStop");
      if(mixStopBtn){
        mixStopBtn.addEventListener("click", () => withButtonBusy(mixStopBtn, "Zastavuji…", () => mixManualStop()));
      }

// OpenTherm config actions
      const otBtn = $("#otCfgApply");
      if(otBtn){
        otBtn.addEventListener("click", async () => withButtonBusy(otBtn, "Ukládám…", async () => {
          try{
            state.ot.cfg = state.ot.cfg || {};
            const enabled = !!$("#otEnable")?.checked;
            const pollMs = clamp(Number($("#otPoll")?.value ?? 2000), 100, 60000);
            const mode = String($("#otFailMode")?.value || "control");
            ensureOtConfigCompatible(enabled, mode);
            const allowRawWrite = !!$("#otLog")?.checked;

            await api.postConfigSection("opentherm", {
              enabled,
              autoStart: enabled,
              pollMs,
              mode,
              boilerControl: mode === "control" ? "opentherm" : "relay",
              allowRawWrite
            });
            toast("OpenTherm", "Nastavení uloženo do zařízení.", "✅");
            log(`ot cfg -> /api/config/opentherm enabled=${enabled} pollMs=${pollMs} mode=${mode} raw=${allowRawWrite}`);
            state.dev = state.dev || {};
            state.dev.otCfgLoaded = false;
            await refresh(false);
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("ot cfg error: " + (e.message || e));
          }
        }));
      }

      const pressBtn = $("#pressAlarmApply");
      if(pressBtn){
        pressBtn.addEventListener("click", async () => withButtonBusy(pressBtn, "Ukládám…", async () => {
          try{
            const enabled = !!$("#pressAlarmEnable")?.checked;
            const minBar = clamp(Number($("#pressAlarmMin")?.value ?? 0.8), 0.1, 6.0);
            const maxBar = clamp(Number($("#pressAlarmMax")?.value ?? 2.8), 0.1, 6.0);
            const hysteresisBar = clamp(Number($("#pressAlarmHys")?.value ?? 0.05), 0.01, 1.0);
            await api.postConfigSection("alerts", { pressure: { enabled, minBar, maxBar, hysteresisBar } });
            applyAlertsConfigToForm({ pressure: { enabled, minBar, maxBar, hysteresisBar }});
            toast("Alarm tlaku", "Nastavení uloženo do zařízení.", "✅");
            log(`pressure alarm -> /api/config/alerts enabled=${enabled} min=${minBar} max=${maxBar} hys=${hysteresisBar}`);
            await refresh(false);
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("pressure alarm error: " + (e.message || e));
          }
        }));
      }

      // DHW view actions
      $("#dhwStart").addEventListener("click", async () => withButtonBusy($("#dhwStart"), "Spouštím…", async () => {
        try{
          const requestMode = String(document.getElementById("dhwRequestMode")?.value || "relay");
          const targetTempC = Number(document.getElementById("dhw2Target")?.value || 50);
          await dhwStartFromUi(targetTempC, requestMode);
          toast("TUV", `Spuštěno (${requestMode === "opentherm" ? "OpenTherm" : "R5"}).`, "🚿");
          log(`dhwStart -> ${requestMode}`);
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhwStart error: " + (e.message || e));
        }
      }));
      $("#dhwStop2").addEventListener("click", async () => withButtonBusy($("#dhwStop2"), "Zastavuji…", async () => {
        try{
          await dhwStopFromUi();
          toast("TUV", "Stop.", "⛔");
          log("dhwStop");
          await refresh(false);
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhwStop error: " + (e.message || e));
        }
      }));
      document.getElementById("dhwReloadCfg")?.addEventListener("click", () => withButtonBusy(document.getElementById("dhwReloadCfg"), "Načítám…", async () => {
        try{
          await dhwReloadConfigFromDevice();
          toast("TUV", "Konfigurace načtena ze zařízení.", "✅");
          log("dhw reload config");
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhw reload config error: " + (e.message || e));
        }
      }));
      document.getElementById("dhwSaveCfg")?.addEventListener("click", () => withButtonBusy(document.getElementById("dhwSaveCfg"), "Ukládám…", async () => {
        try{
          await dhwSaveConfigFromUi();
          toast("TUV", "Konfigurace uložena do zařízení.", "✅");
          log("dhw save config");
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhw save config error: " + (e.message || e));
        }
      }));
      $("#dhwBoost2").addEventListener("click", async () => withButtonBusy($("#dhwBoost2"), "Spouštím boost…", async () => {
        try{
          const requestMode = String(document.getElementById("dhwRequestMode")?.value || "relay");
          const targetTempC = Number(document.getElementById("dhw2Target")?.value || 50);
          await dhwBoostFromUi(targetTempC, requestMode, 15);
          toast("TUV", `Boost 15 min (${requestMode === "opentherm" ? "OpenTherm" : "R5"}).`, "⚡");
          log(`dhwBoost -> ${requestMode} 15 min`);
          await refresh(false);
          return;
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("dhwBoost error: " + (e.message || e));
          return;
        }
      }));

      // IO mass actions
      $("#ioAllOn").addEventListener("click", async () => {
        try{
          const res = await setUnmanagedRelayMask(0xFF);
          toast("Relé", res.managedSkipped ? "Zapnuta jen ruční relé. TUV/cirkulace zůstaly řízené funkcemi." : "Zapnuto vše (zařízení).", "✅");
          renderIO(); renderOverviewBadges();
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("io all on error: " + (e.message || e));
        }
      });
      $("#ioAllOff").addEventListener("click", async () => {
        try{
          const res = await setUnmanagedRelayMask(0x00);
          toast("Relé", res.managedSkipped ? "Vypnuta jen ruční relé. TUV/cirkulace zůstaly řízené funkcemi." : "Vypnuto vše (zařízení).", "⛔");
          renderIO(); renderOverviewBadges();
        }catch(e){
          toast("Chyba", e.message || String(e), "⚠");
          log("io all vypnuto error: " + (e.message || e));
        }
      });

      // Copy diag
      $("#btnCopy").addEventListener("click", async () => {
        const s = [
          `device=${$("#dName").textContent}`,
          `ip=${$("#dIp").textContent}`,
          `uptime=${$("#dUp").textContent}`,
          `source=device`,
          `apiBase=${state.apiBase || "(origin)"}`,
          `out=${$("#kpiOut").textContent}`,
          `ch=${$("#kpiCH").textContent}`,
          `dhw=${$("#kpiDHW").textContent}`,
          `pr=${$("#kpiPr").textContent}`,
          `heapFree=${fmtBytes(state?.diag?.heap?.free)}`,
          `heapMin=${fmtBytes(state?.diag?.heap?.minFree)}`,
          `heapMaxAlloc=${fmtBytes(state?.diag?.heap?.maxAlloc)}`,
        ].join("\n");
        try{
          await navigator.clipboard.writeText(s);
          toast("Zkopírováno", "Diagnostika je v schránce.", "📋");
        }catch{
          toast("Nelze kopírovat", "Prohlížeč nepovolil clipboard.", "⚠");
        }
      });

      // Diag view: API base save
      $("#apiSave").addEventListener("click", () => {
        state.apiBase = $("#apiBase").value.trim();
        localStorage.setItem("ui2026_apiBase", state.apiBase);
        syncApiBaseUi();
        toast("Uloženo", "Base URL nastaveno.", "✅");
        log(`apiBase set to: ${state.apiBase || "(origin)"}`);
      });

      const mqttRefreshBtn = document.getElementById("mqttRefresh");
      if(mqttRefreshBtn){
        mqttRefreshBtn.addEventListener("click", () => withButtonBusy(mqttRefreshBtn, "Načítám…", () => mqttLoad({ silent:false })));
      }
      const mqttSaveBtn = document.getElementById("mqttSave");
      if(mqttSaveBtn){
        mqttSaveBtn.addEventListener("click", () => withButtonBusy(mqttSaveBtn, "Ukládám…", () => mqttSave()));
      }
      document.getElementById("timeRefresh")?.addEventListener("click", () => withButtonBusy(document.getElementById("timeRefresh"), "Načítám…", () => timeLoad({ silent:false })));
      document.getElementById("timeSave")?.addEventListener("click", () => withButtonBusy(document.getElementById("timeSave"), "Ukládám…", () => timeSave()));
      document.getElementById("eventsRefresh")?.addEventListener("click", () => withButtonBusy(document.getElementById("eventsRefresh"), "Načítám…", () => eventsLoad({ silent:false })));
      document.getElementById("eventsClear")?.addEventListener("click", async () => withButtonBusy(document.getElementById("eventsClear"), "Mažu…", async () => { await api.postJson("/api/events/clear", {}); await eventsLoad({ silent:true }); toast("Event log", "Vymazáno.", "🧹"); }));
      document.getElementById("historyRefresh")?.addEventListener("click", () => withButtonBusy(document.getElementById("historyRefresh"), "Načítám…", () => historyLoad({ silent:false })));
      document.getElementById("historyClear")?.addEventListener("click", async () => withButtonBusy(document.getElementById("historyClear"), "Mažu…", async () => { await api.postJson("/api/history/clear", {}); await historyLoad({ silent:true }); toast("Historie", "Vymazána.", "🧹"); }));
      document.getElementById("serviceRelayOn")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceRelayOn"), "Odesílám…", async () => { await serviceIoCall({ relay: Number(document.getElementById("serviceRelay")?.value || 1), on: true }); toast("Servis I/O", "Relé zapnuto.", "✅"); await refresh(false); }));
      document.getElementById("serviceRelayOff")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceRelayOff"), "Odesílám…", async () => { await serviceIoCall({ relay: Number(document.getElementById("serviceRelay")?.value || 1), on: false }); toast("Servis I/O", "Relé vypnuto.", "⭕"); await refresh(false); }));
      document.getElementById("serviceRelayPulse")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceRelayPulse"), "Puls…", async () => { await serviceIoCall({ pulseRelay: Number(document.getElementById("serviceRelay")?.value || 1), pulseMs: 500 }); toast("Servis I/O", "Puls odeslán.", "🧪"); await refresh(false); }));
      document.getElementById("serviceBuzzerStartup")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceBuzzerStartup"), "Odesílám…", async () => { await serviceIoCall({ buzzer: "startup" }); toast("Servis I/O", "Buzzer startup.", "🔔"); }));
      document.getElementById("serviceBuzzerWarn")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceBuzzerWarn"), "Odesílám…", async () => { await serviceIoCall({ buzzer: "warning" }); toast("Servis I/O", "Buzzer warning.", "🔔"); }));
      document.getElementById("serviceBuzzerOff")?.addEventListener("click", async () => withButtonBusy(document.getElementById("serviceBuzzerOff"), "Odesílám…", async () => { await serviceIoCall({ buzzer: "off" }); toast("Servis I/O", "Buzzer vypnut.", "🔕"); }));

      // Diag ping + export
      $("#diagPing").addEventListener("click", async () => withButtonBusy($("#diagPing"), "Ping…", async () => {
        try{
          setSource("device");
          await refresh(true);
        }catch{}
      }));
      const diagExportBtn = $("#diagExport");
      if(diagExportBtn){
        diagExportBtn.addEventListener("click", () => withButtonBusy(diagExportBtn, "Exportuji…", async () => {
          try{
            await diagExportConfig();
            toast("Konfigurace", "Stažen export konfigurace zařízení.", "⬇");
          }catch(e){
            toast("Chyba", e.message || String(e), "⚠");
            log("diag export error: " + (e.message || e));
          }
        }));
      }

      const diagImportBtn = $("#diagImport");
      const diagImportFile = document.getElementById("diagImportFile");
      if(diagImportBtn && diagImportFile){
        diagImportBtn.addEventListener("click", () => { diagImportFile.value = ""; diagImportFile.click(); });
        diagImportFile.addEventListener("change", () => {
          const file = diagImportFile.files && diagImportFile.files[0];
          if(!file) return;
          withButtonBusy(diagImportBtn, "Importuji…", async () => {
            try{
              const resp = await diagImportConfigFile(file);
              const imported = Number(resp?.importedSections ?? 0);
              toast("Konfigurace", `Import hotov (${imported} sekcí).`, "✅");
            }catch(e){
              toast("Chyba", e.message || String(e), "⚠");
              log("diag import error: " + (e.message || e));
            }
          });
        });
      }

      $("#logClear").addEventListener("click", () => { $("#log").textContent=""; toast("Log", "Smazáno.", "🧹"); });
      $("#otaFwUploadBtn")?.addEventListener("click", async () => { try{ await uploadWithProgress("/api/update/firmware", "otaFwFile", "otaFw"); }catch(e){ updateUploadProgress("otaFw", 0, 0, e.message || String(e), false); toast("OTA", e.message || String(e), "⚠"); } });
      $("#otaFsUploadBtn")?.addEventListener("click", async () => { try{ await uploadWithProgress("/api/update/filesystem", "otaFsFile", "otaFs"); }catch(e){ updateUploadProgress("otaFs", 0, 0, e.message || String(e), false); toast("OTA", e.message || String(e), "⚠"); } });

      // Keyboard shortcuts
      window.addEventListener("keydown", (e) => {
        if(e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey || isEditableTarget(e.target)) return;
        if(e.key === "t" || e.key === "T"){ $("#btnTheme").click(); }
        if(e.key === "r" || e.key === "R"){ withButtonBusy($("#btnRefresh"), "Obnovuji…", () => refresh(true)); }
      });
    }

    window.addEventListener("error", e => {
  console.error("UI error:", e.message);
  log("ui error: " + (e.message || e.error || "unknown"));
});

// ----- Boot
    // ----- Boot
function boot(){
  applyTheme();

  maybeAdoptPageOriginBase("boot", true);
  syncApiBaseUi();

  setSource("device");

  setText("#dName", "ESP32");
  setText("#dIp", "--");
  setText("#dBuild", "UI 2026");
  if(!Number.isFinite(Number(state.ot.maxCapacityKw))) state.ot.maxCapacityKw = 9;

  wire();
  renderHeatingOtInfo();
  renderMixCalibrationInfo();
  renderDhwBoilerMode();

  window.addEventListener("hashchange", () => {
    const hv2 = (location.hash || "#overview").replace("#", "");
    if(titles[hv2] && getActiveView() !== hv2) setView(hv2);
  });
  document.addEventListener("visibilitychange", handleVisibilityChange);

  document.getElementById("thRefresh")?.addEventListener("click", () => withButtonBusy(document.getElementById("thRefresh"), "Načítám…", () => thermoLoad({ silent:false })));
  document.getElementById("thSave")?.addEventListener("click", () => withButtonBusy(document.getElementById("thSave"), "Ukládám…", () => thermoSave()));
  document.getElementById("dallasEnable")?.addEventListener("change", (e) => { state.th.dallasEnabled = !!e.target.checked; });

  renderThermometersDevice();
  $("#serviceCountersReset")?.addEventListener("click", resetServiceCounters);

  const hv = (location.hash || "#overview").replace("#","");
  if(titles[hv]) setView(hv);

  stopFallbackPolling();
  updateRefreshCadence();

  const bootLoad = async () => {
    let bootstrapped = false;
    connectWs();
    const bootstrapPromise = api.fetchBootstrap()
      .then((payload) => applyBootstrapPayload(payload))
      .catch(() => false);
    const wsReady = await waitForFirstFastSnapshot(350);
    if(!wsReady){
      try{
        bootstrapped = await bootstrapPromise;
      }catch(_e){}
    }
    if(!bootstrapped && !state.last){
      try{
        await refresh(false);
      }catch(_e){}
    }
    log(bootstrapped ? "boot ok (bootstrap)" : (state.last ? (wsReady ? "boot ok (ws-first)" : "boot ok (ws)") : "boot ok"));
  };
  void bootLoad();
}
boot();
