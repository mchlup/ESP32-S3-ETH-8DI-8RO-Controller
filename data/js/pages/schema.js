/* schematic.js (Dashboard embed)
   Glue between ESP32 API data and the modular dynamic SVG schematic.

   Depends on:
   - /schematic.templates.js
   - /schematic.paths.js
   - /schematic.router.js
   - /schematic.main.js

   Data sources:
   - /api/status: valves + logic states (tuv, recirc, equitherm)
   - /api/dash: temperatures (TEMP1..8, Dallas, MQTT, BLE)
*/
(function () {
  "use strict";

  const NS = "http://www.w3.org/2000/svg";
  const $id = (id) => document.getElementById(id);
  const isFiniteNum = (v) => typeof v === "number" && Number.isFinite(v);

  let lastCfg = null;
  let lastStatus = null;
  let lastDash = null;

  let renderQueued = false;
  let initialized = false;

  function ensureSvgHost() {
    let wrap = $id("schemaWrap");
    if (!wrap) {
      const standalone = $id("schemaStandalone");
      if (standalone) {
        wrap = document.createElement("div");
        wrap.id = "schemaWrap";
        wrap.className = "schemaWrap";
        standalone.innerHTML = "";
        standalone.appendChild(wrap);
      }
    }
    if (!wrap) return null;

    let svg = $id("svg");
    if (svg) return svg;

    svg = document.createElementNS(NS, "svg");
    svg.setAttribute("id", "svg");
    svg.setAttribute("viewBox", "0 0 920 420");
    svg.setAttribute("xmlns", NS);
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label", "Schéma topného systému");

    const defs = document.createElementNS(NS, "defs");
    svg.appendChild(defs);

    const rect = document.createElementNS(NS, "rect");
    rect.setAttribute("x", "0");
    rect.setAttribute("y", "0");
    rect.setAttribute("width", "920");
    rect.setAttribute("height", "420");
    rect.setAttribute("fill", "url(#bgGrid)");
    svg.appendChild(rect);

    const pipes = document.createElementNS(NS, "g");
    pipes.setAttribute("id", "pipes");
    svg.appendChild(pipes);

    const inst = document.createElementNS(NS, "g");
    inst.setAttribute("id", "instances");
    svg.appendChild(inst);

    wrap.innerHTML = "";
    wrap.appendChild(svg);
    return svg;
  }

  async function apiGetJson(url) {
    const res = await fetch(url, { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
  }

  function getInstances() {
    return (window.SchemaPaths && Array.isArray(window.SchemaPaths.INSTANCES)) ? window.SchemaPaths.INSTANCES : [];
  }

  function getInst(id) {
    return getInstances().find((x) => String(x && x.id) === id) || null;
  }

  function patchInstVars(id, patch) {
    const inst = getInst(id);
    if (!inst) return;
    inst.vars = (inst.vars && typeof inst.vars === "object") ? inst.vars : {};
    for (const k of Object.keys(patch || {})) inst.vars[k] = patch[k];
  }

  function findDallasDevice(dash, gpio, rom) {
    const arr = Array.isArray(dash && dash.dallas) ? dash.dallas : [];
    const node = arr.find((x) => Number(x && x.gpio) === Number(gpio)) || null;
    const devs = Array.isArray(node && node.devices) ? node.devices : [];
    const want = String(rom || "").trim().toUpperCase();

    if (want) {
      const hit = devs.find((d) => String(d && d.rom).trim().toUpperCase() === want) || null;
      if (hit && hit.valid && isFiniteNum(hit.tempC)) return hit;
    }
    return devs.find((d) => d && d.valid && isFiniteNum(d.tempC)) || null;
  }

  function tryGetRoleTemp(roleKey) {
    const cfg = lastCfg || {};
    const dash = lastDash || null;
    if (!dash) return { valid: false };

    const roles = (cfg.thermometers && cfg.thermometers.roles && typeof cfg.thermometers.roles === "object")
      ? cfg.thermometers.roles
      : {};
    const role = (roles && typeof roles[roleKey] === "object") ? roles[roleKey] : {};
    const src = String(role && role.source ? role.source : "none");

    if (src === "dallas") {
      const gpio = Number(role.gpio ?? 0);
      const rom = String(role.rom || role.addr || "").trim();
      const dev = findDallasDevice(dash, gpio, rom);
      if (dev && isFiniteNum(dev.tempC)) return { valid: true, tempC: dev.tempC };
      return { valid: false };
    }

    if (src === "mqtt") {
      const idx = Number(role.mqttIdx || role.preset || 0);
      const arr = Array.isArray(dash.mqttTemps) ? dash.mqttTemps : [];
      const mt = arr.find((x) => Number(x && x.idx) === idx) || null;
      if (mt && mt.valid && isFiniteNum(mt.tempC)) return { valid: true, tempC: mt.tempC, ageMs: mt.ageMs };
      return { valid: false };
    }

    if (src === "ble") {
      const id = String(role.bleId || role.id || "").trim();
      const arr = Array.isArray(dash.bleTemps) ? dash.bleTemps : [];
      const bt = arr.find((x) => String(x && x.id) === id) || null;
      if (bt && bt.valid && isFiniteNum(bt.tempC)) return { valid: true, tempC: bt.tempC, ageMs: bt.ageMs };
      return { valid: false };
    }

    if (src && src.toLowerCase().startsWith("temp")) {
      const i = parseInt(src.substring(4) || "0", 10) - 1;
      const temps = Array.isArray(dash.temps) ? dash.temps : [];
      const valid = Array.isArray(dash.tempsValid) ? dash.tempsValid : [];
      if (i >= 0 && i < temps.length) {
        const v = temps[i];
        const ok = !!valid[i];
        if (ok && isFiniteNum(v)) return { valid: true, tempC: v };
      }
      return { valid: false };
    }

    return { valid: false };
  }

  function updateFromApis() {
    const st = lastStatus || {};
    const eq = st.equitherm || {};
    const rec = st.recirc || {};
    const tuv = st.tuv || {};
    const valves = st.valves || {};
    const akuH = st.akuHeater || {};

    // runtime state (for animations)
    const heatOn = !!(st.heatCall && st.heatCall.raw);
    const dhwReq = !!tuv.demandActive;                 // vstup pro požadavek DHW
    const dhwActive = !!tuv.active || dhwReq;          // ohřev DHW běží nebo je vyžádán
    const floorActive = !!eq.active && !dhwActive;     // ekviterm topení (DHW má prioritu)
    const akuSupportActive = !!eq.akuSupportActive;
    const akuHeaterActive = !!akuH.active;
    const anyFlow = heatOn || floorActive || dhwActive || akuSupportActive || akuHeaterActive;

    // --- temperatures from roles (preferred) ---
    const out = tryGetRoleTemp("outdoor");
    if (out.valid) patchInstVars("outdoor", { tempC: out.tempC });
    else if (eq.outdoorValid && isFiniteNum(eq.outdoorC)) patchInstVars("outdoor", { tempC: eq.outdoorC });
    else patchInstVars("outdoor", { tempC: null });

    const flow = tryGetRoleTemp("flow");
    const ret = tryGetRoleTemp("return");
    const dhwT = tryGetRoleTemp("dhw");
    const tTop = tryGetRoleTemp("tankTop");
    const tMid = tryGetRoleTemp("tankMid");
    const tBot = tryGetRoleTemp("tankBottom");

    patchInstVars("ts_boiler_T", { tempC: flow.valid ? flow.tempC : (isFiniteNum(eq.flowC) ? eq.flowC : null) });
    patchInstVars("ts_boiler_V", { tempC: ret.valid ? ret.tempC : null });
    patchInstVars("ts_dhw",      { tempC: dhwT.valid ? dhwT.tempC : null });
    patchInstVars("ts_aku_T",    { tempC: tTop.valid ? tTop.tempC : (isFiniteNum(eq.akuTopC) ? eq.akuTopC : null) });
    patchInstVars("ts_aku_M",    { tempC: tMid.valid ? tMid.tempC : (isFiniteNum(eq.akuMidC) ? eq.akuMidC : null) });
    patchInstVars("ts_aku_V",    { tempC: tBot.valid ? tBot.tempC : (isFiniteNum(eq.akuBottomC) ? eq.akuBottomC : null) });

    // floor in/out (fallback to flow/return roles)
    patchInstVars("ts_floor_in",  { tempC: flow.valid ? flow.tempC : null });
    patchInstVars("ts_floor_out", { tempC: ret.valid ? ret.tempC : null });

    // DHW block fill + recirc
    patchInstVars("dhw", {
      tempC: dhwT.valid ? dhwT.tempC : null,
      recircOn: !!rec.active,
      recircMoving: !!rec.active
    });

    // AKU heater visual
    patchInstVars("aku", { heaterOn: !!akuH.active });

    // --- valves gauges ---
    // V1 = virtuální (stav přepínacího ventilu kotle) -> odvozujeme z požadavku DHW
    patchInstVars("v1", {
      valvePos: dhwReq ? 100 : 0,
      valveMoving: false
    });
    // V2 = směšovací ventil ekviterm
    patchInstVars("v2", {
      valvePos: isFiniteNum(eq.valvePosPct) ? eq.valvePosPct : (isFiniteNum(valves.v2 && valves.v2.currentPct) ? valves.v2.currentPct : 0),
      valveMoving: !!eq.valveMoving || !!(valves.v2 && valves.v2.moving)
    });
    // V3 = přepínací ventil řízený ESP32 při DHW
    patchInstVars("v3", {
      valvePos: isFiniteNum(tuv.valvePosPct) ? tuv.valvePosPct : (isFiniteNum(valves.v3 && valves.v3.currentPct) ? valves.v3.currentPct : (dhwReq ? 100 : 0)),
      valveMoving: !!tuv.valveMoving || !!(valves.v3 && valves.v3.moving)
    });

    // boiler pump: rotate only when there is a heat/flow request
    patchInstVars("boiler", { pumpAngle: 0, pumpOn: !!anyFlow });
  }

  function applyVisualState(svg) {
    if (!svg) return;

    const cfg = lastCfg || {};
    const st = lastStatus || {};
    const eq = st.equitherm || {};
    const tuv = st.tuv || {};

    // Pokud config ještě není načtený, nic neschovávej.
    const haveCfg = !!lastCfg;
    const hideDisabled = !!(cfg.schematic && cfg.schematic.hideDisabled);

    const eqEnabled = haveCfg ? !!(cfg.equitherm && cfg.equitherm.enabled) : true;
    const tuvEnabled = haveCfg ? !!(cfg.tuv && cfg.tuv.enabled) : true;
    const akuSupportEnabled = haveCfg ? !!(cfg.equitherm && cfg.equitherm.akuSupportEnabled) : true;
    const akuHeaterEnabled = haveCfg ? !!(cfg.akuHeater && cfg.akuHeater.enabled) : true;

    const heatOn = !!(st.heatCall && st.heatCall.raw);
    const dhwReq = !!tuv.demandActive;
    const dhwActive = !!tuv.active || dhwReq;
    const floorActive = !!eq.active && !dhwActive;
    const anyFlow = heatOn || floorActive || dhwActive || !!eq.akuSupportActive || !!(st.akuHeater && st.akuHeater.active);
    const akuSupportActive = akuSupportEnabled && !!eq.akuSupportActive;
    const akuHeaterActive = !!(st.akuHeater && st.akuHeater.active);

    // ---- instance visibility / emphasis ----
    const setGroup = (id, enabled, active) => {
      const g = svg.querySelector(`#instances #${CSS.escape(id)}`);
      if (!g) return;
      const show = (!hideDisabled) || enabled || active;
      g.style.display = show ? "" : "none";
      if (show) {
        const op = active ? 1 : (enabled ? 0.55 : 0.25);
        g.style.opacity = String(op);
      }
    };

    // floor
    setGroup("floor", eqEnabled, floorActive);
    setGroup("ts_floor_in", eqEnabled, floorActive);
    setGroup("ts_floor_out", eqEnabled, floorActive);

    // DHW
    setGroup("dhw", tuvEnabled, dhwActive);
    setGroup("ts_dhw", tuvEnabled, dhwActive);

    // AKU (show if either aku support or aku heater is configured)
    const akuEnabled = (akuSupportEnabled || akuHeaterEnabled);
    const akuActive = akuSupportActive || akuHeaterActive;
    // defaultně AKU ukazuj vždy (jen ztlum, když není využitá)
    setGroup("aku", akuEnabled, akuActive);
    setGroup("ts_aku_T", akuEnabled, akuActive);
    setGroup("ts_aku_M", akuEnabled, akuActive);
    setGroup("ts_aku_V", akuEnabled, akuActive);

    // ---- pipes: active/moving dimming ----
    const active = new Set();
    const moving = new Set();
    const hidden = new Set();

    const K = (from, to) => `${from}->${to}`;

    // hide pipes only if user explicitly wants to hide disabled blocks
    if (hideDisabled) {
      if (!eqEnabled) {
        hidden.add(K("v1:A", "floor:IN"));
        hidden.add(K("floor:OUT", "j1:L"));
      }
      if (!tuvEnabled) {
        hidden.add(K("v1:B", "dhw:IN"));
        hidden.add(K("dhw:OUT", "j1:R"));
      }
      if (!(akuSupportEnabled || akuHeaterEnabled)) {
        hidden.add(K("j3:L", "aku:V"));
        hidden.add(K("aku:T", "v2:B"));
      }
    }

    if (anyFlow) {
      // boiler supply always when system is in any active flow
      active.add(K("boiler:T", "v1:AB"));
      moving.add(K("boiler:T", "v1:AB"));

      // common return trunk from branches to junction j2
      [
        K("j1:U", "j2:L"),
      ].forEach((k) => { active.add(k); moving.add(k); });

      if (floorActive) {
        // CH (ekviterm)
        [
          K("v1:A", "floor:IN"),
          K("floor:OUT", "j1:L"),
          K("j2:R", "j3:L"),
          K("j3:U", "v2:A"),
          K("v2:AB", "v3:B"),
          K("v3:AB", "boiler:V"),
        ].forEach((k) => { active.add(k); moving.add(k); });

        if (akuSupportActive) {
          [
            K("j3:L", "aku:V"),
            K("aku:T", "v2:B"),
          ].forEach((k) => { active.add(k); moving.add(k); });
        }
      }

      if (dhwActive) {
        // DHW
        [
          K("v1:B", "dhw:IN"),
          K("dhw:OUT", "j1:R"),
          K("j2:U", "v3:A"),
          K("v3:AB", "boiler:V"),
        ].forEach((k) => { active.add(k); moving.add(k); });
      }
    }

    const paths = svg.querySelectorAll("#pipes path");
    for (const p of paths) {
      if (!p || !(p instanceof SVGPathElement)) continue;
      const key = String(p.getAttribute("data-key") || "");
      const kind = String(p.getAttribute("data-kind") || "");
      const isWire = kind === "wire" || p.classList.contains("wire");

      // always keep sensor wires readable
      if (isWire) {
        p.classList.remove("inactive", "active", "moving");
        p.style.display = "";
        continue;
      }

      if (hidden.has(key)) {
        p.style.display = "none";
        continue;
      }
      p.style.display = "";

      const on = active.has(key);
      p.classList.toggle("active", on);
      p.classList.toggle("inactive", !on);
      p.classList.toggle("moving", moving.has(key));
    }
  }

  function doRender() {
    renderQueued = false;

    const svg = ensureSvgHost();
    if (!svg) return;
    if (!window.SchemaMain || typeof window.SchemaMain.render !== "function") return;

    updateFromApis();

    try {
      window.SchemaMain.render();
    } catch (e) {
      console.error("[schematic] render failed:", e);
    }

    // dynamic state (visibility + pipe motion)
    applyVisualState(svg);

    const card = $id("cardSchema");
    if (card) card.style.display = "";
  }

  function scheduleRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(doRender);
  }

  // Hook into App callbacks without breaking other modules
  window.App = window.App || {};
  const prevCfg = window.App.onConfigLoaded;
  window.App.onConfigLoaded = (cfg) => {
    try { if (typeof prevCfg === "function") prevCfg(cfg); } catch (_) {}
    lastCfg = cfg || null;
    scheduleRender();
  };

  const prevSt = window.App.onStatusLoaded;
  window.App.onStatusLoaded = (st) => {
    try { if (typeof prevSt === "function") prevSt(st); } catch (_) {}
    lastStatus = st || null;
    scheduleRender();
  };

  const prevDash = window.App.onDashLoaded;
  window.App.onDashLoaded = (dash) => {
    try { if (typeof prevDash === "function") prevDash(dash); } catch (_) {}
    lastDash = dash || null;
    scheduleRender();
  };

  const init = () => {
    if (initialized) return;
    initialized = true;
  };

  const mount = () => {
    init();
    lastCfg = window.App.getConfig?.() || lastCfg;
    lastStatus = window.App.getStatus?.() || lastStatus;
    lastDash = window.App.getDash?.() || lastDash;
    ensureSvgHost();
    scheduleRender();
  };

  window.Pages = window.Pages || {};
  window.Pages.schema = { id: "schema", mount, unmount() {} };
})();
