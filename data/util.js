// util.js
window.App = window.App || {};

App.util = {
  /**
   * Selector helper.
   * - If passed "someId" (no CSS prefix), it is treated as element id.
   * - If passed "#id", ".class", "[attr]" etc., it is treated as CSS selector.
   */
  $(s, r) {
    const root = r || document;
    if (!s || typeof s !== 'string') return null;

    const first = s[0];
    const isCssSelector =
      first === '#' || first === '.' || first === '[' || first === ':' || first === '>' ||
      first === '*' || first === ' ' || s.includes(' ');

    return isCssSelector ? root.querySelector(s) : root.getElementById(s);
  },

  $all(s, r) {
    const root = r || document;
    if (!s || typeof s !== 'string') return [];
    return Array.from(root.querySelectorAll(s));
  },

  esc(s) {
    return String(s ?? '').replace(/[&<>"']/g, (m) => ({
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;'
    }[m]));
  },

  toast(msg, type) {
    let box = document.getElementById('toastBox');
    if (!box) {
      box = document.createElement('div');
      box.id = 'toastBox';
      box.style.position = 'fixed';
      box.style.right = '12px';
      box.style.bottom = '12px';
      box.style.zIndex = '9999';
      document.body.appendChild(box);
    }
    const t = document.createElement('div');
    t.textContent = msg;
    t.style.marginTop = '8px';
    t.style.padding = '10px 12px';
    t.style.borderRadius = '10px';
    t.style.background = (type === 'ok') ? '#1b5e20' : (type === 'err' ? '#b71c1c' : '#333');
    t.style.color = '#fff';
    t.style.boxShadow = '0 6px 18px rgba(0,0,0,.35)';
    box.appendChild(t);
    setTimeout(() => t.remove(), 2200);
  }
};

// Backward compatibility: some files call global esc(...)
window.esc = window.esc || App.util.esc;

// --- UI helpers (shared components) ---
App.ui = App.ui || {};

App.ui._tt = null;
App.ui._ttHideTmr = 0;

App.ui.ensureTooltip = function(){
  if(App.ui._tt) return App.ui._tt;
  const tt = document.createElement('div');
  tt.className = 'ttBubble';
  tt.innerHTML = '<div class="ttInner"><div class="ttText"></div></div><div class="ttArrow"></div>';
  document.body.appendChild(tt);
  App.ui._tt = tt;
  return tt;
};

App.ui.hideTooltip = function(){
  const tt = App.ui._tt;
  if(!tt) return;
  tt.classList.remove('isOn');
};

App.ui.showTooltip = function(anchorEl, text){
  const tt = App.ui.ensureTooltip();
  const textEl = tt.querySelector('.ttText');
  if(textEl) textEl.textContent = String(text || '');

  // measure & position near the anchor
  const r = anchorEl.getBoundingClientRect();
  // show first (offscreen) to get size
  tt.style.left = '-9999px';
  tt.style.top = '-9999px';
  tt.classList.add('isOn');
  const tr = tt.getBoundingClientRect();

  // prefer above the segment, fallback below
  const pad = 10;
  let x = r.left + (r.width/2) - (tr.width/2);
  x = Math.max(pad, Math.min(window.innerWidth - tr.width - pad, x));

  let y = r.top - tr.height - 10;
  if(y < pad) y = r.bottom + 10;
  y = Math.max(pad, Math.min(window.innerHeight - tr.height - pad, y));

  tt.style.left = x + 'px';
  tt.style.top = y + 'px';

  // auto hide on touch
  if(App.ui._ttHideTmr) clearTimeout(App.ui._ttHideTmr);
  if('ontouchstart' in window){
    App.ui._ttHideTmr = setTimeout(()=>App.ui.hideTooltip(), 2200);
  }
};

/**
 * Bind bubble tooltips to segments inside a week plan container.
 * It expects each segment to contain data-tt attribute.
 */
App.ui.bindWeekPlanTooltips = function(container){
  if(!container) return;
  const segs = container.querySelectorAll('.timeSeg[data-tt]');
  segs.forEach(seg=>{
    const txt = seg.getAttribute('data-tt') || '';
    seg.addEventListener('mouseenter', ()=>App.ui.showTooltip(seg, txt));
    seg.addEventListener('mouseleave', ()=>App.ui.hideTooltip());
    seg.addEventListener('click', (e)=>{ e.preventDefault(); App.ui.showTooltip(seg, txt); });
  });
};
