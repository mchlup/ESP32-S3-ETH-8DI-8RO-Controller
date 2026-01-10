(function () {
  const debounce = (fn, wait) => {
    let t;
    return (...args) => {
      clearTimeout(t);
      t = setTimeout(() => fn(...args), wait);
    };
  };
  const throttle = (fn, limit) => {
    let inThrottle = false;
    let lastArgs;
    return (...args) => {
      if (!inThrottle) {
        fn(...args);
        inThrottle = true;
        setTimeout(() => {
          inThrottle = false;
          if (lastArgs) {
            const la = lastArgs;
            lastArgs = null;
            fn(...la);
          }
        }, limit);
      } else {
        lastArgs = args;
      }
    };
  };
  const clamp = (v, min, max) => Math.min(max, Math.max(min, v));
  const parseNum = (val, fallback = 0) => {
    const n = Number(val);
    return Number.isFinite(n) ? n : fallback;
  };
  const fmtTemp = (v) => (typeof v === "number" && Number.isFinite(v)) ? `${v.toFixed(1)} °C` : "—";
  const fmtTime = (ms) => {
    if (ms == null || !Number.isFinite(ms)) return "—";
    const s = Math.max(0, Math.floor(ms / 1000));
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    const d = Math.floor(h / 24);
    if (d > 0) return `${d}d ${h % 24}h`;
    if (h > 0) return `${h}h ${m % 60}m`;
    if (m > 0) return `${m}m ${s % 60}s`;
    return `${s}s`;
  };
  const id = (prefix = "id") => `${prefix}-${Math.random().toString(36).slice(2, 9)}`;

  window.Core = window.Core || {};
  window.Core.util = { debounce, throttle, clamp, parseNum, fmtTemp, fmtTime, id };
})();
