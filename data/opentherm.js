// opentherm.js
window.App = window.App || {};

function otFmt(v, suffix=''){
  if (v==null || v==='') return '—';
  const n = Number(v);
  if (!Number.isFinite(n)) return String(v);
  return `${n.toFixed(1)}${suffix}`;
}
function otAge(ms){
  const n = Number(ms);
  if (!Number.isFinite(n)) return '—';
  return `${Math.floor(n/1000)}s`;
}

App.registerWidget({
  id:'opentherm',
  title:'OpenTherm',
  defaultSpan:10,
  async render(el){
    el.innerHTML = `
      <div class="card__title">OpenTherm</div>
      <div class="kv" style="margin-top:8px">
        <div class="k">Stav</div><div class="v" data-st>—</div>
        <div class="k">Fault</div><div class="v" data-flt>—</div>
        <div class="k">T_set</div><div class="v" data-ts>—</div>
        <div class="k">T_boiler</div><div class="v" data-tb>—</div>
        <div class="k">T_ret</div><div class="v" data-tr>—</div>
        <div class="k">T_DHW</div><div class="v" data-td>—</div>
        <div class="k">T_out</div><div class="v" data-to>—</div>
        <div class="k">T_room</div><div class="v" data-troom>—</div>
        <div class="k">Modulace</div><div class="v" data-mod>—</div>
        <div class="k">Age</div><div class="v" data-age>—</div>
      </div>
      <div class="row" style="margin-top:10px; gap:10px; flex-wrap:wrap">
        <button class="btn sm" data-open>Otevřít stránku OpenTherm</button>
        <button class="btn sm" data-reload>Obnovit</button>
      </div>
    `;

    el.querySelector('[data-open]')?.addEventListener('click', ()=>{
      // switch page through nav (reuse existing setPage)
      const btn = document.querySelector('.navItem[data-page="opentherm"]');
      if (btn) btn.click();
    });
    el.querySelector('[data-reload]')?.addEventListener('click', ()=>{
      this._force = true;
      this.update(el, null);
    });

    this._last = 0;
    this._force = true;
    await this.update(el, null);
  },

  async update(el, fast){
    // Prefer /api/fast.ot (SSE/poll) and use /api/opentherm/status as a fallback.
    if (!el || !el.isConnected) return;

    // 1) fast path (no throttling)
    const ot = fast?.ot;
    if (ot && ot.en) {
      el.querySelector('[data-st]').textContent = ot.fl ? 'FAULT' : (ot.rd ? 'OK' : 'init');
      el.querySelector('[data-flt]').textContent = ot.fl ? String(ot.ff ?? 1) : '—';
      el.querySelector('[data-ts]').textContent  = (ot.cs!=null) ? otFmt(ot.cs,' °C') : '—';
      el.querySelector('[data-tb]').textContent  = (ot.bt!=null) ? otFmt(ot.bt,' °C') : '—';
      el.querySelector('[data-tr]').textContent  = (ot.rt!=null) ? otFmt(ot.rt,' °C') : '—';
      el.querySelector('[data-td]').textContent  = (ot.dt!=null) ? otFmt(ot.dt,' °C') : '—';
      el.querySelector('[data-to]').textContent  = (ot.ot!=null) ? otFmt(ot.ot,' °C') : '—';
      el.querySelector('[data-troom]').textContent  = (ot.rm!=null) ? otFmt(ot.rm,' °C') : '—';
      el.querySelector('[data-mod]').textContent = (ot.mt!=null) ? otFmt(ot.mt,' %') : '—';
      el.querySelector('[data-age]').textContent = (ot.lu!=null && fast?.ts!=null) ? otAge(Number(fast.ts)-Number(ot.lu)) : '—';
      return;
    }

    // 2) fallback path - throttle API calls
    const now = Date.now();
    if (!this._force && (now - (this._last||0)) < 3000) return;
    this._force = false;
    this._last = now;

    try{
      const st = await App.api.getJson('/api/opentherm/status');
      const data = st?.status || st?.data || st || {};
      const online = (data?.present ?? data?.enabled ?? data?.ok);
      el.querySelector('[data-st]').textContent = (online===false) ? 'OFFLINE' : (data?.fault ? 'FAULT' : (data?.ready ? 'OK' : 'init'));
      el.querySelector('[data-flt]').textContent = (data?.faultFlags!=null) ? String(data.faultFlags) : (data?.fault!=null ? String(data.fault) : '—');
      el.querySelector('[data-ts]').textContent  = (data?.reqChSetpointC!=null) ? otFmt(data.reqChSetpointC,' °C') : '—';
      el.querySelector('[data-tb]').textContent  = (data?.boilerTempC!=null) ? otFmt(data.boilerTempC,' °C') : '—';
      el.querySelector('[data-tr]').textContent  = (data?.returnTempC!=null) ? otFmt(data.returnTempC,' °C') : '—';
      el.querySelector('[data-td]').textContent  = (data?.dhwTempC!=null) ? otFmt(data.dhwTempC,' °C') : '—';
      el.querySelector('[data-to]').textContent  = (data?.outsideTempC!=null) ? otFmt(data.outsideTempC,' °C') : '—';
      el.querySelector('[data-troom]').textContent  = (data?.roomTempC!=null) ? otFmt(data.roomTempC,' °C') : '—';
      el.querySelector('[data-mod]').textContent = (data?.modulationPct!=null) ? otFmt(data.modulationPct,' %') : '—';
      el.querySelector('[data-age]').textContent = (data?.lastUpdateMs!=null && fast?.ts!=null) ? otAge(Number(fast.ts)-Number(data.lastUpdateMs)) : '—';
    }catch(e){
      const s = el.querySelector('[data-st]');
      if (s) s.textContent = 'API ERR';
    }
  }
});
