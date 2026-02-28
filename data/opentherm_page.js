window.App=window.App||{};App.pages=App.pages||{};(function(){
const $=(s,r)=>App.util.$(s,r),E=App.util.esc;
let H=null,ST=null,CFG=null,PROTO=null;
const protoName=(id)=>{id=Number(id);if(!PROTO||!Array.isArray(PROTO.items))return '#'+id;const it=PROTO.items.find(x=>Number(x.id)===id);return it?it.name:('#'+id);};

// Poll templates (OpenTherm Data-IDs)
const POLL_TEMPLATES={
  minimal:[0,17,18,25,27,28],
  standard:[0,17,18,25,27,28,56,57,15,26,24,5,125],
  debug:[0,1,5,14,15,16,17,18,19,24,25,26,27,28,56,57,124,125]
};
const N=v=>{v=Number(v);return Number.isFinite(v)?v:NaN;};
const TF=v=>Number.isFinite(N(v))?N(v).toFixed(1):'—';
const TC=v=>Number.isFinite(N(v))?TF(v)+' °C':'—';
const TP=v=>Number.isFinite(N(v))?Math.round(N(v))+' %':'—';
const AGE=ms=>{ms=N(ms);if(!Number.isFinite(ms)||ms<0)return '—';if(ms<1000)return Math.round(ms)+' ms';const s=Math.round(ms/1000);if(s<60)return s+' s';const m=Math.floor(s/60);return m+' min '+(s%60)+' s';};
const tag=(txt,cls)=>'<span class="tag '+cls+'">'+E(txt)+'</span>';

function fromFast(fast){
  const ot=fast&&fast.ot;
  if(!ot) return null;
  const a=(Number.isFinite(N(fast.ts))&&Number.isFinite(N(ot.lu))&&N(fast.ts)>=N(ot.lu)&&N(ot.lu)>0)?(N(fast.ts)-N(ot.lu)):null;
  return {
    present:!!ot.en, ready:!!ot.rd, fault:!!ot.fl,
    chEnable:!!ot.ce, dhwEnable:!!ot.de, chActive:!!ot.ca, dhwActive:!!ot.da, flameOn:!!ot.fo,
    boilerTempC:ot.bt, returnTempC:ot.rt, dhwTempC:ot.dt,
    modulationPct:ot.mt, pressureBar:ot.pr,
    faultFlags:ot.ff, oemFaultCode:ot.oc,
    reqChSetpointC:ot.cs, reqDhwSetpointC:ot.ds, reqMaxModulationPct:ot.mm,
    lastUpdateMs:ot.lu, lastCmdMs:ot.lc,
    reason:ot.rs, lastCmd:ot.cmd,
    ageMs:a
  };
}

async function fetchFull(){
  try{
    const r=await App.api.getJson('/api/opentherm/status');
    if(!r) return null;
    const s=r.status||r;
    const c=r.config||null;
    // compute age
    const fast=App.state&&App.state.fast;
    if(fast && Number.isFinite(N(fast.ts)) && Number.isFinite(N(s.lastUpdateMs)) && N(fast.ts)>=N(s.lastUpdateMs) && N(s.lastUpdateMs)>0){
      s.ageMs=N(fast.ts)-N(s.lastUpdateMs);
    }
    return {status:s,config:c};
  }catch(_){return null;}
}

async function postCmd(payload){
  try{await App.api.postJson('/api/opentherm/cmd',payload);return true;}catch(e){alert('OpenTherm CMD selhal');return false;}
}

async function saveConfig(){
  const enabled = !!($('#ot_enabled',H)?.checked);
  const pollMs = Math.max(250, Math.min(30000, Math.round(N($('#ot_pollMs',H)?.value))||2000));
  const minCh = N($('#ot_minCh',H)?.value); // allow NaN -> keep default? we'll send only if finite
  const maxCh = N($('#ot_maxCh',H)?.value);
  const boilerControl = ($('#ot_control',H)?.value)||'relay';
  const mode = ($('#ot_mode',H)?.value)||((boilerControl==='relay')?'readOnly':'control');
  const watchdogTimeoutMs = Math.max(500, Math.min(120000, Math.round(N($('#ot_wdTimeout',H)?.value))||3000));
  const maxConsecutiveFailures = Math.max(0, Math.min(50, Math.round(N($('#ot_wdFails',H)?.value))||3));
  const mapEq = !!($('#ot_mapEq',H)?.checked);
  const mapDhw = !!($('#ot_mapDhw',H)?.checked);
  const mapNight = !!($('#ot_mapNight',H)?.checked);
  const dhwSp = N($('#ot_dhwSp',H)?.value);
  const dhwBoost = N($('#ot_dhwBoost',H)?.value);
  const txPin = Math.round(N($('#ot_txPin',H)?.value));
  const rxPin = Math.round(N($('#ot_rxPin',H)?.value));
  const maxKw = N($('#ot_maxKw',H)?.value);

  const invertTx = !!($('#ot_invertTx',H)?.checked);
  const invertRx = !!($('#ot_invertRx',H)?.checked);
  const autoDetectLogic = !!($('#ot_autoDetect',H)?.checked);

  // pollIds: accept comma-separated input
  const pollIdsTxt = String($('#ot_pollIds',H)?.value||'').trim();
  const pollIds = [];
  if(pollIdsTxt){
    const parts = pollIdsTxt.split(',');
    for(const p of parts){
      const id = Math.round(N(p.trim()));
      if(!Number.isFinite(id)) continue;
      if(id<0 || id>127) continue;
      if(!pollIds.includes(id)) pollIds.push(id);
      if(pollIds.length>=32) break;
    }
  }

  const ot={
    enabled,
    pollMs,
    // Keep boilerControl in sync with mode (back-compat)
    boilerControl: (mode==='readOnly'?'relay':(boilerControl==='relay'?'opentherm':boilerControl)),
    mode,
    watchdogTimeoutMs,
    maxConsecutiveFailures,
    mapEquithermChSetpoint:mapEq,
    mapDhw,
    mapNightMode:mapNight,
    txPin,
    rxPin,
    invertTx,
    invertRx,
    autoDetectLogic
  };
  if(Number.isFinite(minCh)) ot.minChSetpointC=minCh;
  if(Number.isFinite(maxCh)) ot.maxChSetpointC=maxCh;
  if(Number.isFinite(dhwSp)) ot.dhwSetpointC=dhwSp;
  if(Number.isFinite(dhwBoost)) ot.dhwBoostChSetpointC=dhwBoost;
  if(Number.isFinite(maxKw)) ot.assumedMaxBoilerKw=maxKw;
  if(pollIds.length) ot.pollIds = pollIds;

  try{
    await App.api.postJson('/api/config/apply',{opentherm:ot});
    // reload config + refresh status
    await App.roles.loadConfig?.().catch(()=>{});
    const r=await fetchFull();
    if(r){ST=r.status;CFG=r.config||CFG;H.dataset.otBuilt='0';H.dataset.otBuilt='0';render();}
    alert('Uloženo');
  }catch(e){
    alert('Uložení selhalo');
  }
}


function updateLive(){
  if(!H) return;
  const s=ST||{};
  // tags + age
  const tags=[
    tag(s.present?'present':'absent',s.present?'ok':'bad'),
    tag(s.ready?'ready':'not ready',s.ready?'ok':'warn'),
    tag(s.fault?'fault':'ok',s.fault?'bad':'ok'),
    tag(s.flameOn?'flame':'no flame',s.flameOn?'ok':'muted'),
    tag(s.dhwActive?'DHW active':'DHW idle',s.dhwActive?'warn':'muted'),
    tag(s.chActive?'CH active':'CH idle',s.chActive?'ok':'muted')
  ].join(' ');
  const elTags=$('#ot_tags',H); if(elTags) elTags.innerHTML=tags;
  const age=AGE(s.ageMs);
  const elAge=$('#ot_age',H); if(elAge) elAge.textContent=age;

  const lastCmdAge=(Number.isFinite(N(App.state?.fast?.ts)) && Number.isFinite(N(s.lastCmdMs)) && N(App.state.fast.ts)>=N(s.lastCmdMs) && N(s.lastCmdMs)>0)
    ? AGE(N(App.state.fast.ts)-N(s.lastCmdMs))
    : '—';

  const setTxt=(id, val)=>{const el=$(id,H); if(el) el.textContent = (val==null? '—' : String(val));};
  setTxt('#ot_v_boiler', TC(s.boilerTempC));
  setTxt('#ot_v_return', TC(s.returnTempC));
  setTxt('#ot_v_dhw', TC(s.dhwTempC));
  setTxt('#ot_v_mod', TP(s.modulationPct));
  setTxt('#ot_v_press', (Number.isFinite(N(s.pressureBar))? (TF(s.pressureBar)+' bar') : '—'));
  const ff=$('#ot_v_faultFlags',H); if(ff) ff.textContent=String(s.faultFlags??0);
  const of=$('#ot_v_oemFault',H); if(of) of.textContent=String(s.oemFaultCode??0);
  setTxt('#ot_v_reqCh', TC(s.reqChSetpointC));
  setTxt('#ot_v_reqDhw', TC(s.reqDhwSetpointC));
  setTxt('#ot_v_reqMaxMod', TP(s.reqMaxModulationPct));
  setTxt('#ot_v_reason', s.reason||'—');
  setTxt('#ot_v_lastCmd', s.lastCmd||'—');
  setTxt('#ot_v_lastCmdAge', lastCmdAge);
}

function render(){
  if(!H) return;
  const s=ST||{};
  const c=CFG||{};

  const built = (H && H.dataset && H.dataset.otBuilt==='1');
  if(built){
    updateLive();
    return;
  }
  const tags=[
    tag(s.present?'present':'absent',s.present?'ok':'bad'),
    tag(s.ready?'ready':'not ready',s.ready?'ok':'warn'),
    tag(s.fault?'fault':'ok',s.fault?'bad':'ok'),
    tag(s.flameOn?'flame':'no flame',s.flameOn?'ok':'muted'),
    tag(s.dhwActive?'DHW active':'DHW idle',s.dhwActive?'warn':'muted'),
    tag(s.chActive?'CH active':'CH idle',s.chActive?'ok':'muted')
  ].join(' ');

  const age=AGE(s.ageMs);
  const lastCmdAge=(Number.isFinite(N(App.state?.fast?.ts)) && Number.isFinite(N(s.lastCmdMs)) && N(App.state.fast.ts)>=N(s.lastCmdMs) && N(s.lastCmdMs)>0)
    ? AGE(N(App.state.fast.ts)-N(s.lastCmdMs))
    : '—';

  H.innerHTML=
  '<div class="pageHead">'
    +'<div class="pageTitle">OpenTherm</div>'
    +'<div class="pageActions">'
      +'<button class="btn" id="ot_head_reload">Načíst</button>'
      +'<button class="btn primary" id="ot_head_save">Uložit</button>'
    +'</div>'
  +'</div>'
  +'<div class="grid2">'+

    '<div class="card">'+
      '<div class="card__header"><div class="card__title">OpenTherm – stav</div></div>'+
      '<div class="card__body">'+
        '<div class="row" style="gap:8px;flex-wrap:wrap;align-items:center">'+
          '<span id="ot_tags">'+tags+'</span>'+
          '<span class="muted">update:</span><b id="ot_age">'+E(age)+'</b>'+
        '</div>'+

        '<div class="kv" style="margin-top:12px">'+
          '<div class="k">Boiler</div><div class="v"><span id="ot_v_boiler">'+E(TC(s.boilerTempC))+'</span></div>'+
          '<div class="k">Return</div><div class="v"><span id="ot_v_return">'+E(TC(s.returnTempC))+'</span></div>'+
          '<div class="k">DHW</div><div class="v"><span id="ot_v_dhw">'+E(TC(s.dhwTempC))+'</span></div>'+
          '<div class="k">Modulation</div><div class="v"><span id="ot_v_mod">'+E(TP(s.modulationPct))+'</span></div>'+
          '<div class="k">Pressure</div><div class="v"><span id="ot_v_press">'+(Number.isFinite(N(s.pressureBar))?E(TF(s.pressureBar)+' bar'):'—')+'</span></div>'+
          '<div class="k">Fault flags</div><div class="v"><code id="ot_v_faultFlags">'+E(String(s.faultFlags??0))+'</code></div>'+
          '<div class="k">OEM fault</div><div class="v"><code id="ot_v_oemFault">'+E(String(s.oemFaultCode??0))+'</code></div>'+
          '<div class="k">Req CH</div><div class="v"><span id="ot_v_reqCh">'+E(TC(s.reqChSetpointC))+'</span></div>'+
          '<div class="k">Req DHW</div><div class="v"><span id="ot_v_reqDhw">'+E(TC(s.reqDhwSetpointC))+'</span></div>'+
          '<div class="k">Req max mod</div><div class="v"><span id="ot_v_reqMaxMod">'+E(TP(s.reqMaxModulationPct))+'</span></div>'+
          '<div class="k">Reason</div><div class="v"><span id="ot_v_reason">'+E(s.reason||'—')+'</span></div>'+
          '<div class="k">Last cmd</div><div class="v"><span id="ot_v_lastCmd">'+E(s.lastCmd||'—')+'</span> <span class="muted">(<span id="ot_v_lastCmdAge">'+E(lastCmdAge)+'</span>)</span></div>'+
        '</div>'+

        '<div class="row" style="gap:8px;margin-top:12px;flex-wrap:wrap">'+
          '<button class="btn" id="ot_refresh">Obnovit</button>'+
          '<div class="muted small">SSE: /api/fast.ot, fallback: /api/opentherm/status</div>'+
        '</div>'+
      '</div>'+
    '</div>'+

    '<div class="card">'+
      '<div class="card__header"><div class="card__title">Manuální příkazy</div></div>'+
      '<div class="card__body">'+
        '<div class="row" style="gap:12px;flex-wrap:wrap">'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_chEn" '+(s.chEnable?'checked':'')+'> <span>CH enable</span></label>'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_dhwEn" '+(s.dhwEnable?'checked':'')+'> <span>DHW enable</span></label>'+
          '<button class="btn" id="ot_sendFlags">Odeslat</button>'+
        '</div>'+

        '<div class="grid2" style="margin-top:12px">'+
          '<div class="field"><div class="label">CH setpoint (°C)</div><input id="ot_chSp" type="number" step="0.5" placeholder="např. 45" value="'+E(Number.isFinite(N(s.reqChSetpointC))?String(TF(s.reqChSetpointC)):'')+'"></div>'+
          '<div class="field"><div class="label">DHW setpoint (°C)</div><input id="ot_dhwSpCmd" type="number" step="0.5" placeholder="např. 50" value="'+E(Number.isFinite(N(s.reqDhwSetpointC))?String(TF(s.reqDhwSetpointC)):'')+'"></div>'+
          '<div class="field"><div class="label">Max modulation (%)</div><input id="ot_mod" type="number" step="1" min="0" max="100" placeholder="např. 80" value="'+E(Number.isFinite(N(s.reqMaxModulationPct))?String(Math.round(N(s.reqMaxModulationPct))):'')+'"></div>'+
          '<div class="field"><div class="label">Akce</div><div class="row" style="gap:8px;flex-wrap:wrap">'+
            '<button class="btn" id="ot_sendSp">Odeslat setpointy</button>'+
            '<button class="btn danger" id="ot_reset">Reset fault</button>'+
          '</div></div>'+
        '</div>'+

        '<div class="hint" style="margin-top:10px">V režimu <b>mode=control</b> jsou povolené zápisové příkazy (setpointy, enable bity). V režimu <b>mode=readOnly</b> je OpenTherm pouze čtecí (CMD zápisy jsou blokované).</div>'+
      '</div>'+
    '</div>'+

    '<div class="card">'+
      '<div class="card__header"><div class="card__title">Scan OpenTherm Data-ID (detekce podporovaných zpráv)</div></div>'+
      '<div class="card__body">'+
        '<div class="row" style="gap:12px;flex-wrap:wrap;align-items:end">'+
          '<div class="field" style="min-width:120px"><div class="label">startId</div><input id="ot_scan_start" type="number" min="0" max="127" step="1" value="0"></div>'+
          '<div class="field" style="min-width:120px"><div class="label">endId</div><input id="ot_scan_end" type="number" min="0" max="127" step="1" value="127"></div>'+
          '<div class="field" style="min-width:140px"><div class="label">delayMs</div><input id="ot_scan_delay" type="number" min="10" max="2000" step="10" value="50"></div>'+
          '<label class="row" style="gap:6px;align-items:center"><input type="checkbox" id="ot_scan_all"> <span>zobrazit všechny ID (i nepodporované)</span></label>'+
          '<button class="btn" id="ot_scan_startBtn" type="button">Start</button>'+
          '<button class="btn" id="ot_scan_stopBtn" type="button">Stop</button>'+
          '<button class="btn" id="ot_scan_refreshBtn" type="button">Obnovit</button>'+
        '</div>'+

        '<div class="row" style="gap:10px;flex-wrap:wrap;margin-top:10px;align-items:center">'+
          '<span class="muted">Stav:</span> <b id="ot_scan_state">—</b>'+
          '<span class="muted">Podporováno:</span> <b id="ot_scan_cnt">—</b>'+
          '<span class="muted">Aktuální ID:</span> <b id="ot_scan_cur">—</b>'+
          '<button class="btn sm" id="ot_scan_copy" type="button">Kopírovat seznam podporovaných ID</button>'+
        '</div>'+

        '<div class="hint" style="margin-top:10px">Scan dočasně pozastaví běžné polling čtení OpenTherm (aby se minimalizovala zátěž sběrnice). Doporučeno: běžný provoz → scan jednorázově → poté zkopírovat podporované ID do pole <b>pollIds</b>.</div>'+

        '<div style="margin-top:10px;max-height:320px;overflow:auto;border:1px solid var(--border);border-radius:12px">'+
          '<table class="table" style="min-width:520px">'+
            '<thead><tr><th style="width:60px">ID</th><th>Name</th><th style="width:92px">supported</th><th>resp</th><th>msgType</th><th>value</th></tr></thead>'+
            '<tbody id="ot_scan_rows"></tbody>'+
          '</table>'+
        '</div>'+
      '</div>'+
    '</div>'+

    '<div class="card">'+
      '<div class="card__header"><div class="card__title">Konfigurace + mapování</div></div>'+
      '<div class="card__body">'+

        '<div class="row" style="gap:12px;flex-wrap:wrap;align-items:center">'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_enabled" '+(c.enabled?'checked':'')+'> <span>Povolit OpenTherm modul</span></label>'+
          '<div class="field" style="min-width:220px"><div class="label">boilerControl</div>'+
            '<select id="ot_control">'+
              '<option value="relay" '+((c.boilerControl||'relay')==='relay'?'selected':'')+'>relay (read-only)</option>'+
              '<option value="hybrid" '+((c.boilerControl||'relay')==='hybrid'?'selected':'')+'>hybrid</option>'+
              '<option value="opentherm" '+((c.boilerControl||'relay')==='opentherm'?'selected':'')+'>opentherm</option>'+
            '</select>'+
          '<div class="field" style="min-width:160px"><div class="label">mode</div>'+            '<select id="ot_mode">'+              '<option value="readOnly" '+(((c.mode||(((c.boilerControl||"relay")==="relay")?"readOnly":"control"))==="readOnly")?"selected":"")+'>readOnly</option>'+              '<option value="control" '+(((c.mode||(((c.boilerControl||"relay")==="relay")?"readOnly":"control"))==="control")?"selected":"")+'>control</option>'+            '</select>'+          '</div>'+          '<div class="field" style="min-width:210px"><div class="label">watchdogTimeoutMs</div><input id="ot_wdTimeout" type="number" step="100" min="500" max="120000" value="'+(Number.isFinite(N(c.watchdogTimeoutMs))?Math.round(N(c.watchdogTimeoutMs)):3000)+'"></div>'+          '<div class="field" style="min-width:220px"><div class="label">maxConsecutiveFailures</div><input id="ot_wdFails" type="number" step="1" min="0" max="50" value="'+(Number.isFinite(N(c.maxConsecutiveFailures))?Math.round(N(c.maxConsecutiveFailures)):3)+'"></div>'+
          '<div class="field" style="min-width:160px"><div class="label">pollMs</div><input id="ot_pollMs" type="number" step="50" min="250" max="30000" value="'+E(String(c.pollMs??2000))+'"></div>'+
        '</div>'+

        '<div class="grid2" style="margin-top:12px">'+
          '<div class="field"><div class="label">Min CH setpoint (°C)</div><input id="ot_minCh" type="number" step="0.5" value="'+E(String(c.minChSetpointC??25))+'"></div>'+
          '<div class="field"><div class="label">Max CH setpoint (°C)</div><input id="ot_maxCh" type="number" step="0.5" value="'+E(String(c.maxChSetpointC??75))+'"></div>'+
          '<div class="field"><div class="label">DHW setpoint (°C)</div><input id="ot_dhwSp" type="number" step="0.5" value="'+E(String(c.dhwSetpointC??50))+'"></div>'+
          '<div class="field"><div class="label">DHW boost CH (°C)</div><input id="ot_dhwBoost" type="number" step="0.5" value="'+E(String(c.dhwBoostChSetpointC??10))+'"></div>'+
        '</div>'+

        '<div class="row" style="gap:12px;flex-wrap:wrap;margin-top:12px">'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_mapEq" '+(c.mapEquithermChSetpoint!==false?'checked':'')+'> <span>Mapovat Ekviterm → CH setpoint</span></label>'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_mapDhw" '+(c.mapDhw!==false?'checked':'')+'> <span>Mapovat TUV (DHW)</span></label>'+
          '<label class="row" style="gap:6px"><input type="checkbox" id="ot_mapNight" '+(c.mapNightMode!==false?'checked':'')+'> <span>Mapovat den/noc (suppress R6 v OT režimu)</span></label>'+
        '</div>'+

        '<div class="grid2" style="margin-top:12px">'+
          '<div class="field"><div class="label">OT txPin</div><input id="ot_txPin" type="number" step="1" value="'+E(String(c.txPin??c.inPin??47))+'"></div>'+
          '<div class="field"><div class="label">OT rxPin</div><input id="ot_rxPin" type="number" step="1" value="'+E(String(c.rxPin??c.outPin??48))+'"></div>'+
          '<div class="field"><div class="label">assumedMaxBoilerKw</div><input id="ot_maxKw" type="number" step="0.5" min="1" max="100" value="'+E(String(c.assumedMaxBoilerKw??9))+'"></div>'+
          '<div class="field" style="grid-column:1 / -1">'+
            '<div class="label">Logika adaptéru</div>'+
            '<div class="row" style="gap:12px;flex-wrap:wrap">'+
              '<label class="row" style="gap:6px"><input type="checkbox" id="ot_autoDetect" '+(c.autoDetectLogic!==false?'checked':'')+'> <span>autoDetectLogic</span></label>'+
              '<label class="row" style="gap:6px"><input type="checkbox" id="ot_invertTx" '+(c.invertTx?'checked':'')+'> <span>invertTx</span></label>'+
              '<label class="row" style="gap:6px"><input type="checkbox" id="ot_invertRx" '+(c.invertRx?'checked':'')+'> <span>invertRx</span></label>'+
            '</div>'+
            '<div class="hint">Pokud kotel neodpovídá, zkus vypnout auto-detekci a ručně přepnout invertTx/invertRx.</div>'+
          '</div>'+
          '<div class="field" style="grid-column:1 / -1">'+
            '<div class="label">Poll šablona</div>'+
            '<div class="row" style="display:flex; gap:10px; flex-wrap:wrap">'+
              '<select id="ot_pollTemplate" style="min-width:260px">'+
                '<option value="standard">Standard (doporučeno)</option>'+
                '<option value="minimal">Minimal</option>'+
                '<option value="debug">Debug (hodně položek)</option>'+
              '</select>'+
              '<button class="btn" id="ot_applyTemplate" type="button">Použít</button>'+
            '</div>'+
            '<div class="hint">Pozn.: Debug generuje více požadavků → vyšší zátěž sběrnice i CPU.</div>'+
          '</div>'+
          '<div class="field" style="grid-column:1 / -1"><div class="label">pollIds (comma-separated Data-IDs)</div><input id="ot_pollIds" type="text" value="'+E(Array.isArray(c.pollIds)?c.pollIds.join(','):(c.pollIds??''))+'" placeholder="0,17,18,25,27,28,56,57,15"></div>'+
        '</div>'+

        '<div class="row" style="gap:8px;margin-top:12px;flex-wrap:wrap">'+
          '<a class="btn" href="/download/opentherm.csv" target="_blank">Stáhnout log (CSV)</a>'+
          '<button class="btn" id="ot_clearLog" type="button">Vymazat log</button>'+
        '</div>'+

        '<div class="hint" style="margin-top:10px">'+
          '<b>Mapování na existující funkce:</b><br>'+
          '• Ekviterm: když je aktivní a není TUV, posílá se CH setpoint = targetFlowC.<br>'+
          '• TUV: když je aktivní TUV režim, posílá se DHW setpoint = dhwSetpointC a volitelně CH setpoint = targetFlowC + dhwBoostChSetpointC.<br>'+
          '• Pokud je boilerControl=opentherm, relé R5 (DHW request) a R6 (night) se v mapovaných režimech neovládají.<br>'+
          'Zdroje teplot pro systém: <code>opentherm_boiler</code>, <code>opentherm_return</code>, <code>opentherm_dhw</code>, <code>opentherm_outside</code>, <code>opentherm_room</code>, <code>opentherm_ch2_flow</code>, <code>opentherm_dhw2</code>, <code>opentherm_exhaust</code>, <code>opentherm_hx</code>, <code>opentherm_solar_storage</code>, <code>opentherm_solar_collector</code>.'+
        '</div>'+

      '</div>'+
    '</div>'+

  '</div>';

  $('#ot_refresh',H).onclick=async()=>{
    const r=await fetchFull();
    if(r){ST=r.status;CFG=r.config||CFG;render();}
  };

  // ---- Scan UI ----
  const renderScan=(scan)=>{
    const s=scan||{};
    const st=$('#ot_scan_state',H);
    const cnt=$('#ot_scan_cnt',H);
    const cur=$('#ot_scan_cur',H);
    if(st) st.textContent = s.active ? 'RUNNING' : (s.done ? 'DONE' : 'IDLE');
    if(cnt) cnt.textContent = (s.supportedCount!=null)?String(s.supportedCount):'—';
    if(cur) cur.textContent = (s.curId!=null)?String(s.curId):'—';

    const rows=$('#ot_scan_rows',H);
    if(!rows) return;
    const items=Array.isArray(s.items)?s.items:[];
    rows.innerHTML = items.map(it=>{
      const id=it.id;
      const sup=!!it.supported;
      const cls=sup?'tag ok':'tag';
      const name=String(it.name||('ID'+id));
      const desc=String(it.desc||'');
      const isTemp=!!it.isTemp;
      const title=desc?(' title="'+E(desc)+'"'):'';
      const vhex=it.valueHex||'';
      const v=(it.value!=null)?String(it.value):'';
      return '<tr>'+
        '<td><code>'+E(String(id))+'</code></td>'+
        '<td><span'+title+'>'+E(name)+'</span>'+ (isTemp?(' '+tag('temp','muted')):'') +'</td>'+
        '<td>'+tag(sup?'yes':'no',sup?'ok':'muted')+'</td>'+
        '<td><code>'+E(String(it.respStatus||''))+'</code></td>'+
        '<td><code>'+E(String(it.msgType||''))+'</code></td>'+
        '<td><code>'+E(vhex? (vhex+' / '+v):v)+'</code></td>'+
      '</tr>';
    }).join('');
  };

  const refreshScan=async()=>{
    try{
      const all=!!$('#ot_scan_all',H)?.checked;
      const r=await App.api.getJson('/api/opentherm/scan/status'+(all?'?all=1':''));
      renderScan(r && (r.scan||r));
      return r;
    }catch(_){renderScan(null);return null;}
  };

  const startBtn=$('#ot_scan_startBtn',H);
  if(startBtn) startBtn.onclick=async()=>{
    const startId=Math.max(0,Math.min(127,Math.round(N($('#ot_scan_start',H)?.value)||0)));
    const endId=Math.max(0,Math.min(127,Math.round(N($('#ot_scan_end',H)?.value)||127)));
    const delayMs=Math.max(10,Math.min(2000,Math.round(N($('#ot_scan_delay',H)?.value)||50)));
    const includeAll=!!$('#ot_scan_all',H)?.checked;
    await App.api.postJson('/api/opentherm/scan/start',{startId,endId,delayMs,includeAll});
    await refreshScan();
  };

  const stopBtn=$('#ot_scan_stopBtn',H);
  if(stopBtn) stopBtn.onclick=async()=>{
    await App.api.postJson('/api/opentherm/scan/stop',{});
    await refreshScan();
  };

  const refBtn=$('#ot_scan_refreshBtn',H);
  if(refBtn) refBtn.onclick=refreshScan;

  const copyBtn=$('#ot_scan_copy',H);
  if(copyBtn) copyBtn.onclick=async()=>{
    const all=!!$('#ot_scan_all',H)?.checked;
    const r=await App.api.getJson('/api/opentherm/scan/status'+(all?'?all=1':''));
    const s=(r && (r.scan||r))||{};
    const items=Array.isArray(s.items)?s.items:[];
    const ids=items.filter(it=>it.supported).map(it=>it.id).sort((a,b)=>a-b);
    const txt=ids.join(',');
    try{
      await navigator.clipboard.writeText(txt);
      alert('Zkopírováno: '+txt);
    }catch(_){
      prompt('Zkopíruj ručně:', txt);
    }
  };

  // Initial scan status load
  refreshScan();


  // Apply poll template -> fills pollIds input
  const applyTemplate=()=>{
    const sel=$('#ot_pollTemplate',H);
    const tpl=(sel&&sel.value)||'standard';
    const ids=POLL_TEMPLATES[tpl]||POLL_TEMPLATES.standard;
    const inp=$('#ot_pollIds',H);
    if(inp) inp.value=ids.join(',');
  };
  const btn=$('#ot_applyTemplate',H);
  if(btn) btn.onclick=applyTemplate;

  $('#ot_sendFlags',H).onclick=async()=>{
    const ch=!!$('#ot_chEn',H).checked;
    const dhw=!!$('#ot_dhwEn',H).checked;
    await postCmd({chEnable:ch,dhwEnable:dhw});
  };

  $('#ot_sendSp',H).onclick=async()=>{
    const chSp=N($('#ot_chSp',H).value);
    const dhwSp=N($('#ot_dhwSpCmd',H).value);
    const mod=N($('#ot_mod',H).value);
    const p={};
    if(Number.isFinite(chSp)) p.chSetpointC=chSp;
    if(Number.isFinite(dhwSp)) p.dhwSetpointC=dhwSp;
    if(Number.isFinite(mod)) p.maxModulationPct=mod;
    if(Object.keys(p).length===0){alert('Nic k odeslání');return;}
    await postCmd(p);
  };

  $('#ot_reset',H).onclick=async()=>{
    await postCmd({resetFault:true});
  };

  $('#ot_head_save',H).onclick=saveConfig;
  $('#ot_head_reload',H).onclick=async()=>{
    const r=await fetchFull();
    if(r){ST=r.status;CFG=r.config||CFG;render();}
  };


  const clr=$('#ot_clearLog',H);
  if(clr) clr.onclick=async()=>{
    if(!confirm('Vymazat OpenTherm log?')) return;
    try{
      await App.api.postJson('/api/opentherm/clear', {});
      alert('OK');
    }catch(e){
      alert('Chyba');
    }
  };

  H.dataset.otBuilt='1';
  updateLive();

}

App.pages.opentherm={
  mount:async(h)=>{
    H=h;
    ST=fromFast(App.state&&App.state.fast)||ST;
    const r=await fetchFull();
    if(r){ST=r.status||ST;CFG=r.config||CFG;}
    render();
  },
  update:(fast)=>{
    const s=fromFast(fast);
    if(s){ST=Object.assign({},ST||{},s);updateLive();}
  }
};
})();