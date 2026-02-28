window.App=window.App||{};
let sse=null;
let pollTimer=null;
function startPolling(){ if(pollTimer) return; pollTimer=setInterval(refreshOnce,5000); }
function stopPolling(){ if(!pollTimer) return; clearInterval(pollTimer); pollTimer=null; }
let currentPage='dashboard';
App.state={fast:null,bleStatus:null,configText:null,config:null};
const PAGE_TITLES={dashboard:'Dashboard',equitherm:'Ekviterm',opentherm:'OpenTherm',heatloss:'Tepelné ztráty',dhw:'Ohřev TUV',recirc:'Cirkulace',aku:'Akumulační nádrž',thermometers:'Teploměry',settings:'Nastavení',ota:'OTA',fileman:'Správce souborů'};
function setOnline(on){const d=App.util.$('#navOnlineDot');if(d) d.style.background=on?'var(--ok)':'var(--bad)';const t=App.util.$('#navOnlineText');if(t) t.textContent=on?'online':'offline';}
function fmtUptime(tsMs){
  if(tsMs==null||isNaN(tsMs)) return '—';
  const sec=Math.floor(Number(tsMs)/1000);
  const d=Math.floor(sec/86400);
  const h=Math.floor((sec%86400)/3600);
  const m=Math.floor((sec%3600)/60);
  const s=sec%60;
  return (d?`${d}d `:'')+`${h}h ${m}m ${s}s`;
}
function updHeader(fast){
if(!fast) return;
// RTC / device time (from /api/fast -> n.ti)
const nt = fast.n || {};
const timeEl = App.util.$('#hdrTime');
if(timeEl){
  const iso = nt.ti || nt.timeIso || null;
  if(iso){
    // Display HH:MM:SS (local) + optional source badge
    const d = new Date(iso);
    const hh = String(d.getHours()).padStart(2,'0');
    const mm = String(d.getMinutes()).padStart(2,'0');
    const ss = String(d.getSeconds()).padStart(2,'0');
    const src = nt.ts || nt.timeSource || '';
    timeEl.textContent = `${hh}:${mm}:${ss}${src?` (${src})`:``}`;
    timeEl.className = 'v ' + ((nt.tv===false)?'warn':'ok');
  }else{
    timeEl.textContent = '—';
    timeEl.className = 'v warn';
  }
}

// OpenTherm quick status
const otEl = App.util.$('#hdrOt');
if(otEl){
  const ot = fast.ot || {};
  let txt = 'off';
  let cls = 'warn';
  if(ot.en){
    if(ot.fl){ txt = 'FAULT'; cls = 'bad'; }
    else if(ot.rd){
      // show activity flags compactly
      const flags = [ot.ca?'CH':'', ot.da?'DHW':'', ot.fo?'🔥':''].filter(Boolean).join(' ');
      txt = flags ? `OK ${flags}` : 'OK';
      cls = 'ok';
    }
    else { txt = 'init'; cls = 'warn'; }
  }
  otEl.textContent = txt;
  otEl.className = 'v ' + cls;
}

// Header system status
App.util.$('#hdrUptime')&&(App.util.$('#hdrUptime').textContent=fmtUptime(fast.ts));
const ctrl = fast.ctrl==='A' ? 'AUTO' : (fast.ctrl==='M' ? 'MANUAL' : (fast.ctrl||'—'));
App.util.$('#hdrMode')&&(App.util.$('#hdrMode').textContent=`${fast.mode||'—'} / ${ctrl}`);

// BLE quick info (from /api/fast)
const hb=App.util.$('#hdrBle');
if(hb){
  const b=fast.b;
  hb.className='v';
  if(!b || b.en!==true){ hb.textContent='off'; hb.classList.add('warn'); }
  else if(b.ok===true && b.t!=null){ hb.textContent=`${Number(b.t).toFixed(1)} °C`; hb.classList.add('ok'); }
  else if(b.fr===false){ hb.textContent='stale'; hb.classList.add('bad'); }
  else { hb.textContent='—'; hb.classList.add('warn'); }
}

const e=fast.e||{},d=fast.d||{},c=fast.c||{},a=fast.a||{};
App.util.$('#pillEq')&&( App.util.$('#pillEq').textContent=`Ekviterm: ${e.en?(e.ac?'aktivní':'zapnuto'):'vypnuto'}` );
App.util.$('#pillTuv')&&( App.util.$('#pillTuv').textContent=`TUV: ${d.en?(d.ac?'aktivní':'zapnuto'):'vypnuto'}` );
App.util.$('#pillRc')&&( App.util.$('#pillRc').textContent=`Cirkulace: ${c.en?(c.ac?'aktivní':'zapnuto'):'vypnuto'}` );
App.util.$('#pillAku')&&( App.util.$('#pillAku').textContent=`Aku: ${a.en?(a.ac?'aktivní':'zapnuto'):'vypnuto'}` );
}
async function refreshOnce(){
  try{
    const fast=await App.api.getJson('/api/fast');
    App.state.fast=fast;
    setOnline(true);
    updHeader(fast);
    App.dashboard.updateAll(fast);
    if(currentPage && App.pages?.[currentPage]?.update) {
      try{ App.pages[currentPage].update(fast); }catch(_){ }
    }
  }catch(e){
    setOnline(false);
  }
}
async function loadBleStatus(){try{App.state.bleStatus=await App.api.getJson('/api/ble/status');}catch(_){App.state.bleStatus=null;}}

async function loadConfig(){
  // Dashboard widgety (např. "Teploty") potřebují znát role z /config.json hned po startu.
  // Pokud je dostupný App.roles.loadConfig(), použijeme ho (sjednocený parser + defaulty).
  try{
    if(App.roles && typeof App.roles.loadConfig === 'function'){
      await App.roles.loadConfig();
      return;
    }
    const txt = await App.api.getText('/config.json');
    App.state.config = JSON.parse(txt);
  }catch(_){
    App.state.config = App.state.config && typeof App.state.config==='object' ? App.state.config : {};
  }
}
function setPage(page){
  page = page || 'dashboard';
  const prevPage = currentPage;
  // allow pages to cleanup timers/listeners
  if(prevPage && prevPage !== page && App.pages?.[prevPage]?.unmount){
    try{ App.pages[prevPage].unmount(); }catch(_){}
  }
  currentPage = page;App.util.$('#pageTitle').textContent=PAGE_TITLES[page]||page;
document.querySelectorAll('.navItem').forEach(b=>b.classList.toggle('isActive',b.dataset.page===page));
const canLayout=page==='dashboard';App.util.$('#btnLayoutEdit').disabled=!canLayout;App.util.$('#btnLayoutSave').disabled=!canLayout;if(!canLayout) App.dashboard.closeEditor();
const root=App.util.$('#pageContent');
if(page==='dashboard'){root.innerHTML='<div id="dashboardRoot"></div>';App.dashboard.mount('#dashboardRoot');App.dashboard.updateAll(App.state.fast);return;}
if(page==='equitherm'){
  root.innerHTML='<div id="equithermRoot"></div>';
  const host = App.util.$('#equithermRoot');
  if(App.pages?.equitherm?.mount) App.pages.equitherm.mount(host);
  if(App.pages?.equitherm?.update) App.pages.equitherm.update(App.state.fast);
  return;
}
if(page==='opentherm'){
  root.innerHTML='<div id="openthermRoot"></div>';
  const host = App.util.$('#openthermRoot');
  if(App.pages?.opentherm?.mount) App.pages.opentherm.mount(host);
  if(App.pages?.opentherm?.update) App.pages.opentherm.update(App.state.fast);
  return;
}
if(page==='thermometers'){
  root.innerHTML='<div id="thermometersRoot"></div>';
  const host = App.util.$('#thermometersRoot');
  if(App.pages?.thermometers?.mount) App.pages.thermometers.mount(host);
  if(App.pages?.thermometers?.update) App.pages.thermometers.update(App.state.fast);
  return;
}
if(page==='settings'){
  root.innerHTML='<div id="settingsRoot"></div>';
  const host = App.util.$('#settingsRoot');
  if(App.pages?.settings?.mount) App.pages.settings.mount(host);
  if(App.pages?.settings?.update) App.pages.settings.update(App.state.fast);
  return;
}
if(page==='dhw'){
  root.innerHTML='<div id="dhwRoot"></div>';
  const host = App.util.$('#dhwRoot');
  if(App.pages?.dhw?.mount) App.pages.dhw.mount(host);
  if(App.pages?.dhw?.update) App.pages.dhw.update(App.state.fast);
  return;
}
if(page==='recirc'){
  root.innerHTML='<div id="recircRoot"></div>';
  const host = App.util.$('#recircRoot');
  if(App.pages?.recirc?.mount) App.pages.recirc.mount(host);
  if(App.pages?.recirc?.update) App.pages.recirc.update(App.state.fast);
  return;
}
if(page==='aku'){
  root.innerHTML='<div id="akuRoot"></div>';
  const host = App.util.$('#akuRoot');
  if(App.pages?.aku?.mount) App.pages.aku.mount(host);
  if(App.pages?.aku?.update) App.pages.aku.update(App.state.fast);
  return;
}
if(page==='heatloss'){
  root.innerHTML='<div id="heatlossRoot"></div>';
  const host = App.util.$('#heatlossRoot');
  if(App.pages?.heatloss?.mount) App.pages.heatloss.mount(host);
  if(App.pages?.heatloss?.update) App.pages.heatloss.update(App.state.fast);
  return;
}
if(page==='ota'){
  root.innerHTML='<div id="otaRoot"></div>';
  const host = App.util.$('#otaRoot');
  if(App.pages?.ota?.mount) App.pages.ota.mount(host);
  if(App.pages?.ota?.update) App.pages.ota.update(App.state.fast);
  return;
}
if(page==='fileman'){
  root.innerHTML='<div id="filemanRoot"></div>';
  const host = App.util.$('#filemanRoot');
  if(App.pages?.fileman?.mount) App.pages.fileman.mount(host);
  if(App.pages?.fileman?.update) App.pages.fileman.update(App.state.fast);
  return;
}
root.innerHTML=`<div class="card"><div class="card__header"><div class="card__title">${PAGE_TITLES[page]||page}</div></div><div class="card__body muted">Tato stránka je připravená pro další rozšíření.</div></div>`;
}
function bindUI(){
document.querySelectorAll('.navItem').forEach(b=>b.addEventListener('click',()=>setPage(b.dataset.page)));
App.util.$('#btnRefresh').addEventListener('click',async()=>{await loadBleStatus();await refreshOnce();});
App.util.$('#btnRestart').addEventListener('click',async()=>{try{await App.api.postJson('/api/restart',{});}catch(_){alert('Restart endpoint není dostupný.');}});
App.util.$('#btnLayoutEdit').addEventListener('click',()=>App.dashboard.openEditor());
App.util.$('#btnLayoutClose').addEventListener('click',()=>App.dashboard.closeEditor());
App.util.$('#btnLayoutSave').addEventListener('click',()=>App.dashboard.saveReload());
}
function startSSE(){
  sse=App.api.startSSE(
    (fast)=>{
      App.state.fast=fast;
      setOnline(true);
      updHeader(fast);
      App.dashboard.updateAll(fast);
      // page live update
      if(currentPage && App.pages?.[currentPage]?.update) {
        try{ App.pages[currentPage].update(fast); }catch(_){ }
      }
    },
    (st)=>{
      const el=App.util.$('#hdrSse');
      if(el){
        el.textContent=st;
        el.className='v '+(st==='connected'?'ok':(st==='connecting'?'warn':'bad'));
      }
      if(st==='connected') stopPolling(); else startPolling();
      if(typeof App.onSseState==='function') try{ App.onSseState(st); }catch(_){ }
    }
  );
}
async function main(){
  bindUI();
  // načíst config ještě před prvním renderem dashboard widgetů
  await loadConfig();
  setPage('dashboard');
  await loadBleStatus();
  await refreshOnce();
  startPolling();
  startSSE();
}
document.addEventListener('DOMContentLoaded',main);
