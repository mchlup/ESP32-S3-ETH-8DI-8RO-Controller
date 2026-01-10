(function () {
  const listeners = new Map();

  const on = (event, cb) => {
    if (!listeners.has(event)) listeners.set(event, new Set());
    listeners.get(event).add(cb);
    return () => off(event, cb);
  };
  const off = (event, cb) => {
    const set = listeners.get(event);
    if (set) set.delete(cb);
  };
  const emit = (event, data) => {
    const set = listeners.get(event);
    if (!set) return;
    set.forEach((cb) => cb(data));
  };

  window.Core = window.Core || {};
  window.Core.events = { on, off, emit };
})();
