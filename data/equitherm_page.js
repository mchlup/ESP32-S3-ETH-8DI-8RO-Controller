// equitherm_page.js
// Ekviterm – graf denní/noční křivky + konfigurace kompatibilní s backendem (LogicController.cpp).
// Ukládá se do /config.json přes POST /api/config/apply (celý JSON dokument jako text).

window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  const clamp = (v,a,b)=>Math.max(a,Math.min(b,v));
  const num = (v,def)=>{
    const n = Number(v);
    return Number.isFinite(n) ? n : def;
  };
  const fmt = (v,d=1)=> (v==null || !Number.isFinite(Number(v))) ? '—' : Number(v).toFixed(d);

  // ---------- config helpers ----------
  function ensurePath(obj, pathArr, defVal){
    let o = obj;
    for(let i=0;i<pathArr.length;i++){
      const k = pathArr[i];
      if(i === pathArr.length-1){
        if(o[k] == null) o[k] = defVal;
        return o[k];
      }
      if(typeof o[k] !== 'object' || o[k] == null) o[k] = {};
      o = o[k];
    }
    return defVal;
  }

  function getConfig(){
    const cfg = (App.state && App.state.config && typeof App.state.config === 'object') ? App.state.config : null;
    return cfg;
  }

  async function loadConfig(){
    if(!App.state) App.state = {};
    if(App.state.config && typeof App.state.config === 'object') return App.state.config;

    let txt = '';
    try {
      txt = await App.api.getText('/config.json');
      App.state.config = JSON.parse(txt);
    } catch(e){
      // pokud config neexistuje, vytvoříme prázdný objekt (backend umí apply vytvořit)
      App.state.config = {};
    }
    return App.state.config;
  }

  function getEqCfg(cfg){
    cfg = cfg || {};
    const eq = ensurePath(cfg, ['equitherm'], {});

    // Defaults konzistentní s firmware:
    if(eq.enabled == null) eq.enabled = false;
    if(eq.slopeDay == null) eq.slopeDay = 1.6;   // Et1 default dle manuálu (P1.1)
    if(eq.shiftDay == null) eq.shiftDay = 0.0;   // Et2
    if(eq.slopeNight == null) eq.slopeNight = 1.6; // Et3
    if(eq.shiftNight == null) eq.shiftNight = 0.0; // Et4

    if(eq.minFlow == null) eq.minFlow = 25;
    if(eq.maxFlow == null) eq.maxFlow = 75;
    if(eq.curveOffsetC == null) eq.curveOffsetC = 0;
    if(eq.maxBoilerInC == null) eq.maxBoilerInC = 80;
    if(eq.fallbackOutdoorC == null) eq.fallbackOutdoorC = 0;

    if(eq.noFlowDetectEnabled == null) eq.noFlowDetectEnabled = false;
    if(eq.noFlowTimeoutMs == null) eq.noFlowTimeoutMs = 120000;
    if(eq.noFlowTestPeriodMs == null) eq.noFlowTestPeriodMs = 30000;

    const ctrl = ensurePath(eq, ['control'], {});
    if(ctrl.deadbandC == null) ctrl.deadbandC = 0.5;
    if(ctrl.stepPct == null) ctrl.stepPct = 0; // 0 = auto step (firmware)
    if(ctrl.periodMs == null) ctrl.periodMs = 15000;
    if(ctrl.minPct == null) ctrl.minPct = 0;
    if(ctrl.maxPctDay == null) ctrl.maxPctDay = 100;
    if(ctrl.maxPctNight == null) ctrl.maxPctNight = 100;

    return eq;
  }

  function getMixValveParams(cfg){
    // V projektu je směšovací ventil pevně R1+R2 (master = outputs[0], peer = outputs[1]).
    // Parametry jsou v iofunc.outputs[0].params:
    // travelTimeS, pulseTimeS, guardTimeS, minSwitchS, invertDir, defaultPos
    cfg = cfg || {};
    const out0 = ensurePath(cfg, ['iofunc','outputs','0'], {});
    const p = ensurePath(out0, ['params'], {});

    // Defaults konzistentní s firmware (viz configureValve2Rel): travel=6s, pulse=0.6s, guard=0.3s, minSwitch=1s
    if(p.travelTimeS == null && p.travelTime == null) p.travelTimeS = 6.0;
    if(p.pulseTimeS == null && p.pulseTime == null) p.pulseTimeS = 0.6;
    if(p.guardTimeS == null && p.guardTime == null) p.guardTimeS = 0.3;
    if(p.minSwitchS == null && p.minSwitch == null) p.minSwitchS = 1.0;
    if(p.invertDir == null) p.invertDir = false;
    if(p.defaultPos == null) p.defaultPos = 'A';

    // Normalizuj na *S* klíče, aby ukládání bylo konzistentní.
    if(p.travelTimeS == null) p.travelTimeS = num(p.travelTime, 6.0);
    if(p.pulseTimeS == null)  p.pulseTimeS  = num(p.pulseTime, 0.6);
    if(p.guardTimeS == null)  p.guardTimeS  = num(p.guardTime, 0.3);
    if(p.minSwitchS == null)  p.minSwitchS  = num(p.minSwitch, 1.0);

    return p;
  }

  // ---------- equation (matches firmware eqComputeTargetFromSlopeShift) ----------
  function eqTarget(tout, slope, shift){
    // firmware: (20 - tout) * slope + 20 + shift
    return (20 - tout) * slope + 20 + shift;
  }

  // ---------- graph ----------
  function drawGraph(canvas, model){
    const ctx = canvas.getContext('2d');
    const W = canvas.width, H = canvas.height;

    // plot area
    const padL=58, padR=18, padT=18, padB=40;
    const x0=padL, y0=padT, x1=W-padR, y1=H-padB;
    const pw=x1-x0, ph=y1-y0;

    // axes ranges
    const tMin = -20, tMax = 20;
    let yMin = num(model.eq.minFlow, 25);
    let yMax = num(model.eq.maxFlow, 75);
    if(yMin > yMax){ const tmp=yMin; yMin=yMax; yMax=tmp; }
    // keep sane range
    yMin = clamp(yMin, 0, 80);
    yMax = clamp(yMax, 0, 80);
    if(Math.abs(yMax-yMin) < 5){ yMax = clamp(yMin+5, 0, 80); }

    const xFromT = t => x0 + (t - tMin) / (tMax - tMin) * pw;
    const yFromV = v => y1 - (v - yMin) / (yMax - yMin) * ph;

    // bg
    ctx.clearRect(0,0,W,H);
    ctx.fillStyle = '#0b0f14';
    ctx.fillRect(0,0,W,H);

    // grid
    ctx.strokeStyle = 'rgba(255,255,255,.08)';
    ctx.lineWidth = 1;
    for(let t=-20; t<=20; t+=5){
      const x = xFromT(t);
      ctx.beginPath(); ctx.moveTo(x,y0); ctx.lineTo(x,y1); ctx.stroke();
    }
    const stepY = 5;
    for(let v=Math.floor(yMin/stepY)*stepY; v<=yMax; v+=stepY){
      const y = yFromV(v);
      ctx.beginPath(); ctx.moveTo(x0,y); ctx.lineTo(x1,y); ctx.stroke();
    }

    // axes
    ctx.strokeStyle = 'rgba(255,255,255,.25)';
    ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(x0,y0); ctx.lineTo(x0,y1); ctx.lineTo(x1,y1); ctx.stroke();

    // tick labels
    ctx.fillStyle = 'rgba(231,238,252,.85)';
    ctx.font = '12px system-ui, sans-serif';
    for(let t=-20; t<=20; t+=10){
      const x = xFromT(t);
      ctx.fillText(String(t), x-8, y1+18);
    }
    for(let v=Math.ceil(yMin/10)*10; v<=yMax; v+=10){
      const y = yFromV(v);
      ctx.fillText(String(v), 8, y+4);
    }

    // axis labels
    ctx.fillStyle = 'rgba(231,238,252,.9)';
    ctx.fillText('Venkovní teplota Tout (°C)', x0 + 10, H-10);
    ctx.save();
    ctx.translate(14, y0 + ph/2);
    ctx.rotate(-Math.PI/2);
    ctx.fillText('Požadovaná teplota topné vody Tset (°C)', 0, 0);
    ctx.restore();

    // curves
    function curve(slope, shift, color){
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      for(let t=tMin; t<=tMax; t+=1){
        let sp = eqTarget(t, slope, shift) + num(model.eq.curveOffsetC,0);
        sp = clamp(sp, yMin, yMax);
        const x = xFromT(t);
        const y = yFromV(sp);
        if(t===tMin) ctx.moveTo(x,y);
        else ctx.lineTo(x,y);
      }
      ctx.stroke();
    }

    curve(num(model.eq.slopeDay,1.6), num(model.eq.shiftDay,0), '#2e7dff');
    curve(num(model.eq.slopeNight,1.6), num(model.eq.shiftNight,0), '#ff9800');

    // current point
    const tout = num(model.outdoorC, 0);
    const isNight = !!model.night;
    const slope = isNight ? num(model.eq.slopeNight,1.6) : num(model.eq.slopeDay,1.6);
    const shift = isNight ? num(model.eq.shiftNight,0) : num(model.eq.shiftDay,0);
    let sp = eqTarget(tout, slope, shift) + num(model.eq.curveOffsetC,0);
    sp = clamp(sp, yMin, yMax);

    const cx = xFromT(clamp(tout, tMin, tMax));
    const cy = yFromV(sp);
    ctx.fillStyle = '#00e676';
    ctx.beginPath(); ctx.arc(cx, cy, 5, 0, Math.PI*2); ctx.fill();

    // legend
    ctx.fillStyle = 'rgba(231,238,252,.85)';
    ctx.fillText('Denní (Et1/Et2)', x0+10, y0+14);
    ctx.fillStyle = '#2e7dff'; ctx.fillRect(x0+112, y0+7, 16, 3);

    ctx.fillStyle = 'rgba(231,238,252,.85)';
    ctx.fillText('Noční (Et3/Et4)', x0+150, y0+14);
    ctx.fillStyle = '#ff9800'; ctx.fillRect(x0+258, y0+7, 16, 3);

    ctx.fillStyle = 'rgba(231,238,252,.75)';
    ctx.fillText(`Tout=${fmt(tout,1)}°C, Tset=${fmt(sp,1)}°C  (${isNight?'noční':'denní'})`, x0+10, y0+32);
  }

  // ---------- page state ----------
  const model = {
    cfg: null,
    eq: null,
    valve: null,
    // live values
    eqEnabledLive: false,
    eqActiveLive: false,
    night: false,
    outdoorC: 0,
    outdoorSrc: "",
    valveMaster: 1,
    flowC: null,
    targetC: null,
    valvePosPct: null,
    mv: false,
    vt: null,
  };

  function render(root){
    root.innerHTML = `
      <div class="pageHead">
        <div class="pageTitle">Ekviterm</div>
        <div class="pageActions">
          <button class="btn" id="eqLoadBtn">Načíst config</button>
          <button class="btn" id="eqSaveBtn">Uložit ekviterm</button>
        </div>
      </div>

      <div class="grid12">
        <div class="card col12">
          <h2>Graf křivek</h2>
          <canvas id="eqGraph" width="980" height="420" style="width:100%;height:auto;border-radius:14px;border:1px solid rgba(255,255,255,.08);"></canvas>
          <div class="hint">Zelený bod = aktuální požadavek (počítáno z venkovní teploty a zvolené denní/noční křivky + curveOffsetC). Křivky odpovídají vzorcům z manuálu kotle THERM EL: <span class="mono">Tset = (20 − Tout) * EtX + 20 + EtY</span>.</div>
        </div>

        <div class="card col6">
          <h2>Aktuální stav</h2>
          <div class="kv">
            <div class="k">Režim</div><div class="v" id="eqMode">—</div>
            <div class="k">Ekviterm</div><div class="v"><span id="eqEnabledLive">—</span> <span class="small" id="eqActiveLive" style="margin-left:10px;opacity:.85"></span></div>
            <div class="k">Povolit ekviterm</div><div class="v"><label class="chk"><input id="eqEnabledCfg" type="checkbox" /> <span class="small">uloží se hned</span></label></div>
            <div class="k">Venkovní teplota</div><div class="v"><span id="eqTout">—</span> °C <span class="small" id="eqToutSrc" style="opacity:.8"></span></div>
            <div class="k">Cíl (Tset)</div><div class="v"><span id="eqTset">—</span> °C</div>
            <div class="k">Rozdíl (Tset − Flow)</div><div class="v"><span id="eqDelta">—</span> °C <span class="small" id="eqDeltaHint" style="opacity:.8"></span></div>
            <div class="k">Ventil pozice</div><div class="v"><span id="eqValvePct">—</span> %</div>
            <div class="k">Ventil cíl</div><div class="v"><span id="eqValveTarget">—</span> %</div>
            <div class="k">Ventil pohyb</div><div class="v" id="eqValveMoving">—</div>
          </div>
        </div>

        <div class="card col6">
          <h2>Parametry křivek (Et1..Et4)</h2>
          <div class="rolesGrid" style="margin-top:8px">
            <div>
              <div class="small" style="margin-bottom:6px;opacity:.9">Denní (NU rozpojeno)</div>
              <label class="lbl small">Et1 – sklon: <span class="mono" id="vEt1"></span></label>
              <input id="Et1" type="range" min="0.2" max="3.0" step="0.1" />
              <label class="lbl small">Et2 – posun: <span class="mono" id="vEt2"></span></label>
              <input id="Et2" type="range" min="-20" max="20" step="0.5" />
            </div>
            <div>
              <div class="small" style="margin-bottom:6px;opacity:.9">Noční (NU spojeno)</div>
              <label class="lbl small">Et3 – sklon: <span class="mono" id="vEt3"></span></label>
              <input id="Et3" type="range" min="0.2" max="3.0" step="0.1" />
              <label class="lbl small">Et4 – posun: <span class="mono" id="vEt4"></span></label>
              <input id="Et4" type="range" min="-20" max="20" step="0.5" />
            </div>
          </div>
        </div>

        <div class="card col6">
          <h2>Limity a korekce (kotel)</h2>
          <div class="kv" style="grid-template-columns: 200px 1fr;">
            <div class="k">Dolní mez teploty TOP</div><div class="v"><input id="minFlow" type="number" min="0" max="80" step="0.5" style="width:140px" /> °C</div>
            <div class="k">Horní mez teploty TOP</div><div class="v"><input id="maxFlow" type="number" min="0" max="80" step="0.5" style="width:140px" /> °C</div>
            <div class="k">Korekce křivky (curveOffsetC)</div><div class="v"><input id="curveOffsetC" type="number" min="-10" max="10" step="0.1" style="width:140px" /> °C</div>
            <div class="k">Max. teplota kotle (maxBoilerInC)</div><div class="v"><input id="maxBoilerInC" type="number" min="20" max="80" step="0.5" style="width:140px" /> °C</div>
            <div class="k">Fallback venkovní teplota</div><div class="v"><input id="fallbackOutdoorC" type="number" min="-40" max="60" step="0.5" style="width:140px" /> °C</div>
          </div>
          <div class="hint">Pozn.: V manuálu je maximální teplota topné vody uvedena jako 80 °C.</div>
        </div>

        <div class="card col6">
          <h2>Regulace směšovacího ventilu (ekviterm)</h2>
          <div class="kv" style="grid-template-columns: 220px 1fr;">
            <div class="k">Hystereze (deadbandC)</div><div class="v"><input id="deadbandC" type="number" min="0" max="5" step="0.1" style="width:140px" /> °C</div>
            <div class="k">Krok (stepPct, 0 = auto)</div><div class="v"><input id="stepPct" type="number" min="0" max="25" step="1" style="width:140px" /> %</div>
            <div class="k">Perioda (periodMs)</div><div class="v"><input id="periodMs" type="number" min="500" max="600000" step="100" style="width:140px" /> ms</div>
            <div class="k">Min. otevření (minPct)</div><div class="v"><input id="minPct" type="number" min="0" max="100" step="1" style="width:140px" /> %</div>
            <div class="k">Max. otevření den</div><div class="v"><input id="maxPctDay" type="number" min="0" max="100" step="1" style="width:140px" /> %</div>
            <div class="k">Max. otevření noc</div><div class="v"><input id="maxPctNight" type="number" min="0" max="100" step="1" style="width:140px" /> %</div>
          </div>
        </div>

        <div class="card col6">
          <h2>Parametry směšovacího ventilu (R1+R2)</h2>
          <div class="kv" style="grid-template-columns: 250px 1fr;">
            <div class="k">Doba přesunu A↔B (travelTimeS)</div><div class="v"><input id="travelTimeS" type="number" min="1" max="600" step="0.1" style="width:140px" /> s</div>
            <div class="k">Délka pulzu (pulseTimeS)</div><div class="v"><input id="pulseTimeS" type="number" min="0.05" max="10" step="0.05" style="width:140px" /> s</div>
            <div class="k">Ochranná prodleva (guardTimeS)</div><div class="v"><input id="guardTimeS" type="number" min="0" max="10" step="0.05" style="width:140px" /> s</div>
            <div class="k">Min. přepnutí směru (minSwitchS)</div><div class="v"><input id="minSwitchS" type="number" min="0" max="600" step="0.1" style="width:140px" /> s</div>
            <div class="k">Invertovat směr (invertDir)</div><div class="v"><label class="chk"><input id="invertDir" type="checkbox" /> <span class="small">prohoď OPEN/CLOSE</span></label></div>
            <div class="k">Výchozí poloha (defaultPos)</div><div class="v">
              <select id="defaultPos" class="sel" style="width:140px">
                <option value="A">A (0%)</option>
                <option value="B">B (100%)</option>
              </select>
            </div>
          </div>
          <div class="btnRow" style="margin-top:10px;display:flex;gap:8px;align-items:center;flex-wrap:wrap">
            <button class="btn sm" id="btnValveOpen">Otevřít krok</button>
            <button class="btn sm" id="btnValveClose">Zavřít krok</button>
            <button class="btn sm" id="btnValveStop">STOP</button>
            <span class="small" id="eqValveTestInfo" style="opacity:.75"></span>
          </div>
          <div class="hint">Tyto parametry odpovídají backendu (<span class="mono">iofunc.outputs[0].params</span>). Směšovací ventil je v tomto projektu pevně R1+R2.</div>
        </div>

        <div class="card col6">
          <h2>Diagnostika průtoku (volitelné)</h2>
          <div class="kv" style="grid-template-columns: 260px 1fr;">
            <div class="k">Detekce „no flow“</div><div class="v"><label class="chk"><input id="noFlowDetectEnabled" type="checkbox" /> <span class="small">povolit</span></label></div>
            <div class="k">Timeout (noFlowTimeoutMs)</div><div class="v"><input id="noFlowTimeoutMs" type="number" min="10000" max="3600000" step="1000" style="width:140px" /> ms</div>
            <div class="k">Test perioda (noFlowTestPeriodMs)</div><div class="v"><input id="noFlowTestPeriodMs" type="number" min="10000" max="3600000" step="1000" style="width:140px" /> ms</div>
          </div>
        </div>

      </div>
    `;
  }

  function syncUI(){
    const eq = model.eq;
    const ctrl = eq.control || {};
    const v = model.valve;

    // curve
    Et1.value = num(eq.slopeDay, 1.6);
    Et2.value = num(eq.shiftDay, 0);
    Et3.value = num(eq.slopeNight, 1.6);
    Et4.value = num(eq.shiftNight, 0);

    vEt1.textContent = fmt(Et1.value,2);
    vEt2.textContent = fmt(Et2.value,1);
    vEt3.textContent = fmt(Et3.value,2);
    vEt4.textContent = fmt(Et4.value,1);

    // boiler limits
    minFlow.value = num(eq.minFlow, 25);
    maxFlow.value = num(eq.maxFlow, 75);
    curveOffsetC.value = num(eq.curveOffsetC, 0);
    maxBoilerInC.value = num(eq.maxBoilerInC, 80);
    fallbackOutdoorC.value = num(eq.fallbackOutdoorC, 0);

    // control
    deadbandC.value = num(ctrl.deadbandC, 0.5);
    stepPct.value = num(ctrl.stepPct, 0);
    periodMs.value = num(ctrl.periodMs, 15000);
    minPct.value = num(ctrl.minPct, 0);
    maxPctDay.value = num(ctrl.maxPctDay, 100);
    maxPctNight.value = num(ctrl.maxPctNight, 100);

    // valve params
    travelTimeS.value = num(v.travelTimeS, 6.0);
    pulseTimeS.value = num(v.pulseTimeS, 0.6);
    guardTimeS.value = num(v.guardTimeS, 0.3);
    minSwitchS.value = num(v.minSwitchS, 1.0);
    invertDir.checked = !!v.invertDir;
    defaultPos.value = (String(v.defaultPos||'A').toUpperCase()==='B') ? 'B' : 'A';

    // no-flow
    noFlowDetectEnabled.checked = !!eq.noFlowDetectEnabled;
    noFlowTimeoutMs.value = num(eq.noFlowTimeoutMs, 120000);
    noFlowTestPeriodMs.value = num(eq.noFlowTestPeriodMs, 30000);

    redraw();
  }

  function readUIToModel(){
    const eq = model.eq;
    const ctrl = ensurePath(eq, ['control'], {});
    const v = model.valve;

    eq.slopeDay = num(Et1.value, eq.slopeDay);
    eq.shiftDay = num(Et2.value, eq.shiftDay);
    eq.slopeNight = num(Et3.value, eq.slopeNight);
    eq.shiftNight = num(Et4.value, eq.shiftNight);

    eq.minFlow = num(minFlow.value, eq.minFlow);
    eq.maxFlow = num(maxFlow.value, eq.maxFlow);
    eq.curveOffsetC = num(curveOffsetC.value, eq.curveOffsetC);
    eq.maxBoilerInC = num(maxBoilerInC.value, eq.maxBoilerInC);
    eq.fallbackOutdoorC = num(fallbackOutdoorC.value, eq.fallbackOutdoorC);

    ctrl.deadbandC = num(deadbandC.value, ctrl.deadbandC);
    ctrl.stepPct = Math.round(num(stepPct.value, ctrl.stepPct));
    ctrl.periodMs = Math.round(num(periodMs.value, ctrl.periodMs));
    ctrl.minPct = Math.round(num(minPct.value, ctrl.minPct));
    ctrl.maxPctDay = Math.round(num(maxPctDay.value, ctrl.maxPctDay));
    ctrl.maxPctNight = Math.round(num(maxPctNight.value, ctrl.maxPctNight));

    v.travelTimeS = num(travelTimeS.value, v.travelTimeS);
    v.pulseTimeS = num(pulseTimeS.value, v.pulseTimeS);
    v.guardTimeS = num(guardTimeS.value, v.guardTimeS);
    v.minSwitchS = num(minSwitchS.value, v.minSwitchS);
    v.invertDir = !!invertDir.checked;
    v.defaultPos = defaultPos.value;

    eq.noFlowDetectEnabled = !!noFlowDetectEnabled.checked;
    eq.noFlowTimeoutMs = Math.round(num(noFlowTimeoutMs.value, eq.noFlowTimeoutMs));
    eq.noFlowTestPeriodMs = Math.round(num(noFlowTestPeriodMs.value, eq.noFlowTestPeriodMs));

    // update value labels
    vEt1.textContent = fmt(eq.slopeDay,2);
    vEt2.textContent = fmt(eq.shiftDay,1);
    vEt3.textContent = fmt(eq.slopeNight,2);
    vEt4.textContent = fmt(eq.shiftNight,1);
  }

  function computeCurrentSetpoint(){
    const eq = model.eq;
    const slope = model.night ? num(eq.slopeNight,1.6) : num(eq.slopeDay,1.6);
    const shift = model.night ? num(eq.shiftNight,0) : num(eq.shiftDay,0);
    const tout = num(model.outdoorC, 0);
    let sp = eqTarget(tout, slope, shift) + num(eq.curveOffsetC, 0);
    let mn = num(eq.minFlow, 25);
    let mx = num(eq.maxFlow, 75);
    if(mn>mx){ const t=mn; mn=mx; mx=t; }
    sp = clamp(sp, mn, mx);
    return sp;
  }

  function redraw(){
    // status
    eqMode.textContent = model.night ? 'Noční' : 'Denní';
    eqEnabledLive.textContent = model.eqEnabledLive ? 'ZAP' : 'VYP';
    eqActiveLive.textContent = model.eqActiveLive ? 'aktivní' : '';
    eqTout.textContent = fmt(model.outdoorC,1);
    eqToutSrc.textContent = model.outdoorSrc ? `(${model.outdoorSrc})` : '';
    // sync config toggle (only if cfg loaded)
    if(model.eq && eqEnabledCfg){ eqEnabledCfg.checked = !!model.eq.enabled; }

    const sp = computeCurrentSetpoint();
    eqTset.textContent = fmt(sp,1);

    // Δ = target - flow (from backend if available, else from computed setpoint)
    const tgt = Number.isFinite(model.targetC) ? model.targetC : sp;
    const flo = Number.isFinite(model.flowC) ? model.flowC : null;
    let d = null;
    if (flo!=null && Number.isFinite(flo) && Number.isFinite(tgt)) d = tgt - flo;
    const db = num(model.eq?.control?.deadbandC, 0.5);
    const eqDelta = document.getElementById('eqDelta');
    const eqDeltaHint = document.getElementById('eqDeltaHint');
    if (eqDelta) eqDelta.textContent = (d==null) ? '—' : fmt(d,1);
    if (eqDeltaHint) {
      let hint = '';
      if (d!=null && Number.isFinite(d)) {
        const vp = Number.isFinite(model.valvePosPct) ? Math.round(model.valvePosPct) : null;
        if (vp===0 && d < -db) hint = 'na minimu (zavřeno)';
        else if (vp===100 && d > db) hint = 'na maximu (otevřeno)';
        else if (Math.abs(d) <= db) hint = 'v pásmu hystereze';
        else hint = 'čeká na další korekci';
      }
      eqDeltaHint.textContent = hint ? `• ${hint}` : '';
    }

    eqValvePct.textContent = (model.valvePosPct==null) ? '—' : fmt(model.valvePosPct,0);
    eqValveTarget.textContent = (model.vt==null) ? '—' : fmt(model.vt,0);
    eqValveMoving.textContent = model.mv ? 'ANO' : 'ne';

    drawGraph(eqGraph, model);
  }

  function bind(){
    const onAny = ()=>{ readUIToModel(); redraw(); };

    // sliders
    Et1.oninput = onAny;
    Et2.oninput = onAny;
    Et3.oninput = onAny;
    Et4.oninput = onAny;

    // numbers
    for (const id of ['minFlow','maxFlow','curveOffsetC','maxBoilerInC','fallbackOutdoorC','deadbandC','stepPct','periodMs','minPct','maxPctDay','maxPctNight','travelTimeS','pulseTimeS','guardTimeS','minSwitchS','noFlowTimeoutMs','noFlowTestPeriodMs']){
      document.getElementById(id).addEventListener('input', onAny);
    }
    invertDir.addEventListener('change', onAny);
    defaultPos.addEventListener('change', onAny);
    noFlowDetectEnabled.addEventListener('change', onAny);

    // enable/disable ekviterm (apply immediately)
    let eqEnableDebounce = null;
    eqEnabledCfg.addEventListener('change', async ()=>{
      if(!model.eq) return;
      model.eq.enabled = !!eqEnabledCfg.checked;
      // apply only equitherm.enabled change (keep the rest as is)
      if(eqEnableDebounce) clearTimeout(eqEnableDebounce);
      eqEnableDebounce = setTimeout(async ()=>{
        try{
          const json = JSON.stringify(model.cfg || {}, null, 2);
          await App.api.postText('/api/config/apply', json, 'application/json');
          App.util && App.util.toast && App.util.toast(model.eq.enabled ? 'Ekviterm zapnut' : 'Ekviterm vypnut', 'ok');
        }catch(e){
          console.error(e);
          App.util && App.util.toast && App.util.toast('Uložení selhalo', 'err');
        }
      }, 250);
    });

    eqLoadBtn.addEventListener('click', async ()=>{
      try{
        App.state.config = null;
        await initFromConfig();
        App.util && App.util.toast && App.util.toast('Načteno z /config.json', 'ok');
      }catch(e){
        console.error(e);
        App.util && App.util.toast && App.util.toast('Načtení selhalo', 'err');
      }
    });

    eqSaveBtn.addEventListener('click', async ()=>{
      try{
        readUIToModel();
        const cfg = model.cfg || {};

        // Normalizace: vyhazujeme legacy klíče, aby config zůstal čistý.
        // (Backend umí číst i staré, ale UI bude ukládat v nové podobě.)
        const p = model.valve;
        if(p.travelTime != null) delete p.travelTime;
        if(p.pulseTime != null) delete p.pulseTime;
        if(p.guardTime != null) delete p.guardTime;
        if(p.minSwitch != null) delete p.minSwitch;

        const json = JSON.stringify(cfg, null, 2);
        await App.api.postText('/api/config/apply', json, 'application/json');
        App.util && App.util.toast && App.util.toast('Uloženo', 'ok');
      }catch(e){
        console.error(e);
        App.util && App.util.toast && App.util.toast('Uložení selhalo', 'err');
      }
    });

    // --- Manual valve test (commissioning) ---
    async function valveCmd(kind, val){
      const master = model.valveMaster || 1; // fallback: R1 is master for mixing valve
      try{
        if(kind==='pulse'){
          await App.api.postText(`/api/valve/pulse?master=${master}&dir=${val}`, '', 'text/plain');
        } else if(kind==='stop'){
          await App.api.postText(`/api/valve/stop?master=${master}`, '', 'text/plain');
        }
        const txt = `master R${master} • ${kind}${val!=null?`(${val})`:''}`;
        const el = document.getElementById('eqValveTestInfo');
        if(el) el.textContent = txt;
      }catch(e){
        console.error(e);
        App.util && App.util.toast && App.util.toast('Povel ventilu selhal', 'err');
      }
    }
    const btnOpen = document.getElementById('btnValveOpen');
    const btnClose = document.getElementById('btnValveClose');
    const btnStop = document.getElementById('btnValveStop');
    if(btnOpen) btnOpen.addEventListener('click', ()=>valveCmd('pulse', 1));
    if(btnClose) btnClose.addEventListener('click', ()=>valveCmd('pulse', -1));
    if(btnStop) btnStop.addEventListener('click', ()=>valveCmd('stop', null));
  }

  async function initFromConfig(){
    model.cfg = await loadConfig();
    model.eq = getEqCfg(model.cfg);
    model.valve = getMixValveParams(model.cfg);
    syncUI();
  }

  function updateFromFast(fast){
    // Fast state fields (WebServerController.cpp): fast.e.*, fast.b.*
    try{
      const e = fast && fast.e;
      if(e){
        model.eqEnabledLive = !!e.en;
        model.eqActiveLive = !!e.ac;
        model.night = !!e.ni;
        model.outdoorC = num(e.to, model.outdoorC);
        model.outdoorSrc = (e.rs!=null) ? String(e.rs) : model.outdoorSrc;
        if (e.vm != null) model.valveMaster = num(e.vm, model.valveMaster);
        model.flowC = (e.tf==null) ? model.flowC : num(e.tf, model.flowC);
        model.targetC = (e.tg==null) ? model.targetC : num(e.tg, model.targetC);
        model.valvePosPct = (e.vp==null) ? model.valvePosPct : num(e.vp, model.valvePosPct);
        model.vt = (e.vt==null) ? model.vt : num(e.vt, model.vt);
        model.mv = !!e.mv;
      }
      // Pokud venek není validní a equitherm se chytá z BLE meteo, lze doplnit z fast.b
      if((model.outdoorC==null || !Number.isFinite(model.outdoorC)) && fast && fast.b){
        if(fast.b.tempC!=null) model.outdoorC = num(fast.b.tempC, model.outdoorC);
      }
    }catch(_){ }

    redraw();
  }

  App.pages.equitherm = {
    async mount(root){
      render(root);
      await initFromConfig();
      bind();
      redraw();
    },
    update(fast){
      updateFromFast(fast);
    }
  };
})();
