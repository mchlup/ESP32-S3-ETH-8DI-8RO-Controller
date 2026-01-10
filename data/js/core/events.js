(function(){
  const listeners = {};
  const on = (event, cb) => {
    listeners[event] = listeners[event] || new Set();
    listeners[event].add(cb);
    return () => off(event, cb);
  };
  const off = (event, cb) => {
    if (listeners[event]) listeners[event].delete(cb);
  };
  const emit = (event, data) => {
    if (!listeners[event]) return;
    listeners[event].forEach((cb) => {
      try { cb(data); } catch (e) { console.error(e); }
    });
  };

  window.Core = window.Core || {};
  window.Core.events = { on, off, emit };
})();
