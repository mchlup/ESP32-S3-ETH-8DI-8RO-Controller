// aku_page.js
// Akumulační nádrž – konfigurace podpory Ekvitermu z AKU + řízení topné tyče (R8).
// Konfigurace se ukládá do /config.json přes POST /api/config/apply.

window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  const $ = (s,r)=>App.util.$(s,r);
  const esc = App.util.esc;

  const DAYS = [
    {d:1,t:'Po'}, {d:2,t:'Út'}, {d:3,t:'St'}, {d:4,t:'Čt'}, {d:5,t:'Pá'}, {d:6,t:'So'}, {d:7,t:'Ne'}
  ];

  const num = (v,def)=>{ const n=Number(v); return Number.isFinite(n)?n:def; };
  const clamp = (v,a,b)=>Math.max(a,Math.min(b,v));

  function ensureObj(root, path){
    let o = root;
    for(const k of path){
      if(typeof o[k] !== 'object' || o[k]==null) o[k] = {};
      o = o[k];
    }
    return o;
  }

  async function loadCfg(){
    if(App.state && App.state.config && typeof App.state.config==='object') return App.state.config;
    try{
      const txt = await App.api.getText('/config.json');
      App.state = App.state || {};
      App.state.config = JSON.parse(txt);
    }catch(_){
      App.state = App.state || {};
      App.state.config = {};
    }
    return App.state.config;
  }

  function getAkuEqCfg(cfg){
    cfg = cfg || {};
    const eq = ensureObj(cfg, ['equitherm']);

    // AKU support defaults (backend má vlastní, ale UI drží explicitně)
    if(eq.akuSupportEnabled == null) eq.akuSupportEnabled = true;
    if(!eq.akuNoSupportBehavior) eq.akuNoSupportBehavior = 'close'; // close | hold

    // thresholds (day/night)
    if(eq.akuMinTopC_day == null && eq.akuMinTopCDay == null) eq.akuMinTopC_day = 40;
    if(eq.akuMinTopC_night == null && eq.akuMinTopCNight == null) eq.akuMinTopC_night = 45;

    if(eq.akuMinDeltaToTargetC_day == null && eq.akuMinDeltaToTargetCDay == null) eq.akuMinDeltaToTargetC_day = 2;
    if(eq.akuMinDeltaToTargetC_night == null && eq.akuMinDeltaToTargetCNight == null) eq.akuMinDeltaToTargetC_night = 3;

    if(eq.akuMinDeltaToBoilerInC_day == null && eq.akuMinDeltaToBoilerInCDay == null) eq.akuMinDeltaToBoilerInC_day = 3;
    if(eq.akuMinDeltaToBoilerInC_night == null && eq.akuMinDeltaToBoilerInCNight == null) eq.akuMinDeltaToBoilerInC_night = 4;

    // keep only new keys in UI model
    if(eq.akuMinTopC_day == null) eq.akuMinTopC_day = eq.akuMinTopCDay;
    if(eq.akuMinTopC_night == null) eq.akuMinTopC_night = eq.akuMinTopCNight;
    if(eq.akuMinDeltaToTargetC_day == null) eq.akuMinDeltaToTargetC_day = eq.akuMinDeltaToTargetCDay;
    if(eq.akuMinDeltaToTargetC_night == null) eq.akuMinDeltaToTargetC_night = eq.akuMinDeltaToTargetCNight;
    if(eq.akuMinDeltaToBoilerInC_day == null) eq.akuMinDeltaToBoilerInC_day = eq.akuMinDeltaToBoilerInCDay;
    if(eq.akuMinDeltaToBoilerInC_night == null) eq.akuMinDeltaToBoilerInC_night = eq.akuMinDeltaToBoilerInCNight;

    // source objects (optional) – pokud není explicitně, backend použije role z stránky Teploměry
    if(typeof eq.akuTop !== 'object' || eq.akuTop==null) eq.akuTop = {};
    if(typeof eq.akuMid !== 'object' || eq.akuMid==null) eq.akuMid = {};
    if(typeof eq.akuBottom !== 'object' || eq.akuBottom==null) eq.akuBottom = {};

    const fixSrc = (o)=>{
      if(!o) return;
      if(o.source == null) o.source = '';
      if(o.maxAgeMs == null) o.maxAgeMs = 3600000;
      if(o.bleId == null) o.bleId = '';
      if(o.mqttIdx == null) o.mqttIdx = 1;
      if(o.topic == null) o.topic = '';
      if(o.jsonKey == null) o.jsonKey = '';
      if(o.gpio == null) o.gpio = 0;
      if(o.rom == null && o.addr == null && o.romHex == null) o.rom = '';
    };
    fixSrc(eq.akuTop);
    fixSrc(eq.akuMid);
    fixSrc(eq.akuBottom);

    return eq;
  }

  function getAkuHeaterCfg(cfg){
    cfg = cfg || {};
    const h = ensureObj(cfg, ['akuHeater']);
    if(h.enabled == null) h.enabled = false;
    if(!h.mode) h.mode = 'manual'; // manual | schedule | thermostatic
    if(h.manualOn == null) h.manualOn = false;
    if(h.targetTopC == null) h.targetTopC = 50;
    if(h.hysteresisC == null) h.hysteresisC = 2;
    if(h.maxOnMs == null) h.maxOnMs = 2*60*60*1000;
    if(h.minOffMs == null) h.minOffMs = 10*60*1000;
    if(!Array.isArray(h.windows)) h.windows = [];
    return h;
  }

  function srcLabel(src){
    src = String(src||'').toLowerCase();
    if(!src) return 'Auto (role)';
    if(src==='none') return 'Vypnuto';
    if(src==='ble') return 'BLE';
    if(src==='mqtt') return 'MQTT';
    if(src.startsWith('temp')) return src.toUpperCase();
    return src;
  }

  function renderSrcPicker(idPrefix, srcObj){
    const id = (k)=>`${idPrefix}_${k}`;
    return `
      <div class="kv" style="grid-template-columns: 210px 1fr;">
        <div class="k">Zdroj</div>
        <div class="v">
          <select id="${id('source')}" class="sel" style="width:220px">
            <option value="">Auto (role)</option>
            <option value="none">Vypnuto</option>
            <option value="temp1">TEMP1</option>
            <option value="temp2">TEMP2</option>
            <option value="temp3">TEMP3</option>
            <option value="temp4">TEMP4</option>
            <option value="temp5">TEMP5</option>
            <option value="temp6">TEMP6</option>
            <option value="temp7">TEMP7</option>
            <option value="temp8">TEMP8</option>
            <option value="opentherm_boiler">OpenTherm: boiler</option>
            <option value="opentherm_return">OpenTherm: return</option>
            <option value="ble">BLE</option>
            <option value="mqtt1">MQTT #1</option>
            <option value="mqtt2">MQTT #2</option>
          </select>
          <span class="small" style="opacity:.75; margin-left:8px" id="${id('hint')}">${esc(srcLabel(srcObj.source))}</span>
        </div>

        <div class="k">Max stáří (maxAgeMs)</div>
        <div class="v"><input class="inp w140" id="${id('maxAgeMs')}" type="number" min="0" max="3600000" step="1000" /> ms</div>

        <div class="k">BLE id (volitelné)</div>
        <div class="v"><input id="${id('bleId')}" type="text" style="width:220px" placeholder="" /></div>

        <div class="k">MQTT topic (volitelné)</div>
        <div class="v"><input id="${id('topic')}" type="text" style="width:100%" placeholder="topic" /></div>

        <div class="k">MQTT jsonKey (volitelné)</div>
        <div class="v"><input id="${id('jsonKey')}" type="text" style="width:220px" placeholder="např. temp" /></div>
      </div>
    `;
  }

  function parseHmToMin(hm){
    if(!hm) return 0;
    const m = String(hm).trim().match(/^(\d{1,2}):(\d{2})/);
    if(!m) return 0;
    let hh = Number(m[1]);
    let mm = Number(m[2]);
    if(!Number.isFinite(hh)||!Number.isFinite(mm)) return 0;
    hh = Math.max(0, Math.min(23, hh));
    mm = Math.max(0, Math.min(59, mm));
    return hh*60+mm;
  }

  function computeWeekSegmentsFromWindows(windows){
    const out = Array.from({length:7}, ()=>[]);
    const pushSeg = (di, a, b)=>{
      if(di<0||di>6) return;
      const aa = Math.max(0, Math.min(1440, a));
      const bb = Math.max(0, Math.min(1440, b));
      if(bb <= aa) return;
      out[di].push({a:aa,b:bb});
    };

    (windows||[]).forEach(w=>{
      if(!w) return;
      const days = Array.isArray(w.days) && w.days.length ? w.days : [1,2,3,4,5,6,7];
      const start = parseHmToMin(w.start || '00:00');
      const end = parseHmToMin(w.end || '00:00');

      days.forEach(d=>{
        const di = Number(d)-1;
        if(di<0||di>6) return;
        if(start === end){
          pushSeg(di, 0, 1440);
          return;
        }
        if(start < end){
          pushSeg(di, start, end);
        } else {
          pushSeg(di, start, 1440);
          const di2 = (di + 1) % 7;
          pushSeg(di2, 0, end);
        }
      });
    });

    // merge overlaps
    for(let di=0;di<7;di++){
      const a = out[di].slice().sort((x,y)=>x.a-y.a || x.b-y.b);
      const m = [];
      for(const s of a){
        const last = m[m.length-1];
        if(!last){ m.push({...s}); continue; }
        if(s.a <= last.b) last.b = Math.max(last.b, s.b);
        else m.push({...s});
      }
      out[di] = m;
    }
    return out;
  }

  function renderWeekPlanInto(el, segmentsByDay){
    if(!el) return;
    const ticks = [0, 12, 24];
    const tickHtml = ticks.map(h=>`<div class="tick" style="left:${(h/24)*100}%"></div>`).join('');
    el.classList.add('weekPlan');

    const hm = (m)=>{
      const mm = Math.max(0, Math.min(1440, Number(m)||0));
      const hh = Math.floor(mm/60);
      const mi = Math.floor(mm%60);
      return `${String(hh).padStart(2,'0')}:${String(mi).padStart(2,'0')}`;
    };

    el.innerHTML = DAYS.map((d,di)=>{
      const segs = (segmentsByDay && segmentsByDay[di]) ? segmentsByDay[di] : [];
      const html = segs.map(s=>{
        const left = (s.a/1440)*100;
        const w = Math.max(0, ((s.b-s.a)/1440)*100);
        const tt = `Topná tyč ${hm(s.a)}–${hm(s.b)}`;
        return `<div class="timeSeg" data-tt="${esc(tt)}" style="left:${left}%; width:${w}%"></div>`;
      }).join('');
      return `
        <div class="weekRow">
          <div class="weekDay">${esc(d.t)}</div>
          <div class="timeBar">${tickHtml}${html}</div>
        </div>
      `;
    }).join('');

    if(App.ui && typeof App.ui.bindWeekPlanTooltips === 'function'){
      App.ui.bindWeekPlanTooltips(el);
    }
  }

  const model = {
    cfg: null,
    eq: null,
    heater: null,
    live: {
      enabled:false, active:false, mode:'', reason:'', topC:null, topValid:false, relay8:false
    }
  };

  function pageHtml(){
    return `
      <div class="pageHead">
        <div class="pageTitle">Akumulační nádrž</div>
        <div class="pageActions">
          <button class="btn" id="aku_btn_load">Načíst</button>
          <button class="btn primary" id="aku_btn_save">Uložit</button>
          <button class="btn btn--ghost" id="aku_btn_goThermom">Teploměry</button>
        </div>
      </div>

      <div class="grid12">

        <div class="card col6">
          <h2>Stav (live)</h2>
          <div class="kv" style="grid-template-columns: 210px 1fr;">
            <div class="k">Funkce</div><div class="v"><span id="aku_live_en">—</span> <span class="small" id="aku_live_ac" style="opacity:.75"></span></div>
            <div class="k">Režim</div><div class="v" id="aku_live_mode">—</div>
            <div class="k">TOP teplota</div><div class="v"><span id="aku_live_top">—</span> <span class="small" id="aku_live_topv" style="opacity:.75"></span></div>
            <div class="k">Relé8 (topná tyč)</div><div class="v" id="aku_live_r8">—</div>
            <div class="k">Důvod</div><div class="v" id="aku_live_reason">—</div>
          </div>
          <div class="hint">Pozn.: Relé8 je v tomto projektu pevně přiřazené pro stykač topné tyče.</div>
        </div>

        <div class="card col6">
          <h2>Info</h2>
          <div class="hr"></div>
          <div class="hint">Tato stránka nastavuje:
            <ul>
              <li>Podporu Ekvitermu z AKU (mezní hodnoty + chování, když AKU nemá dost tepla).</li>
              <li>Zdroj teplot AKU TOP/MID/BOTTOM (pokud chceš přepsat automatický výběr dle rolí).</li>
              <li>Řízení topné tyče v AKU (akuHeater).</li>
            </ul>
          </div>
        </div>

        <div class="card col6">
          <h2>Podpora Ekvitermu z AKU</h2>
          <div class="kv" style="grid-template-columns: 240px 1fr;">
            <div class="k">Povolit podporu (akuSupportEnabled)</div>
            <div class="v"><label class="chk"><input id="eq_akuSupportEnabled" type="checkbox" /> <span class="small">používat limity AKU</span></label></div>

            <div class="k">Chování bez podpory (akuNoSupportBehavior)</div>
            <div class="v">
              <select id="eq_akuNoSupportBehavior" class="sel w220">
                <option value="close">Zavřít ventil (0%)</option>
                <option value="hold">Držet polohu</option>
              </select>
            </div>

            <div class="k">Min. TOP – den</div><div class="v"><input class="inp w140" id="eq_akuMinTopC_day" type="number" min="0" max="90" step="0.5" /> °C</div>
            <div class="k">Min. TOP – noc</div><div class="v"><input class="inp w140" id="eq_akuMinTopC_night" type="number" min="0" max="90" step="0.5" /> °C</div>

            <div class="k">Min. Δ k cíli – den</div><div class="v"><input class="inp w140" id="eq_akuMinDeltaToTargetC_day" type="number" min="0" max="40" step="0.5" /> °C</div>
            <div class="k">Min. Δ k cíli – noc</div><div class="v"><input class="inp w140" id="eq_akuMinDeltaToTargetC_night" type="number" min="0" max="40" step="0.5" /> °C</div>

            <div class="k">Min. Δ ke kotli – den</div><div class="v"><input class="inp w140" id="eq_akuMinDeltaToBoilerInC_day" type="number" min="0" max="40" step="0.5" /> °C</div>
            <div class="k">Min. Δ ke kotli – noc</div><div class="v"><input class="inp w140" id="eq_akuMinDeltaToBoilerInC_night" type="number" min="0" max="40" step="0.5" /> °C</div>
          </div>
          <div class="hint">Limity se vyhodnocují proti teplotě <span class="mono">akuTop</span> (TOP vrstvy nádrže). Pokud AKU nemá dost tepla, ekvitermní směšování se omezí podle nastavení.</div>
        </div>

        <div class="card col6">
          <h2>Teploměry AKU (přepsání zdroje)</h2>
          <div class="hint">Výchozí chování: zdroje se berou automaticky dle rolí na stránce „Teploměry“ (aku_top / aku_mid / aku_bottom). Zde je můžeš přepsat explicitně.</div>
          <div class="hr"></div>

          <details open>
            <summary class="small" style="cursor:pointer">AKU TOP</summary>
            <div id="aku_src_top"></div>
          </details>
          <div class="hr"></div>

          <details>
            <summary class="small" style="cursor:pointer">AKU MID</summary>
            <div id="aku_src_mid"></div>
          </details>
          <div class="hr"></div>

          <details>
            <summary class="small" style="cursor:pointer">AKU BOTTOM</summary>
            <div id="aku_src_bottom"></div>
          </details>

        </div>

        <div class="card col12">
          <h2>Topná tyč AKU (Relé8)</h2>
          <div class="grid12" style="align-items:start">

            <div class="col6">
              <div class="kv" style="grid-template-columns: 240px 1fr;">
                <div class="k">Povolit (akuHeater.enabled)</div>
                <div class="v"><label class="chk"><input id="h_enabled" type="checkbox" /> <span class="small">aktivní funkce</span></label></div>

                <div class="k">Režim (akuHeater.mode)</div>
                <div class="v">
                  <select id="h_mode" class="sel" style="width:220px">
                    <option value="manual">Manuálně</option>
                    <option value="schedule">Časová okna</option>
                    <option value="thermostatic">Termostaticky (TOP)</option>
                  </select>
                </div>

                <div class="k">Manuální sepnutí</div>
                <div class="v"><label class="chk"><input id="h_manualOn" type="checkbox" /> <span class="small">zapnout relé</span></label></div>

                <div class="k">Cíl TOP (targetTopC)</div>
                <div class="v"><input class="inp w140" id="h_targetTopC" type="number" min="0" max="95" step="0.5" /> °C</div>

                <div class="k">Hystereze (hysteresisC)</div>
                <div class="v"><input class="inp w140" id="h_hysteresisC" type="number" min="0" max="20" step="0.1" /> °C</div>

                <div class="k">Max. doba zapnutí</div>
                <div class="v"><input class="inp w140" id="h_maxOnMin" type="number" min="0" max="1440" step="1" /> min</div>

                <div class="k">Min. doba vypnutí</div>
                <div class="v"><input class="inp w140" id="h_minOffMin" type="number" min="0" max="1440" step="1" /> min</div>
              </div>
              <div class="hint">Bezpečnost: max/min časy omezují cyklování. V režimu „Termostaticky“ se řídí podle TOP teploty AKU.</div>
            </div>

            <div class="col6">
              <div class="row" style="align-items:center; gap:8px; flex-wrap:wrap">
                <div class="small" style="opacity:.75">Časová okna (akuHeater.windows)</div>
                <button class="btn sm" id="h_addWin">Přidat okno</button>
              </div>
              <div id="h_winTable" style="margin-top:8px"></div>
              <div class="hr"></div>
              <div id="h_week" style="margin-top:8px"></div>
              <div class="hint">Okno může přecházet přes půlnoc (např. 22:00–06:00). Pokud start=end, bere se jako celý den.</div>
            </div>

          </div>
        </div>

      </div>
    `;
  }

  function syncUI(){
    const eq = model.eq;
    const h = model.heater;

    // eq aku support
    $('#eq_akuSupportEnabled').checked = !!eq.akuSupportEnabled;
    $('#eq_akuNoSupportBehavior').value = String(eq.akuNoSupportBehavior||'close').toLowerCase()==='hold' ? 'hold' : 'close';

    $('#eq_akuMinTopC_day').value = num(eq.akuMinTopC_day, 40);
    $('#eq_akuMinTopC_night').value = num(eq.akuMinTopC_night, 45);
    $('#eq_akuMinDeltaToTargetC_day').value = num(eq.akuMinDeltaToTargetC_day, 2);
    $('#eq_akuMinDeltaToTargetC_night').value = num(eq.akuMinDeltaToTargetC_night, 3);
    $('#eq_akuMinDeltaToBoilerInC_day').value = num(eq.akuMinDeltaToBoilerInC_day, 3);
    $('#eq_akuMinDeltaToBoilerInC_night').value = num(eq.akuMinDeltaToBoilerInC_night, 4);

    // sources
    $('#aku_src_top').innerHTML = renderSrcPicker('top', eq.akuTop);
    $('#aku_src_mid').innerHTML = renderSrcPicker('mid', eq.akuMid);
    $('#aku_src_bottom').innerHTML = renderSrcPicker('bottom', eq.akuBottom);

    syncSrcPicker('top', eq.akuTop);
    syncSrcPicker('mid', eq.akuMid);
    syncSrcPicker('bottom', eq.akuBottom);

    // heater
    $('#h_enabled').checked = !!h.enabled;
    $('#h_mode').value = String(h.mode||'manual').toLowerCase();
    $('#h_manualOn').checked = !!h.manualOn;
    $('#h_targetTopC').value = num(h.targetTopC, 50);
    $('#h_hysteresisC').value = num(h.hysteresisC, 2);
    $('#h_maxOnMin').value = Math.round(num(h.maxOnMs, 0) / 60000);
    $('#h_minOffMin').value = Math.round(num(h.minOffMs, 0) / 60000);

    renderWindows();
    redrawLive();
  }

  function syncSrcPicker(prefix, srcObj){
    const sourceSel = $(`#${prefix}_source`);
    const hint = $(`#${prefix}_hint`);
    const maxAgeMs = $(`#${prefix}_maxAgeMs`);
    const bleId = $(`#${prefix}_bleId`);
    const topic = $(`#${prefix}_topic`);
    const jsonKey = $(`#${prefix}_jsonKey`);

    const src = String(srcObj.source||'');
    let val = src;
    if(src==='mqtt') val = `mqtt${clamp(num(srcObj.mqttIdx,1),1,2)}`;
    sourceSel.value = val;
    hint.textContent = srcLabel(src);

    maxAgeMs.value = Math.round(num(srcObj.maxAgeMs, 3600000));
    bleId.value = String(srcObj.bleId||'');
    topic.value = String(srcObj.topic||'');
    jsonKey.value = String(srcObj.jsonKey||'');

    const applyVisibility = ()=>{
      const v = sourceSel.value;
      const isBle = v==='ble';
      const isMqtt = v==='mqtt1' || v==='mqtt2' || v==='mqtt';
      bleId.closest('.v').style.opacity = isBle ? '1' : '0.4';
      topic.closest('.v').style.opacity = isMqtt ? '1' : '0.4';
      jsonKey.closest('.v').style.opacity = isMqtt ? '1' : '0.4';
    };
    applyVisibility();

    sourceSel.onchange = ()=>{
      applyVisibility();
      hint.textContent = srcLabel(sourceSel.value);
    };
  }

  function readUIToModel(){
    const eq = model.eq;
    const h = model.heater;

    eq.akuSupportEnabled = !!$('#eq_akuSupportEnabled').checked;
    eq.akuNoSupportBehavior = String($('#eq_akuNoSupportBehavior').value||'close').toLowerCase()==='hold' ? 'hold' : 'close';

    eq.akuMinTopC_day = num($('#eq_akuMinTopC_day').value, eq.akuMinTopC_day);
    eq.akuMinTopC_night = num($('#eq_akuMinTopC_night').value, eq.akuMinTopC_night);
    eq.akuMinDeltaToTargetC_day = num($('#eq_akuMinDeltaToTargetC_day').value, eq.akuMinDeltaToTargetC_day);
    eq.akuMinDeltaToTargetC_night = num($('#eq_akuMinDeltaToTargetC_night').value, eq.akuMinDeltaToTargetC_night);
    eq.akuMinDeltaToBoilerInC_day = num($('#eq_akuMinDeltaToBoilerInC_day').value, eq.akuMinDeltaToBoilerInC_day);
    eq.akuMinDeltaToBoilerInC_night = num($('#eq_akuMinDeltaToBoilerInC_night').value, eq.akuMinDeltaToBoilerInC_night);

    // sources
    readSrc('top', eq.akuTop);
    readSrc('mid', eq.akuMid);
    readSrc('bottom', eq.akuBottom);

    // heater
    h.enabled = !!$('#h_enabled').checked;
    h.mode = String($('#h_mode').value||'manual').toLowerCase();
    h.manualOn = !!$('#h_manualOn').checked;
    h.targetTopC = num($('#h_targetTopC').value, h.targetTopC);
    h.hysteresisC = num($('#h_hysteresisC').value, h.hysteresisC);
    h.maxOnMs = Math.max(0, Math.round(num($('#h_maxOnMin').value, 0) * 60000));
    h.minOffMs = Math.max(0, Math.round(num($('#h_minOffMin').value, 0) * 60000));

    // normalize windows
    h.windows = (h.windows||[]).slice(0, 8).map(w=>({
      days: Array.isArray(w.days) && w.days.length ? w.days.slice() : [1,2,3,4,5,6,7],
      start: w.start || '06:00',
      end: w.end || '07:00'
    }));

    // keep UI-friendly legacy keys out
    delete eq.akuMinTopCDay;
    delete eq.akuMinTopCNight;
    delete eq.akuMinDeltaToTargetCDay;
    delete eq.akuMinDeltaToTargetCNight;
    delete eq.akuMinDeltaToBoilerInCDay;
    delete eq.akuMinDeltaToBoilerInCNight;

    // write in backend expected names (it accepts multiple variants, but keep consistent)
    eq.akuMinTopC_day = num(eq.akuMinTopC_day, 0);
    eq.akuMinTopC_night = num(eq.akuMinTopC_night, 0);
    eq.akuMinDeltaToTargetC_day = num(eq.akuMinDeltaToTargetC_day, 0);
    eq.akuMinDeltaToTargetC_night = num(eq.akuMinDeltaToTargetC_night, 0);
    eq.akuMinDeltaToBoilerInC_day = num(eq.akuMinDeltaToBoilerInC_day, 0);
    eq.akuMinDeltaToBoilerInC_night = num(eq.akuMinDeltaToBoilerInC_night, 0);
  }

  function readSrc(prefix, srcObj){
    const sourceSel = $(`#${prefix}_source`);
    const maxAgeMs = $(`#${prefix}_maxAgeMs`);
    const bleId = $(`#${prefix}_bleId`);
    const topic = $(`#${prefix}_topic`);
    const jsonKey = $(`#${prefix}_jsonKey`);

    const v = String(sourceSel.value||'');
    if(!v){
      // Auto (role) -> clear explicit selection
      srcObj.source = '';
      srcObj.mqttIdx = 1;
    } else if(v==='none'){
      srcObj.source = 'none';
      srcObj.mqttIdx = 1;
    } else if(v==='ble'){
      srcObj.source = 'ble';
      srcObj.bleId = String(bleId.value||'');
      srcObj.mqttIdx = 1;
    } else if(v==='mqtt1' || v==='mqtt2'){
      srcObj.source = 'mqtt';
      srcObj.mqttIdx = (v==='mqtt2') ? 2 : 1;
      srcObj.topic = String(topic.value||'');
      srcObj.jsonKey = String(jsonKey.value||'');
    } else if(v.startsWith('temp')){
      srcObj.source = v; // temp1..temp8
      srcObj.mqttIdx = 1;
    } else {
      srcObj.source = v;
    }

    srcObj.maxAgeMs = Math.round(num(maxAgeMs.value, srcObj.maxAgeMs));

    // Cleanup fields for readability
    if(srcObj.source !== 'ble') srcObj.bleId = '';
    if(srcObj.source !== 'mqtt'){
      srcObj.topic = '';
      srcObj.jsonKey = '';
      srcObj.mqttIdx = 1;
    }

    // For temp sources keep rom fields empty (Dallas mapping is on TEMP slots)
    if(srcObj.source && srcObj.source.startsWith('temp')){
      delete srcObj.gpio;
      delete srcObj.rom;
      delete srcObj.addr;
      delete srcObj.romHex;
    }
  }

  function renderWindows(){
    const tbl = $('#h_winTable');
    const h = model.heater;
    const wins = Array.isArray(h.windows) ? h.windows : [];

    const dayChecks = (idx, days)=>{
      const set = new Set((days||[]).map(Number));
      return DAYS.map(d=>`<label class="chk" style="margin-right:6px"><input type="checkbox" data-win="${idx}" data-day="${d.d}" ${set.has(d.d)?'checked':''}/> <span class="small">${esc(d.t)}</span></label>`).join('');
    };

    tbl.innerHTML = wins.map((w,idx)=>{
      const s = esc(w.start||'06:00');
      const e = esc(w.end||'07:00');
      return `
        <div class="card" style="padding:10px; margin-bottom:10px">
          <div class="row" style="justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap">
            <div class="small" style="opacity:.75">Okno #${idx+1}</div>
            <button class="btn sm danger" data-act="del" data-win="${idx}">Smazat</button>
          </div>
          <div class="kv" style="grid-template-columns: 160px 1fr; margin-top:8px">
            <div class="k">Dny</div><div class="v">${dayChecks(idx, w.days)}</div>
            <div class="k">Start</div><div class="v"><input type="time" data-act="start" data-win="${idx}" value="${s}" /></div>
            <div class="k">End</div><div class="v"><input type="time" data-act="end" data-win="${idx}" value="${e}" /></div>
          </div>
        </div>
      `;
    }).join('') || `<div class="hint">Žádná okna. Přidej okno, pokud chceš režim „Časová okna“.</div>`;

    // bind events
    tbl.querySelectorAll('button[data-act=del]').forEach(b=>b.addEventListener('click', ()=>{
      const idx = Number(b.dataset.win);
      model.heater.windows.splice(idx, 1);
      renderWindows();
    }));

    tbl.querySelectorAll('input[type=time]').forEach(inp=>inp.addEventListener('change', ()=>{
      const idx = Number(inp.dataset.win);
      const act = inp.dataset.act;
      if(!model.heater.windows[idx]) return;
      const v = String(inp.value||'00:00').slice(0,5);
      if(act==='start') model.heater.windows[idx].start = v;
      if(act==='end') model.heater.windows[idx].end = v;
      renderWeek();
    }));

    tbl.querySelectorAll('input[type=checkbox][data-day]').forEach(inp=>inp.addEventListener('change', ()=>{
      const idx = Number(inp.dataset.win);
      const d = Number(inp.dataset.day);
      if(!model.heater.windows[idx]) return;
      const set = new Set((model.heater.windows[idx].days||[]).map(Number));
      if(inp.checked) set.add(d); else set.delete(d);
      model.heater.windows[idx].days = Array.from(set).sort((a,b)=>a-b);
      renderWeek();
    }));

    renderWeek();
  }

  function renderWeek(){
    const el = $('#h_week');
    const wins = model.heater && Array.isArray(model.heater.windows) ? model.heater.windows : [];
    const seg = computeWeekSegmentsFromWindows(wins);
    renderWeekPlanInto(el, seg);
  }

  function redrawLive(){
    $('#aku_live_en').textContent = model.live.enabled ? 'ZAP' : 'VYP';
    $('#aku_live_ac').textContent = model.live.active ? '• aktivní' : '';
    $('#aku_live_mode').textContent = model.live.mode || '—';
    $('#aku_live_top').textContent = (model.live.topValid && isFinite(model.live.topC)) ? Number(model.live.topC).toFixed(1) + ' °C' : '—';
    $('#aku_live_topv').textContent = model.live.topValid ? '' : '(neplatné)';
    $('#aku_live_r8').textContent = model.live.relay8 ? 'SEPNUTO' : 'vypnuto';
    $('#aku_live_reason').textContent = model.live.reason || '—';
  }

  function updateFromFast(fast){
    if(!fast) return;
    const a = fast.a || {};
    model.live.enabled = !!a.en;
    model.live.active = !!a.ac;
    model.live.topC = (a.tp==null) ? model.live.topC : num(a.tp, model.live.topC);
    model.live.topValid = !!a.tv;
    model.live.mode = a.mo != null ? String(a.mo) : model.live.mode;
    model.live.reason = a.rs != null ? String(a.rs) : model.live.reason;

    // relay8 state from bitmask (r: bit0=R1)
    const rMask = fast.r;
    model.live.relay8 = (typeof rMask==='number') ? ((rMask & (1<<7)) !== 0) : model.live.relay8;
    redrawLive();
  }

  function bind(){
    $('#aku_btn_goThermom').addEventListener('click', ()=>{
      // simulate click in nav
      const btn = document.querySelector('.navItem[data-page="thermometers"]');
      if(btn) btn.click();
    });

    $('#aku_btn_load').addEventListener('click', async ()=>{
      try{
        App.state.config = null;
        await initFromConfig();
        App.util.toast && App.util.toast('Načteno', 'ok');
      }catch(e){
        console.error(e);
        App.util.toast && App.util.toast('Načtení selhalo', 'err');
      }
    });

    $('#aku_btn_save').addEventListener('click', async ()=>{
      try{
        readUIToModel();

        // Map UI keys to backend-expected names (backend accepts multiple, but keep clean)
        model.eq.akuMinTopC_day = clamp(num(model.eq.akuMinTopC_day,0), 0, 95);
        model.eq.akuMinTopC_night = clamp(num(model.eq.akuMinTopC_night,0), 0, 95);
        model.eq.akuMinDeltaToTargetC_day = clamp(num(model.eq.akuMinDeltaToTargetC_day,0), 0, 95);
        model.eq.akuMinDeltaToTargetC_night = clamp(num(model.eq.akuMinDeltaToTargetC_night,0), 0, 95);
        model.eq.akuMinDeltaToBoilerInC_day = clamp(num(model.eq.akuMinDeltaToBoilerInC_day,0), 0, 95);
        model.eq.akuMinDeltaToBoilerInC_night = clamp(num(model.eq.akuMinDeltaToBoilerInC_night,0), 0, 95);

        // Ensure backend sees new keys (it also reads *_day/*_night)
        model.eq.akuMinTopC_day = model.eq.akuMinTopC_day;
        model.eq.akuMinTopC_night = model.eq.akuMinTopC_night;
        model.eq.akuMinDeltaToTargetC_day = model.eq.akuMinDeltaToTargetC_day;
        model.eq.akuMinDeltaToTargetC_night = model.eq.akuMinDeltaToTargetC_night;
        model.eq.akuMinDeltaToBoilerInC_day = model.eq.akuMinDeltaToBoilerInC_day;
        model.eq.akuMinDeltaToBoilerInC_night = model.eq.akuMinDeltaToBoilerInC_night;

        // Write into cfg
        model.cfg.equitherm = model.cfg.equitherm || {};
        Object.assign(model.cfg.equitherm, {
          akuSupportEnabled: model.eq.akuSupportEnabled,
          akuNoSupportBehavior: model.eq.akuNoSupportBehavior,
          akuMinTopC_day: model.eq.akuMinTopC_day,
          akuMinTopC_night: model.eq.akuMinTopC_night,
          akuMinDeltaToTargetC_day: model.eq.akuMinDeltaToTargetC_day,
          akuMinDeltaToTargetC_night: model.eq.akuMinDeltaToTargetC_night,
          akuMinDeltaToBoilerInC_day: model.eq.akuMinDeltaToBoilerInC_day,
          akuMinDeltaToBoilerInC_night: model.eq.akuMinDeltaToBoilerInC_night,
          akuTop: model.eq.akuTop,
          akuMid: model.eq.akuMid,
          akuBottom: model.eq.akuBottom
        });

        model.cfg.akuHeater = model.cfg.akuHeater || {};
        Object.assign(model.cfg.akuHeater, {
          enabled: model.heater.enabled,
          mode: model.heater.mode,
          manualOn: model.heater.manualOn,
          targetTopC: model.heater.targetTopC,
          hysteresisC: model.heater.hysteresisC,
          maxOnMs: model.heater.maxOnMs,
          minOffMs: model.heater.minOffMs,
          windows: model.heater.windows
        });

        const json = JSON.stringify(model.cfg, null, 2);
        await App.api.postText('/api/config/apply', json, 'application/json');
        App.util.toast && App.util.toast('Uloženo', 'ok');
      }catch(e){
        console.error(e);
        App.util.toast && App.util.toast('Uložení selhalo', 'err');
      }
    });

    // add window
    $('#h_addWin').addEventListener('click', ()=>{
      model.heater.windows = model.heater.windows || [];
      if(model.heater.windows.length >= 8){
        App.util.toast && App.util.toast('Max 8 oken', 'err');
        return;
      }
      model.heater.windows.push({days:[1,2,3,4,5,6,7], start:'06:00', end:'07:00'});
      renderWindows();
    });

    // any change -> update model preview (no autosave)
    const ids = [
      'eq_akuSupportEnabled','eq_akuNoSupportBehavior','eq_akuMinTopC_day','eq_akuMinTopC_night',
      'eq_akuMinDeltaToTargetC_day','eq_akuMinDeltaToTargetC_night','eq_akuMinDeltaToBoilerInC_day','eq_akuMinDeltaToBoilerInC_night',
      'h_enabled','h_mode','h_manualOn','h_targetTopC','h_hysteresisC','h_maxOnMin','h_minOffMin'
    ];
    ids.forEach(id=>{
      const el = $('#'+id);
      if(!el) return;
      el.addEventListener('change', ()=>{ try{ readUIToModel(); renderWeek(); }catch(_){ } });
      el.addEventListener('input', ()=>{ try{ readUIToModel(); }catch(_){ } });
    });

    // source pickers – delegated
    ['top','mid','bottom'].forEach(p=>{
      const sel = $(`#${p}_source`);
      if(sel) sel.addEventListener('change', ()=>{ try{ readUIToModel(); }catch(_){ } });
      const maxAge = $(`#${p}_maxAgeMs`);
      if(maxAge) maxAge.addEventListener('input', ()=>{ try{ readUIToModel(); }catch(_){ } });
      const bleId = $(`#${p}_bleId`);
      if(bleId) bleId.addEventListener('input', ()=>{ try{ readUIToModel(); }catch(_){ } });
      const topic = $(`#${p}_topic`);
      if(topic) topic.addEventListener('input', ()=>{ try{ readUIToModel(); }catch(_){ } });
      const key = $(`#${p}_jsonKey`);
      if(key) key.addEventListener('input', ()=>{ try{ readUIToModel(); }catch(_){ } });
    });
  }

  async function initFromConfig(){
    model.cfg = await loadCfg();
    model.eq = getAkuEqCfg(model.cfg);
    model.heater = getAkuHeaterCfg(model.cfg);
    syncUI();
  }

  const P = {
    mount(host){
      if(!host) return;
      host.innerHTML = pageHtml();
      initFromConfig().then(()=>{ try{ bind(); }catch(_){ } });
    },
    update(fast){
      updateFromFast(fast);
    }
  };

  App.pages.aku = P;
})();
