(function(){
  window.Pages = window.Pages || {};
  window.Pages.files = {
    id: 'files',
    mount() {
      window.Core?.legacy?.showPage('files');
      window.Core?.legacy?.loadFiles?.();
    },
    unmount() {},
    onStoreUpdate() {}
  };
})();
