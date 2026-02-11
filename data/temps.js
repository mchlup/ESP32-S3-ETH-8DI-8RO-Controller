// temps.js
window.App = window.App || {};

function fmtTemp(v){
  if (v==null || isNaN(v)) return '—';
  return `${Number(v).toFixed(2)} °C`;
}

function roleForIdx(idx){
  const cfg = App.state.config;
  if (!cfg || !Array.isArray(cfg.tempRoles)) return '';
  const r = cfg.tempRoles[idx];
  return r ? String(r) : '';
}

App.registerWidget({
  id:'temps',
  title:'Teploty',
  defaultSpan:6,
  render(root){
    root.innerHTML = `
      <div class="card__head"><div class="card__title">Teploty</div></div>
      <div class="card__body">
        <div class="temps" id="tempsGrid"></div>
      </div>`;
    const g = root.querySelector('#tempsGrid');
    g.innerHTML = Array.from({length:8}).map((_,i)=>{
      const id = `T${i+1}`;
      return `
      <div class="item" id="temp_${i}">
        <div class="label">
          <div class="id">${id}</div>
          <div class="desc" id="tempRole_${i}">bez role</div>
        </div>
        <div class="value" id="tempVal_${i}">—</div>
      </div>`;
    }).join('');
  },
  update(fast){
    const t = fast?.t || [];
    for (let i=0;i<8;i++){
      const role = roleForIdx(i);
      const roleEl = document.getElementById(`tempRole_${i}`);
      if (roleEl) roleEl.textContent = role ? role : 'bez role';
      const vEl = document.getElementById(`tempVal_${i}`);
      if (vEl) vEl.textContent = fmtTemp(t[i]);
    }
  }
});
