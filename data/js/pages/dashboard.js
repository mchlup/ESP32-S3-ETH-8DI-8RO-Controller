(function(){
  window.Pages = window.Pages || {};
  window.Pages.dashboard = {
    id: 'dashboard',
    mount() {
      window.Core?.legacy?.showPage('dashboard');
      window.Core?.legacy?.renderDashboard?.();
      window.Core?.legacy?.renderStatus?.();
      window.Core?.legacy?.renderTop?.();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'status') {
        window.Core?.legacy?.renderStatus?.();
        window.Core?.legacy?.renderTop?.();
      }
      if (type === 'dash' || type === 'config' || type === 'status') {
        window.Core?.legacy?.renderDashboard?.();
      }
    }
  };
})();
