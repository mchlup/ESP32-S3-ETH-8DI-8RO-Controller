(function(){
  const ensureConfigShape = (cfg) => {
    if (window.App?.ensureConfigShape) return window.App.ensureConfigShape(cfg);
    return cfg || {};
  };

  window.Core = window.Core || {};
  window.Core.configShape = { ensureConfigShape };
})();
