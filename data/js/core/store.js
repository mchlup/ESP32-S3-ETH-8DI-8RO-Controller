(function(){
  const state = {
    status: null,
    dash: null,
    config: null,
  };
  const events = () => window.Core?.events;

  const setStatus = (data) => {
    state.status = data;
    events()?.emit('store:status', data);
  };
  const setDash = (data) => {
    state.dash = data;
    events()?.emit('store:dash', data);
  };
  const setConfig = (data) => {
    state.config = data;
    events()?.emit('store:config', data);
  };

  window.Core = window.Core || {};
  window.Core.store = { state, setStatus, setDash, setConfig };
})();
