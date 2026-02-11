window.App=window.App||{};
const RELAY_LABELS=[
"Směšovací ventil A (OPEN)","Směšovací ventil B (CLOSE)","TUV ventil (přepínací)","Rezerva",
"Požadavek kotle: TUV","Křivka: den / noc","Cirkulace TUV","Topná tyč AKU"
];
const INPUT_LABELS=["Požadavek TUV","Den / noc","Požadavek cirkulace","Volné","Volné","Volné","Volné","Volné"];
function fmtUptime(ts){if(ts==null) return '—';const sec=Math.floor(Number(ts)/1000);const d=Math.floor(sec/86400);const h=Math.floor((sec%86400)/3600);const m=Math.floor((sec%3600)/60);const s=sec%60;return (d?`${d}d `:'')+`${h}h ${m}m ${s}s`;}
function fmtAge(ms){if(ms==null) return '—';const s=Math.floor(Number(ms)/1000);if(s<60) return `${s}s`;const m=Math.floor(s/60);return `${m}m ${s%60}s`;}
function bits(mask){const out=[];const m=(Number(mask)>>>0);for(let i=0;i<8;i++) out.push(((m>>i)&1)===1);return out;}
function normTemps(t,tv){const out=[];for(let i=0;i<8;i++){const v=Array.isArray(tv)?tv[i]===true:false;const x=Array.isArray(t)?t[i]:null;out.push({i,v,temp:v?x:null});}return out;}

App.dashboard.register({
 id:'status',
 title:'Stav',
 render(){return `<div class="kv">
 <div class="k">Uptime</div><div class="v" id="s_uptime">—</div>
 <div class="k">IP</div><div class="v" id="s_ip">—</div>
 <div class="k">Režim</div><div class="v" id="s_mode">—</div>
 <div class="k">SSE</div><div class="v" id="s_sse">—</div>
</div><div class="hint" id="s_hint"></div>`;},
 update(f){if(!f) return;App.util.$('#s_uptime').textContent=fmtUptime(f.ts);App.util.$('#s_mode').textContent=`${f.mode||'—'} / ${f.ctrl||'—'}`;App.util.$('#s_ip').textContent=(f.n&&f.n.ip)||'—';}
});

App.dashboard.register({
 id:'relays',
 title:'Relé',
 render(){let h='<div class="hint">Popisky odpovídají pevné HW konfiguraci ve firmwaru (R4 je rezerva).</div><div class="relays" id="w_relays"></div><div class="row"><button class="btn" id="allOff">Vše OFF</button><button class="btn" id="allOn">Vše ON</button></div>';return h;},
 update(f){if(!f) return;const root=App.util.$('#w_relays');if(!root) return;const on=bits(f.r);root.innerHTML='';for(let i=0;i<8;i++){const label=RELAY_LABELS[i]||'';const isFixed=!!label&&label!=='Rezerva';const tile=document.createElement('div');tile.className='tile';tile.innerHTML=`<div class="t"><div class="head"><div class="id"><b>R${i+1}</b></div><div class="desc">${esc(label)}</div></div><div class="badges">${isFixed?'<div class="badge fixed">FIXED</div>':''}<div class="badge ${on[i]?'on':'off'}">${on[i]?'ON':'OFF'}</div></div></div><div class="row"><button class="btn" data-id="${i+1}" data-cmd="on">ON</button><button class="btn" data-id="${i+1}" data-cmd="off">OFF</button><button class="btn" data-id="${i+1}" data-cmd="toggle">Toggle</button></div>`;root.appendChild(tile);} 
 root.querySelectorAll('button[data-id]').forEach(b=>b.onclick=async()=>{try{await App.api.getJson(`/api/relay?id=${b.dataset.id}&cmd=${b.dataset.cmd}`);}catch(e){App.util.$('#s_hint')&&(App.util.$('#s_hint').textContent=`Chyba relé: ${e.message}`);}});
 App.util.$('#allOff')&&( App.util.$('#allOff').onclick=()=>App.api.getJson('/api/relay_all?cmd=off').catch(()=>{}) );
 App.util.$('#allOn')&&( App.util.$('#allOn').onclick=()=>App.api.getJson('/api/relay_all?cmd=on').catch(()=>{}) );
 }
});

App.dashboard.register({
 id:'inputs',
 title:'Vstupy',
 render(){return `<div class="inputs" id="w_inputs"></div>`;},
 update(f){if(!f) return;const root=App.util.$('#w_inputs');if(!root) return;const v=bits(f.i);root.innerHTML='';for(let i=0;i<8;i++){let txt=v[i]?'ACTIVE':'inactive';if(i===0) txt=v[i]?'TUV požadavek':'bez požadavku';else if(i===1) txt=v[i]?'noc':'den';else if(i===2) txt=v[i]?'cirkulace ON':'cirkulace OFF';const el=document.createElement('div');el.className='item';el.innerHTML=`<div class="label"><div class="id">IN${i+1}</div><div class="desc">${esc(INPUT_LABELS[i]||'')}</div></div><div class="value">${esc(txt)}</div>`;root.appendChild(el);} }
});

App.dashboard.register({
 id:'temps',
 title:'Teploty',
 render(){return `<div class="temps" id="w_temps"></div>`;},
 update(f){if(!f) return;const root=App.util.$('#w_temps');if(!root) return;root.innerHTML='';for(const it of normTemps(f.t,f.tv)){const el=document.createElement('div');el.className='item';const v=(it.temp==null||isNaN(it.temp))?'—':`${Number(it.temp).toFixed(2)} °C`;el.innerHTML=`<div class="label">T${it.i+1}</div><div class="value">${esc(v)}</div>`;root.appendChild(el);} }
});

App.dashboard.register({
 id:'ble',
 title:'BLE meteo',
 render(){return `<div class="kv">
 <div class="k">Enabled</div><div class="v" id="b_en">—</div>
 <div class="k">Typ</div><div class="v" id="b_typ">—</div>
 <div class="k">MAC</div><div class="v mono" id="b_mac">—</div>
 <div class="k">Fresh</div><div class="v" id="b_fr">—</div>
 <div class="k">RSSI</div><div class="v" id="b_r">—</div>
 <div class="k">Age</div><div class="v" id="b_a">—</div>
</div><div class="row"><div class="pill" id="b_t">— °C</div><div class="pill" id="b_h">— %</div><div class="pill" id="b_p">— hPa</div></div>`;},
 update(f,st){const b=f&&f.b;const meta=st&&st.bleMeta;App.util.$('#b_en').textContent=(meta&&meta.enabled!=null)?(meta.enabled?'true':'false'):'—';App.util.$('#b_typ').textContent=(meta&&meta.type)||'—';App.util.$('#b_mac').textContent=(meta&&meta.allowMac)||'—';
 if(!b||b.ok!==true){App.util.$('#b_fr').textContent='no data';App.util.$('#b_fr').className='v bad';App.util.$('#b_r').textContent='—';App.util.$('#b_a').textContent='—';App.util.$('#b_t').textContent='— °C';App.util.$('#b_h').textContent='— %';App.util.$('#b_p').textContent='— hPa';return;}
 App.util.$('#b_fr').textContent=b.fr===true?'fresh':'stale';App.util.$('#b_fr').className='v '+(b.fr===true?'ok':'warn');App.util.$('#b_r').textContent=(b.r!=null)?`${b.r} dBm`:'—';App.util.$('#b_a').textContent=fmtAge(b.a);
 App.util.$('#b_t').textContent=(b.t!=null)?`${Number(b.t).toFixed(2)} °C`:'— °C';App.util.$('#b_h').textContent=(b.h!=null)?`${Number(b.h).toFixed(0)} %`:'— %';App.util.$('#b_p').textContent=(b.p!=null)?`${Number(b.p).toFixed(0)} hPa`:'— hPa';
 }
});

App.dashboard.register({
 id:'diag',
 title:'Diagnostika',
 render(){return `<pre class="log" id="w_raw">—</pre>`;},
 update(f){const p=App.util.$('#w_raw');if(p) p.textContent=f?JSON.stringify(f,null,2):'—';}
});

// Roles UI (used on page "Teploměry"; can be enabled on dashboard too)
App.dashboard.register({
 id:'roles',
 title:'Role teploměrů',
 render(){
 return `<div class="hint">Role se ukládají do /config.json (tempRoles pro T1..T8 a bleThermometer pro BLE).</div>
 <div class="row"><button class="btn" id="rolesLoad">Načíst config</button><button class="btn" id="rolesSave" disabled>Uložit role</button></div>
 <div class="hint" id="rolesMsg"></div>
 <div class="rolesGrid" id="rolesGrid"></div>`;},
 update(){/* no fast update */}
});
