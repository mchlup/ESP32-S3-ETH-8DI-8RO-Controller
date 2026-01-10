(function(){
  const debounce = (fn, wait=200) => {
    let t;
    return (...args) => {
      clearTimeout(t);
      t = setTimeout(() => fn(...args), wait);
    };
  };
  const throttle = (fn, wait=200) => {
    let last = 0;
    let t;
    return (...args) => {
      const now = Date.now();
      const remaining = wait - (now - last);
      if (remaining <= 0) {
        if (t) clearTimeout(t);
        last = now;
        fn(...args);
      } else if (!t) {
        t = setTimeout(() => {
          last = Date.now();
          t = null;
          fn(...args);
        }, remaining);
      }
    };
  };
  const clamp = (val, min, max) => Math.min(max, Math.max(min, val));
  const parseNum = (val, fallback=0) => {
    const n = Number(val);
    return Number.isFinite(n) ? n : fallback;
  };
  const fmtTemp = (val) => (Number.isFinite(val) ? `${Number(val).toFixed(1)}°C` : "—");
  const fmtTime = (ms) => {
    if (ms == null || !Number.isFinite(ms)) return "—";
    ms = Math.max(0, Math.floor(ms));
    const s = Math.floor(ms/1000);
    const m = Math.floor(s/60);
    const h = Math.floor(m/60);
    const d = Math.floor(h/24);
    if (d>0) return `${d}d ${h%24}h`;
    if (h>0) return `${h}h ${m%60}m`;
    if (m>0) return `${m}m ${s%60}s`;
    return `${s}s`;
  };
  const id = (() => {
    let i = 0;
    return (prefix="id") => `${prefix}-${++i}`;
  })();
  const escapeHtml = (s) => String(s ?? "").replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const escapeAttr = (s) => escapeHtml(s).replace(/\n/g, " ");
  const safeJson = (txt) => { try { return JSON.parse(txt); } catch { return null; } };
  const prettyJson = (obj) => JSON.stringify(obj, null, 2);

  window.Core = window.Core || {};
  window.Core.util = { debounce, throttle, clamp, parseNum, fmtTemp, fmtTime, id, escapeHtml, escapeAttr, safeJson, prettyJson };
})();
