(function(){
  window.Pages = window.Pages || {};
  window.Pages.rules = {
    id: 'rules',
    mount() {
      window.Core?.legacy?.showPage('rules');
      window.Core?.legacy?.loadRules?.();
      window.Core?.legacy?.renderRules?.();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'config') {
        window.Core?.legacy?.renderRules?.();
      }
    }
  };
})();
