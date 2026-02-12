// recirc_page.js
// Cirkulace TUV – konfigurace + diagnostika.
// Konfigurace se ukládá do /config.json přes POST /api/config/apply (celý JSON dokument).

window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  const $ = (s,r)=>App.util.$(s,r);
  const esc = App.util.esc;

  const DAYS = [
    {d:1, t:'Po'}, {d:2, t:'Út'}, {d:3, t:'St'}, {d:4, t:'Čt'}, {d:5, t:'Pá'}, {d:6, t:'So'}, {d:7, t:'Ne'}
  ];

  const clamp = (v,a,b)=>Math.max(a,Math.min(b,v));
  const num = (v,def)=>{ const n=Number(v); return Number.isFinite(n)?n:def; };

  function ensurePath(obj, pathArr, defVal){
    let o = obj;
    for(let i=0;i<pathArr.length;i++){
      const k = pathArr[i];
      if(i===pathArr.length-1){
        if(o[k]==null) o[k]=defVal;
        return o[k];
      }
      if(typeof o[k] !== 'object' || o[k]==null) o[k] = {};
      o = o[k];
    }
    return defVal;
  }

  async function loadCfg(){
    if(!App.state) App.state = {};
    if(App.state.config && typeof App.state.config === 'object') return App.state.config;
    try{
      const txt = await App.api.getText('/config.json');
      App.state.config = JSON.parse(txt);
    }catch(_){
      App.state.config = {};
    }
    return App.state.config;
  }

  function getRecircCfg(cfg){
    cfg = cfg || {};
    const rec = ensurePath(cfg, ['dhwRecirc'], {});
    if(rec.enabled == null) rec.enabled = false;
    if(!rec.mode) rec.mode = 'time_windows';
    if(rec.onDemandRunMs == null) rec.onDemandRunMs = 120000;
    if(rec.minOffMs == null) rec.minOffMs = 300000;
    if(rec.minOnMs == null) rec.minOnMs = 30000;
    if(rec.stopTempC == null) rec.stopTempC = 42;
    if(!Array.isArray(rec.windows)) rec.windows = [];
    if(rec.cycleOnMin == null && rec.cycleOnMs == null && rec.cycleOnS == null) rec.cycleOnMin = 0;
    if(rec.cycleOffMin == null && rec.cycleOffMs == null && rec.cycleOffS == null) rec.cycleOffMin = 0;
    const src = ensurePath(rec, ['tempReturnSource'], {});
    if(src.source == null) src.source = 'none';
    if(src.bleId == null) src.bleId = '';
    if(src.mqttIdx == null) src.mqttIdx = 1;
    if(src.topic == null) src.topic = '';
    if(src.jsonKey == null) src.jsonKey = '';
    if(src.maxAgeMs == null) src.maxAgeMs = 600000;
    return rec;
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
        if(s.a <= last.b){ last.b = Math.max(last.b, s.b); }
        else m.push({...s});
      }
      out[di] = m;
    }
    return out;
  }

  function renderWeekPlanInto(el, segmentsByDay){
    if(!el) return;
    const ticks = Array.from({length:25}, (_,i)=>i);
    const tickHtml = ticks.map(h=>`<div class="tick" style="left:${(h/24)*100}%"></div>`).join('');

    const minToHm = (m)=>{
      const mm = Math.max(0, Math.min(1440, Number(m)||0));
      let hh = Math.floor(mm/60);
      let mi = Math.floor(mm%60);
      if(hh===24) return '24:00';
      return `${hh}:${String(mi).padStart(2,'0')}`;
    };

    el.classList.add('weekPlan');
    el.innerHTML = DAYS.map((d,di)=>{
      const segs = (segmentsByDay && segmentsByDay[di]) ? segmentsByDay[di] : [];
      const html = segs.map(s=>{
        const left = (s.a/1440)*100;
        const w = Math.max(0, ((s.b-s.a)/1440)*100);
        const tt = `Cirkulace od ${minToHm(s.a)}–${minToHm(s.b)}`;
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

  function dayPills(selectedDays){
    const set = new Set((selectedDays||[]).map(Number));
    return `<div class="row" style="gap:6px; flex-wrap:wrap">` + DAYS.map(x=>{
      const on = set.has(x.d);
      return `<label class="pill" style="cursor:pointer; user-select:none; opacity:${on?1:.55};"><input type="checkbox" data-day="${x.d}" ${on?'checked':''} style="margin-right:6px;">${x.t}</label>`;
    }).join('') + `</div>`;
  }

  function readDaysFromRow(row){
    const days = [];
    row.querySelectorAll('input[data-day]').forEach(ch=>{ if(ch.checked) days.push(Number(ch.dataset.day)); });
    return days.length ? days : [1,2,3,4,5,6,7];
  }

  function clampTimeStr(s){
    const m = String(s||'').trim().match(/^(\d{1,2}):(\d{2})/);
    if(!m) return '06:00';
    let hh = clamp(Number(m[1]),0,23);
    let mm = clamp(Number(m[2]),0,59);
    return `${String(hh).padStart(2,'0')}:${String(mm).padStart(2,'0')}`;
  }

  function msFromMinutes(v){
    return Math.max(0, Math.round(num(v,0) * 60000));
  }

  let hostEl = null;
  let lastFast = null;

  function render(){
    if(!hostEl) return;
    hostEl.innerHTML = `
      <div class="grid2">
        <div class="card">
          <div class="card__header"><div class="card__title">Cirkulace TUV</div></div>
          <div class="card__body">
            <div class="row" style="justify-content:space-between; align-items:center; gap:10px; flex-wrap:wrap">
              <label class="chk"><input type="checkbox" id="rc_enabled"> <span>Funkce povolena</span></label>
              <div class="row" style="gap:8px">
                <button class="btn" id="rc_reload">Načíst</button>
                <button class="btn primary" id="rc_save">Uložit</button>
              </div>
            </div>

            <div class="hr"></div>

            <div class="grid2">
              <div>
                <label class="lbl">Režim</label>
                <select class="sel" id="rc_mode">
                  <option value="time_windows">Týdenní plán</option>
                  <option value="on_demand">Pouze na požadavek (IN3 edge)</option>
                  <option value="hybrid">Hybrid (plán + požadavek)</option>
                </select>
                <div class="hint">Relé pro čerpadlo: <b>R4</b>. Vstup požadavku: <b>IN3</b>.</div>
              </div>
              <div>
                <label class="lbl">Stop teplota vratky</label>
                <div class="row" style="gap:8px">
                  <input class="inp" id="rc_stopTemp" type="number" step="0.1" min="0" max="80" style="max-width:140px">
                  <div class="muted" style="align-self:center">°C</div>
                </div>
                <div class="hint">Pokud je dostupná "vratka" a dosáhne této teploty, cirkulace se ukončí (po min. době běhu).</div>
              </div>
            </div>

            <div class="hr"></div>

            <div class="grid2">
              <div>
                <label class="lbl">Požadavek (IN3) – doba běhu</label>
                <div class="row" style="gap:8px">
                  <input class="inp" id="rc_onDemandMin" type="number" step="0.5" min="0" max="240" style="max-width:140px">
                  <div class="muted" style="align-self:center">min</div>
                </div>
                <div class="hint">Použije se jen v režimech "Pouze na požadavek" a "Hybrid".</div>
              </div>
              <div>
                <label class="lbl">Ochrany: min. OFF / min. ON</label>
                <div class="row" style="gap:8px; flex-wrap:wrap">
                  <div class="row" style="gap:6px; align-items:center">
                    <input class="inp" id="rc_minOffMin" type="number" step="1" min="0" max="1440" style="max-width:120px">
                    <div class="muted">min OFF</div>
                  </div>
                  <div class="row" style="gap:6px; align-items:center">
                    <input class="inp" id="rc_minOnSec" type="number" step="1" min="0" max="3600" style="max-width:120px">
                    <div class="muted">s ON</div>
                  </div>
                </div>
                <div class="hint">Zabraňuje častému cyklování a umožní doběh při dosažení teploty.</div>
              </div>
            </div>

            <div class="hr"></div>

            <div>
              <div class="row" style="justify-content:space-between; align-items:center; gap:10px; flex-wrap:wrap">
                <div>
                  <div class="card__title" style="font-size:1.05rem">Týdenní plán</div>
                  <div class="hint">Během aktivního okna se spíná čerpadlo (R4). Pokud nastavíš cyklus, bude se střídat ON/OFF po dobu aktivního okna.</div>
                </div>
                <button class="btn" id="rc_addWin">Přidat okno</button>
              </div>
              <div id="rc_windows" style="margin-top:10px"></div>
              <div id="rc_week" style="margin-top:12px"></div>
            </div>

            <div class="hr"></div>

            <div class="grid2">
              <div>
                <label class="lbl">Chování v plánu</label>
                <select class="sel" id="rc_cycleMode">
                  <option value="solid">Trvale sepnuté v okně</option>
                  <option value="cycle">Střídání ON/OFF</option>
                </select>
              </div>
              <div>
                <label class="lbl">Intervaly (jen při "Střídání")</label>
                <div class="row" style="gap:8px; flex-wrap:wrap">
                  <div class="row" style="gap:6px; align-items:center">
                    <input class="inp" id="rc_cycleOnMin" type="number" step="1" min="0" max="240" style="max-width:120px">
                    <div class="muted">min ON</div>
                  </div>
                  <div class="row" style="gap:6px; align-items:center">
                    <input class="inp" id="rc_cycleOffMin" type="number" step="1" min="0" max="240" style="max-width:120px">
                    <div class="muted">min OFF</div>
                  </div>
                </div>
                <div class="hint">Příklad: 5 / 15 bude opakovat 5 min ON a 15 min OFF, dokud trvá aktivní okno.</div>
              </div>
            </div>

            <div class="hr"></div>

            <div>
              <div class="card__title" style="font-size:1.05rem">Zdroj teploty vratky</div>
              <div class="grid2" style="margin-top:8px">
                <div>
                  <label class="lbl">Zdroj</label>
                  <select class="sel" id="rc_retSource">
                    <option value="none">Automaticky dle role ("return")</option>
                    <option value="temp1">T1</option><option value="temp2">T2</option><option value="temp3">T3</option><option value="temp4">T4</option>
                    <option value="temp5">T5</option><option value="temp6">T6</option><option value="temp7">T7</option><option value="temp8">T8</option>
                    <option value="opentherm_boiler">OpenTherm: boiler</option>
                    <option value="opentherm_return">OpenTherm: return</option>
                    <option value="ble">BLE teploměr</option>
                    <option value="mqtt">MQTT teploměr</option>
                  </select>
                  <div class="hint">Nech "Automaticky" a nastav roli "Vratka" v Teploměrech – firmware si zdroj dohledá sám.</div>
                </div>
                <div>
                  <label class="lbl">Doplňující nastavení (jen pro BLE/MQTT)</label>
                  <div class="row" style="gap:8px; flex-wrap:wrap">
                    <input class="inp" id="rc_bleId" placeholder="BLE id" style="min-width:160px">
                    <select class="sel" id="rc_mqttIdx" style="max-width:120px"><option value="1">MQTT 1</option><option value="2">MQTT 2</option></select>
                  </div>
                  <div class="row" style="gap:8px; flex-wrap:wrap; margin-top:8px">
                    <input class="inp" id="rc_mqttTopic" placeholder="MQTT topic" style="min-width:220px">
                    <input class="inp" id="rc_mqttKey" placeholder="JSON key" style="min-width:140px">
                  </div>
                </div>
              </div>
            </div>

            <div class="hr"></div>
            <div class="card__title" style="font-size:1.05rem">Diagnostika</div>
            <div class="kv" style="margin-top:8px">
              <div class="k">Enabled</div><div class="v" id="rc_s_en">—</div>
              <div class="k">Active</div><div class="v" id="rc_s_ac">—</div>
              <div class="k">Mode</div><div class="v" id="rc_s_mo">—</div>
              <div class="k">Remaining</div><div class="v" id="rc_s_rm">—</div>
              <div class="k">Return</div><div class="v" id="rc_s_rt">—</div>
            </div>
            <div class="hint" id="rc_msg" style="margin-top:10px"></div>
          </div>
        </div>

        <div class="card">
          <div class="card__header"><div class="card__title">Tipy</div></div>
          <div class="card__body">
            <ul style="margin:0; padding-left:18px">
              <li>Pokud máš definované okno s <b>start=end</b>, bere se jako <b>celodenní</b>.</li>
              <li>Při okně přes půlnoc se interval rozdělí do dvou dnů.</li>
              <li>Relé čerpadla je pevně <b>R4</b> (v UI relé je označeno jako "Cirkulace TUV").</li>
            </ul>
          </div>
        </div>
      </div>
    `;
  }

  function renderWindows(windows){
    const root = $('#rc_windows', hostEl);
    if(!root) return;

    const rows = (windows||[]).slice(0,6);
    root.innerHTML = rows.map((w,idx)=>{
      const start = clampTimeStr(w.start || '06:00');
      const end = clampTimeStr(w.end || '07:00');
      const days = Array.isArray(w.days) ? w.days : [1,2,3,4,5,6,7];
      return `
        <div class="card" style="padding:12px; margin:0 0 10px 0">
          <div class="row" style="justify-content:space-between; align-items:center; gap:10px; flex-wrap:wrap">
            <div class="muted">Okno #${idx+1}</div>
            <button class="btn sm" data-act="del" data-i="${idx}">Smazat</button>
          </div>
          <div style="margin-top:8px">${dayPills(days)}</div>
          <div class="row" style="gap:10px; flex-wrap:wrap; margin-top:8px">
            <label class="lbl" style="margin:0">Start <input class="inp" type="time" step="60" data-k="start" value="${esc(start)}" data-i="${idx}" style="margin-left:6px; max-width:140px"></label>
            <label class="lbl" style="margin:0">Konec <input class="inp" type="time" step="60" data-k="end" value="${esc(end)}" data-i="${idx}" style="margin-left:6px; max-width:140px"></label>
          </div>
        </div>
      `;
    }).join('') || `<div class="hint">Zatím není přidané žádné okno. Klikni na „Přidat okno“.</div>`;

    // bind remove
    root.querySelectorAll('button[data-act="del"]').forEach(b=>{
      b.onclick = ()=>{
        const i = Number(b.dataset.i);
        const cfg = App.state.config || {};
        const rec = getRecircCfg(cfg);
        rec.windows = Array.isArray(rec.windows) ? rec.windows : [];
        rec.windows.splice(i,1);
        renderWindows(rec.windows);
        renderWeekPlanInto($('#rc_week', hostEl), computeWeekSegmentsFromWindows(rec.windows));
      };
    });

    // bind changes
    root.querySelectorAll('input[data-k]').forEach(inp=>{
      inp.onchange = ()=>{
        const i = Number(inp.dataset.i);
        const cfg = App.state.config || {};
        const rec = getRecircCfg(cfg);
        rec.windows = Array.isArray(rec.windows) ? rec.windows : [];
        if(!rec.windows[i]) rec.windows[i] = {days:[1,2,3,4,5,6,7], start:'06:00', end:'07:00'};
        const k = inp.dataset.k;
        rec.windows[i][k] = clampTimeStr(inp.value);
        renderWeekPlanInto($('#rc_week', hostEl), computeWeekSegmentsFromWindows(rec.windows));
      };
    });

    root.querySelectorAll('input[data-day]').forEach(ch=>{
      ch.onchange = ()=>{
        const card = ch.closest('.card');
        const delBtn = card ? card.querySelector('button[data-act="del"]') : null;
        const i = delBtn ? Number(delBtn.dataset.i) : -1;
        if(i<0) return;
        const cfg = App.state.config || {};
        const rec = getRecircCfg(cfg);
        rec.windows = Array.isArray(rec.windows) ? rec.windows : [];
        if(!rec.windows[i]) rec.windows[i] = {days:[1,2,3,4,5,6,7], start:'06:00', end:'07:00'};
        rec.windows[i].days = readDaysFromRow(card);
        renderWeekPlanInto($('#rc_week', hostEl), computeWeekSegmentsFromWindows(rec.windows));
      };
    });
  }

  function readUiIntoConfig(cfg){
    cfg = cfg || {};
    const rec = getRecircCfg(cfg);

    rec.enabled = !!$('#rc_enabled', hostEl)?.checked;
    rec.mode = String($('#rc_mode', hostEl)?.value || 'time_windows');
    rec.stopTempC = num($('#rc_stopTemp', hostEl)?.value, rec.stopTempC);

    rec.onDemandRunMs = msFromMinutes($('#rc_onDemandMin', hostEl)?.value);
    rec.minOffMs = msFromMinutes($('#rc_minOffMin', hostEl)?.value);
    rec.minOnMs = Math.max(0, Math.round(num($('#rc_minOnSec', hostEl)?.value, 30) * 1000));

    const cycleMode = String($('#rc_cycleMode', hostEl)?.value || 'solid');
    if(cycleMode === 'cycle'){
      const onMin = Math.max(0, Math.round(num($('#rc_cycleOnMin', hostEl)?.value, 0)));
      const offMin = Math.max(0, Math.round(num($('#rc_cycleOffMin', hostEl)?.value, 0)));
      rec.cycleOnMin = onMin;
      rec.cycleOffMin = offMin;
      // keep also ms keys for back-end (it accepts Min/S/Ms)
      rec.cycleOnMs = onMin * 60000;
      rec.cycleOffMs = offMin * 60000;
    } else {
      rec.cycleOnMin = 0;
      rec.cycleOffMin = 0;
      rec.cycleOnMs = 0;
      rec.cycleOffMs = 0;
    }

    const src = ensurePath(rec, ['tempReturnSource'], {});
    src.source = String($('#rc_retSource', hostEl)?.value || 'none');
    src.bleId = String($('#rc_bleId', hostEl)?.value || '');
    src.mqttIdx = Number($('#rc_mqttIdx', hostEl)?.value || '1');
    src.topic = String($('#rc_mqttTopic', hostEl)?.value || '');
    src.jsonKey = String($('#rc_mqttKey', hostEl)?.value || '');
    return cfg;
  }

  async function saveCfg(){
    const msg = $('#rc_msg', hostEl);
    try{
      const cfg = await loadCfg();
      readUiIntoConfig(cfg);
      await App.api.postJson('/api/config/apply', cfg);
      if(msg){ msg.textContent = 'Uloženo.'; msg.className = 'hint ok'; }
      // reload config so dashboard/widget sees it immediately
      App.state.config = null;
      await loadCfg();
    }catch(e){
      if(msg){ msg.textContent = `Chyba uložení: ${e.message}`; msg.className = 'hint bad'; }
    }
  }

  async function loadToUi(){
    const cfg = await loadCfg();
    const rec = getRecircCfg(cfg);

    const setVal = (id, v)=>{ const el = $(id, hostEl); if(!el) return; el.value = String(v==null?'':v); };

    $('#rc_enabled', hostEl).checked = !!rec.enabled;
    setVal('#rc_mode', rec.mode || 'time_windows');
    setVal('#rc_stopTemp', rec.stopTempC);

    setVal('#rc_onDemandMin', (num(rec.onDemandRunMs,120000) / 60000).toFixed(1));
    setVal('#rc_minOffMin', Math.round(num(rec.minOffMs,300000) / 60000));
    setVal('#rc_minOnSec', Math.round(num(rec.minOnMs,30000) / 1000));

    // cycle values (prefer min, fallback from ms)
    let onMin = num(rec.cycleOnMin, 0);
    let offMin = num(rec.cycleOffMin, 0);
    if(!onMin && num(rec.cycleOnMs,0)) onMin = Math.round(num(rec.cycleOnMs,0)/60000);
    if(!offMin && num(rec.cycleOffMs,0)) offMin = Math.round(num(rec.cycleOffMs,0)/60000);
    const cycleEn = (onMin > 0 && offMin > 0);
    setVal('#rc_cycleMode', cycleEn ? 'cycle' : 'solid');
    setVal('#rc_cycleOnMin', onMin);
    setVal('#rc_cycleOffMin', offMin);

    const src = rec.tempReturnSource || {};
    setVal('#rc_retSource', src.source || 'none');
    setVal('#rc_bleId', src.bleId || '');
    setVal('#rc_mqttIdx', src.mqttIdx || 1);
    setVal('#rc_mqttTopic', src.topic || '');
    setVal('#rc_mqttKey', src.jsonKey || '');

    renderWindows(rec.windows);
    renderWeekPlanInto($('#rc_week', hostEl), computeWeekSegmentsFromWindows(rec.windows));
  }

  function fmtDur(ms){
    const x = Number(ms);
    if(!isFinite(x) || x <= 0) return '—';
    const s = Math.max(0, Math.round(x / 1000));
    if(s < 60) return `${s}s`;
    const m = Math.floor(s / 60);
    return `${m}m ${s % 60}s`;
  }

  function updateDiag(fast){
    lastFast = fast || lastFast;
    const c = lastFast?.c || {};
    const setText = (id, txt)=>{ const el = $(id, hostEl); if(el) el.textContent = txt; };
    setText('#rc_s_en', c.en ? 'true' : 'false');
    setText('#rc_s_ac', c.ac ? 'true' : 'false');
    setText('#rc_s_mo', c.mo || '—');
    setText('#rc_s_rm', fmtDur(c.rm));
    const rt = (c.rv && isFinite(c.rt)) ? `${Number(c.rt).toFixed(1)} °C` : '—';
    setText('#rc_s_rt', rt);
  }

  App.pages.recirc = {
    async mount(host){
      hostEl = host;
      render();

      $('#rc_save', hostEl).onclick = saveCfg;
      $('#rc_reload', hostEl).onclick = async()=>{
        App.state.config = null;
        await loadToUi();
        const msg = $('#rc_msg', hostEl);
        if(msg){ msg.textContent = 'Načteno.'; msg.className = 'hint'; }
      };
      $('#rc_addWin', hostEl).onclick = ()=>{
        const cfg = App.state.config || {};
        const rec = getRecircCfg(cfg);
        rec.windows = Array.isArray(rec.windows) ? rec.windows : [];
        if(rec.windows.length >= 6) return;
        rec.windows.push({days:[1,2,3,4,5,6,7], start:'06:00', end:'07:00'});
        renderWindows(rec.windows);
        renderWeekPlanInto($('#rc_week', hostEl), computeWeekSegmentsFromWindows(rec.windows));
      };

      // auto UI enable/disable for cycle fields
      $('#rc_cycleMode', hostEl).onchange = ()=>{
        const m = String($('#rc_cycleMode', hostEl).value || 'solid');
        const dis = (m !== 'cycle');
        $('#rc_cycleOnMin', hostEl).disabled = dis;
        $('#rc_cycleOffMin', hostEl).disabled = dis;
      };

      await loadToUi();
      updateDiag(App.state.fast);
      // apply initial disabled state
      $('#rc_cycleMode', hostEl).onchange();
    },
    update(fast){
      updateDiag(fast);
    }
  };
})();
