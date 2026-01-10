(function () {
  const initHelpers = () => {
    if (window.Modules?.opentherm?.init) window.Modules.opentherm.init();
    if (window.Modules?.buzzer?.init) window.Modules.buzzer.init();
    if (window.Modules?.schedules?.init) window.Modules.schedules.init();
    if (window.Modules?.time_ntp?.init) window.Modules.time_ntp.init();
    if (window.Modules?.valve_calib?.init) window.Modules.valve_calib.init();
  };

  const mount = () => {
    initHelpers();
  };
  const unmount = () => {
    window.Modules?.opentherm?.destroy?.();
    window.Modules?.buzzer?.destroy?.();
    window.Modules?.schedules?.destroy?.();
    window.Modules?.time_ntp?.destroy?.();
    window.Modules?.valve_calib?.destroy?.();
  };

  window.Pages = window.Pages || {};
  window.Pages.system = { id: "system", mount, unmount };
})();
