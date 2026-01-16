(function(){
  window.App = window.App || {};
  window.Pages = window.Pages || {};

  const legacy = window.Core?.legacy;
  const router = window.Core?.router;
  const polling = window.Core?.polling;
  const events = window.Core?.events;
  const toast = window.Core?.toast?.show;

  if (legacy?.init) legacy.init();

  // App facade (compatibility)
  window.App.saveConfig = legacy?.saveConfig || window.App.saveConfig;
  window.App.reloadConfig = legacy?.loadConfig || window.App.reloadConfig;
  window.App.toast = window.App.toast || ((msg, kind) => toast?.(msg, kind));
  window.App.navigate = (hash) => router?.navigate?.(hash);

  if (events) {
    ['status','dash','config'].forEach((type) => {
      events.on(`store:${type}`, (data) => {
        const id = router?.currentId;
        const page = id ? window.Pages?.[id] : null;
        if (page?.onStoreUpdate) page.onStoreUpdate(type, data);
      });
    });
  }

  if (router?.init) router.init();

  // Polling startujeme až po prvním načtení dat.
  // Jinak (při pomalém /api/status) vzniká „request storm“ a UI se může zablokovat.
  const startPollingSafe = () => {
    try { polling?.start?.(); } catch (_) {}
  };

  if (legacy?.loadAll) {
    legacy.loadAll().then(() => {
      toast?.('Připraveno');
    }).catch((err) => {
      toast?.(`Načtení selhalo: ${err.message}`, 'bad');
    }).finally(() => {
      startPollingSafe();
    });
  } else {
    startPollingSafe();
  }
})();
