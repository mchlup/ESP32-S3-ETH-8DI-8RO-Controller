window.App = window.App || {};

(function(){
  const DAYS = [
    {d:1,t:'Po'}, {d:2,t:'Út'}, {d:3,t:'St'}, {d:4,t:'Čt'}, {d:5,t:'Pá'}, {d:6,t:'So'}, {d:7,t:'Ne'}
  ];

  function fmtTemp(v, valid){
    if(!valid || v===null || v===undefined || !isFinite(v)) return '—';
    return `${Number(v).toFixed(1)} °C`;
  }

  function fmtDur(ms){
    const x = Number(ms);
    if(!isFinite(x) || x <= 0) return '—';
    const s = Math.max(0, Math.round(x / 1000));
    if(s < 60) return `${s}s`;
    const m = Math.floor(s / 60);
    return `${m}m ${s % 60}s`;
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

  function computeWeekSegmentsFromWindows(windows){
    // windows: [{days:[1..7], start:'HH:MM', end:'HH:MM'}]
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
          // whole day
          pushSeg(di, 0, 1440);
          return;
        }
        if(start < end){
          pushSeg(di, start, end);
        } else {
          // crosses midnight -> split across two days
          pushSeg(di, start, 1440);
          const di2 = (di + 1) % 7;
          pushSeg(di2, 0, end);
        }
      });
    });

    // normalize: sort and merge overlaps per day
    for(let di=0;di<7;di++){
      const a = out[di].slice().sort((x,y)=>x.a-y.a || x.b-y.b);
      const m = [];
      for(const s of a){
        const last = m[m.length-1];
        if(!last || s.a > last.b){ m.push({...s}); }
        else last.b = Math.max(last.b, s.b);
      }
      out[di] = m;
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

    const hm = (m)=>{
      const mm = Math.max(0, Math.min(1440, Number(m)||0));
      const hh = Math.floor(mm/60);
      const mi = Math.floor(mm%60);
      return `${String(hh).padStart(2,'0')}:${String(mi).padStart(2,'0')}`;
    };

    el.innerHTML = DAYS.map((d,di)=>{
      const daySegs = (segmentsByDay && segmentsByDay[di]) ? segmentsByDay[di] : [];
      const segs = daySegs.map(s=>{
        const left = (s.a/1440)*100;
        const w = Math.max(0, ((s.b-s.a)/1440)*100);
        const tt = `Cirkulace ${hm(s.a)}–${hm(s.b)}`;
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

  function extractRecircWindows(cfg){
    const rec = (cfg && cfg.dhwRecirc && typeof cfg.dhwRecirc === 'object') ? cfg.dhwRecirc : null;
    const win = rec && Array.isArray(rec.windows) ? rec.windows : [];
    return win.map(w=>({
      days: Array.isArray(w.days) ? w.days : [1,2,3,4,5,6,7],
      start: w.start || '06:00',
      end: w.end || '07:00'
    }));
  }

  const W = {
    id: 'recirc',
    title: 'Cirkulace',

    render(host){
      if(!host) return;
      W._cfgTs = 0;
      host.innerHTML = `
        <div class="row" style="gap:8px;flex-wrap:wrap">
          <div id="rc_pills"></div>
        </div>
        <div id="rc_weekmini" style="margin-top:10px"></div>
        <div class="hr"></div>
        <div class="kv">
          <div class="k">Režim</div><div class="v" id="rc_mode">—</div>
          <div class="k">Zbývá</div><div class="v" id="rc_rem">—</div>
          <div class="k">Návratka</div><div class="v" id="rc_ret">—</div>
          <div class="k">Čerpadlo (R4)</div><div class="v" id="rc_r4">—</div>
        </div>
      `;
    },

    update(fast){
      const host = document.getElementById('w_recirc');
      if(!host || !fast) return;

      const c = fast.c || {};
      const rMask = Number(fast.r) >>> 0;
      const iMask = Number(fast.i) >>> 0;

      // FIXED project mapping:
      //   IN3 = požadavek cirkulace
      //   R4  = cirkulační čerpadlo
      const in3 = ((iMask >> 2) & 1) === 1;
      const r4 = ((rMask >> 3) & 1) === 1;

      const en = !!c.en;
      const ac = !!c.ac;
      const rem = (c.rm != null) ? Number(c.rm) : 0;

      const pillsEl = host.querySelector('#rc_pills');
      if(pillsEl){
        pillsEl.innerHTML = [
          pill(ac ? 'Aktivní' : 'Neaktivní', ac),
          pill(in3 ? 'Požadavek (IN3)' : 'Bez požadavku', in3),
          pill(r4 ? 'Čerpadlo ON' : 'Čerpadlo OFF', r4)
        ].join(' ');
      }

      const modeEl = host.querySelector('#rc_mode');
      if(modeEl) modeEl.textContent = c.mo || '—';

      const remEl = host.querySelector('#rc_rem');
      if(remEl) remEl.textContent = fmtDur(rem);

      const retEl = host.querySelector('#rc_ret');
      if(retEl) retEl.textContent = fmtTemp(c.rt, !!c.rv);

      const r4El = host.querySelector('#rc_r4');
      if(r4El) r4El.textContent = r4 ? 'ON' : 'OFF';

      // weekly mini plan
      const now = Date.now();
      const cfg = (App.state && App.state.config && typeof App.state.config === 'object') ? App.state.config : null;
      const el = host.querySelector('#rc_weekmini');
      const needs = !W._cfgTs || (now - W._cfgTs) > 30000 || (el && el.children.length===0);
      if(needs && el){
        W._cfgTs = now;
        const win = cfg ? extractRecircWindows(cfg) : [];
        const segs = computeWeekSegmentsFromWindows(win);
        renderWeekPlanInto(el, segs);
      }
    }
  };

  App.registerWidget(W);
})();
