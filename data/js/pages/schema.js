(function(){
  window.Pages = window.Pages || {};
  window.Pages.schema = {
    id: 'schema',
    mount() {
      window.Core?.legacy?.showPage('schema');
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'status' || type === 'config' || type === 'dash') {
        if (window.Core?.legacy?.renderStatus) window.Core.legacy.renderStatus();
      }
    }
  };
})();
