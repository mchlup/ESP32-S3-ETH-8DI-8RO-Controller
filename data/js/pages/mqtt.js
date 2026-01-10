(function(){
  window.Pages = window.Pages || {};
  window.Pages.mqtt = {
    id: 'mqtt',
    mount() {
      window.Core?.legacy?.showPage('mqtt');
      window.Core?.legacy?.loadConfig?.();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === 'config') {
        window.Core?.legacy?.renderStatus?.();
      }
    }
  };
})();
