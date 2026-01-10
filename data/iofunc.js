/* I/O function mapping UI module (v3) */
(() => {
  const App = window.App;
  if (!App) return;

  const $ = App.$;
  const $$ = App.$$;
  const toast = App.toast;
  const INPUT_COUNT = 8;
  const RELAY_COUNT = 8;

  const escapeHtml = (s) =>
    String(s ?? "").replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const escapeAttr = (s) => escapeHtml(s);

  const INPUT_ROLES = [
    { v: "none", t: "—" },
    { v: "thermostat", t: "Termostat (kontakt)" },
    { v: "heat_call", t: "Požadavek topení (heat call)" },
    { v: "mode_trigger", t: "Přepínač režimu (MODE)" },
    { v: "dhw_enable", t: "Aktivace ohřevu TUV" },
    { v: "night_mode", t: "Aktivace nočního útlumu" },
    { v: "recirc_demand", t: "Požadavek cirkulace TUV" },
    { v: "generic", t: "Obecný digitální vstup" },
  ];

  const OUTPUT_ROLES = [
    { v: "none", t: "—" },
    { v: "valve_onoff", t: "Ventil ON/OFF (230V)" },
    // 3c ventily: rozlišujeme směšovací (Ekviterm, ovládá 2 relé) a přepínací (TUV, ovládá 1 relé)
    { v: "valve_3way_mix", t: "Směšovací trojcestný ventil (Ekviterm) – 2-bod (230V, 2 relé A/B)" },
    { v: "valve_3way_tuv", t: "Přepínací trojcestný ventil (TUV) – 2-bod (230V, 1 relé A/B)" },
    { v: "valve_3way_dhw", t: "Přepínací trojcestný ventil (TUV) – legacy alias (nepoužívat)" },
    { v: "valve_3way_peer", t: "Trojcestný ventil – peer relé (nepřiřazovat ručně)" },
    { v: "boiler_enable_dhw", t: "Kotel - signál TUV" },
    { v: "boiler_enable_nm", t: "Kotel - signál NÚ" },
    { v: "heater_aku", t: "Topná spirála AKU (stykač)" },
    { v: "dhw_recirc_pump", t: "Cirkulační čerpadlo TUV" },
    { v: "circ_pump", t: "Cirkulační čerpadlo" },
    { v: "generic", t: "Obecný výstup" }
  ];
  
  const ensureShape = (cfg) => {
    cfg.iofunc = (cfg.iofunc && typeof cfg.iofunc === "object") ? cfg.iofunc : {};

    cfg.iofunc.inputs = Array.isArray(cfg.iofunc.inputs) ? cfg.iofunc.inputs : [];
    cfg.iofunc.inputs = cfg.iofunc.inputs.map(i => ({
      role: String(i?.role || "none"),
      params: (i?.params && typeof i.params === "object") ? i.params : {},
    }));
    while (cfg.iofunc.inputs.length < INPUT_COUNT) cfg.iofunc.inputs.push({ role: "none", params: {} });
    cfg.iofunc.inputs = cfg.iofunc.inputs.slice(0, INPUT_COUNT);

    cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : [];
    cfg.iofunc.outputs = cfg.iofunc.outputs.map(o => ({
      role: String(o?.role || "none"),
      params: (o?.params && typeof o.params === "object") ? o.params : {},
    }));
    while (cfg.iofunc.outputs.length < RELAY_COUNT) cfg.iofunc.outputs.push({ role: "none", params: {} });
    cfg.iofunc.outputs = cfg.iofunc.outputs.slice(0, RELAY_COUNT);
  };

  // Trojcestný ventil 2‑bod: master=libovolné relé, peer=vybrané relé (1‑based). Parametry se sdílí s "Kalibrace ventilů".
  const reconcile3WayPairs = (cfg) => {
    const outs = cfg.iofunc.outputs;

    // legacy migrate
    for (let i = 0; i < outs.length; i++) {
      if (outs[i].role === "valve_3way_spring") outs[i].role = "valve_3way_2rel";
      // starší konfigurace: původní role se mapuje na směšovací ventil (Ekviterm)
      if (outs[i].role === "valve_3way_2rel") outs[i].role = "valve_3way_mix";
    }

    // reset existing peers (budou znovu nastaveny podle masterů)
    for (let i = 0; i < outs.length; i++) {
      if (outs[i].role === "valve_3way_peer") {
        outs[i].role = "none";
        outs[i].params = {};
      }
    }

    // seznam master relé, abychom je nepoužili jako peer
    const isMaster = new Set();
    for (let i = 0; i < outs.length; i++) {
       const r = String(outs[i]?.role || "none");
       if (r === "valve_3way_mix" || r === "valve_3way_2rel") isMaster.add(i);
     }

    const usedPeers = new Set();

    const pickFallbackPeer = (masterIdx) => {
      for (let j = 0; j < outs.length; j++) {
        if (j === masterIdx) continue;
        if (isMaster.has(j)) continue;
        if (usedPeers.has(j)) continue;
        return j;
      }
      return -1;
    };

    for (let i = 0; i < outs.length; i++) {
      const o = outs[i];
      const r = String(o?.role || "none");
      //if (o.role !== "valve_3way_2rel") continue;
      if (r !== "valve_3way_mix" && r !== "valve_3way_2rel") continue;

      o.params = (o.params && typeof o.params === "object") ? o.params : {};

      // prefer peerRel, fallback to legacy partnerRelay, otherwise default = next relay
      let peer1 = Number(o.params.peerRel ?? o.params.partnerRelay ?? (i + 2));
      if (!Number.isFinite(peer1)) peer1 = i + 2;
      peer1 = Math.round(peer1);

      let peer0 = peer1 - 1;
      const invalid = (peer0 < 0 || peer0 >= outs.length || peer0 === i || isMaster.has(peer0) || usedPeers.has(peer0));
      if (invalid) {
        peer0 = pickFallbackPeer(i);
        if (peer0 < 0) {
          // není k dispozici žádné peer relé -> ventil nelze nakonfigurovat
          o.role = "none";
          o.params = {};
          toast?.("Nelze nastavit 3c ventil – není volné peer relé.");
          continue;
        }
        peer1 = peer0 + 1;
      }

      // Persist for both tabs + backward compatibility
      o.params.peerRel = peer1;
      o.params.partnerRelay = peer1;

      usedPeers.add(peer0);
      outs[peer0].role = "valve_3way_peer";
      outs[peer0].params = { master: i + 1 };
    }
  };

  const inputName = (cfg, i) => {
    const n = String(cfg?.inputNames?.[i] || "").trim();
    return n ? n : `Vstup ${i + 1}`;
  };
  const relayName = (cfg, i) => {
    const n = String(cfg?.relayNames?.[i] || "").trim();
    return n ? n : `Relé ${i + 1}`;
  };

  const roleOptions = (roles, cur, disabledValueSet = new Set()) => {
    return roles.map(r => {
      const dis = disabledValueSet.has(r.v) ? "disabled" : "";
      const sel = (r.v === cur) ? "selected" : "";
      return `<option value="${escapeHtml(r.v)}" ${sel} ${dis}>${escapeHtml(r.t)}</option>`;
    }).join("");
  };

  const renderInputParams = (role, params, idx) => {
    params = (params && typeof params === "object") ? params : {};
    if (role === "temp_dallas") {
      const gpio = Number(params.gpio || 0);
      const addr = String(params.addr || "");
      return `
        <div class="inline" style="flex-wrap:wrap;gap:10px">
          <div class="field" style="min-width:170px">
            <label>OneWire GPIO</label>
            <input class="pfield" data-k="gpio" data-typ="n" type="number" placeholder="např. 4" value="${gpio || ""}">
          </div>
          <div class="field" style="min-width:260px">
            <label>Adresa senzoru (volit.)</label>
            <input class="pfield" data-k="addr" data-typ="s" placeholder="např. 28-FF..." value="${escapeHtml(addr)}">
          </div>
          <div class="muted" style="align-self:flex-end">Pokud adresa není vyplněná, použije se první nalezený senzor na sběrnici.</div>
        </div>
      `;
    }

    if (role === "mode_trigger") {
      const m = String(params.mode || "MODE1");
      const opts = ["MODE1","MODE2","MODE3","MODE4","MODE5"].map(x => `<option value="${x}" ${x===m?"selected":""}>${x}</option>`).join("");
      return `
        <div class="inline" style="flex-wrap:wrap;gap:10px">
          <div class="field" style="min-width:220px">
            <label>Přepíná režim</label>
            <select class="pfield" data-k="mode" data-typ="s">${opts}</select>
          </div>
          <div class="muted" style="align-self:flex-end">Vstup aktivní → aktivuje zvolený režim.</div>
        </div>
      `;
    }

    if (role === "thermostat" || role === "heat_call") {
      return `<div class="muted">Kontakt termostatu (logika HIGH/LOW se nastavuje v „Vstupy & relé“).</div>`;
    }

    if (role === "dhw_enable") {
      return `<div class="muted">Aktivní vstup zapne „Ohřev TUV → Aktivace ohřevu TUV“.</div>`;
    }

    if (role === "night_mode") {
      return `<div class="muted">Aktivní vstup zapne „Ekviterm → Aktivace nočního útlumu“.</div>`;
    }

    return `<div class="muted">—</div>`;
  };

  const renderOutputParams = (role, params, idx, cfg) => {
    params = (params && typeof params === "object") ? params : {};
    if (role === "valve_3way_peer") {
      const master = Number(params.master || 0);
      return `<div class="muted">Pevně nastaveno jako <b>směr B</b> pro ventil na DO${master}.</div>`;
    }

    if (role === "valve_3way_mix" || role === "valve_3way_2rel") {
      const travel = Number(params.travelTime ?? 6);
      const pulse = Number(params.pulseTime ?? 0.8);
      const guard = Number(params.guardTime ?? 0.3);
      const minSwitchS = Number(params.minSwitchS ?? 30);
      const defPos = String(params.defaultPos || "A");
      const inv = !!params.invertDir;
      const outs = Array.isArray(cfg?.iofunc?.outputs) ? cfg.iofunc.outputs : [];
      const usedMasters = new Set();
      const usedPeers = new Set();
      for (let j = 0; j < outs.length; j++) {
        if (j === idx) continue;
        const r = String(outs[j]?.role || "none");
        if (r !== "valve_3way_mix" && r !== "valve_3way_2rel") continue;
        usedMasters.add(j + 1);
        const p = Math.round(Number(outs[j]?.params?.peerRel ?? outs[j]?.params?.partnerRelay ?? (j + 2)));
        if (p >= 1 && p <= RELAY_COUNT) usedPeers.add(p);
      }

      let peerCur = Math.round(Number(params.peerRel ?? params.partnerRelay ?? (idx + 2)));
      if (!Number.isFinite(peerCur) || peerCur < 1 || peerCur > RELAY_COUNT || peerCur === (idx + 1)) peerCur = Math.min(RELAY_COUNT, idx + 2);
      const peerOpts = [];
      for (let r = 1; r <= RELAY_COUNT; r++) {
        if (r === (idx + 1)) continue;
        const dis = usedMasters.has(r) || usedPeers.has(r);
        peerOpts.push(`<option value="${r}" ${r===peerCur?"selected":""} ${dis?"disabled":""}>DO${r}${dis?" (obsazeno)":""}</option>`);
      }

      return `
        <div class="inline" style="flex-wrap:wrap;gap:10px">
          <div class="muted" style="min-width:260px;align-self:flex-end">
            Použije DO${idx+1} (směr A) + zvolené <b>peer relé</b> (směr B). Peer relé se automaticky nastaví jako „3c ventil (PEER)“.
          </div>
          <div class="field" style="min-width:170px">
            <label>Peer relé (směr B)</label>
            <select class="pfield" data-k="peerRel" data-typ="n">${peerOpts.join("")}</select>
          </div>
          <div class="field" style="min-width:150px"><label>Přeběh (s)</label><input class="pfield" data-k="travelTime" data-typ="n" type="number" step="0.1" value="${travel}"></div>
          <div class="field" style="min-width:150px"><label>Pulse (s)</label><input class="pfield" data-k="pulseTime" data-typ="n" type="number" step="0.1" value="${pulse}"></div>
          <div class="field" style="min-width:160px"><label>Pauza (s)</label><input class="pfield" data-k="guardTime" data-typ="n" type="number" step="0.1" value="${guard}"></div>
          <div class="field" style="min-width:190px"><label>Min. perioda přestavení (s)</label><input class="pfield" data-k="minSwitchS" data-typ="n" type="number" step="1" min="0" value="${minSwitchS}"></div>
          <div class="field" style="min-width:170px">
            <label>Výchozí poloha</label>
            <select class="pfield" data-k="defaultPos" data-typ="s">
              <option value="A" ${defPos==="A"?"selected":""}>A</option>
              <option value="B" ${defPos==="B"?"selected":""}>B</option>
            </select>
          </div>
          <label class="chk" style="align-self:flex-end">
            <input class="pfield" data-k="invertDir" data-typ="b" type="checkbox" ${inv?"checked":""}>
            <span>Prohodit směr A/B</span>
          </label>
        </div>
      `;
    }

    if (role === "valve_3way_tuv" || role === "valve_3way_dhw") {
      const travel = Number(params.travelTime ?? 6);
      const pulse = Number(params.pulseTime ?? 0.8);
      const guard = Number(params.guardTime ?? 0.3);
      const minSwitchS = Number(params.minSwitchS ?? 30);
      const defPos = String(params.defaultPos || "A");
      const inv = !!params.invertDir;
      return `
        <div class="inline" style="flex-wrap:wrap;gap:10px">
          <div class="muted" style="min-width:240px;align-self:flex-end">
            Použije DO${idx+1} jako přepínací relé (0/100%).
          </div>
          <div class="field" style="min-width:150px"><label>Přeběh (s)</label><input class="pfield" data-k="travelTime" data-typ="n" type="number" step="0.1" value="${travel}"></div>
          <div class="field" style="min-width:150px"><label>Pulse (s)</label><input class="pfield" data-k="pulseTime" data-typ="n" type="number" step="0.1" value="${pulse}"></div>
          <div class="field" style="min-width:160px"><label>Pauza (s)</label><input class="pfield" data-k="guardTime" data-typ="n" type="number" step="0.1" value="${guard}"></div>
          <div class="field" style="min-width:190px"><label>Min. perioda přestavení (s)</label><input class="pfield" data-k="minSwitchS" data-typ="n" type="number" step="1" min="0" value="${minSwitchS}"></div>
          <div class="field" style="min-width:170px">
            <label>Výchozí poloha</label>
            <select class="pfield" data-k="defaultPos" data-typ="s">
              <option value="A" ${defPos==="A"?"selected":""}>A</option>
              <option value="B" ${defPos==="B"?"selected":""}>B</option>
            </select>
          </div>
          <label class="chk" style="align-self:flex-end">
            <input class="pfield" data-k="invertDir" data-typ="b" type="checkbox" ${inv?"checked":""}>
            <span>Prohodit směr A/B</span>
          </label>
        </div>
      `;
    }

    if (role === "heater_stage") {
      const phase = String(params.phase || "");
      return `
        <div class="inline" style="flex-wrap:wrap;gap:10px">
          <div class="field" style="min-width:220px">
            <label>Popis fáze</label>
            <input class="pfield" data-k="phase" data-typ="s" placeholder="např. L1" value="${escapeHtml(phase)}">
          </div>
        </div>
      `;
    }

    return `<div class="muted">—</div>`;
  };

  const filterState = {
    onlyAssigned: false,
    onlyInputs: false,
    onlyOutputs: false,
    query: "",
  };

  const applyFilters = () => {
    const q = filterState.query.trim().toLowerCase();
    const showInputs = filterState.onlyInputs && !filterState.onlyOutputs;
    const showOutputs = filterState.onlyOutputs && !filterState.onlyInputs;
    ["#tblInputFuncs", "#tblOutputFuncs"].forEach((sel) => {
      $$(sel + " .row2").forEach((row) => {
        if (row.classList.contains("head")) return;
        const type = row.dataset.type || "";
        if (showInputs && type !== "in") {
          row.style.display = "none";
          return;
        }
        if (showOutputs && type !== "out") {
          row.style.display = "none";
          return;
        }
        const role = (row.dataset.role || "").toLowerCase();
        const name = (row.dataset.name || "").toLowerCase();
        if (filterState.onlyAssigned && (!role || role === "none")) {
          row.style.display = "none";
          return;
        }
        if (q && !role.includes(q) && !name.includes(q)) {
          row.style.display = "none";
          return;
        }
        row.style.display = "";
      });
    });
  };

  const renderTables = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return;
    ensureShape(cfg);
    reconcile3WayPairs(cfg);

    const inTbl = $("#tblInputFuncs");
    const outTbl = $("#tblOutputFuncs");
    if (!inTbl || !outTbl) return;

    // Inputs
    const inRows = [];
    inRows.push(`<div class="row2 head"><div>Vstup</div><div>Roli / šablona</div><div>Parametry</div></div>`);
    for (let i = 0; i < INPUT_COUNT; i++) {
      const it = cfg.iofunc.inputs[i];
      const role = String(it.role || "none");
      const params = it.params || {};
      inRows.push(`
        <div class="row2" data-type="in" data-idx="${i}" data-role="${escapeAttr(role)}" data-name="${escapeAttr(inputName(cfg, i))}">
          <div><b>${escapeHtml(inputName(cfg, i))}</b><div class="muted">DI${i+1}</div></div>
          <div>
            <select class="roleSel">${roleOptions(INPUT_ROLES, role)}</select>
          </div>
          <div class="params">${renderInputParams(role, params, i)}</div>
        </div>
      `);
    }
    inTbl.innerHTML = inRows.join("");

    // Outputs
    const outRows = [];
    outRows.push(`<div class="row2 head"><div>Výstup</div><div>Roli / šablona</div><div>Parametry</div></div>`);
    for (let i = 0; i < RELAY_COUNT; i++) {
      const ot = cfg.iofunc.outputs[i];
      const role = String(ot.role || "none");
      const params = ot.params || {};

      const isPeer = role === "valve_3way_peer";
      const disabledSet = new Set();
      if (isPeer) {
        // lock to peer role
        OUTPUT_ROLES.forEach(r => { if (r.v !== "valve_3way_peer") disabledSet.add(r.v); });
      }

      outRows.push(`
        <div class="row2" data-type="out" data-idx="${i}" data-role="${escapeAttr(role)}" data-name="${escapeAttr(relayName(cfg, i))}">
          <div><b>${escapeHtml(relayName(cfg, i))}</b><div class="muted">DO${i+1}</div></div>
          <div>
            <select class="roleSel" ${isPeer ? "disabled" : ""}>${roleOptions(OUTPUT_ROLES, role, disabledSet)}</select>
          </div>
          <div class="params">${renderOutputParams(role, params, i, cfg)}</div>
        </div>
      `);
    }
    outTbl.innerHTML = outRows.join("");
    applyFilters();

  };

  // Alias for older handlers (prevents "render is not defined")
  const render = () => renderTables();

  const parseValue = (el) => {
    const typ = el.getAttribute("data-typ") || "s";
    if (typ === "n") {
      const n = Number(el.value);
      return Number.isFinite(n) ? n : 0;
    }
    if (typ === "b") return !!el.checked;
    return String(el.value || "");
  };

  const bind = () => {
    if (!$("#cfg-iofun")) return;

    // role change
    const onRoleChange = (ev) => {
      const sel = ev.target.closest(".roleSel");
      if (!sel) return;
      const row = ev.target.closest(".row2");
      if (!row) return;

      const typ = row.getAttribute("data-type");
      const idx = Number(row.getAttribute("data-idx"));
      if (!Number.isFinite(idx)) return;

      const cfg = App.getConfig?.();
      if (!cfg) return;
      ensureShape(cfg);

      if (typ === "in") {
        cfg.iofunc.inputs[idx].role = sel.value;
        cfg.iofunc.inputs[idx].params = cfg.iofunc.inputs[idx].params || {};
      } else {
        // outputs
        const curRole = String(cfg.iofunc.outputs[idx].role || "none");
        if (curRole === "valve_3way_peer") {
          // locked
          sel.value = "valve_3way_peer";
          return;
        }
        cfg.iofunc.outputs[idx].role = sel.value;
        cfg.iofunc.outputs[idx].params = cfg.iofunc.outputs[idx].params || {};
      }

      reconcile3WayPairs(cfg);
      renderTables();
    };

    $("#tblInputFuncs")?.addEventListener("change", onRoleChange);
    $("#tblOutputFuncs")?.addEventListener("change", onRoleChange);

    // param changes
    const onParamInput = (ev) => {
      const field = ev.target.closest(".pfield");
      if (!field) return;
      const row = ev.target.closest(".row2");
      if (!row) return;

      const k = field.getAttribute("data-k");
      if (!k) return;

      const typ = row.getAttribute("data-type");
      const idx = Number(row.getAttribute("data-idx"));
      if (!Number.isFinite(idx)) return;

      const cfg = App.getConfig?.();
      if (!cfg) return;
      ensureShape(cfg);

      const container = (typ === "in") ? cfg.iofunc.inputs[idx] : cfg.iofunc.outputs[idx];
      container.params = (container.params && typeof container.params === "object") ? container.params : {};

      container.params[k] = parseValue(field);

      // Změna peer relé -> přepárování (nastaví/aktualizuje roli PEER na zvoleném relé)
      if (typ === "out" && (container.role === "valve_3way_mix" || container.role === "valve_3way_2rel") && (k === "peerRel" || k === "partnerRelay")) {
        const cfg = App.getConfig?.();
        if (cfg) {
          ensureShape(cfg);
          reconcile3WayPairs(cfg);
          renderTables();
        }
      }
    };

    $("#tblInputFuncs")?.addEventListener("input", onParamInput);
    $("#tblOutputFuncs")?.addEventListener("input", onParamInput);

    // Save buttons
    $("#btnSaveInputFuncs")?.addEventListener("click", async () => {
      try {
        const cfg = App.getConfig?.();
        if (!cfg) return;
        ensureShape(cfg);
        reconcile3WayPairs(cfg);
        await App.saveConfig?.();
        toast?.("Uloženo (Funkce vstupů).");
      } catch (e) {
        toast?.("Chyba při ukládání.");
      }
    });

    $("#btnSaveOutputFuncs")?.addEventListener("click", async () => {
      try {
        const cfg = App.getConfig?.();
        if (!cfg) return;
        ensureShape(cfg);
        reconcile3WayPairs(cfg);
        await App.saveConfig?.();
        toast?.("Uloženo (Funkce výstupů).");
      } catch (e) {
        toast?.("Chyba při ukládání.");
      }
    });

    const updateFilters = () => {
      filterState.onlyAssigned = $("#ioOnlyAssigned")?.checked ?? false;
      filterState.onlyInputs = $("#ioOnlyInputs")?.checked ?? false;
      filterState.onlyOutputs = $("#ioOnlyOutputs")?.checked ?? false;
      filterState.query = $("#ioSearch")?.value || "";
      applyFilters();
    };
    $("#ioOnlyAssigned")?.addEventListener("change", updateFilters);
    $("#ioOnlyInputs")?.addEventListener("change", updateFilters);
    $("#ioOnlyOutputs")?.addEventListener("change", updateFilters);
    $("#ioSearch")?.addEventListener("input", updateFilters);
    updateFilters();
  };

  // chain App hook (allow multiple modules)
  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    try { prev && prev(cfg); } catch (e) {}
    renderTables();
  };

  window.addEventListener("DOMContentLoaded", () => {
    bind();
    if (App.getConfig?.()) renderTables();
  });

  // Při přepnutí záložky Konfigurace chceme vždy renderovat z aktuálního cfg,
  // aby se parametry 3c ventilů synchronizovaly i s "Kalibrace ventilů".
  window.addEventListener("app:cfgTabChanged", (ev) => {
    const tab = ev?.detail?.tab;
    if (tab === "iofunc") renderTables();
  });

  // expose for debugging
  window.IOFunc = { render, renderTables };
})();
