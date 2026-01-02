const GPIO_PRESET = [
  {label:"IO1 (ADC)", value:1},
  {label:"IO2 (ADC)", value:2},
  {label:"IO3 (ADC)", value:3},
];

async function apiGet(path){
  const r = await fetch(path, {cache:"no-store"});
  if(!r.ok) throw new Error(await r.text());
  return await r.json();
}

async function apiPost(path, obj){
  const r = await fetch(path, {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify(obj),
  });
  if(!r.ok) throw new Error(await r.text());
  return await r.json().catch(()=> ({}));
}

function ensureCfg(cfg){
  cfg.sensors ||= {};
  cfg.sensors.ntc ||= [];
  while(cfg.sensors.ntc.length < 3){
    cfg.sensors.ntc.push({en:false,gpio:1,beta:3950,r0:10000,t0:25,rs:10000,off:0});
  }
  return cfg;
}

function renderForm(cfg){
  cfg = ensureCfg(cfg);
  const root = document.getElementById("ntcForm");
  root.innerHTML = "";

  cfg.sensors.ntc.slice(0,3).forEach((ch, i)=>{
    const row = document.createElement("div");
    row.className = "grid2";
    row.innerHTML = `
      <div>
        <label>NTC #${i+1} – povolit</label>
        <select id="en${i}">
          <option value="false">OFF</option>
          <option value="true">ON</option>
        </select>
      </div>
      <div>
        <label>GPIO / IO</label>
        <select id="gpio${i}"></select>
      </div>
      <div>
        <label>Offset (°C)</label>
        <input id="off${i}" type="number" step="0.1" value="${ch.off ?? 0}">
      </div>
      <div>
        <label>Beta</label>
        <input id="beta${i}" type="number" step="1" value="${ch.beta ?? 3950}">
      </div>
    `;
    root.appendChild(row);

    document.getElementById(`en${i}`).value = String(!!ch.en);
    const sel = document.getElementById(`gpio${i}`);
    GPIO_PRESET.forEach(p=>{
      const o = document.createElement("option");
      o.value = p.value;
      o.textContent = p.label;
      sel.appendChild(o);
    });
    sel.value = String(ch.gpio ?? 1);
  });
}

function collectForm(cfg){
  cfg = ensureCfg(cfg);
  cfg.sensors.ntc = cfg.sensors.ntc.slice(0,3);

  for(let i=0;i<3;i++){
    cfg.sensors.ntc[i].en = (document.getElementById(`en${i}`).value === "true");
    cfg.sensors.ntc[i].gpio = Number(document.getElementById(`gpio${i}`).value);
    cfg.sensors.ntc[i].off = Number(document.getElementById(`off${i}`).value || 0);
    cfg.sensors.ntc[i].beta = Number(document.getElementById(`beta${i}`).value || 3950);
    cfg.sensors.ntc[i].r0 = 10000;
    cfg.sensors.ntc[i].t0 = 25;
    cfg.sensors.ntc[i].rs = 10000;
  }
  return cfg;
}

async function loadCfg(){
  let cfg = await apiGet("/api/config");
  if(typeof cfg === "string") cfg = JSON.parse(cfg);
  renderForm(cfg);
  return cfg;
}

async function saveCfg(cfg){
  const msg = document.getElementById("saveMsg");
  msg.textContent = "Ukládám...";
  try{
    const updated = collectForm(cfg);
    await apiPost("/api/config", updated);
    msg.textContent = "Uloženo ✅";
  }catch(e){
    msg.textContent = "Chyba: " + e.message;
  }
}

async function tickDiag(){
  const box = document.getElementById("diag");
  try{
    const d = await apiGet("/api/temps");
    const rows = (d.ntc || []).map(ch=>{
      const c = (ch.c == null) ? "-" : Number(ch.c).toFixed(1);
      return `<tr>
        <td>${ch.idx}</td>
        <td>${ch.en ? "ON" : "OFF"}</td>
        <td>${ch.gpio}</td>
        <td>${ch.valid ? "OK" : "ERR"}</td>
        <td>${ch.raw}</td>
        <td>${c}</td>
      </tr>`;
    }).join("");

    box.innerHTML = `
      <table class="table">
        <thead><tr><th>#</th><th>EN</th><th>GPIO</th><th>Valid</th><th>RAW</th><th>°C</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`;
  }catch(e){
    box.textContent = "Chyba /api/temps: " + e.message;
  }
}

let g_cfg = null;
document.getElementById("btnSave").addEventListener("click", ()=> saveCfg(g_cfg));
document.getElementById("btnReload").addEventListener("click", async()=> { g_cfg = await loadCfg(); });

(async()=>{
  g_cfg = await loadCfg();
  setInterval(tickDiag, 1000);
  tickDiag();
})();

