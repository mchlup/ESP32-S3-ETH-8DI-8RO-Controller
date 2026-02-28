window.App = window.App || {};

(function(){
  function normRole(s){
    s = String(s||'').trim().toLowerCase();
    if (s === 'dhw' || s === 'tuv') return 'dhw_tank';
    return s;
  }

  function getTempByRole(role, fast, cfg){
    role = normRole(role);
    const t = fast && Array.isArray(fast.t) ? fast.t : null;
    const tv = fast && Array.isArray(fast.tv) ? fast.tv : null;
    const roles = cfg && Array.isArray(cfg.tempRoles) ? cfg.tempRoles : null;
    if (!t || !tv || !roles) return { v: null, valid: false };
    const idx = roles.findIndex(r => normRole(r) === role);
    if (idx < 0 || idx >= t.length || idx >= tv.length) return { v: null, valid: false };
    return { v: t[idx], valid: !!tv[idx] };
  }

  function fmtTemp(v, valid){
    if(!valid || v === null || v === undefined || !isFinite(v)) return '—';
    return `${Number(v).toFixed(1)} °C`;
  }

  function fmtNum(v){
    if(v === null || v === undefined || !isFinite(v)) return '—';
    return String(Number(v).toFixed(1));
  }

  function pill(text, ok){
    const cls = ok ? 'pill fresh' : 'pill';
    return `<span class="${cls}">${text}</span>`;
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

  const DAYS = [
    {d:1,t:'Po'}, {d:2,t:'Út'}, {d:3,t:'St'}, {d:4,t:'Čt'}, {d:5,t:'Pá'}, {d:6,t:'So'}, {d:7,t:'Ne'}
  ];

  function computeWeekSegmentsFromEvents(events, baselineEnabled){
    const expanded = [];
    (events||[]).forEach((e)=>{
      if(!e || e.enabled===false) return;
      const days = Array.isArray(e.days)&&e.days.length ? e.days : [1,2,3,4,5,6,7];
      const t = parseHmToMin(e.at||'00:00');
      days.forEach(d=>{
        const di = Number(d)-1;
        if(di<0||di>6) return;
        expanded.push({di, t, v: !!e.value});
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
        if(state && t>cur) out[di].push({a:cur,b:t});
        state = !!it.v;
        cur = t;
      }
      if(state && cur<1440) out[di].push({a:cur,b:1440});
    }
    return out;
  }

  function renderWeekPlanInto(el, segmentsByDay){
    if(!el) return;
    const ticks = [0, 12, 24];
    const tickHtml = ticks.map(h=>{
      const left = (h/24)*100;
      return `<div class="tick" style="left:${left}%"></div>`;
    }).join('');
    el.classList.add('weekPlan', 'weekPlanMini');
    el.innerHTML = DAYS.map((d,di)=>{
      const daySegs = (segmentsByDay && segmentsByDay[di]) ? segmentsByDay[di] : [];
      const segs = daySegs.map(s=>{
        const left = (s.a/1440)*100;
        const w = Math.max(0, ((s.b-s.a)/1440)*100);
        const aHm = String(Math.floor(s.a/60)).padStart(2,'0') + ':' + String(s.a%60).padStart(2,'0');
        const bHm = String(Math.floor(s.b/60)).padStart(2,'0') + ':' + String(s.b%60).padStart(2,'0');
        const tt = `Ohřev od ${aHm}–${bHm}`;
        return `<div class="timeSeg" data-tt="${App.util.esc(tt)}" style="left:${left}%; width:${w}%"></div>`;
      }).join('');
      return `
        <div class="weekRow">
          <div class="weekDay">${d.t}</div>
          <div class="timeBar">${tickHtml}${segs}</div>
        </div>
      `;
    }).join('');

    if(App.ui && typeof App.ui.bindWeekPlanTooltips === 'function'){
      App.ui.bindWeekPlanTooltips(el);
    }
  }

  function extractDhwSchedules(cfg){
    const all = (cfg && Array.isArray(cfg.schedules)) ? cfg.schedules : [];
    return all.filter(it=> String((it && (it.kind || it.type)) || '').toLowerCase() === 'dhw_enable')
      .map(s=>({
        enabled: s.enabled !== false,
        days: Array.isArray(s.days) ? s.days : [1,2,3,4,5,6,7],
        at: s.at || s.time || '06:00',
        value: !!(s.value ? (s.value.enable ?? s.value.enabled) : (s.params ? (s.params.enable ?? s.params.enabled) : true))
      }));
  }

  const W = {
    id: 'dhw',
    title: 'Ohřev TUV',

    render(host){
      if(!host) return;
      // Ensure weekly mini-plan is rendered immediately after widget mount
      W._cfgTs = 0;
      host.innerHTML = `
        <div class="row" style="gap:8px;flex-wrap:wrap">
          <div id="dhw_pills"></div>
        </div>
        <div id="dhw_weekmini" style="margin-top:10px"></div>
        <div class="hr"></div>
        <div class="kv">
          <div class="k">Bojler</div><div class="v" id="dhw_tank">—</div>
          <div class="k">Cíl / hyst.</div><div class="v" id="dhw_target">—</div>
          <div class="k">Ventil</div><div class="v" id="dhw_valve">—</div>
          <div class="k">Kotel (R5)</div><div class="v" id="dhw_boiler">—</div>
        </div>
        <div class="muted" id="dhw_reason" style="margin-top:8px"></div>
      `;
    },

    update(fast){
      const host = document.getElementById('w_dhw');
      if(!host || !fast) return;

      const d = fast.d || {};
      const rMask = Number(fast.r) >>> 0;
      const iMask = Number(fast.i) >>> 0;

      // FIXED project mapping:
      //   IN1 = požadavek TUV
      //   R3  = přepínací TUV ventil
      //   R5  = požadavek kotli pro TUV
      const inDhw = ((iMask >> 1) & 1) === 1;
      const r3 = ((rMask >> 2) & 1) === 1;
      const r5 = ((rMask >> 4) & 1) === 1;

      const pillsEl = host.querySelector('#dhw_pills');
      const en = !!d.en;
      const ac = !!d.ac;
      const br = (d.br !== undefined) ? !!d.br : r5;

      if(pillsEl){
        pillsEl.innerHTML = [
          pill(ac ? 'Aktivní' : 'Neaktivní', ac),
          pill(r3 ? 'Ventil (R3) TUV' : 'Ventil (R3) topení', r3),
          pill(br ? 'Kotel (R5) ON' : 'Kotel (R5) OFF', br)
        ].join(' ');
      }

      // Bojler TUV: prefer role mapping (config.tempRoles) -> source "dhw_tank".
      // Fallback: allow future back-end fields (d.tt/d.tv) if ever added.
      const tankEl = host.querySelector('#dhw_tank');
      if(tankEl){
        const cfg = (App.state && App.state.config && typeof App.state.config === 'object') ? App.state.config : null;
        const rt = getTempByRole('dhw_tank', fast, cfg);
        if (rt && rt.valid) {
          tankEl.textContent = fmtTemp(rt.v, rt.valid);
        } else {
          tankEl.textContent = fmtTemp(d.tt, !!d.tv);
        }
      }

const targetEl = host.querySelector('#dhw_target');
      if(targetEl){
        const tgt = fmtNum(d.tg);
        const hy = fmtNum(d.hy);
        targetEl.textContent = (tgt !== '—') ? `${tgt} °C / ${hy} °C` : '—';
      }

      const valveEl = host.querySelector('#dhw_valve');
      if(valveEl){
        // Prefer explicit valve fields if ever added to /api/fast, otherwise show R3 state.
        if(d.vm || (d.vp !== null && d.vp !== undefined)){
          const mode = d.vm || '—';
          const pos = (d.vp !== null && d.vp !== undefined && isFinite(d.vp)) ? `${Math.round(d.vp)}%` : '—';
          valveEl.textContent = `${mode} (${pos})`;
        } else {
          valveEl.textContent = r3 ? 'TUV (0%)' : 'TOPENÍ (100%)';
        }
      }

      const boilerEl = host.querySelector('#dhw_boiler');
      if(boilerEl) boilerEl.textContent = br ? 'ON' : 'OFF';

      const reasonEl = host.querySelector('#dhw_reason');
      if(reasonEl){
        const r = d.ta || d.reason || '';
        reasonEl.textContent = r ? `Stav: ${r}` : '';
      }

      // weekly plan: prefer already loaded App.state.config (no extra HTTP every minute)
      const now = Date.now();
      const cfg = (App.state && App.state.config && typeof App.state.config === 'object') ? App.state.config : null;
      if(cfg){
        const el = host.querySelector('#dhw_weekmini');
        const needs = !W._cfgTs || (now - W._cfgTs) > 30000 || (el && el.children.length===0);
        if(needs){
          W._cfgTs = now;
          const sched = extractDhwSchedules(cfg);
          // Weekly plan visualizes only schedule windows (dhw_enable). 'cfg.tuv.enabled' is just "arm".
          const segs = computeWeekSegmentsFromEvents(sched, false);
          renderWeekPlanInto(el, segs);
        }
      } else if(App.api && typeof App.api.getJson === 'function'){
        // Fallback if config isn't loaded for some reason.
        const el = host.querySelector('#dhw_weekmini');
        const needs = !W._cfgTs || (now - W._cfgTs) > 60000 || (el && el.children.length===0);
        if(needs){
          W._cfgTs = now;
          App.api.getJson('/api/config').then(cfg2=>{
            const sched = extractDhwSchedules(cfg2);
            // Weekly plan visualizes only schedule windows (dhw_enable). 'cfg.tuv.enabled' is just "arm".
            const segs = computeWeekSegmentsFromEvents(sched, false);
            renderWeekPlanInto(el, segs);
          }).catch(()=>{});
        }
      }
    }
  };

  App.registerWidget(W);
})();
