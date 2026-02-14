(function(){
  const api=App.api;
  const util=App.util;

  function el(html){const t=document.createElement('template');t.innerHTML=html.trim();return t.content.firstChild;}
  let root=null;

  async function load(){
    if(!root) return;
    const s=util.$('#hl_status',root);
    try{
      const j=await api.getJson('/api/heatloss/status');
      const st=j.status||{};
      const cfg=j.config||{};
      s.textContent=
        `Enabled: ${st.enabled}\n`+
        `Indoor: ${util.fmtTemp(st.indoorC)}  Outdoor: ${util.fmtTemp(st.outdoorC)}\n`+
        `Power: ${util.fmtNum(st.powerKw,2)} kW\n`+
        `UA: ${util.fmtNum(st.ua_W_per_K,1)} W/K\n`+
        `Projected loss @ ${util.fmtNum(cfg.designOutdoorC,0)}°C: ${util.fmtNum(st.projectedLossKw,2)} kW\n`+
        `Samples: ${st.samples}\n`+
        (st.reason?`Reason: ${st.reason}\n`:'');
      util.$('#hl_cfg',root).textContent=JSON.stringify(cfg,null,2);
    }catch(e){
      s.textContent='Failed to load: '+e;
    }
  }

  function mount(container){
    root=container;
    container.innerHTML='';
    container.appendChild(el(`
      <div class="page">
        <div class="card">
          <div class="cardTitle">Tepelné ztráty – aktuální výpočet</div>
          <pre id="hl_status" class="mono">Loading…</pre>
          <div class="row">
            <a class="btn" href="/download/heatloss.csv" target="_blank">Stáhnout log (CSV)</a>
            <a class="btn" href="/download/opentherm.csv" target="_blank">Stáhnout OpenTherm (CSV)</a>
            <button class="btn" id="hl_clear">Vymazat log</button>
          </div>
        </div>
        <div class="card">
          <div class="cardTitle">Konfigurace (read-only)</div>
          <pre id="hl_cfg" class="mono"></pre>
          <div class="hint">Konfigurace je v /config.json pod klíčem "heatloss".</div>
        </div>
      </div>
    `));
    util.on(util.$('#hl_clear',container),'click', async ()=>{
      try{ await api.postJson('/api/heatloss/clear',{}); }catch(e){}
      load();
    });
    load();
  }

  App.pages=App.pages||{};
  App.pages.heatloss={mount, update:load};
})();