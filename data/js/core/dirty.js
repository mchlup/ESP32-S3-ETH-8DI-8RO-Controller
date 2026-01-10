(function(){
  let dirty = false;
  const markDirty = () => { dirty = true; window.App?.setDirty?.(true); };
  const clearDirty = () => { dirty = false; window.App?.setDirty?.(false); };
  const isDirty = () => window.App?.isDirty ? window.App.isDirty() : dirty;

  window.Core = window.Core || {};
  window.Core.dirty = { markDirty, clearDirty, isDirty };
})();
