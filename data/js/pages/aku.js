/* AKU support tab – konfigurace ekvitermní AKU podpory
 * - Uloží konfiguraci do cfg.equitherm.* (kompatibilní s firmware)
 * - V ne-expert režimu umí sync mapování rolí teploměrů / ventilu do cfg.equitherm.*
 * - Zobrazuje runtime stav z /api/status (equitherm)
 */
(() => {
  let initialized = false;

  const init = () => {
    if (initialized) return;
    initialized = true;

    const $ = (id) => document.getElementById(id);

    const el = {
      btnSave: $("btnSaveAku"),
      roleList: $("akuRoleList"),
      statusBox: $("akuStatusBox"),
      chip: $("chip-aku"),

      supportEnabled: $("eqAkuSupportEnabled"),
      noSupportBehavior: $("eqAkuNoSupportBehavior"),

      minTopDay: $("eqAkuMinTopCDay"),
      minDeltaTargetDay: $("eqAkuMinDeltaToTargetCDay"),
      minDeltaBoilerDay: $("eqAkuMinDeltaToBoilerInCDay"),

      minTopNight: $("eqAkuMinTopCNight"),
      minDeltaTargetNight: $("eqAkuMinDeltaToTargetCNight"),
      minDeltaBoilerNight: $("eqAkuMinDeltaToBoilerInCNight"),
    };

    if (!window.App || !el.btnSave) {
      initialized = false;
      return;
    }

    const expertModeKey = "ekviterm_expert_mode";
    const isExpertMode = () => {
      try { return localStorage.getItem(expertModeKey) === "1"; } catch (_) { return false; }
    };

    const readNumber = (v, def) => {
      const x = Number(String(v ?? "").replace(",", "."));
      return Number.isFinite(x) ? x : def;
    };

    const fmtTemp = (v) => (typeof v === "number" && Number.isFinite(v)) ? `${v.toFixed(1)} °C` : "--";

    const applyTempRole = (role, target) => {
      target.source = String(role?.source || "none");
      target.gpio = Number(role?.gpio ?? 0);
      target.rom = String(role?.rom || "");
      target.topic = String(role?.topic || "");
      target.jsonKey = String(role?.jsonKey || "");
      target.mqttIdx = Number(role?.mqttIdx || role?.preset || 0);
      target.bleId = String(role?.bleId || role?.id || "");
    };

    const syncRolesToEquitherm = (cfg) => {
      const map = App.getRoleMap?.();
      if (!map) return;
      const roles = cfg?.thermometers?.roles || {};

      cfg.equitherm = cfg.equitherm || {};
      const e = cfg.equitherm;

      e.outdoor = e.outdoor || {};
      e.flow = e.flow || {};
      e.boilerIn = e.boilerIn || {};
      e.akuTop = e.akuTop || {};
      e.akuMid = e.akuMid || {};
      e.akuBottom = e.akuBottom || {};

      applyTempRole(roles.outdoor || {}, e.outdoor);
      applyTempRole(roles.flow || {}, e.flow);
      applyTempRole(roles.flow || {}, e.boilerIn);

      applyTempRole(roles.tankTop || {}, e.akuTop);
      applyTempRole(roles.tankMid || {}, e.akuMid);
      applyTempRole(roles.tankBottom || {}, e.akuBottom);

      e.valve = e.valve || {};
      if (map.outputs?.valve_3way_mix) {
        e.valve.master = map.outputs.valve_3way_mix.index;
        if (map.outputs.valve_3way_mix.peer) {
          e.valve.peerRel = map.outputs.valve_3way_mix.peer;
        }
      } else {
        e.valve.master = 0;
      }
    };

    const renderRoleList = (cfg) => {
      const map = App.getRoleMap?.();
      if (!el.roleList || !map) return;

      const fmtRole = (role) => {
        if (!role || role.source === "none") return "Nepřiřazeno";
        return `${role.label} (${role.detail})`;
      };

      const valve = map.outputs?.valve_3way_mix;
      const valveLabel = valve ? `${valve.label}${valve.peer ? ` (peer ${valve.peer})` : ""}` : "Nepřiřazeno";

      el.roleList.innerHTML = `
        <li><span>Venkovní teplota</span><strong>${fmtRole(map.temps?.outdoor)}</strong></li>
        <li><span>Boiler_in (flow)</span><strong>${fmtRole(map.temps?.flow)}</strong></li>
        <li><span>AKU top</span><strong>${fmtRole(map.temps?.tankTop)}</strong></li>
        <li><span>AKU mid</span><strong>${fmtRole(map.temps?.tankMid)}</strong></li>
        <li><span>AKU bottom</span><strong>${fmtRole(map.temps?.tankBottom)}</strong></li>
        <li><span>Směšovací ventil</span><strong>${valveLabel}</strong></li>
      `;
    };

    const updateChip = (cfg, status) => {
      if (!el.chip) return;
      const enabled = !!cfg?.equitherm?.akuSupportEnabled;
      const active = !!status?.equitherm?.akuSupportActive;
      const missing = !cfg?.thermometers?.roles?.tankTop || String(cfg?.thermometers?.roles?.tankTop?.source || "none") === "none";

      let cls = "chip";
      let txt = "—";
      if (!enabled) { cls = "chip off"; txt = "OFF"; }
      else if (missing) { cls = "chip missing"; txt = "MISSING"; }
      else if (active) { cls = "chip ok"; txt = "OK"; }
      else { cls = "chip ok"; txt = "OK"; }

      el.chip.className = cls;
      el.chip.textContent = txt;
    };

    const loadFromConfig = (cfg) => {
      App.ensureConfigShape(cfg);
      cfg.equitherm = cfg.equitherm || {};
      const e = cfg.equitherm;

      el.supportEnabled.checked = (typeof e.akuSupportEnabled === "boolean") ? e.akuSupportEnabled : true;
      el.noSupportBehavior.value = e.akuNoSupportBehavior || "close";

      el.minTopDay.value = (typeof e.akuMinTopC_day === "number") ? e.akuMinTopC_day : 42;
      el.minTopNight.value = (typeof e.akuMinTopC_night === "number") ? e.akuMinTopC_night : 45;

      el.minDeltaTargetDay.value = (typeof e.akuMinDeltaToTargetC_day === "number") ? e.akuMinDeltaToTargetC_day : 2;
      el.minDeltaTargetNight.value = (typeof e.akuMinDeltaToTargetC_night === "number") ? e.akuMinDeltaToTargetC_night : 3;

      el.minDeltaBoilerDay.value = (typeof e.akuMinDeltaToBoilerInC_day === "number") ? e.akuMinDeltaToBoilerInC_day : 3;
      el.minDeltaBoilerNight.value = (typeof e.akuMinDeltaToBoilerInC_night === "number") ? e.akuMinDeltaToBoilerInC_night : 4;

      renderRoleList(cfg);
      updateStatusBox(App.getStatus?.(), cfg);
      updateChip(cfg, App.getStatus?.());
    };

    const saveToConfig = (cfg) => {
      App.ensureConfigShape(cfg);
      cfg.equitherm = cfg.equitherm || {};
      const e = cfg.equitherm;

      e.akuSupportEnabled = !!el.supportEnabled.checked;
      e.akuNoSupportBehavior = String(el.noSupportBehavior.value || "close");

      e.akuMinTopC_day = readNumber(el.minTopDay.value, 42);
      e.akuMinTopC_night = readNumber(el.minTopNight.value, 45);

      e.akuMinDeltaToTargetC_day = readNumber(el.minDeltaTargetDay.value, 2);
      e.akuMinDeltaToTargetC_night = readNumber(el.minDeltaTargetNight.value, 3);

      e.akuMinDeltaToBoilerInC_day = readNumber(el.minDeltaBoilerDay.value, 3);
      e.akuMinDeltaToBoilerInC_night = readNumber(el.minDeltaBoilerNight.value, 4);

      // kompatibilita s legacy klíči
      e.akuMinTopC = e.akuMinTopC_day;
      e.akuMinDeltaToTargetC = e.akuMinDeltaToTargetC_day;
      e.akuMinDeltaToBoilerInC = e.akuMinDeltaToBoilerInC_day;

      if (!isExpertMode()) syncRolesToEquitherm(cfg);
    };

    const updateStatusBox = (status, cfg) => {
      if (!el.statusBox) return;
      const st = status?.equitherm || {};
      const lines = [];

      const eqEnabled = (typeof st.enabled === "boolean") ? st.enabled : !!cfg?.equitherm?.enabled;
      const eqActive = (typeof st.active === "boolean") ? st.active : false;
      const night = (typeof st.night === "boolean") ? st.night : false;

      lines.push(`Ekviterm: ${eqEnabled ? "povolen" : "zakázán"} • ${eqActive ? "aktivní" : "neaktivní"}${night ? " • noc" : ""}`);
      if (st.reason) lines.push(`Důvod: ${String(st.reason)}`);

      if (typeof st.akuSupportActive !== "undefined") {
        const support = st.akuSupportActive ? "ON" : "OFF";
        const reason = st.akuSupportReason ? ` • ${st.akuSupportReason}` : "";
        lines.push(`AKU podpora: ${support}${reason}`);
      }

      const top = fmtTemp(st.akuTopC);
      const mid = fmtTemp(st.akuMidC);
      const bot = fmtTemp(st.akuBottomC);
      if (top !== "--" || mid !== "--" || bot !== "--") {
        const vTop = (typeof st.akuTopValid === "boolean") ? (st.akuTopValid ? "OK" : "X") : "-";
        const vMid = (typeof st.akuMidValid === "boolean") ? (st.akuMidValid ? "OK" : "X") : "-";
        const vBot = (typeof st.akuBottomValid === "boolean") ? (st.akuBottomValid ? "OK" : "X") : "-";
        lines.push(`AKU: top ${top} (${vTop}) • mid ${mid} (${vMid}) • bottom ${bot} (${vBot})`);
      }

      if (typeof st.flowC === "number") {
        lines.push(`Boiler_in: ${fmtTemp(st.flowC)} • target ${fmtTemp(st.targetFlowC)}`);
      }

      const master = Number(st.valveMaster || cfg?.equitherm?.valve?.master || 0);
      if (master > 0) {
        const pos = Math.round(Number(st.valvePosPct || 0));
        const tgt = Math.round(Number(st.valveTargetPct || 0));
        const moving = st.valveMoving ? " • pohyb" : "";
        lines.push(`Ventil: master ${master} • ${pos}% → ${tgt}%${moving}`);
      }

      el.statusBox.textContent = lines.join("\n");
    };

    const bindEvents = () => {
      el.btnSave.addEventListener("click", async () => {
        const cfg = App.getConfig();
        saveToConfig(cfg);
        await App.saveConfig(cfg);
        renderRoleList(cfg);
        updateStatusBox(App.getStatus?.(), cfg);
        updateChip(cfg, App.getStatus?.());
      });
    };

    const prevOnConfigLoaded = App.onConfigLoaded;
    App.onConfigLoaded = (cfg) => {
      if (typeof prevOnConfigLoaded === "function") prevOnConfigLoaded(cfg);
      loadFromConfig(cfg);
    };

    const prevOnStatusLoaded = App.onStatusLoaded;
    App.onStatusLoaded = (status) => {
      if (typeof prevOnStatusLoaded === "function") prevOnStatusLoaded(status);
      updateStatusBox(status, App.getConfig());
      updateChip(App.getConfig(), status);
    };

    bindEvents();

    const cfg = App.getConfig();
    if (cfg) loadFromConfig(cfg);
    updateStatusBox(App.getStatus?.(), cfg);
    updateChip(cfg, App.getStatus?.());
  };

  const unmount = () => {};

  window.Pages = window.Pages || {};
  window.Pages.aku = { id: "aku", mount: init, unmount };
})();
