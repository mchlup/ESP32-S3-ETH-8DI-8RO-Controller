// equitherm.js
window.App = window.App || {};

(function(){
  const clamp = (v,a,b)=>Math.max(a,Math.min(b,v));
  const num = (v,def)=>{ const n = Number(v); return Number.isFinite(n) ? n : def; };
  const fmt = (v,d=1)=> (v==null || !Number.isFinite(Number(v))) ? '—' : Number(v).toFixed(d);

  function eqTarget(tout, slope, shift){
    // firmware: (20 - tout) * slope + 20 + shift
    return (20 - tout) * slope + 20 + shift;
  }

  function drawMiniGraph(canvas, eq, outdoorC, night, targetFlowC){
    if(!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width, H = canvas.height;

    const padL=40, padR=10, padT=10, padB=24;
    const x0=padL, y0=padT, x1=W-padR, y1=H-padB;
    const pw=x1-x0, ph=y1-y0;

    const tMin=-20, tMax=20;

    let yMin = num(eq?.minFlow, 25);
    let yMax = num(eq?.maxFlow, 75);
    if(yMin>yMax){ const t=yMin; yMin=yMax; yMax=t; }
    yMin = clamp(yMin, 0, 80);
    yMax = clamp(yMax, 0, 80);
    if(Math.abs(yMax-yMin) < 5) yMax = clamp(yMin+5, 0, 80);

    const xFromT = t => x0 + (t - tMin)/(tMax-tMin) * pw;
    const yFromV = v => y1 - (v - yMin)/(yMax-yMin) * ph;

    ctx.clearRect(0,0,W,H);
    ctx.fillStyle = '#0b0f14';
    ctx.fillRect(0,0,W,H);

    // grid
    ctx.strokeStyle = 'rgba(255,255,255,.06)';
    ctx.lineWidth = 1;
    for(let t=-20; t<=20; t+=10){
      const x=xFromT(t);
      ctx.beginPath(); ctx.moveTo(x,y0); ctx.lineTo(x,y1); ctx.stroke();
      ctx.fillStyle='rgba(231,238,252,.6)';
      ctx.font='10px system-ui, sans-serif';
      ctx.fillText(String(t), x-6, y1+14);
    }
    for(let v=Math.ceil(yMin/10)*10; v<=yMax; v+=10){
      const y=yFromV(v);
      ctx.beginPath(); ctx.moveTo(x0,y); ctx.lineTo(x1,y); ctx.stroke();
    }

    // axes
    ctx.strokeStyle='rgba(255,255,255,.18)';
    ctx.lineWidth=1.2;
    ctx.beginPath(); ctx.moveTo(x0,y0); ctx.lineTo(x0,y1); ctx.lineTo(x1,y1); ctx.stroke();

    // curves
    function curve(slope, shift, color){
      ctx.strokeStyle=color;
      ctx.lineWidth=2;
      ctx.beginPath();
      for(let t=tMin; t<=tMax; t+=1){
        let sp = eqTarget(t, slope, shift) + num(eq?.curveOffsetC,0);
        sp = clamp(sp, yMin, yMax);
        const x=xFromT(t), y=yFromV(sp);
        if(t===tMin) ctx.moveTo(x,y); else ctx.lineTo(x,y);
      }
      ctx.stroke();
    }
    curve(num(eq?.slopeDay,1.6), num(eq?.shiftDay,0), '#2e7dff');
    curve(num(eq?.slopeNight,1.6), num(eq?.shiftNight,0), '#ff9800');

    // current point (green)
    const tout = num(outdoorC, 0);
    const slope = night ? num(eq?.slopeNight,1.6) : num(eq?.slopeDay,1.6);
    const shift = night ? num(eq?.shiftNight,0) : num(eq?.shiftDay,0);
    let sp = eqTarget(tout, slope, shift) + num(eq?.curveOffsetC,0);
    sp = clamp(sp, yMin, yMax);
    const cx=xFromT(clamp(tout,tMin,tMax)), cy=yFromV(sp);
    ctx.fillStyle='#00e676';
    ctx.beginPath(); ctx.arc(cx, cy, 4, 0, Math.PI*2); ctx.fill();

// labels
const tgt = (targetFlowC==null || !Number.isFinite(Number(targetFlowC))) ? sp : Number(targetFlowC);
ctx.fillStyle = 'rgba(231,238,252,.85)';
ctx.font = '11px system-ui, sans-serif';
const modeTxt = night ? 'NIGHT' : 'DAY';
ctx.fillText(`${modeTxt} | Tout ${tout.toFixed(1)}°C → Ttarget ${tgt.toFixed(1)}°C`, 8, 14);

  }

  function ensureConfigLoaded(){
    if(App.state?.config && typeof App.state.config === 'object') return Promise.resolve(App.state.config);
    return App.api.getText('/config.json')
      .then(t=>{ App.state.config = JSON.parse(t); return App.state.config; })
      .catch(_=>{ App.state = App.state || {}; App.state.config = App.state.config || {}; return App.state.config; });
  }

  App.registerWidget({
    id:'equitherm',
    title:'Ekviterm',
    defaultSpan:6,
    render(el){
      el.innerHTML = `
        <div class="card__title">Ekviterm</div>

        <canvas class="eqMiniCanvas" data-el="g" width="420" height="190"></canvas>

        <div class="eqValveBox" style="margin-top:10px">
          <div class="row" style="justify-content:space-between;gap:10px;align-items:center">
            <div class="muted">Směšovací ventil</div>
            <div class="muted"><span data-k="vp">—</span> → <span data-k="vt">—</span></div>
          </div>
          <div class="eqValveBar" data-el="vb">
            <div class="eqValveFill" data-el="vf"></div>
            <div class="eqValveMark" data-el="vm"></div>
          </div>
          <div class="eqValveHint muted" data-k="vm">—</div>
        </div>

        <div class="kv" style="margin-top:10px">
          <div class="k">Enabled</div><div class="v" data-k="en">—</div>
          <div class="k">Active</div><div class="v" data-k="ac">—</div>
          <div class="k">Night</div><div class="v" data-k="ni">—</div>
          <div class="k">Outdoor</div><div class="v" data-k="to">—</div>
          <div class="k">Flow</div><div class="v" data-k="tf">—</div>
          <div class="k">Target</div><div class="v" data-k="tg">—</div>
          <div class="k">Reason</div><div class="v" data-k="re">—</div>
        </div>
      `;

      // Load config once so graph can be drawn even before first SSE tick
      ensureConfigLoaded().then(cfg=>{
        const c = el.querySelector('[data-el="g"]');
        drawMiniGraph(c, cfg?.equitherm || {}, 0, false, 0);
      });
    },
    update(fast, host){
      const e = fast?.e || {};
      const root = host || document;
      const map = {
        en: e.en, ac: e.ac, ni: e.ni,
        to: e.to==null?'—':`${fmt(e.to)} °C`,
        tf: e.tf==null?'—':`${fmt(e.tf)} °C`,
        tg: e.tg==null?'—':`${fmt(e.tg)} °C`,
        vp: e.vp==null?'—':`${Math.round(e.vp)} %`,
        vt: e.vt==null?'—':`${Math.round(e.vt)} %`,
        re: e.rs ?? e.reason ?? '',
        vm: ''
      };

      // Valve diagram
      const vp = num(e.vp, 0);
      const vt = (e.vt==null) ? vp : num(e.vt, vp);
      const mv = !!(e.mv ?? e.moving ?? e.vmoving);
      const dir = (vt > vp + 0.2) ? 'opening' : ((vt < vp - 0.2) ? 'closing' : 'hold');
      map.vm = mv ? (dir==='opening'?'otevírá…':(dir==='closing'?'zavírá…':'pohyb…')) : 'stojí';

      const bar = root.querySelector?.('[data-el="vb"]') || document.querySelector('[data-el="vb"]');
      const fill = root.querySelector?.('[data-el="vf"]') || document.querySelector('[data-el="vf"]');
      const mark = root.querySelector?.('[data-el="vm"]') || document.querySelector('[data-el="vm"]');
      if (fill) fill.style.width = `${clamp(vp,0,100)}%`;
      if (mark) mark.style.left = `${clamp(vt,0,100)}%`;
      if (bar) {
        bar.classList.toggle('isMoving', mv);
        bar.classList.toggle('isOpening', mv && dir==='opening');
        bar.classList.toggle('isClosing', mv && dir==='closing');
      }

      for (const k in map) {
        const el = root.querySelector?.(`[data-k="${k}"]`) || document.querySelector(`[data-k="${k}"]`);
        if (el) el.textContent = (map[k]===true?'true':map[k]===false?'false':(map[k]??'—'));
      }

      // Graph update (uses current config + outdoor/night)
      const canvas = root.querySelector?.('[data-el="g"]') || document.querySelector('[data-el="g"]');
      const cfg = App.state?.config || {};
      if (canvas) drawMiniGraph(canvas, cfg?.equitherm || {}, num(e.to,0), !!e.ni, num(e.tg, NaN));
    }
  });
})();
