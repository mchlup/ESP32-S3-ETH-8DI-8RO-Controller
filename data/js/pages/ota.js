(function(){
  window.Pages = window.Pages || {};
  window.Pages.ota = {
    id: 'ota',
    mount() {
      window.Core?.legacy?.showPage('ota');
    },
    unmount() {},
    onStoreUpdate() {}
  };
})();
