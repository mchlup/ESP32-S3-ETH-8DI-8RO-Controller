(function(){
  const cache = {};
  let currentId = null;

  const getView = () => document.getElementById('view');

  const getPageId = (hash) => {
    const raw = (hash || location.hash || '#dashboard').replace('#', '').trim();
    return raw || 'dashboard';
  };

  const loadHtml = async (id) => {
    if (cache[id]) return cache[id];
    const api = window.Core?.api;
    const html = api
      ? await api.getText(`/pages/${id}.html`)
      : await fetch(`/pages/${id}.html`).then(r => r.text());

    const wrap = document.createElement('div');
    wrap.innerHTML = html;
    const node = wrap.firstElementChild || wrap;
    cache[id] = { html, node };
    return cache[id];
  };

  const ensureScript = async (src) => {
    const loader = window.Core?.loader;
    if (!loader || typeof loader.loadScript !== 'function') return;
    await loader.loadScript(src);
  };

  const ensurePageModule = async (id) => {
    if (window.Pages?.[id]?.mount) return;
    await ensureScript(`/js/pages/${id}.js?v=2`);
  };

  const ensureSchematicDeps = async () => {
    const deps = [
      '/js/core/schematic.templates.js?v=2',
      '/js/core/schematic.paths.js?v=2',
      '/js/core/schematic.router.js?v=2',
      '/js/core/schematic.main.js?v=2',
      '/js/core/schematic.js?v=2',
    ];

    for (const d of deps) {
      try { await ensureScript(d); } catch (_) {}
    }
  };

  const ensureDepsForPage = async (id) => {
    if (id === 'dashboard' || id === 'schema') {
      await ensureSchematicDeps();
    }
  };

  const navigate = async (hash) => {
    const id = getPageId(hash);
    const view = getView();
    if (!view) return;

    // Unmount previous page
    if (currentId && window.Pages?.[currentId]?.unmount) {
      try { window.Pages[currentId].unmount(); } catch (e) { console.error(e); }
    }
    if (currentId) {
      window.dispatchEvent(new CustomEvent('app:pageUnmounted', { detail: { id: currentId } }));
    }

    // Ensure page JS + optional deps (lazy load)
    try {
      await ensureDepsForPage(id);
      await ensurePageModule(id);
    } catch (e) {
      console.error('[router] failed to load page module', id, e);
      window.Core?.toast?.show?.(`Načtení modulu stránky ${id} selhalo`, 'bad');
    }

    // Load HTML fragment
    let payload;
    try {
      payload = await loadHtml(id);
    } catch (e) {
      console.error('[router] failed to load html', id, e);
      window.Core?.toast?.show?.(`Načtení stránky ${id} selhalo`, 'bad');
      if (id !== 'dashboard') return navigate('#dashboard');
      return;
    }

    view.innerHTML = '';
    const node = payload.node;
    view.appendChild(node);
    const root = node;
    currentId = id;

    try { window.Core?.legacy?.showPage?.(id); } catch (_) {}
    try { window.Core?.legacy?.mountSummaryControls?.(); } catch (_) {}

    if (window.Pages?.[id]?.mount) {
      try {
        window.Pages[id].mount({
          root,
          store: window.Core?.store,
          api: window.Core?.api,
          toast: window.Core?.toast,
          roles: window.Core?.roles,
          dirty: window.Core?.dirty,
          navigate,
        });
      } catch (e) {
        console.error('[router] page mount failed', id, e);
      }
    }

    window.dispatchEvent(new CustomEvent('app:pageMounted', { detail: { id, root } }));
  };

  const init = () => {
    window.addEventListener('hashchange', () => navigate(location.hash));
    navigate(location.hash);
  };

  window.Core = window.Core || {};
  window.Core.router = { init, navigate, get currentId() { return currentId; } };
})();
