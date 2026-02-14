// ble.js
window.App = window.App || {};

function fmtTemp(v){
  if (v==null || isNaN(v)) return '—';
  return `${Number(v).toFixed(2)} °C`;
}

App.registerWidget({
  id:'ble',
  title:'BLE meteo',
  defaultSpan:6,
  async render(el){
    el.innerHTML = `
      <div class="card__title">BLE meteo</div>
      <div class="kv" style="margin-top:8px">
        <div class="k">Enabled</div><div class="v" data-en>-</div>
        <div class="k">Typ</div><div class="v" data-typ>-</div>
        <div class="k">MAC</div><div class="v" data-mac>-</div>
        <div class="k">RSSI</div><div class="v" data-rssi>-</div>
        <div class="k">Age</div><div class="v" data-age>-</div>
      </div>
      <div class="row" style="margin-top:10px">
        <div class="pill" data-t>—</div>
        <div class="pill" data-h>—</div>
        <div class="pill" data-p>—</div>
        <div class="pill" data-st>NO DATA</div>
      </div>
    `;

    // meta from /api/ble/status
    try {
      const st = await App.api.getJson('/api/ble/status');
      App.state.bleStatus = st;
      el.querySelector('[data-en]').textContent = String(st?.data?.enabled ?? '-');
      el.querySelector('[data-typ]').textContent = st?.data?.type ?? '-';
      el.querySelector('[data-mac]').textContent = st?.data?.allowMac || '—';
    } catch(e) {
      el.querySelector('[data-st]').textContent = 'API ERR';
    }
  },
  update(el, fast){
    const b = fast?.b || {};
    const en = App.state.bleStatus?.data?.enabled;
    if (en != null) el.querySelector('[data-en]').textContent = String(en);

    el.querySelector('[data-rssi]').textContent = (b.r != null) ? `${b.r} dBm` : '—';
    el.querySelector('[data-age]').textContent = (b.a != null) ? `${Math.floor(b.a/1000)}s` : '—';

    el.querySelector('[data-t]').textContent = fmtTemp(b.t);
    el.querySelector('[data-h]').textContent = (b.h != null) ? `${b.h} %` : '—';
    el.querySelector('[data-p]').textContent = (b.p != null) ? `${b.p} hPa` : '—';

    const st = el.querySelector('[data-st]');
    if (!b.ok) {
      st.textContent = 'NO DATA';
      st.style.borderColor = 'rgba(255,92,92,.25)';
      st.style.color = 'var(--bad)';
    } else if (b.fr === false) {
      st.textContent = 'STALE';
      st.style.borderColor = 'rgba(241,196,15,.25)';
      st.style.color = 'var(--warn)';
    } else {
      st.textContent = 'OK';
      st.style.borderColor = 'rgba(46,204,113,.35)';
      st.style.color = 'var(--ok)';
    }
  }
});
