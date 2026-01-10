(function () {
  const dirty = {
    form: false,
    cfgJson: false,
  };

  const markDirty = (key = "form") => {
    dirty[key] = true;
    if (window.Core?.events) window.Core.events.emit("dirty", { ...dirty });
  };
  const clearDirty = (key) => {
    if (key) dirty[key] = false;
    else {
      dirty.form = false;
      dirty.cfgJson = false;
    }
    if (window.Core?.events) window.Core.events.emit("dirty", { ...dirty });
  };
  const isDirty = (key = "form") => !!dirty[key];
  const getState = () => ({ ...dirty });

  window.Core = window.Core || {};
  window.Core.dirty = { markDirty, clearDirty, isDirty, getState };
})();
