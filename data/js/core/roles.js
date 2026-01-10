(function () {
  const describeTempRole = (cfg, roleKey) => {
    const role = (cfg?.thermometers?.roles && typeof cfg.thermometers.roles === "object")
      ? (cfg.thermometers.roles[roleKey] || {})
      : {};
    const source = String(role?.source || "none");
    if (source === "dallas") {
      const gpio = Number(role?.gpio ?? 0);
      const rom = String(role?.rom || "").trim();
      const nm = String(cfg?.dallasNames?.[gpio] || "").trim() || `Dallas GPIO${gpio}`;
      return { source, label: `${nm}`, detail: rom ? `ROM ${rom.toUpperCase()}` : "auto" };
    }
    if (source === "mqtt") {
      const idx = Number(role?.mqttIdx || role?.preset || 0);
      const list = Array.isArray(cfg?.thermometers?.mqtt) ? cfg.thermometers.mqtt : [];
      const preset = list[idx - 1] || {};
      const name = String(preset.name || "").trim();
      const topic = String(role.topic || preset.topic || "").trim();
      const jsonKey = String(role.jsonKey || preset.jsonKey || "tempC").trim();
      return { source, label: name || "MQTT teploměr", detail: topic ? `${topic}${jsonKey ? ` • ${jsonKey}` : ""}` : "nenastaveno" };
    }
    if (source === "ble") {
      const id = String(role?.bleId || role?.id || "meteo.tempC").trim();
      return { source, label: "BLE teploměr", detail: id || "—" };
    }
    if (source && source.startsWith("temp")) {
      return { source, label: "Legacy TEMP", detail: source.toUpperCase() };
    }
    return { source: "none", label: "Nepřiřazeno", detail: "—" };
  };

  const getRoleMap = (cfg) => {
    const config = cfg || window.Core?.store?.getConfig?.() || {};
    const inputs = {};
    const outputs = {};

    (config.iofunc?.inputs || []).forEach((it, idx) => {
      const role = String(it?.role || "none");
      if (role === "none" || inputs[role]) return;
      const name = String(config.inputNames?.[idx] || "").trim() || `Vstup ${idx + 1}`;
      inputs[role] = { index: idx + 1, name, label: `${idx + 1} – ${name}` };
    });

    (config.iofunc?.outputs || []).forEach((it, idx) => {
      const role = String(it?.role || "none");
      if (role === "none" || outputs[role]) return;
      const name = String(config.relayNames?.[idx] || "").trim() || `Relé ${idx + 1}`;
      const params = (it && typeof it.params === "object") ? it.params : {};
      outputs[role] = {
        index: idx + 1,
        name,
        label: `${idx + 1} – ${name}`,
        peer: Number(params.peerRel ?? params.partnerRelay ?? 0) || 0,
      };
    });

    const mixValve = outputs.valve_3way_mix || outputs.valve_3way_2rel || null;
    if (mixValve) outputs.valve_3way_mix = mixValve;
    const tuzValve = outputs.valve_3way_tuv || outputs.valve_3way_dhw || null;
    if (tuzValve) outputs.valve_3way_tuv = tuzValve;

    const temps = {};
    ["outdoor", "flow", "return", "dhw", "tankTop", "tankMid", "tankBottom"].forEach((role) => {
      temps[role] = describeTempRole(config, role);
    });

    return { inputs, outputs, temps, cfg: config };
  };

  window.Core = window.Core || {};
  window.Core.roles = { getRoleMap, describeTempRole };
})();
