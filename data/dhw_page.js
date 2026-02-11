// dhw_page.js
// Ohřev TUV (DHW) – konfigurace + diagnostika.
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

  function getCfg(){
    return (App.state && App.state.config && typeof App.state.config === 'object') ? App.state.config : null;
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

  function getTuvCfg(cfg){
    cfg = cfg || {};
    const tuv = ensurePath(cfg, ['tuv'], {});
    if(tuv.enabled == null) tuv.enabled = false;

    // Přepínací ventil (R3) – v projektu: DHW=0%, topení=100%.
    // Firmware používá bypassValve: bypassPct (active), chPct (inactive).
    // Pro přepínací ventil na R3 (single-relay) v tomto projektu platí:
    //   - při aktivním ohřevu TUV se relé sepne
    //   - při neaktivním stavu je relé rozepnuté
    // Proto výchozí stavy překlápíme na 100/0 (bez nutnosti "Invertovat směr").
    const bv = ensurePath(tuv, ['bypassValve'], {});
    if(bv.enabled == null) bv.enabled = true;
    if(bv.bypassPct == null) bv.bypassPct = 100;
    if(bv.chPct == null) bv.chPct = 0;
    if(bv.invert == null) bv.invert = false;

    if(tuv.restoreEqValveAfter == null) tuv.restoreEqValveAfter = true;

    // Volitelné budoucí klíče (firmware je může ignorovat – UI je ale připravené):
    if(tuv.targetTempC == null) tuv.targetTempC = 50;
    if(tuv.hysteresisC == null) tuv.hysteresisC = 2;

    return tuv;
  }

  function schedulesAll(cfg){
    const arr = ensurePath(cfg, ['schedules'], []);
    return Array.isArray(arr) ? arr : [];
  }

  function extractDhwSchedules(cfg){
    const all = schedulesAll(cfg);
    const dhw = [];
    for(const it of all){
      const kind = String((it && (it.kind || it.type)) || '').toLowerCase();
      if(kind !== 'dhw_enable') continue;
      dhw.push({
        enabled: it.enabled !== false,
        days: Array.isArray(it.days) ? it.days.slice() : [1,2,3,4,5,6,7],
        at: String(it.at || it.time || '06:00'),
        value: !!(it.value ? (it.value.enable ?? it.value.enabled) : (it.params ? (it.params.enable ?? it.params.enabled) : true))
      });
    }
    // Stabilní řazení (čas, zap/vyp)
    dhw.sort((a,b)=> (a.at||'').localeCompare(b.at||'') || (Number(b.value)-Number(a.value)) );
    return dhw;
  }

  function replaceDhwSchedules(cfg, newItems){
    const all = schedulesAll(cfg).filter(it=> String((it && (it.kind || it.type)) || '').toLowerCase() !== 'dhw_enable');
    for(const s of (newItems||[])){
      all.push({
        enabled: s.enabled !== false,
        days: Array.isArray(s.days) && s.days.length ? s.days.map(x=>Number(x)).filter(x=>x>=1&&x<=7) : [1,2,3,4,5,6,7],
        at: String(s.at||'06:00'),
        kind: 'dhw_enable',
        value: { enable: !!s.value }
      });
    }
    cfg.schedules = all;
  }

  function bits(mask){
    const m=(Number(mask)>>>0);
    const out=[];
    for(let i=0;i<8;i++) out.push(((m>>i)&1)===1);
    return out;
  }

  let hostEl = null;
  let dhwSched = [];
  let lastFullStatus = null;
  let statusTimer = 0;
  let baseTuvEnabled = false; // cfg.tuv.enabled (arm) – baseline pro vizualizaci týdenního plánu

  function dayPills(selectedDays){
    const set = new Set((selectedDays||[]).map(Number));
    return `<div class="row" style="gap:6px; margin-top:6px;">` + DAYS.map(x=>{
      const on = set.has(x.d);
      return `<label class="pill" style="cursor:pointer; user-select:none; opacity:${on?1:.55};"><input type="checkbox" data-day="${x.d}" ${on?'checked':''} style="margin-right:6px;">${x.t}</label>`;
    }).join('') + `</div>`;
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

  function computeWeekSegmentsFromEvents(events, baselineEnabled){
    // events: [{enabled, days:[1..7], at:'HH:MM', value:true/false}]
    // baselineEnabled: default state at Monday 00:00
    const expanded = [];
    (events||[]).forEach((e, idx)=>{
      if(!e || e.enabled===false) return;
      const days = Array.isArray(e.days)&&e.days.length ? e.days : [1,2,3,4,5,6,7];
      const t = parseHmToMin(e.at||'00:00');
      days.forEach(d=>{
        const di = Number(d)-1;
        if(di<0||di>6) return;
        expanded.push({di, t, v: !!e.value, _idx: idx});
      });
    });
    expanded.sort((a,b)=> (a.di-b.di) || (a.t-b.t) || (Number(a.v)-Number(b.v)) );

    const byDay = Array.from({length:7}, ()=>[]);
    expanded.forEach(e=>byDay[e.di].push(e));

    const out = Array.from({length:7}, ()=>[]);
    let state = !!baselineEnabled;
    for(let di=0;di<7;di++){
      let cur = 0;
      const ev = byDay[di];
      for(const it of ev){
        const t = Math.max(0, Math.min(1440, it.t));
        if(state && t>cur){
          out[di].push({a:cur, b:t});
        }
        state = !!it.v;
        cur = t;
      }
      if(state && cur<1440){
        out[di].push({a:cur, b:1440});
      }
    }
    return out;
  }

  function renderWeekPlanInto(el, segmentsByDay, opts={}){
    if(!el) return;
    const mini = !!opts.mini;
    // Mřížka po hodinách (0..24). Popisky času řešíme samostatnou osou pod posledním řádkem.
    const ticks = Array.from({length:25}, (_,i)=>i);
    const tickHtml = ticks.map(h=>{
      const left = (h/24)*100;
      return `<div class="tick" style="left:${left}%"></div>`;
    }).join('');

    const minToHm = (m)=>{
      const mm = Math.max(0, Math.min(1440, Number(m)||0));
      let hh = Math.floor(mm/60);
      let mi = Math.floor(mm%60);
      if(hh===24) return '24:00';
      return `${hh}:${String(mi).padStart(2,'0')}`;
    };

    el.classList.toggle('weekPlanMini', mini);
    el.innerHTML = DAYS.map((d,di)=>{
      const daySegs = (segmentsByDay && segmentsByDay[di]) ? segmentsByDay[di] : [];
      const segs = daySegs.map(s=>{
        const left = (s.a/1440)*100;
        const w = Math.max(0, ((s.b-s.a)/1440)*100);
        const tt = `Ohřev od ${minToHm(s.a)}–${minToHm(s.b)}`;
        return `<div class="timeSeg" data-tt="${esc(tt)}" style="left:${left}%; width:${w}%"></div>`;
      }).join('');
      return `
        <div class="weekRow">
          <div class="weekDay">${esc(d.t)}</div>
          <div class="timeBar">
            ${tickHtml}
            ${segs}
          </div>
        </div>
      `;
    }).join('');

    if(App.ui && typeof App.ui.bindWeekPlanTooltips === 'function'){
      App.ui.bindWeekPlanTooltips(el);
    }
  }

  function renderWeekAxis(barEl){
    if(!barEl) return;
    const ticks = Array.from({length:25}, (_,i)=>i);
    barEl.innerHTML = ticks.map(h=>{
      const left = (h/24)*100;
      const lbl = (h===24) ? '24:00' : `${h}:00`;
      const cls = ['axisLabel', (h===0?'isStart':''), (h===24?'isEnd':'')].filter(Boolean).join(' ');
      return `<div class="${cls}" style="left:${left}%">${esc(lbl)}</div>`;
    }).join('');
  }

  function render(){
    if(!hostEl) return;
    hostEl.innerHTML = `
      <div class="pageHead">
        <div class="pageTitle">Ohřev TUV</div>
        <div class="pageActions">
          <button class="btn" id="dhwReload">Načíst</button>
          <button class="btn" id="dhwSave">Uložit</button>
        </div>
      </div>

      <div class="grid12">

        <div class="card col6">
          <h2>Stav</h2>
          <div class="kv">
            <div class="k">Funkce (enabled)</div><div class="v" id="dhwStEnabled">—</div>
            <div class="k">Aktivní ohřev</div><div class="v" id="dhwStActive">—</div>
            <div class="k">Požadavek (IN1)</div><div class="v" id="dhwStDemand">—</div>
            <div class="k">Relé kotel (R5)</div><div class="v" id="dhwStBoiler">—</div>
            <div class="k">Důvod</div><div class="v" id="dhwStReason">—</div>
          </div>
          <div class="hint">Pozn.: Pevné mapování v projektu: IN1 = požadavek TUV, IN2 = den/noc ekviterm, R3 = přepínací ventil, R5 = požadavek kotli.</div>
        </div>

        <div class="card col6 cfgCompact">
          <h2>Konfigurace</h2>

          <div class="dhwCfgRow" style="margin-top:6px;">
            <label class="chk"><input type="checkbox" id="tuvEnabled"><span>Povolit ohřev TUV (arm)</span></label>
            <label class="chk"><input type="checkbox" id="tuvRestoreEq"><span>Vrátit ekviterm ventil po TUV</span></label>
          </div>

          <div class="dhwCfgGrid">
            <div>
              <div class="k">Cíl bojleru (°C)</div>
              <input class="sel" id="tuvTarget" type="number" min="30" max="80" step="0.5" style="width:100%;">
            </div>
            <div>
              <div class="k">Hystereze (°C)</div>
              <input class="sel" id="tuvHyst" type="number" min="0" max="20" step="0.5" style="width:100%;">
            </div>
          </div>

          <h3>Přepínací ventil TUV (R3)</h3>
          <div class="dhwCfgRow">
            <label class="chk"><input type="checkbox" id="tuvBypassEnabled"><span>Použít 0/100% (bypass)</span></label>
            <label class="chk"><input type="checkbox" id="tuvInvert"><span>Invertovat směr</span></label>
          </div>

          <div class="dhwCfgGrid">
            <div>
              <div class="k">Poloha při TUV aktivní (%)</div>
              <input class="sel" id="tuvDhwPct" type="number" min="0" max="100" step="1" style="width:100%;">
            </div>
            <div>
              <div class="k">Poloha při TUV neaktivní (%)</div>
              <input class="sel" id="tuvChPct" type="number" min="0" max="100" step="1" style="width:100%;">
            </div>
          </div>

          <div class="row">
            <button class="btn" id="btnValveToDhw">Ventil → TUV</button>
            <button class="btn" id="btnValveToCh">Ventil → topení</button>
            <button class="btn" id="btnValveStop">STOP</button>
          </div>

          <details style="margin-top:8px;">
            <summary class="hint">Pokročilé / API</summary>
            <div class="hint">Ovládání ventilu používá <span class="mono">POST /api/valve/goto?master=3&amp;pct=…</span> (R3 jako master).</div>
            <div class="hint">Pozn.: <span class="mono">tuv.targetTempC</span> a <span class="mono">tuv.hysteresisC</span> může firmware v této verzi ignorovat.</div>
          </details>
        </div>

        <div class="card col12">
          <h2>Časové plánování ohřevu TUV (týdenní přehled)</h2>
          <div class="hint">Zobrazení vychází z <span class="mono">cfg.schedules[]</span> (kind <span class="mono">dhw_enable</span>). Oranžová = funkce „TUV enabled“.</div>

          <div id="dhwWeekPlan" class="weekPlan"></div>
          <div id="dhwWeekAxis" class="weekAxisRow">
            <div class="axisDay">&nbsp;</div>
            <div id="dhwWeekAxisBar" class="weekAxisBar"></div>
          </div>
          <div class="weekLegend">
            <span class="legendSwatch"></span>
            <span>aktivní okno (TUV enabled)</span>
          </div>

          <h3 style="margin-top:14px;">Rychlé přidání okna</h3>
          <div class="grid3">
            <div>
              <div class="k">Dny</div>
              <div id="dhwWinDays"></div>
            </div>
            <div>
              <div class="k">Od</div>
              <input class="sel" id="dhwWinFrom" type="time" step="60" value="06:00" style="width:100%;"/>
            </div>
            <div>
              <div class="k">Do</div>
              <input class="sel" id="dhwWinTo" type="time" step="60" value="22:00" style="width:100%;"/>
            </div>
          </div>
          <div class="row" style="margin-top:10px;">
            <button class="btn" id="dhwAddWindow">+ Přidat okno</button>
            <button class="btn" id="dhwAddOn">+ Událost: Zapnout</button>
            <button class="btn" id="dhwAddOff">+ Událost: Vypnout</button>
            <button class="btn danger" id="dhwClearPlan">Smazat týdenní plán</button>
          </div>

          <details style="margin-top:12px;">
            <summary class="hint">Detailní editace událostí (pokročilé)</summary>
            <div id="dhwSchedList" style="margin-top:12px;"></div>
          </details>
        </div>

      </div>
    `;

    $('#dhwReload').onclick = async()=>{ await reloadAll(true); };
    $('#dhwSave').onclick = async()=>{ await saveAll(); };

    // day selector for quick window
    const winDaysEl = $('#dhwWinDays');
    if(winDaysEl){
      winDaysEl.innerHTML = dayPills([1,2,3,4,5,6,7]);
    }

    $('#dhwAddOn').onclick = ()=>{ dhwSched.push({enabled:true, days:[1,2,3,4,5,6,7], at:'06:00', value:true}); renderSchedules(); };
    $('#dhwAddOff').onclick = ()=>{ dhwSched.push({enabled:true, days:[1,2,3,4,5,6,7], at:'22:00', value:false}); renderSchedules(); };

    const clearBtn = $('#dhwClearPlan');
    if(clearBtn){
      clearBtn.onclick = ()=>{
        dhwSched = [];
        renderSchedules();
        App.util.toast('Týdenní plán smazán (nezapomeň Uložit).');
      };
    }

    const addWinBtn = $('#dhwAddWindow');
    if(addWinBtn){
      addWinBtn.onclick = ()=>{
        const fromEl = $('#dhwWinFrom');
        const toEl = $('#dhwWinTo');
        const from = String((fromEl && fromEl.value) || '06:00');
        const to = String((toEl && toEl.value) || '22:00');
        const fromM = parseHmToMin(from);
        const toM = parseHmToMin(to);
        // selected days
        let days = [1,2,3,4,5,6,7];
        if(winDaysEl){
          const checked = Array.from(winDaysEl.querySelectorAll('input[data-day]')).filter(cb=>cb.checked).map(cb=>Number(cb.dataset.day));
          if(checked.length) days = checked;
        }

        // ON event
        dhwSched.push({enabled:true, days: days.slice(), at: from, value:true});

        // OFF event – handle cross-midnight by shifting days +1
        if(toM <= fromM){
          const offDays = days.map(d=> ((d % 7) + 1));
          dhwSched.push({enabled:true, days: offDays, at: to, value:false});
        } else {
          dhwSched.push({enabled:true, days: days.slice(), at: to, value:false});
        }

        renderSchedules();
      };
    }

    $('#btnValveToDhw').onclick = ()=>valveGoto(clamp(num($('#tuvDhwPct').value, 100), 0, 100));
    $('#btnValveToCh').onclick = ()=>valveGoto(clamp(num($('#tuvChPct').value, 0), 0, 100));
    $('#btnValveStop').onclick = ()=>valveStop();
  }

  function renderSchedules(){
    // weekly overview
    const planEl = $('#dhwWeekPlan');
    if(planEl){
      // Weekly plan should show only schedule windows (dhw_enable). "arm" (tuv.enabled) must not fill the whole week.
      const segs = computeWeekSegmentsFromEvents(dhwSched, false);
      renderWeekPlanInto(planEl, segs, {mini:false});
    }

    const axisBar = $('#dhwWeekAxisBar');
    if(axisBar) renderWeekAxis(axisBar);

    const box = $('#dhwSchedList');
    if(!box) return;
    if(!Array.isArray(dhwSched) || dhwSched.length===0){
      box.innerHTML = `<div class="hint">Žádné události. Přidejte „Zapnout“/„Vypnout“.</div>`;
      return;
    }

    box.innerHTML = dhwSched.map((s,idx)=>{
      const title = s.value ? 'Zapnout' : 'Vypnout';
      return `
        <div class="tile" style="margin-bottom:10px;">
          <div class="t">
            <div class="head">
              <div class="id"><b>${esc(title)}</b></div>
              <div class="desc">kind: <span class="mono">dhw_enable</span></div>
            </div>
            <div class="badges">
              <span class="badge ${s.enabled?'on':'off'}">${s.enabled?'ENABLED':'disabled'}</span>
            </div>
          </div>

          <div class="grid2">
            <div>
              <div class="k">Čas</div>
              <input class="sel" type="time" step="60" data-k="at" data-idx="${idx}" value="${esc(s.at||'06:00')}" style="width:100%;" />
            </div>
            <div>
              <div class="k">Povoleno</div>
              <label class="chk" style="margin-top:6px;">
                <input type="checkbox" data-k="enabled" data-idx="${idx}" ${s.enabled?'checked':''}>
                <span>aktivní</span>
              </label>
            </div>
          </div>

          <div class="k" style="margin-top:10px;">Dny</div>
          <div data-days="${idx}">${dayPills(s.days)}</div>

          <div class="row" style="justify-content:space-between;">
            <button class="btn danger" data-del="${idx}">Smazat</button>
            <div class="hint">Tip: pro „okno“ nastavte dvě události se stejnými dny.</div>
          </div>
        </div>
      `;
    }).join('');

    // bind
    box.querySelectorAll('input[data-k]').forEach(inp=>{
      const idx = Number(inp.dataset.idx);
      const k = inp.dataset.k;
      inp.onchange = ()=>{
        if(!dhwSched[idx]) return;
        if(k==='enabled') dhwSched[idx].enabled = !!inp.checked;
        if(k==='at') dhwSched[idx].at = String(inp.value||'06:00');
      };
    });

    box.querySelectorAll('[data-days]').forEach(wrap=>{
      const idx = Number(wrap.dataset.days);
      wrap.querySelectorAll('input[data-day]').forEach(cb=>{
        cb.onchange = ()=>{
          if(!dhwSched[idx]) return;
          const day = Number(cb.dataset.day);
          const set = new Set((dhwSched[idx].days||[]).map(Number));
          if(cb.checked) set.add(day); else set.delete(day);
          dhwSched[idx].days = Array.from(set).sort((a,b)=>a-b);
          // re-render pills opacity/text quickly
          renderSchedules();
        };
      });
    });

    box.querySelectorAll('button[data-del]').forEach(btn=>{
      btn.onclick = ()=>{
        const idx = Number(btn.dataset.del);
        dhwSched.splice(idx,1);
        renderSchedules();
      };
    });
  }

  function fillFormFromConfig(cfg){
    const tuv = getTuvCfg(cfg);
    const bv = tuv.bypassValve || {};

    baseTuvEnabled = !!tuv.enabled;

    $('#tuvEnabled').checked = !!tuv.enabled;
    $('#tuvRestoreEq').checked = !!tuv.restoreEqValveAfter;
    $('#tuvTarget').value = String(num(tuv.targetTempC, 50));
    $('#tuvHyst').value = String(num(tuv.hysteresisC, 2));

    $('#tuvBypassEnabled').checked = bv.enabled !== false;
    $('#tuvDhwPct').value = String(clamp(num(bv.bypassPct, 0),0,100));
    $('#tuvChPct').value = String(clamp(num(bv.chPct, 100),0,100));
    $('#tuvInvert').checked = !!bv.invert;
  }

  function readFormIntoConfig(cfg){
    const tuv = getTuvCfg(cfg);
    tuv.enabled = !!$('#tuvEnabled').checked;
    tuv.restoreEqValveAfter = !!$('#tuvRestoreEq').checked;

    tuv.targetTempC = clamp(num($('#tuvTarget').value, 50), 30, 80);
    tuv.hysteresisC = clamp(num($('#tuvHyst').value, 2), 0, 20);

    const bv = ensurePath(tuv, ['bypassValve'], {});
    bv.enabled = !!$('#tuvBypassEnabled').checked;
    bv.bypassPct = clamp(num($('#tuvDhwPct').value, 0), 0, 100);
    bv.chPct = clamp(num($('#tuvChPct').value, 100), 0, 100);
    bv.invert = !!$('#tuvInvert').checked;
  }

  async function saveAll(){
    const cfg = await loadCfg();
    readFormIntoConfig(cfg);
    replaceDhwSchedules(cfg, dhwSched);
    try{
      await App.api.postText('/api/config/apply', JSON.stringify(cfg, null, 2), 'text/plain');
      App.state.config = cfg;
      App.util.toast('Uloženo.', 'ok');
      // načti hned status (aby UI rychle ukázalo změnu)
      await reloadStatus();
    }catch(e){
      App.util.toast('Uložení selhalo: ' + e.message, 'err');
    }
  }

  async function reloadStatus(){
    try{
      lastFullStatus = await App.api.getJson('/api/status');
    }catch(_){
      lastFullStatus = null;
    }
    updateStatusUI(App.state.fast);
  }

  async function reloadAll(force){
    if(force){
      App.state.config = null;
    }
    const cfg = await loadCfg();
    fillFormFromConfig(cfg);
    dhwSched = extractDhwSchedules(cfg);
    renderSchedules();
    await reloadStatus();
  }

  function updateStatusUI(fast){
    const st = lastFullStatus;
    const tuv = st && st.tuv ? st.tuv : null;

    const iBits = fast ? bits(fast.i) : [];
    // IN1 = požadavek TUV
    const demand = (iBits.length>=1) ? iBits[0] : null;

    $('#dhwStDemand').textContent = (demand==null) ? '—' : (demand ? 'ACTIVE' : 'bez požadavku');

    if(!tuv){
      $('#dhwStEnabled').textContent = '—';
      $('#dhwStActive').textContent = '—';
      $('#dhwStBoiler').textContent = '—';
      $('#dhwStReason').textContent = '—';
      return;
    }

    $('#dhwStEnabled').textContent = tuv.enabled ? 'true' : 'false';
    $('#dhwStEnabled').className = 'v ' + (tuv.enabled ? 'ok' : '');
    $('#dhwStActive').textContent = tuv.active ? 'true' : 'false';
    $('#dhwStActive').className = 'v ' + (tuv.active ? 'ok' : '');
    $('#dhwStBoiler').textContent = tuv.boilerRelayOn ? 'ON' : 'OFF';
    $('#dhwStBoiler').className = 'v ' + (tuv.boilerRelayOn ? 'ok' : '');
    $('#dhwStReason').textContent = tuv.reason || '—';
  }

  async function valveGoto(pct){
    pct = clamp(Number(pct),0,100);
    try{
      await App.api.postJson(`/api/valve/goto?master=3&pct=${pct}`, {});
      App.util.toast(`Ventil → ${pct}%`, 'ok');
    }catch(e){
      App.util.toast('Ventil: ' + e.message, 'err');
    }
  }

  async function valveStop(){
    try{
      await App.api.postJson(`/api/valve/stop?master=3`, {});
      App.util.toast('Ventil STOP', 'ok');
    }catch(e){
      App.util.toast('Ventil: ' + e.message, 'err');
    }
  }

  App.pages.dhw = {
    async mount(host){
      hostEl = host;
      render();
      await reloadAll(false);

      // background refresh of /api/status just for this page
      if(statusTimer) clearInterval(statusTimer);
      statusTimer = setInterval(()=>{
        if(!hostEl) return;
        reloadStatus();
      }, 7000);
    },
    update(fast){
      updateStatusUI(fast);
    }
  };

})();
