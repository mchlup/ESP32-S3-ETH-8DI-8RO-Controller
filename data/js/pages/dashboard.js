(function () {
  function updateEquithermBlock() {
    const cfg = window.App?.getConfig?.() || {};
    const st = window.App?.getStatus?.() || {};
    const eCfg = cfg.equitherm || {};
    const e = st.equitherm || {};

    const block = document.getElementById("dashEqBlock");
    if (!block) return;

    const enabled = !!eCfg.enabled;
    block.hidden = !enabled;
    if (!enabled) return;

    const modeEl = document.getElementById("dashEqMode");
    const tempsEl = document.getElementById("dashEqTemps");
    const tgtEl = document.getElementById("dashEqTarget");
    const valveEl = document.getElementById("dashEqValve");

    const night =
      (typeof e.nightMode === "boolean") ? e.nightMode :
      ((typeof e.night === "boolean") ? e.night : false);

    const active = (typeof e.active === "boolean") ? e.active : false;

    const mode = active
      ? (night ? "AKTIVNÍ (noc)" : "AKTIVNÍ (den)")
      : (night ? "NEAKTIVNÍ (noc)" : "NEAKTIVNÍ (den)");
    if (modeEl) modeEl.textContent = mode;

    const out = (typeof e.outdoorC === "number") ? `${e.outdoorC.toFixed(1)} °C` : "—";

    const boilerInC =
      (typeof e.boilerInC === "number") ? e.boilerInC :
      ((typeof e.flowC === "number") ? e.flowC : null);
    const boilerIn = (typeof boilerInC === "number") ? `${boilerInC.toFixed(1)} °C` : "—";

    if (tempsEl) tempsEl.textContent = `Tout: ${out} • boiler_in: ${boilerIn}`;

    const target = (typeof e.targetFlowC === "number") ? `${e.targetFlowC.toFixed(1)} °C` : "—";
    if (tgtEl) tgtEl.textContent = target;

    const pos = (typeof e.valvePosPct === "number") ? `${Math.round(e.valvePosPct)} %` : "—";
    const vt = (typeof e.valveTargetPct === "number") ? `${Math.round(e.valveTargetPct)} %` : "—";
    const moving = e.valveMoving ? " (pohyb)" : "";
    if (valveEl) valveEl.textContent = `${pos} › ${vt}${moving}`;
  }

  window.Pages = window.Pages || {};
  window.Pages.dashboard = {
    id: "dashboard",
    mount() {
      window.Core?.legacy?.showPage?.("dashboard");
      window.Core?.legacy?.renderDashboard?.();
      window.Core?.legacy?.renderStatus?.();
      window.Core?.legacy?.renderTop?.();
      updateEquithermBlock();
    },
    unmount() {},
    onStoreUpdate(type) {
      if (type === "status") {
        window.Core?.legacy?.renderStatus?.();
        window.Core?.legacy?.renderTop?.();
      }
      if (type === "dash" || type === "config" || type === "status") {
        window.Core?.legacy?.renderDashboard?.();
        updateEquithermBlock();
      }
    },
  };
})();

