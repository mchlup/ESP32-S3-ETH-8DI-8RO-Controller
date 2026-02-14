// heatloss.js - dashboard widget
window.App = window.App || {};

(function(){
  const $ = (s,r)=> (App.util && App.util.$) ? App.util.$(s,r) : (r||document).querySelector(s);
  const E = (s)=> (App.util && App.util.esc) ? App.util.esc(s) : String(s).replace(/[&<>"']/g, c=>({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const fmt = (v, d=1)=> (Number.isFinite(Number(v)) ? Number(v).toFixed(d) : '—');

  function renderInner(root, st){
    const s = (st && st.status) ? st.status : st;
    const cfg = (st && st.config) ? st.config : null;

    const ua = (s && Number.isFinite(Number(s.ua_W_per_K))) ? Number(s.ua_W_per_K) : NaN;
    const designKw = (s && Number.isFinite(Number(s.projectedLossKw))) ? Number(s.projectedLossKw) : NaN;
    const pKw = (s && Number.isFinite(Number(s.powerKw))) ? Number(s.powerKw) : NaN;
    const ti = (s && Number.isFinite(Number(s.indoorC))) ? Number(s.indoorC) : NaN;
    const te = (s && Number.isFinite(Number(s.outdoorC))) ? Number(s.outdoorC) : NaN;
    const dt = (Number.isFinite(ti) && Number.isFinite(te)) ? (ti - te) : NaN;

    const enabled = !!(s && s.enabled);
    const ok = enabled && s && !!s.haveSample && Number.isFinite(ua);

    root.innerHTML = `
      <div class="h2">Tepelné ztráty</div>
      <div class="kv">
        <div class="k">Stav</div><div class="v">${ok ? 'OK' : (enabled ? 'Čekám na data' : 'Vypnuto')}</div>

        <div class="k">T<sub>i</sub></div><div class="v">${fmt(ti,1)} °C</div>
        <div class="k">T<sub>e</sub></div><div class="v">${fmt(te,1)} °C</div>
        <div class="k">ΔT</div><div class="v">${fmt(dt,1)} K</div>

        <div class="k">Výkon (OT)</div><div class="v">${fmt(pKw,2)} kW</div>
        <div class="k">UA</div><div class="v">${fmt(ua,0)} W/K</div>
        <div class="k">Ztráta @ návrh</div><div class="v">${fmt(designKw,2)} kW</div>
      </div>

      <div class="row" style="margin-top:10px; display:flex; gap:10px; flex-wrap:wrap">
        <a class="btn" href="/download/heatloss.csv">Stáhnout heatloss.csv</a>
        <a class="btn" href="/download/opentherm.csv">Stáhnout opentherm.csv</a>
      </div>

      <div class="muted" style="margin-top:8px">
        ${cfg ? ('Okno: '+E(cfg.windowSec)+' s, interval logu: '+E(cfg.logIntervalMs)+' ms, návrh T<sub>e</sub>: '+E(cfg.designOutdoorC)+' °C') : ''}
      </div>
    `;
  }

  App.registerWidget && App.registerWidget({
    id: 'heatloss',
    title: 'Tepelné ztráty',
    defaultSpan: 6,
    render(root){
      let alive = true;
      const tick = async ()=>{
        if (!alive) return;
        try{
          const st = await App.api.getJson('/api/heatloss/status');
          if (!alive) return;
          renderInner(root, st);
        }catch(e){
          if (!alive) return;
          root.innerHTML = `<div class="h2">Tepelné ztráty</div><div class="muted">Chyba načtení</div>`;
        }
      };
      tick();
      const t = setInterval(tick, 2000);
      return ()=>{ alive=false; clearInterval(t); };
    }
  });
})();
