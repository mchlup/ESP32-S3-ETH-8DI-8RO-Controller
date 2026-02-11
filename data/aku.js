window.App = window.App || {};

(function(){
  const DAYS = [
    {d:1,t:'Po'}, {d:2,t:'Út'}, {d:3,t:'St'}, {d:4,t:'Čt'}, {d:5,t:'Pá'}, {d:6,t:'So'}, {d:7,t:'Ne'}
  ];

  function fmtTemp(v, valid){
    if(!valid || v===null || v===undefined || !isFinite(v)) return '—';
    return `${Number(v).toFixed(1)} °C`;
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
        if(start === end){ pushSeg(di, 0, 1440); return; }
        if(start < end){ pushSeg(di, start, end); }
        else {
          pushSeg(di, start, 1440);
          pushSeg((di+1)%7, 0, end);
        }
      });
    });

    for(let di=0;di<7;di++){
      const a = out[di].slice().sort((x,y)=>x.a-y.a || x.b-y.b);
      const m = [];
      for(const s of a){
        const last = m[m.length-1];
        if(!last || s.a > last.b) m.push({...s});
        else last.b = Math.max(last.b, s.b);
      }
      out[di] = m;
    }
    return out;
  }

  function renderWeekPlanInto(el, segmentsByDay){
    if(!el) return;
    const ticks = [0, 12, 24];
    const tickHtml = ticks.map(h=>`<div class="tick" style="left:${(h/24)*100}%"></div>`).join('');
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
        const tt = `Topná tyč ${hm(s.a)}–${hm(s.b)}`;
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

  function extractAkuWindows(cfg){
    const h = (cfg && cfg.akuHeater && typeof cfg.akuHeater==='object') ? cfg.akuHeater : null;
    const win = h && Array.isArray(h.windows) ? h.windows : [];
    return win.map(w=>({
      days: Array.isArray(w.days) ? w.days : [1,2,3,4,5,6,7],
      start: w.start || '06:00',
      end: w.end || '07:00'
    }));
  }

  const W = {
    id: 'aku',
    title: 'Akumulační nádrž',

    render(host){
      if(!host) return;
      W._cfgTs = 0;
      host.innerHTML = `
        <div class="row" style="gap:8px;flex-wrap:wrap">
          <div id="aku_pills"></div>
        </div>
        <div class="row" style="margin-top:10px;align-items:stretch;gap:12px;flex-wrap:wrap">
          <div style="flex:1;min-width:160px">
            <div class="hint" style="margin-bottom:6px">Teploty v nádrži</div>
            <div id="aku_tank" style="position:relative;max-width:220px;height:140px;border:1px solid rgba(255,255,255,.12);border-radius:18px;overflow:hidden">
              <div style="position:absolute;inset:0;background:linear-gradient(180deg, rgba(255,255,255,.08), rgba(255,255,255,.02))"></div>
              <div style="position:absolute;left:12px;right:12px;top:16px;height:1px;background:rgba(255,255,255,.10)"></div>
              <div style="position:absolute;left:12px;right:12px;top:70px;height:1px;background:rgba(255,255,255,.10)"></div>
              <div style="position:absolute;left:12px;right:12px;bottom:16px;height:1px;background:rgba(255,255,255,.10)"></div>

              <div style="position:absolute;right:10px;top:6px;text-align:right">
                <div class="muted" style="font-size:12px">TOP</div>
                <div id="aku_t_top" style="font-weight:700">—</div>
              </div>

              <div style="position:absolute;right:10px;top:52px;text-align:right">
                <div class="muted" style="font-size:12px">MID</div>
                <div id="aku_t_mid" style="font-weight:700">—</div>
              </div>

              <div style="position:absolute;right:10px;bottom:6px;text-align:right">
                <div class="muted" style="font-size:12px">BOTTOM</div>
                <div id="aku_t_bot" style="font-weight:700">—</div>
              </div>
            </div>
          </div>

          <div style="flex:1;min-width:180px">
            <div id="aku_weekmini" style="margin-top:0"></div>
          </div>
        </div>
        <div class="hr"></div>
        <div class="kv">
          <div class="k">Režim</div><div class="v" id="aku_mode">—</div>
          <div class="k">Relé8 (topná tyč)</div><div class="v" id="aku_r8">—</div>
          <div class="k">Důvod</div><div class="v" id="aku_reason">—</div>
        </div>
        <div class="hint" style="margin-top:8px">Tip: Detailní konfiguraci najdeš na stránce „Akumulační nádrž“.</div>
      `;
    },

    update(fast){
      const host = document.getElementById('w_aku');
      if(!host || !fast) return;
      const a = fast.a || {};

      const en = !!a.en;
      const ac = !!a.ac;
      // Prefer role-based temps (AKU nahoře/uprostřed/dole) if configured.
      const roles = (App.state && App.state.config && Array.isArray(App.state.config.tempRoles)) ? App.state.config.tempRoles : [];
      const t = Array.isArray(fast.t) ? fast.t : [];
      const tv = Array.isArray(fast.tv) ? fast.tv : [];
      const findRoleIdx = (role)=>{
        for(let i=0;i<roles.length;i++) if(String(roles[i]||'')===role) return i;
        return -1;
      };
      const roleTemp = (role, fallbackVal, fallbackValid)=>{
        const idx = findRoleIdx(role);
        if(idx>=0){
          const v = Number(t[idx]);
          const ok = !!tv[idx];
          return {v, ok};
        }
        return {v:fallbackVal, ok:fallbackValid};
      };

      const topFallback = (a.tp==null) ? NaN : Number(a.tp);
      const topFallbackValid = !!a.tv;
      const topT = roleTemp('aku_top', topFallback, topFallbackValid);
      const midT = roleTemp('aku_mid', NaN, false);
      const botT = roleTemp('aku_bottom', NaN, false);
      const mode = (a.mo!=null) ? String(a.mo) : '';
      const reason = (a.rs!=null) ? String(a.rs) : '';

      const rMask = Number(fast.r||0);
      const r8 = !!(rMask & (1<<7));

      const pills = [
        pill(ac ? 'topná tyč: aktivní' : 'topná tyč: neaktivní', ac),
      ];
      if(mode) pills.push(pill(`režim: ${mode}`, true));

      const elP = host.querySelector('#aku_pills');
      if(elP) elP.innerHTML = pills.join(' ');

      const elTop = host.querySelector('#aku_t_top');
      if(elTop) elTop.textContent = fmtTemp(topT.v, topT.ok);
      const elMid = host.querySelector('#aku_t_mid');
      if(elMid) elMid.textContent = fmtTemp(midT.v, midT.ok);
      const elBot = host.querySelector('#aku_t_bot');
      if(elBot) elBot.textContent = fmtTemp(botT.v, botT.ok);

      const elMode = host.querySelector('#aku_mode');
      if(elMode) elMode.textContent = mode || '—';

      const elR8 = host.querySelector('#aku_r8');
      if(elR8) elR8.innerHTML = r8 ? '<span class="badge ok">sepnuté</span>' : '<span class="badge">vypnuté</span>';

      const elReason = host.querySelector('#aku_reason');
      if(elReason) elReason.textContent = reason || (en ? '—' : 'disabled');

      // mini schedule plan (only if cfg present)
      try{
        const cfg = App.state && App.state.config;
        const w = extractAkuWindows(cfg);
        const mini = host.querySelector('#aku_weekmini');
        const m = (cfg && cfg.akuHeater) ? String(cfg.akuHeater.mode||'').toLowerCase() : '';
        if(mini){
          if(m === 'schedule' && w.length){
            mini.style.display = '';
            renderWeekPlanInto(mini, computeWeekSegmentsFromWindows(w));
          } else {
            mini.style.display = 'none';
          }
        }
      }catch(_){ }
    }
  };

  App.registerWidget && App.registerWidget(W);
})();
