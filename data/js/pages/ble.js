(function(){
  window.Pages = window.Pages || {};
  window.Pages.ble = {
    id: 'ble',
    mount() {
      window.Core?.legacy?.showPage('ble');
      window.Core?.legacy?.loadBle?.();
    },
    unmount() {},
    onStoreUpdate() {}
  };
})();
