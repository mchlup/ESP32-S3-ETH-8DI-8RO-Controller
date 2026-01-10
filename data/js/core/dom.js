(function(){
  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));
  const on = (el, evt, handler, opts) => {
    if (!el) return () => {};
    el.addEventListener(evt, handler, opts);
    return () => el.removeEventListener(evt, handler, opts);
  };
  const delegate = (root, evt, selector, handler) => {
    if (!root) return () => {};
    const listener = (e) => {
      const target = e.target && e.target.closest(selector);
      if (target && root.contains(target)) handler(e, target);
    };
    root.addEventListener(evt, listener);
    return () => root.removeEventListener(evt, listener);
  };
  const createEl = (tag, cls) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    return el;
  };
  const setText = (el, txt) => { if (el) el.textContent = txt ?? ""; };
  const setHtmlSafe = (el, html) => { if (el) el.innerHTML = html ?? ""; };

  window.Core = window.Core || {};
  window.Core.dom = { $, $$, on, delegate, createEl, setText, setHtmlSafe };
})();
