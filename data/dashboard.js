window.App = window.App || {};
App.widgets = App.widgets || {};

(function(){
  const GRID_COLS_BASE = 20; // virtual coords stored in layout
  const ROW_PX = 36;
  const GAP_PX = 14;
  const SNAP_TOL = 1;

  const LS_PROFILE = 'dashProfile';
  const LS_LAYOUT_PREFIX = 'dashLayout:'; // + profile

  const DEF_WIDGETS = [
    { id:'equitherm', on:true,  w:10, h:9 },
    { id:'dhw',      on:true,  w:10, h:9 },
    { id:'recirc',   on:true,  w:10, h:9 },
    { id:'aku',      on:true,  w:10, h:9 },
    { id:'relays',   on:true,  w:10, h:10 },
    { id:'inputs',   on:true,  w:10, h:7 },
    { id:'temps',    on:true,  w:10, h:7 },
    { id:'ble',      on:true,  w:10, h:9 },
    { id:'diag',     on:true,  w:20, h:9 },
    { id:'roles',    on:false, w:20, h:10 },
  ];

  function wdef(id){
    return DEF_WIDGETS.find(x=>x.id===id);
  }

  function getProfile(){
    try{ return localStorage.getItem(LS_PROFILE) || 'default'; }catch(_){ return 'default'; }
  }
  function setProfile(p){
    try{ localStorage.setItem(LS_PROFILE, p); }catch(_){ }
  }

  function keyForProfile(p){
    return LS_LAYOUT_PREFIX + (p || 'default');
  }

  function normalizeLayout(arr){
    if (!Array.isArray(arr)) return null;
    const out = [];
    const seen = new Set();

    for (const it of arr) {
      if (!it || !it.id) continue;
      const id = String(it.id);
      if (seen.has(id)) continue;
      seen.add(id);
      const d = wdef(id) || {w:10,h:9,on:true};
      out.push({
        id,
        on: it.on !== false,
        x: clampInt(it.x ?? 1, 1, GRID_COLS_BASE),
        y: clampInt(it.y ?? 1, 1, 9999),
        w: clampInt(it.w ?? d.w ?? 10, 1, GRID_COLS_BASE),
        h: clampInt(it.h ?? d.h ?? 9, 1, 9999),
      });
    }

    // append any new widgets
    for (const d of DEF_WIDGETS) {
      if (!seen.has(d.id)) out.push({ id:d.id, on:d.on, x:1, y:1, w:d.w, h:d.h });
    }

    // clamp to bounds
    for (const it of out) {
      if (it.x + it.w - 1 > GRID_COLS_BASE) it.x = Math.max(1, GRID_COLS_BASE - it.w + 1);
      if (it.x < 1) it.x = 1;
      if (it.y < 1) it.y = 1;
    }

    // if layout has no valid positions, auto-place
    if (!out.some(x => Number.isFinite(x.x) && Number.isFinite(x.y))) {
      return autoPlace(out);
    }

    // Ensure at least some sane ordering of y
    return out;
  }

  function autoPlace(arr){
    const out = arr.map(x=>({...x}));
    let y = 1;
    const rowH = 10;
    let left = true;
    for (const it of out) {
      const d = wdef(it.id) || it;
      it.w = clampInt(it.w ?? d.w ?? 10, 1, GRID_COLS_BASE);
      it.h = clampInt(it.h ?? d.h ?? 9, 1, 9999);
      if (it.w >= GRID_COLS_BASE) {
        it.x = 1;
        it.y = y;
        y += it.h + 1;
        left = true;
        continue;
      }
      if (left) {
        it.x = 1;
        it.y = y;
        left = false;
      } else {
        it.x = GRID_COLS_BASE - it.w + 1;
        it.y = y;
        y += Math.max(rowH, it.h) + 1;
        left = true;
      }
    }
    return out;
  }

  function loadLocal(profile){
    try{
      const raw = localStorage.getItem(keyForProfile(profile));
      if (!raw) return null;
      return normalizeLayout(JSON.parse(raw));
    } catch(_) {
      return null;
    }
  }

  function saveLocal(profile, layout){
    try{
      localStorage.setItem(keyForProfile(profile), JSON.stringify(layout));
    } catch(_) {}
  }

  async function loadFromDevice(profile){
    if (!window.App?.api?.getJson) return null;
    try{
      const l = await App.api.getJson(`/api/ui/layout?profile=${encodeURIComponent(profile)}`);
      return normalizeLayout(l);
    } catch(_) {
      return null;
    }
  }

  async function saveToDevice(profile, layout){
    if (!window.App?.api?.postJson) return false;
    try{
      await App.api.postJson(`/api/ui/layout?profile=${encodeURIComponent(profile)}`, layout);
      return true;
    } catch(_) {
      return false;
    }
  }

  async function listDeviceProfiles(){
    if (!window.App?.api?.getJson) return null;
    try{
      const r = await App.api.getJson('/api/ui/layout/profiles');
      if (!r || !Array.isArray(r.profiles)) return null;
      return r.profiles.map(x=>String(x)).filter(Boolean);
    } catch(_) {
      return null;
    }
  }

  async function deleteDeviceProfile(profile){
    if (!window.App?.api?.postJson) return false;
    try{
      await App.api.postJson(`/api/ui/layout/delete?profile=${encodeURIComponent(profile)}`, {});
      return true;
    } catch(_) {
      return false;
    }
  }

  function clampInt(v, lo, hi){
    const n = Math.round(Number(v));
    if (!Number.isFinite(n)) return lo;
    return Math.max(lo, Math.min(hi, n));
  }

  function widget(id){
    return App.widgets[id] || { title: id, render:(root)=>{root.innerHTML = `<div class="hint">Widget '${id}' nenalezen.</div>`;}, update:()=>{} };
  }

  function renderCols(){
    const w = window.innerWidth || 1200;
    if (w <= 520) return 1;
    if (w <= 780) return 6;
    if (w <= 1100) return 12;
    return 20;
  }

  function canEdit(cols){
    // edit only in base grid (20 cols) and desktop layout
    return cols === 20 && (window.innerWidth || 0) > 980;
  }

  function rectsOverlap(a,b){
    return !(a.x + a.w - 1 < b.x || b.x + b.w - 1 < a.x || a.y + a.h - 1 < b.y || b.y + b.h - 1 < a.y);
  }

  function findById(layout, id){
    return layout.find(x=>x.id===id);
  }

  function others(layout, id){
    return layout.filter(x => x.on && x.id !== id);
  }

  function isFree(layout, id, cand){
    const o = others(layout, id);
    for (const it of o) {
      if (rectsOverlap(cand, it)) return false;
    }
    return true;
  }

  function applySnap(layout, id, cand){
    const o = others(layout, id);
    let x = cand.x;
    let y = cand.y;

    const left = x;
    const right = x + cand.w;
    const top = y;
    const bottom = y + cand.h;

    for (const it of o) {
      const itL = it.x;
      const itR = it.x + it.w;
      const itT = it.y;
      const itB = it.y + it.h;

      // snap X
      if (Math.abs(left - itL) <= SNAP_TOL) x = itL;
      if (Math.abs(left - itR) <= SNAP_TOL) x = itR;
      if (Math.abs(right - itL) <= SNAP_TOL) x = itL - cand.w;
      if (Math.abs(right - itR) <= SNAP_TOL) x = itR - cand.w;

      // snap Y
      if (Math.abs(top - itT) <= SNAP_TOL) y = itT;
      if (Math.abs(top - itB) <= SNAP_TOL) y = itB;
      if (Math.abs(bottom - itT) <= SNAP_TOL) y = itT - cand.h;
      if (Math.abs(bottom - itB) <= SNAP_TOL) y = itB - cand.h;
    }

    x = clampInt(x, 1, GRID_COLS_BASE - cand.w + 1);
    y = clampInt(y, 1, 9999);
    return {...cand, x, y};
  }

  function resolveCollision(layout, id, cand){
    const maxX = GRID_COLS_BASE - cand.w + 1;
    const startY = Math.max(1, cand.y);
    const startX = Math.max(1, Math.min(maxX, cand.x));

    // try immediate
    if (isFree(layout, id, cand)) return cand;

    // scan rows downward
    for (let dy = 0; dy < 220; dy++) {
      const y = startY + dy;

      // scan x starting from startX and wrap
      for (let dx = 0; dx < GRID_COLS_BASE; dx++) {
        let x = startX + dx;
        if (x > maxX) x = ((x - 1) % maxX) + 1;
        const probe = {...cand, x, y};
        if (isFree(layout, id, probe)) return probe;
      }
    }

    return cand;
  }

  function toRendered(it, cols){
    if (cols === GRID_COLS_BASE) return {...it};
    const scale = cols / GRID_COLS_BASE;
    let x = Math.round((it.x - 1) * scale) + 1;
    let w = Math.max(1, Math.round(it.w * scale));
    if (w > cols) w = cols;
    if (x + w - 1 > cols) x = Math.max(1, cols - w + 1);
    return {...it, x, w};
  }

  // --- UI rendering + interactions ---
  let _hostSelector = '#pageContent';
  let _layout = null;
  let _profile = getProfile();
  let _cols = renderCols();
  let _edit = false;
  let _drag = null;

  function mountWidgets(layout){
    for (const it of layout.filter(x=>x.on)) {
      const host = document.getElementById(`w_${it.id}`);
      const ww = widget(it.id);
      try { if (host && typeof ww.render === 'function') ww.render(host); } catch(_) {}
    }
  }

  function render(){
    const root = App.util.$(_hostSelector);
    if (!root) return;

    _cols = renderCols();
    const layout = (_layout || []).filter(x=>x && x.id);
    const active = layout.filter(x=>x.on);

    const can = canEdit(_cols);
    if (!can) _edit = false;

    root.innerHTML = `<div class="dashGrid${_edit?' dashEditMode':''}" style="--dash-cols:${_cols}">${active.map(it=>{
      const r = toRendered(it, _cols);
      const ww = widget(it.id);
      return `\
        <section class="dashItem" data-id="${it.id}" style="grid-column:${r.x} / span ${r.w};grid-row:${r.y} / span ${r.h};">\
          <div class="card">\
            <div class="dashHandle"><h2>${(ww.title||it.id)}</h2></div>\
            <div id="w_${it.id}"></div>\
          </div>\
          <div class="dashResizeHandle" title="Změnit velikost"></div>\
        </section>`;
    }).join('')}</div>`;

    mountWidgets(layout);
    bindInteractions();
  }

  function bindInteractions(){
    const grid = document.querySelector('.dashGrid');
    if (!grid) return;

    const can = canEdit(_cols);
    if (!can || !_edit) return;

    grid.querySelectorAll('.dashItem').forEach(itemEl => {
      const id = itemEl.dataset.id;
      const handle = itemEl.querySelector('.dashHandle');
      const res = itemEl.querySelector('.dashResizeHandle');

      if (handle) {
        handle.addEventListener('mousedown', (ev) => startDrag(ev, id, 'move'));
      }
      if (res) {
        res.addEventListener('mousedown', (ev) => startDrag(ev, id, 'resize'));
      }
    });
  }

  function gridMetrics(){
    const grid = document.querySelector('.dashGrid');
    if (!grid) return null;
    const r = grid.getBoundingClientRect();
    const colW = (r.width - (GAP_PX * (_cols - 1))) / _cols;
    const rowH = ROW_PX;
    return { rect:r, colW, rowH };
  }

  function pointerToCell(ev, m){
    const xPx = ev.clientX - m.rect.left;
    const yPx = ev.clientY - m.rect.top;

    const colUnit = m.colW + GAP_PX;
    const rowUnit = m.rowH + GAP_PX;

    const cx = Math.floor(xPx / colUnit) + 1;
    const cy = Math.floor(yPx / rowUnit) + 1;

    return { x: clampInt(cx, 1, GRID_COLS_BASE), y: clampInt(cy, 1, 9999) };
  }

  function startDrag(ev, id, kind){
    ev.preventDefault();
    ev.stopPropagation();

    const it = findById(_layout, id);
    if (!it) return;

    const m = gridMetrics();
    if (!m) return;

    const cell = pointerToCell(ev, m);

    _drag = {
      kind,
      id,
      startMouse: { x: ev.clientX, y: ev.clientY },
      startCell: cell,
      startItem: { ...it },
      metrics: m,
    };

    const el = document.querySelector(`.dashItem[data-id="${CSS.escape(id)}"]`);
    if (el) el.classList.add('dashDragging');

    window.addEventListener('mousemove', onDragMove);
    window.addEventListener('mouseup', onDragEnd);
  }

  function onDragMove(ev){
    if (!_drag) return;
    ev.preventDefault();

    const it = findById(_layout, _drag.id);
    if (!it) return;

    const m = _drag.metrics;
    const cell = pointerToCell(ev, m);

    const dx = cell.x - _drag.startCell.x;
    const dy = cell.y - _drag.startCell.y;

    if (_drag.kind === 'move') {
      let cand = {
        x: clampInt(_drag.startItem.x + dx, 1, GRID_COLS_BASE - it.w + 1),
        y: clampInt(_drag.startItem.y + dy, 1, 9999),
        w: it.w,
        h: it.h,
      };

      cand = applySnap(_layout, it.id, cand);
      cand = resolveCollision(_layout, it.id, cand);

      it.x = cand.x;
      it.y = cand.y;
    } else {
      // resize from bottom-right
      let newW = clampInt(_drag.startItem.w + dx, 1, GRID_COLS_BASE);
      let newH = clampInt(_drag.startItem.h + dy, 1, 9999);

      if (_drag.startItem.x + newW - 1 > GRID_COLS_BASE) newW = GRID_COLS_BASE - _drag.startItem.x + 1;

      let cand = { x: it.x, y: it.y, w: newW, h: newH };
      cand = applySnap(_layout, it.id, cand);
      cand = resolveCollision(_layout, it.id, cand);

      it.w = cand.w;
      it.h = cand.h;
      it.x = cand.x;
      it.y = cand.y;
    }

    // re-render positions only (cheap: full render is ok for few widgets)
    render();
  }

  function onDragEnd(){
    if (!_drag) return;

    const el = document.querySelector(`.dashItem[data-id="${CSS.escape(_drag.id)}"]`);
    if (el) el.classList.remove('dashDragging');

    window.removeEventListener('mousemove', onDragMove);
    window.removeEventListener('mouseup', onDragEnd);

    _drag = null;

    // persist
    saveLocal(_profile, _layout);
    saveToDevice(_profile, _layout).catch(()=>{});

    // editor refresh
    if (!_layoutPanelHidden()) editorRender();
  }

  function _layoutPanelHidden(){
    const p = App.util.$('#layoutPanel');
    return !p || p.hidden;
  }

  function editorRender(){
    const ed = App.util.$('#layoutEditor');
    if (!ed) return;

    const prof = _profile;

    const rows = _layout.map((it, i) => {
      const ww = widget(it.id);
      const title = ww.title || it.id;

      const wSel = [4,5,6,8,10,12,15,20].map(v => `<option value="${v}" ${it.w===v?'selected':''}>${v}/20</option>`).join('');
      const hSel = [5,6,7,8,9,10,12,14,16].map(v => `<option value="${v}" ${it.h===v?'selected':''}>${v} ř.</option>`).join('');

      return `\
        <div class="layoutRow">\
          <label class="chk"><input type="checkbox" data-i="${i}" ${it.on?'checked':''}/> <span>${title}</span></label>\
          <div style="display:flex;gap:8px;align-items:center">\
            <select class="sel" data-i="${i}" data-k="w">${wSel}</select>\
            <select class="sel" data-i="${i}" data-k="h">${hSel}</select>\
            <div class="layoutBtns">\
              <button class="btn sm" data-i="${i}" data-act="up">▲</button>\
              <button class="btn sm" data-i="${i}" data-act="down">▼</button>\
            </div>\
          </div>\
        </div>`;
    }).join('');

    ed.innerHTML = `\
      <div class="layoutRow" style="justify-content:space-between;gap:10px">\
        <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">\
          <b>Profil:</b>\
          <select class="sel" id="dashProfileSel"></select>\
          <button class="btn sm" id="dashProfileNew">Nový</button>\
          <button class="btn sm danger" id="dashProfileDel">Smazat</button>\
          <button class="btn sm" id="dashProfileReset">Reset</button>\
        </div>\
        <div class="hint" style="margin:0">Drag & drop a resize funguje jen na desktopu (grid20).</div>\
      </div>\
      ${rows}`;

    // fill profiles
    (async()=>{
      const sel = App.util.$('#dashProfileSel');
      if (!sel) return;
      let profiles = ['default'];
      const dev = await listDeviceProfiles();
      if (Array.isArray(dev) && dev.length) profiles = Array.from(new Set(['default', ...dev]));
      else {
        // local fallback: scan localStorage
        try{
          const keys = Object.keys(localStorage);
          const local = keys
            .filter(k => k.startsWith(LS_LAYOUT_PREFIX))
            .map(k => k.substring(LS_LAYOUT_PREFIX.length))
            .filter(Boolean);
          profiles = Array.from(new Set(['default', ...local]));
        }catch(_){ }
      }
      sel.innerHTML = profiles.map(p => `<option value="${p}" ${p===prof?'selected':''}>${p}</option>`).join('');
      sel.onchange = async()=>{
        const p = sel.value || 'default';
        await switchProfile(p);
      };
    })();

    // bind row controls
    ed.querySelectorAll('input[type=checkbox]').forEach(inp => inp.addEventListener('change', ()=>{
      const i = +inp.dataset.i;
      _layout[i].on = !!inp.checked;
      saveLocal(_profile, _layout);
      render();
    }));

    ed.querySelectorAll('select.sel[data-k]').forEach(sel => sel.addEventListener('change', ()=>{
      const i = +sel.dataset.i;
      const k = sel.dataset.k;
      const v = +sel.value;
      if (k === 'w') {
        _layout[i].w = clampInt(v, 1, GRID_COLS_BASE);
        if (_layout[i].x + _layout[i].w - 1 > GRID_COLS_BASE) _layout[i].x = GRID_COLS_BASE - _layout[i].w + 1;
      }
      if (k === 'h') _layout[i].h = clampInt(v, 1, 9999);
      _layout[i] = resolveCollision(_layout, _layout[i].id, _layout[i]);
      saveLocal(_profile, _layout);
      render();
    }));

    ed.querySelectorAll('button[data-act]').forEach(btn => btn.addEventListener('click', ()=>{
      const i = +btn.dataset.i;
      const act = btn.dataset.act;
      const j = act==='up' ? i-1 : i+1;
      if (j<0 || j>=_layout.length) return;
      const tmp = _layout[i];
      _layout[i] = _layout[j];
      _layout[j] = tmp;
      saveLocal(_profile, _layout);
      editorRender();
      render();
    }));

    // profile buttons
    const bNew = App.util.$('#dashProfileNew');
    if (bNew) bNew.onclick = async()=>{
      const name = prompt('Název profilu (a-z, 0-9, _, -):', 'novy');
      if (!name) return;
      const safe = name.trim();
      if (!safe) return;
      // clone current
      const cloned = normalizeLayout(JSON.parse(JSON.stringify(_layout)));
      saveLocal(safe, cloned);
      await saveToDevice(safe, cloned);
      await switchProfile(safe);
    };

    const bDel = App.util.$('#dashProfileDel');
    if (bDel) bDel.onclick = async()=>{
      if (_profile === 'default') { alert('Profil default nelze smazat.'); return; }
      if (!confirm(`Smazat profil '${_profile}'?`)) return;
      try{ localStorage.removeItem(keyForProfile(_profile)); }catch(_){ }
      await deleteDeviceProfile(_profile);
      await switchProfile('default');
    };

    const bReset = App.util.$('#dashProfileReset');
    if (bReset) bReset.onclick = async()=>{
      if (!confirm('Resetovat layout profilu na výchozí?')) return;
      const reset = autoPlace(normalizeLayout(DEF_WIDGETS.map(d=>({id:d.id,on:d.on,w:d.w,h:d.h,x:1,y:1}))));
      _layout = reset;
      saveLocal(_profile, _layout);
      await saveToDevice(_profile, _layout);
      editorRender();
      render();
    };
  }

  async function switchProfile(profile){
    const p = profile || 'default';
    _profile = p;
    setProfile(p);

    const dev = await loadFromDevice(p);
    const loc = loadLocal(p);

    _layout = dev || loc || autoPlace(normalizeLayout(DEF_WIDGETS.map(d=>({id:d.id,on:d.on,w:d.w,h:d.h,x:1,y:1}))));
    saveLocal(_profile, _layout);

    editorRender();
    render();
  }

  function updateAll(fast){
    for (const id in App.widgets) {
      const ww = App.widgets[id];
      if (typeof ww.update !== 'function') continue;
      const host = document.getElementById(`w_${id}`);
      try {
        if (ww.update.length <= 1) ww.update(fast);
        else {
          try { ww.update(fast, host); }
          catch (_) { ww.update(host, fast); }
        }
      } catch(_) {}
    }
  }

  // Public API
  App.dashboard = {
    register(w){ if(w && w.id) App.widgets[w.id] = w; },
    getLayout(){ return (_layout || []).map(x=>({...x})); },
    setLayout(l){ _layout = normalizeLayout(l) || _layout; },

    mount(hostSelector){
      if (hostSelector) _hostSelector = hostSelector;

      _profile = getProfile();
      const loc = loadLocal(_profile);
      _layout = loc || autoPlace(normalizeLayout(DEF_WIDGETS.map(d=>({id:d.id,on:d.on,w:d.w,h:d.h,x:1,y:1}))));

      render();

      // Try to load device layout (preferred) and re-render if differs.
      loadFromDevice(_profile).then(dev=>{
        if (!dev) return;
        _layout = dev;
        saveLocal(_profile, _layout);
        render();
      }).catch(()=>{});

      window.addEventListener('resize', ()=>{
        const newCols = renderCols();
        if (newCols !== _cols) render();
      });
    },

    updateAll,

    openEditor(){
      const p = App.util.$('#layoutPanel');
      if (p) p.hidden = false;
      _edit = true;
      editorRender();
      render();
    },

    closeEditor(){
      const p = App.util.$('#layoutPanel');
      if (p) p.hidden = true;
      _edit = false;
      render();
    },

    async saveReload(){
      saveLocal(_profile, _layout);
      await saveToDevice(_profile, _layout);
      location.reload();
    }
  };

  // Legacy/compat: most widget files register via App.registerWidget(...)
  App.registerWidget = App.registerWidget || function(widget){
    App.dashboard.register(widget);
  };
})();
