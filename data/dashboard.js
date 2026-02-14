window.App = window.App || {};
App.widgets = App.widgets || {};

(function(){
  const GRID_COLS_BASE = 20; // virtual coords stored in layout
  const ROW_PX = 36;
  const GAP_PX = 14;
  const SNAP_TOL = 1;

  const LS_PROFILE = 'dashProfile';
  const LS_LAYOUT_PREFIX = 'dashLayout:'; // + profile
  const LS_PANEL_COLLAPSED = 'dashLayoutPanelCollapsed';

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

  function getPanelCollapsed(){
    try{ return (localStorage.getItem(LS_PANEL_COLLAPSED) === '1'); }catch(_){ return false; }
  }
  function setPanelCollapsed(v){
    try{ localStorage.setItem(LS_PANEL_COLLAPSED, v ? '1' : '0'); }catch(_){ }
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
    // Allow editing also on 12-col responsive layout (typical ~1024px screens).
    // We keep storing layout in 20-col virtual grid and convert pointer coords accordingly.
    return (window.innerWidth || 0) > 820 && cols >= 12;
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
  let _panelCollapsed = getPanelCollapsed();

  function getItemEl(id){
    try{
      return document.querySelector(`.dashItem[data-id="${CSS.escape(id)}"]`);
    }catch(_){
      // CSS.escape might not exist in very old browsers; fall back.
      return document.querySelector(`.dashItem[data-id="${id}"]`);
    }
  }

  function applyItemStyle(id){
    const it = findById(_layout, id);
    if (!it) return;
    const el = getItemEl(id);
    if (!el) return;
    const r = toRendered(it, _cols);
    el.style.gridColumn = `${r.x} / span ${r.w}`;
    el.style.gridRow = `${r.y} / span ${r.h}`;
  }

  function mountWidgets(layout){
    for (const it of layout.filter(x=>x.on)) {
      const host = document.getElementById(`w_${it.id}`);
      const ww = widget(it.id);
      try { if (host && typeof ww.render === 'function') ww.render(host); } catch(_) {}
    }
  }

  
  function setDockedEditorMode(on){
    const main = document.querySelector('main.main');
    if (!main) return;
    if (on) main.classList.add('dashLayoutMode');
    else main.classList.remove('dashLayoutMode');
  }

  function applyPanelCollapsedClass(){
    const main = document.querySelector('main.main');
    if (!main) return;
    if (_edit && _panelCollapsed) main.classList.add('dashLayoutPanelCollapsed');
    else main.classList.remove('dashLayoutPanelCollapsed');
  }

  function togglePanelCollapsed(){
    _panelCollapsed = !_panelCollapsed;
    setPanelCollapsed(_panelCollapsed);
    applyPanelCollapsedClass();
    editorRender(); // refresh header/icon + body visibility controlled by CSS
  }

  function render(){
    const root = App.util.$(_hostSelector);
    if (!root) return;

    _cols = renderCols();
    const layout = (_layout || []).filter(x=>x && x.id);
    // In edit mode show all widgets (including hidden ones) so the user can re-enable them directly.
    const visibleNow = _edit ? layout : layout.filter(x=>x.on);

    const can = canEdit(_cols);
    if (!can) {
      _edit = false;
      setDockedEditorMode(false);
      applyPanelCollapsedClass();
      const p = App.util.$('#layoutPanel');
      if (p) p.hidden = true;
    }

    root.innerHTML = `<div class="dashGrid${_edit?' dashEditMode':''}" style="--dash-cols:${_cols}">${visibleNow.map(it=>{
      const r = toRendered(it, _cols);
      const ww = widget(it.id);
      const isOff = !it.on;
      return `\
        <section class="dashItem${isOff?' dashItemOff':''}" data-id="${it.id}" style="grid-column:${r.x} / span ${r.w};grid-row:${r.y} / span ${r.h};">\
          <div class="card">\
            <div class="dashHandle">\
              <h2>${(ww.title||it.id)}${isOff?'<span class="dashBadgeOff" title="Widget je skrytý (v normálním režimu se nezobrazuje)">skryté</span>':''}</h2>\
              <button class="dashEye" data-act="toggle" title="Zobrazit / skrýt">${it.on?'👁':'🙈'}</button>\
            </div>\
            <div id="w_${it.id}"></div>\
          </div>\
          <div class="dashResizeHandle" title="Změnit velikost"></div>\
          <div class="dashResizeHandleR" title="Změnit šířku"></div>\
          <div class="dashResizeHandleB" title="Změnit výšku"></div>\
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
      const resR = itemEl.querySelector('.dashResizeHandleR');
      const resB = itemEl.querySelector('.dashResizeHandleB');
      const eye = itemEl.querySelector('.dashEye');

      // Pointer Events (modern): unified mouse/touch/pen.
      if (handle) {
        handle.addEventListener('pointerdown', (ev) => {
          // Ignore drag start when clicking on the eye button.
          if (ev.target && ev.target.closest && ev.target.closest('.dashEye')) return;
          startDrag(ev, id, 'move');
        });
      }
      if (res) {
        res.addEventListener('pointerdown', (ev) => startDrag(ev, id, 'resize'));
      }
      if (resR) {
        resR.addEventListener('pointerdown', (ev) => startDrag(ev, id, 'resizeR'));
      }
      if (resB) {
        resB.addEventListener('pointerdown', (ev) => startDrag(ev, id, 'resizeB'));
      }
      if (eye) {
        eye.addEventListener('click', (ev)=>{
          ev.preventDefault();
          ev.stopPropagation();
          toggleWidgetVisibility(id);
        });
      }
    });
  }

  function maxBottomY(layout){
    let m = 1;
    for (const it of layout) {
      if (!it) continue;
      const b = (it.y || 1) + (it.h || 1) - 1;
      if (b > m) m = b;
    }
    return m;
  }

  function toggleWidgetVisibility(id){
    const it = findById(_layout, id);
    if (!it) return;

    const wasOn = !!it.on;
    it.on = !wasOn;

    // When hiding, move it to the last position so when re-enabled it appears at the end.
    if (wasOn) {
      const bottom = maxBottomY(_layout);
      it.x = 1;
      it.y = bottom + 2;
      const fixed = resolveCollision(_layout, id, it);
      it.x = fixed.x; it.y = fixed.y;
    } else {
      // When showing, just ensure it's not overlapping.
      const fixed = resolveCollision(_layout, id, it);
      it.x = fixed.x; it.y = fixed.y; it.w = fixed.w; it.h = fixed.h;
    }

    saveLocal(_profile, _layout);
    saveToDevice(_profile, _layout).catch(()=>{});
    editorRender();
    render();
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
    const xPx = (ev.clientX ?? 0) - m.rect.left;
    const yPx = (ev.clientY ?? 0) - m.rect.top;

    const colUnit = m.colW + GAP_PX;
    const rowUnit = m.rowH + GAP_PX;

    const cx = Math.floor(xPx / colUnit) + 1;  // 1.._cols
    const cy = Math.floor(yPx / rowUnit) + 1;

    // Convert rendered column (1.._cols) into virtual base grid (1..20)
    // so edits work even in responsive 12-col mode.
    const cols = _cols || GRID_COLS_BASE;
    let xBase;
    if (cols === GRID_COLS_BASE) {
      xBase = cx;
    } else {
      const scale = GRID_COLS_BASE / cols;
      xBase = Math.round((cx - 1) * scale) + 1;
    }

    return {
      x: clampInt(xBase, 1, GRID_COLS_BASE),
      y: clampInt(cy, 1, 9999)
    };
  }

  function startDrag(ev, id, kind){
    // Only left click for mouse.
    if (ev.pointerType === 'mouse' && ev.button !== 0) return;
    ev.preventDefault();
    ev.stopPropagation();

    const it = findById(_layout, id);
    if (!it) return;

    const m = gridMetrics();
    if (!m) return;

    const cell = pointerToCell(ev, m);

    const el = getItemEl(id);
    if (!el) return;

    _drag = {
      kind,
      id,
      el,
      startMouse: { x: ev.clientX, y: ev.clientY },
      startCell: cell,
      startItem: { ...it },
      metrics: m,
    };

    // Capture pointer so the drag continues even if the cursor leaves the element.
    try { el.setPointerCapture(ev.pointerId); } catch(_) {}
    el.classList.add('dashDragging');

    // Prevent text selection while dragging.
    try { document.body.style.userSelect = 'none'; } catch(_) {}

    window.addEventListener('pointermove', onDragMove, { passive: false });
    window.addEventListener('pointerup', onDragEnd, { passive: false });
    window.addEventListener('pointercancel', onDragEnd, { passive: false });
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
      // resize from handles
      let newW = _drag.startItem.w;
      let newH = _drag.startItem.h;

      if (_drag.kind === 'resize' || _drag.kind === 'resizeR') {
        newW = clampInt(_drag.startItem.w + dx, 1, GRID_COLS_BASE);
      }
      if (_drag.kind === 'resize' || _drag.kind === 'resizeB') {
        newH = clampInt(_drag.startItem.h + dy, 1, 9999);
      }

      if (_drag.startItem.x + newW - 1 > GRID_COLS_BASE) newW = GRID_COLS_BASE - _drag.startItem.x + 1;

      let cand = { x: it.x, y: it.y, w: newW, h: newH };
      cand = applySnap(_layout, it.id, cand);
      cand = resolveCollision(_layout, it.id, cand);

      it.w = cand.w;
      it.h = cand.h;
      it.x = cand.x;
      it.y = cand.y;
    }

    // Apply only the moved/resized item style (no full re-render during drag).
    applyItemStyle(it.id);
  }

  function onDragEnd(ev){
    if (!_drag) return;

    const el = _drag.el;
    if (el) {
      el.classList.remove('dashDragging');
      try { if (ev && ev.pointerId != null) el.releasePointerCapture(ev.pointerId); } catch(_) {}
    }

    window.removeEventListener('pointermove', onDragMove);
    window.removeEventListener('pointerup', onDragEnd);
    window.removeEventListener('pointercancel', onDragEnd);

    try { document.body.style.userSelect = ''; } catch(_) {}

    const endedId = _drag.id;
    _drag = null;

    // persist
    saveLocal(_profile, _layout);
    saveToDevice(_profile, _layout).catch(()=>{});

    // Full re-render once at the end (keeps DOM + widget mounts in sync).
    render();

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

    const header = `
      <div class="layoutPanelTop">
        <button class="btn sm" id="dashPanelCollapse" title="${_panelCollapsed?'Rozbalit panel':'Sbalit panel'}">
          ${_panelCollapsed ? '»' : '«'}
        </button>
        <div class="layoutPanelTopTitle">Layout</div>
      </div>
    `;

    const rows = _layout.map((it, i) => {
      const ww = widget(it.id);
      const title = ww.title || it.id;
      return `\
        <div class="layoutRow">\
          <div class="layoutLeft">\
            <button class="dashEye inPanel" data-i="${i}" data-act="toggle" title="Zobrazit / skrýt">${it.on?'👁':'🙈'}</button>\
            <div class="layoutTitle">${title}</div>\
          </div>\
          <div class="layoutMeta">${it.w}/20 · ${it.h} ř. · x:${it.x} y:${it.y}</div>\
        </div>`;
    }).join('');

    ed.innerHTML = header + `\
      <div class="layoutPanelBody">\
      <div class="layoutRow" style="justify-content:space-between;gap:10px">\
        <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">\
          <b>Profil:</b>\
          <select class="sel" id="dashProfileSel"></select>\
          <button class="btn sm" id="dashProfileNew">Nový</button>\
          <button class="btn sm danger" id="dashProfileDel">Smazat</button>\
          <button class="btn sm" id="dashProfileReset">Reset</button>\
        </div>\
        <div class="hint" style="margin:0">Tip: chyť widget za hlavičku a přesuň. Změnu velikosti uděláš tažením za okraj/roh.</div>\
      </div>\
      <div class="hint" style="margin:8px 0 10px 0">V editačním režimu jsou vidět i skryté widgety (šedé). Oko přepíná viditelnost v normálním režimu.</div>\
      ${rows}\
      </div>`;

    // fill profiles
    const bCol = App.util.$('#dashPanelCollapse');
    if (bCol) bCol.onclick = togglePanelCollapsed;

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

    // bind eye toggles in panel
    ed.querySelectorAll('button[data-act="toggle"]').forEach(btn => btn.addEventListener('click', ()=>{
      const i = +btn.dataset.i;
      const it = _layout[i];
      if (!it) return;
      toggleWidgetVisibility(it.id);
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
      setDockedEditorMode(true);
      applyPanelCollapsedClass();
      editorRender();
      render();
    },

    closeEditor(){
      const p = App.util.$('#layoutPanel');
      if (p) p.hidden = true;
      _edit = false;
      setDockedEditorMode(false);
      applyPanelCollapsedClass();
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
