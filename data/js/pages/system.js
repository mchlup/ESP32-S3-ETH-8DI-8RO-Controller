(function(){
  window.Pages = window.Pages || {};
  window.Pages.system = {
    id: 'system',
    mount() {
      window.Core?.legacy?.showPage('system');
      window.Core?.legacy?.loadConfig?.();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'config') {
        window.Core?.legacy?.updateDirtyIndicators?.();
      }
    }
  };
})();
