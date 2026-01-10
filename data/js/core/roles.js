(function(){
  const getRoleMap = () => {
    if (window.App?.getRoleMap) return window.App.getRoleMap();
    return { inputs: {}, outputs: {}, temps: {}, cfg: {} };
  };

  window.Core = window.Core || {};
  window.Core.roles = { getRoleMap };
})();
