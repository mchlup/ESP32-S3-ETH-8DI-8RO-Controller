// status.js
window.App = window.App || {};

function fmtUptime(tsMs){
  if (tsMs==null || isNaN(tsMs)) return '—';
  const sec = Math.floor(Number(tsMs)/1000);
  const h = Math.floor(sec/3600);
  const m = Math.floor((sec%3600)/60);
  const s = sec%60;
  return `${h}h ${m}m ${s}s`;
}

App.registerWidget({
  id:'status',
  title:'Stav',
  defaultSpan:6,
  render(root){
    root.innerHTML = `
      <div class="h2">Stav</div>
      <div class="kv">
        <div class="k">Uptime</div><div class="v" id="w_uptime">—</div>
        <div class="k">IP</div><div class="v" id="w_ip">—</div>
        <div class="k">Režim</div><div class="v" id="w_mode">—</div>
        <div class="k">SSE</div><div class="v" id="w_sse">—</div>
      </div>
      <div class="hint" id="w_modesHint">—</div>
    `;
  },
  update(fast){
    if (!fast) return;
    App.util.$("w_uptime").textContent = fmtUptime(fast.ts);
    App.util.$("w_mode").textContent = `${fast.mode||'-'} / ${(fast.ctrl||'-')}`;
    if (fast?.n?.ip) App.util.$("w_ip").textContent = fast.n.ip;
    const parts = [];
    if (fast?.e) parts.push(`Ekviterm: ${fast.e.en ? (fast.e.ac?'aktivní':'zapnuto') : 'vypnuto'}`);
    if (fast?.d) parts.push(`TUV: ${fast.d.en ? (fast.d.ac?'aktivní':'zapnuto') : 'vypnuto'}`);
    if (fast?.c) parts.push(`Cirkulace: ${fast.c.en ? (fast.c.ac?'aktivní':'zapnuto') : 'vypnuto'}`);
    if (fast?.a) parts.push(`Aku: ${fast.a.en ? (fast.a.ac?'aktivní':'zapnuto') : 'vypnuto'}`);
    App.util.$("w_modesHint").textContent = parts.join(' • ');
  }
});

App.onSseState = (s) => {
  try {
    App.util.$("w_sse").textContent = s;
    App.util.$("sseState").textContent = s;
    const dot = App.util.$("navOnlineDot");
    const txt = App.util.$("navOnlineText");
    if (s === 'connected') { dot.style.background = 'var(--ok)'; txt.textContent='online'; }
    else { dot.style.background = 'var(--bad)'; txt.textContent='offline'; }
  } catch(e) {}
};
