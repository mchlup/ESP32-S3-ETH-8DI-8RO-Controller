(function () {
  const state = {
    status: null,
    dash: null,
    config: null,
  };

  const emit = (type, data) => {
    if (window.Core && window.Core.events) {
      window.Core.events.emit(`store:${type}`, data);
    }
  };

  const setStatus = (status) => {
    state.status = status;
    emit("status", status);
  };
  const setDash = (dash) => {
    state.dash = dash;
    emit("dash", dash);
  };
  const setConfig = (config) => {
    state.config = config;
    emit("config", config);
  };

  const getStatus = () => state.status;
  const getDash = () => state.dash;
  const getConfig = () => state.config;

  const subscribe = (type, cb) => {
    if (!window.Core || !window.Core.events) return () => {};
    return window.Core.events.on(`store:${type}`, cb);
  };

  window.Core = window.Core || {};
  window.Core.store = { state, setStatus, setDash, setConfig, getStatus, getDash, getConfig, subscribe };
})();
