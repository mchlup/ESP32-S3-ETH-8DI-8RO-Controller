(function(){
  window.Pages = window.Pages || {};
  window.Pages.aku = {
    id: 'aku',
    mount() {
      window.Core?.legacy?.showPage('aku');
      window.Core?.legacy?.updateDirtyIndicators?.();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'config') {
        window.Core?.legacy?.updateDirtyIndicators?.();
      }
    }
  };
})();
