(function () {
  window.Pages = window.Pages || {};

  const initFacade = () => {
    window.App = window.App || {};
    window.App.toast = (msg, kind, opts) => window.Core.toast.show(msg, kind, opts);
    window.App.navigate = (hash) => window.Core.router.navigate(hash);
    window.App.getConfig = window.App.getConfig || (() => window.Core.store.getConfig());
    window.App.getStatus = window.App.getStatus || (() => window.Core.store.getStatus());
    window.App.getDash = window.App.getDash || (() => window.Core.store.getDash());
  };

  const init = async () => {
    initFacade();
    window.Core.router.start();
    await window.Core.legacy.init();
    window.Core.polling.start();

    setTimeout(() => {
      window.Core.router.prefetch(["schema", "ekviterm", "tuv"]);
    }, 1200);
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
