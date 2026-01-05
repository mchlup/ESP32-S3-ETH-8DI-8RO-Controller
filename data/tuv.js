/* TUV (ohřev bojleru) – režim + přepínací ventily */
(() => {
  const $ = (id) => document.getElementById(id);

  const el = {
    btnSave: $("btnSaveTuv"),
    enabled: $("tuvEnabled"),
    enableHint: $("tuvEnableHint"),
    demandInput: $("tuvDemandInput"),
    requestRelay: $("tuvRequestRelay"),
    eqValveTargetPct: $("tuvEqValveTargetPct"),
    valveMaster: $("tuvValveMaster"),
    bypassEnabled: $("tuvBypassEnabled"),
    bypassInvert: $("tuvBypassInvert"),
    bypassPct: $("tuvBypassPct"),
    chPct: $("tuvChPct"),
    restoreEqValve: $("tuvRestoreEqValve"),
    statusBox: $("tuvStatusBox"),
  };

  if (!window.App || !el.btnSave) return;

  const clampPct = (v) => {
    const n = Number(v);
    if (!Number.isFinite(n)) return 0;
    if (n < 0) return 0;
    if (n > 100) return 100;
    return Math.round(n);
  };

  const readInt = (v, fallback = 0) => {
    const n = parseInt(v, 10);
    return Number.isFinite(n) ? n : fallback;
  };

  function setSelectOptions(selectEl, options, keepValue) {
    if (!selectEl) return;
    const prev = keepValue ? selectEl.value : null;
    selectEl.innerHTML = "";
    for (const o of options) {
      const opt = document.createElement("option");
      opt.value = o.value;
      opt.textContent = o.label;
      if (o.disabled) opt.disabled = true;
      selectEl.appendChild(opt);
    }
    if (keepValue && prev) {
      const exists = Array.from(selectEl.options).some((o) => o.value === prev);
      if (exists) selectEl.value = prev;
    }
  }

  function buildRelayOptions(cfg) {
    const opts = [{ value: "0", label: "— (nepoužívat)" }];
    const names = Array.isArray(cfg?.relayNames) ? cfg.relayNames : [];
    const outputs = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
    for (let i = 0; i < 8; i++) {
      const name = String(names[i] || "").trim() || `Relé ${i + 1}`;
      const role = String(outputs[i]?.role || "none");
      const suffix = (role === "boiler_enable_dhw") ? "" : " (nastaví roli boiler_enable_dhw)";
      opts.push({ value: String(i + 1), label: `${i + 1} – ${name}${suffix}` });
    }
    return opts;
  }

  function buildInputOptions(cfg) {
    const opts = [{ value: "0", label: "— (nepoužívat)" }];
    const names = Array.isArray(cfg?.inputNames) ? cfg.inputNames : [];
    for (let i = 0; i < 8; i++) {
      const name = String(names[i] || "").trim() || `Vstup ${i + 1}`;
      opts.push({ value: String(i + 1), label: `${i + 1} – ${name}` });
    }
    return opts;
  }

  function updateValveOptions(dash, cfg, keep) {
    const opts = [{ value: "0", label: "Nepoužívat" }];
    const seen = new Set();
    const relayNames = Array.isArray(cfg?.relayNames) ? cfg.relayNames : [];

    const addOption = (master, peer) => {
      const m = Number(master);
      if (!Number.isFinite(m) || m < 1 || m > 8 || seen.has(m)) return;
      const rawName = String(relayNames[m - 1] || "").trim();
      const base = rawName || `Ventil ${m}`;
      const peerTxt = (peer && Number.isFinite(Number(peer))) ? ` / peer ${peer}` : "";
      opts.push({ value: String(m), label: `${base} (master ${m}${peerTxt})` });
      seen.add(m);
    };

    const valves = (dash && Array.isArray(dash.valves)) ? dash.valves : [];
    for (const v of valves) addOption(v?.master, v?.peer);

    const outs = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
    outs.forEach((o, idx) => {
      const r = String(o?.role || "");
      // preferujeme explicitní "Přepínací" ventil; fallback na legacy "valve_3way_2rel"
      if (r !== "valve_3way_tuv" && r !== "valve_3way_dhw" && r !== "valve_3way_2rel") return;
      const params = (o && typeof o.params === "object") ? o.params : {};
      if (r === "valve_3way_tuv" || r === "valve_3way_dhw") {
        addOption(idx + 1, null);
      } else {
        const peer = params.peerRel ?? params.partnerRelay;
        addOption(idx + 1, peer);
      }
    });

    const cur = cfg?.tuv?.valveMaster || 0;
    if (cur && !opts.some((o) => o.value === String(cur))) {
      opts.push({ value: String(cur), label: `Relé ${cur} (nenakonfigurováno jako 3c)` });
    }

    setSelectOptions(el.valveMaster, opts, keep);
  }

  function loadFromConfig(cfg) {
    App.ensureConfigShape(cfg);
    const t = cfg.tuv || {};
    el.enabled.checked = !!t.enabled;
    el.demandInput.value = String(t.demandInput || 0);
    el.requestRelay.value = String(t.requestRelay || 0);
    el.eqValveTargetPct.value = clampPct(t.eqValveTargetPct ?? 0);
    el.valveMaster.value = String(t.valveMaster || 0);
    const bv = t.bypassValve || {};
    el.bypassEnabled.checked = !!bv.enabled;
    el.bypassInvert.checked = !!bv.invert;
    el.bypassPct.value = clampPct(bv.bypassPct ?? 100);
    el.chPct.value = clampPct(bv.chPct ?? 100);
    el.restoreEqValve.checked = (typeof t.restoreEqValveAfter === "boolean") ? t.restoreEqValveAfter : true;
    updateEnableHint(cfg);
  }

  function findInputRoleIndex(cfg, role) {
    const inputs = Array.isArray(cfg?.iofunc?.inputs) ? cfg.iofunc.inputs : [];
    for (let i = 0; i < inputs.length; i++) {
      if (String(inputs[i]?.role || "") === role) return i;
    }
    return -1;
  }

  function updateEnableHint(cfg) {
    const idx = findInputRoleIndex(cfg, "dhw_enable");
    if (el.enableHint) {
      el.enableHint.textContent = (idx >= 0)
        ? `Řízeno vstupem ${idx + 1} (Funkce I/O) – přepínač je uzamčen.`
        : "";
    }
    if (el.enabled) {
      el.enabled.disabled = (idx >= 0);
    }
  }

  function saveToConfig(cfg) {
    App.ensureConfigShape(cfg);
    cfg.tuv = cfg.tuv || {};
    if (!el.enabled.disabled) cfg.tuv.enabled = !!el.enabled.checked;
    cfg.tuv.demandInput = readInt(el.demandInput.value, 0);
    cfg.tuv.requestRelay = readInt(el.requestRelay.value, 0);
    cfg.tuv.eqValveTargetPct = clampPct(el.eqValveTargetPct.value);
    cfg.tuv.valveMaster = readInt(el.valveMaster.value, 0);
    cfg.tuv.restoreEqValveAfter = !!el.restoreEqValve.checked;
    cfg.tuv.bypassValve = (cfg.tuv.bypassValve && typeof cfg.tuv.bypassValve === "object") ? cfg.tuv.bypassValve : {};
    cfg.tuv.bypassValve.enabled = !!el.bypassEnabled.checked;
    cfg.tuv.bypassValve.mode = "single_relay_spdt";
    cfg.tuv.bypassValve.masterRelay = cfg.tuv.valveMaster;
    cfg.tuv.bypassValve.bypassPct = clampPct(el.bypassPct.value);
    cfg.tuv.bypassValve.chPct = clampPct(el.chPct.value);
    cfg.tuv.bypassValve.invert = !!el.bypassInvert.checked;
  }

  function updateStatusBox(status, cfg) {
    const st = status?.tuv || {};
    const lines = [];
    const active = (typeof st.active === "boolean") ? st.active : !!st.modeActive;
    lines.push(`Režim: ${active ? "AKTIVNÍ" : "neaktivní"}`);
    if (st.reason || st.source) lines.push(`Důvod: ${String(st.reason || st.source)}`);
    if (typeof st.scheduleEnabled !== "undefined") lines.push(`Plán: ${st.scheduleEnabled ? "ON" : "OFF"}`);
    if (typeof st.boilerRelayOn !== "undefined") lines.push(`Relé TUV: ${st.boilerRelayOn ? "ON" : "OFF"}`);

    const eqMaster = Number(st.eqValveMaster || cfg?.equitherm?.valve?.master || 0);
    if (eqMaster > 0) {
      const saved = st.eqValveSavedValid ? ` • saved ${clampPct(st.eqValveSavedPct)}%` : "";
      lines.push(`Směšovací ventil (ekviterm): master ${eqMaster} → ${clampPct(st.eqValveTargetPct ?? cfg?.tuv?.eqValveTargetPct ?? 0)}%${saved}`);
    }

    const tuvMaster = Number(st.valveMaster || cfg?.tuv?.valveMaster || 0);
    if (tuvMaster > 0) {
      const pos = clampPct(st.valvePosPct ?? 0);
      const bv = cfg?.tuv?.bypassValve || {};
      const tgt = clampPct(st.valveTargetPct ?? (active ? (bv.bypassPct ?? 100) : (bv.chPct ?? 100)));
      const moving = st.valveMoving ? " • pohyb" : "";
      lines.push(`Bypass ventil: master ${tuvMaster} • ${pos}% → ${tgt}%${moving}`);
    }

    el.statusBox.textContent = lines.join("\n");
  }

  async function refreshDash(cfg, keep) {
    try {
      const dash = await App.apiGetJson("/api/dash");
      updateValveOptions(dash, cfg, keep);
    } catch (_) {
      // ignore
    }
  }

  function bindEvents() {
    if (el.requestRelay) {
      el.requestRelay.addEventListener("change", () => {
        const cfg = App.getConfig?.();
        if (!cfg) return;
        const idx = readInt(el.requestRelay.value, 0);
        if (idx < 1 || idx > 8) return;
        App.ensureConfigShape(cfg);
        cfg.iofunc = cfg.iofunc || {};
        cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : [];
        while (cfg.iofunc.outputs.length < 8) cfg.iofunc.outputs.push({ role: "none", params: {} });
        cfg.iofunc.outputs[idx - 1].role = "boiler_enable_dhw";
      });
    }

    el.btnSave.addEventListener("click", async () => {
      const cfg = App.getConfig();
      saveToConfig(cfg);
      await App.saveConfig(cfg);
      refreshDash(cfg, true);
    });
  }

  const prevOnConfigLoaded = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    if (typeof prevOnConfigLoaded === "function") prevOnConfigLoaded(cfg);
    const relayOpts = buildRelayOptions(cfg);
    setSelectOptions(el.requestRelay, relayOpts, false);
    const inputOpts = buildInputOptions(cfg);
    setSelectOptions(el.demandInput, inputOpts, false);
    loadFromConfig(cfg);
    refreshDash(cfg, true);
  };

  const prevOnStatusLoaded = App.onStatusLoaded;
  App.onStatusLoaded = (status) => {
    if (typeof prevOnStatusLoaded === "function") prevOnStatusLoaded(status);
    updateStatusBox(status, App.getConfig());
  };

  window.addEventListener("DOMContentLoaded", () => {
    bindEvents();
    const cfg = App.getConfig();
    if (cfg) {
      const relayOpts = buildRelayOptions(cfg);
      setSelectOptions(el.requestRelay, relayOpts, false);
      const inputOpts = buildInputOptions(cfg);
      setSelectOptions(el.demandInput, inputOpts, false);
      loadFromConfig(cfg);
      refreshDash(cfg, true);
      updateStatusBox(App.getStatus(), cfg);
    }
  });
})();
