(function () {
  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));
  const on = (el, event, handler, opts) => {
    if (!el) return () => {};
    el.addEventListener(event, handler, opts);
    return () => el.removeEventListener(event, handler, opts);
  };
  const delegate = (root, event, selector, handler) => {
    if (!root) return () => {};
    const listener = (e) => {
      const target = e.target.closest(selector);
      if (target && root.contains(target)) handler(e, target);
    };
    root.addEventListener(event, listener);
    return () => root.removeEventListener(event, listener);
  };
  const createEl = (tag, cls) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    return el;
  };
  const setText = (el, text) => {
    if (el) el.textContent = text == null ? "" : String(text);
  };
  const setHtmlSafe = (el, html) => {
    if (el) el.innerHTML = html == null ? "" : String(html);
  };

  window.Core = window.Core || {};
  window.Core.dom = { $, $$, on, delegate, createEl, setText, setHtmlSafe };
})();
