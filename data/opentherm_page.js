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
  const mapEq = !!($('#ot_mapEq',H)?.checked);
  const mapDhw = !!($('#ot_mapDhw',H)?.checked);
  const mapNight = !!($('#ot_mapNight',H)?.checked);
  const dhwSp = N($('#ot_dhwSp',H)?.value);
  const dhwBoost = N($('#ot_dhwBoost',H)?.value);
  const txPin = Math.round(N($('#ot_txPin',H)?.value));
  const rxPin = Math.round(N($('#ot_rxPin',H)?.value));

  const ot={enabled,pollMs,boilerControl,mapEquithermChSetpoint:mapEq,mapDhw,mapNightMode:mapNight};
  if(Number.isFinite(minCh)) ot.minChSetpointC=minCh;
  if(Number.isFinite(maxCh)) ot.maxChSetpointC=maxCh;
  if(Number.isFinite(dhwSp)) ot.dhwSetpointC=dhwSp;
  if(Number.isFinite(dhwBoost)) ot.dhwBoostChSetpointC=dhwBoost;
  if(Number.isFinite(txPin)) ot.txPin=txPin;
  if(Number.isFinite(rxPin)) ot.rxPin=rxPin;

  try{
    await App.api.postJson('/api/config/apply',{opentherm:ot});
    // reload config + refresh status
    await App.roles.loadConfig?.().catch(()=>{});
    const r=await fetchFull();
    if(r){ST=r.status;CFG=r.config||CFG;render();}
    alert('Uloženo');
  }catch(e){
    alert('Uložení selhalo');
  }
}

function render(){
  if(!H) return;
  const s=ST||{};
  const c=CFG||{};

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
          tags+
          '<span class="muted">update:</span><b>'+E(age)+'</b>'+
        '</div>'+

        '<div class="kv" style="margin-top:12px">'+
          '<div class="k">Boiler</div><div class="v">'+E(TC(s.boilerTempC))+'</div>'+
          '<div class="k">Return</div><div class="v">'+E(TC(s.returnTempC))+'</div>'+
          '<div class="k">DHW</div><div class="v">'+E(TC(s.dhwTempC))+'</div>'+
          '<div class="k">Modulation</div><div class="v">'+E(TP(s.modulationPct))+'</div>'+
          '<div class="k">Pressure</div><div class="v">'+(Number.isFinite(N(s.pressureBar))?E(TF(s.pressureBar)+' bar'):'—')+'</div>'+
          '<div class="k">Fault flags</div><div class="v"><code>'+E(String(s.faultFlags??0))+'</code></div>'+
          '<div class="k">OEM fault</div><div class="v"><code>'+E(String(s.oemFaultCode??0))+'</code></div>'+
          '<div class="k">Req CH</div><div class="v">'+E(TC(s.reqChSetpointC))+'</div>'+
          '<div class="k">Req DHW</div><div class="v">'+E(TC(s.reqDhwSetpointC))+'</div>'+
          '<div class="k">Req max mod</div><div class="v">'+E(TP(s.reqMaxModulationPct))+'</div>'+
          '<div class="k">Reason</div><div class="v">'+E(s.reason||'—')+'</div>'+
          '<div class="k">Last cmd</div><div class="v">'+E(s.lastCmd||'—')+' <span class="muted">('+E(lastCmdAge)+')</span></div>'+
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

        '<div class="hint" style="margin-top:10px">V režimu <b>boilerControl=opentherm</b> jsou příkazy použity také automaticky (Ekviterm / TUV). V režimu <b>relay</b> je OpenTherm pouze čtecí.</div>'+
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
          '</div>'+
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
          'Zdroje teplot pro systém: <code>opentherm_boiler</code>, <code>opentherm_return</code>.'+
        '</div>'+

      '</div>'+
    '</div>'+

  '</div>';

  $('#ot_refresh',H).onclick=async()=>{
    const r=await fetchFull();
    if(r){ST=r.status;CFG=r.config||CFG;render();}
  };


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
    if(s){ST=Object.assign({},ST||{},s);render();}
  }
};
})();
