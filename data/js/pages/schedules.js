/* Schedules + TUV mapping module (v2) */
(() => {
  let initialized = false;
  let intervalId = null;
  const init = () => {
    if (initialized) return;

  const App = window.App;
  if (!App) return;

  const $ = App.$;
  const $$ = App.$$;
  const toast = App.toast;
  const apiPostJson = App.apiPostJson;
  const apiGetJson = App.apiGetJson;
  const schedHint = $("#schedInputHint");

  const MODE_IDS = ["MODE1","MODE2","MODE3","MODE4","MODE5"];
  const BOOL_KINDS = new Set(["dhw_enable","night_mode"]);

  const pad2 = (n) => String(n).padStart(2, "0");
  const addMinutes = (hhmm, mins) => {
    const [h, m] = String(hhmm || "00:00").split(":").map(x => Number(x));
    const base = (Number.isFinite(h) ? h : 0) * 60 + (Number.isFinite(m) ? m : 0);
    let t = (base + mins) % (24 * 60);
    if (t < 0) t += 24 * 60;
    return `${pad2(Math.floor(t / 60))}:${pad2(t % 60)}`;
  };

  const makeId = () => (Date.now().toString(36) + Math.random().toString(36).slice(2, 6));

  const ensure = (cfg) => {
    cfg.schedules = Array.isArray(cfg.schedules) ? cfg.schedules : [];
    cfg.schedules = cfg.schedules.map(s => ({
      id: String(s?.id || makeId()),
      enabled: s?.enabled !== false,
      name: String(s?.name || "Plán"),
      days: Array.isArray(s?.days) ? s.days.map(n => Number(n)).filter(n => n >= 1 && n <= 7) : [1,2,3,4,5,6,7],
      at: String(s?.at || s?.time || "06:00"),
      kind: String(s?.kind || s?.type || "set_mode"),
      value: (s && typeof s.value === "object") ? s.value : ((s && typeof s.params === "object") ? s.params : {}),
    }));
  };

  const findInputRoleIndex = (cfg, role) => {
    const inputs = Array.isArray(cfg?.iofunc?.inputs) ? cfg.iofunc.inputs : [];
    for (let i = 0; i < inputs.length; i++) {
      if (String(inputs[i]?.role || "") === role) return i;
    }
    return -1;
  };

  const updateInputHints = (cfg) => {
    if (!schedHint) return;
    const tuvIdx = findInputRoleIndex(cfg, "dhw_enable");
    const nightIdx = findInputRoleIndex(cfg, "night_mode");
    const parts = [];
    if (tuvIdx >= 0) parts.push(`Ohřev TUV řídí vstup ${tuvIdx + 1} (Funkce I/O) – plánování TUV se ignoruje.`);
    if (nightIdx >= 0) {
      parts.push(`Noční útlum řídí vstup ${nightIdx + 1} (Funkce I/O) – plánování nočního útlumu se ignoruje.`);
    } else if (String(cfg?.system?.nightModeSource || "") === "heat_call") {
      parts.push("Noční útlum je řízen Heat call – plánování nočního útlumu se ignoruje.");
    }
    schedHint.textContent = parts.join(" ");
  };

  const ensureTuv = (cfg) => {
    cfg.tuv = (cfg.tuv && typeof cfg.tuv === "object") ? cfg.tuv : {};
    cfg.tuv.demandInput = Number.isFinite(Number(cfg.tuv.demandInput)) ? Number(cfg.tuv.demandInput) : 0;
    cfg.tuv.requestRelay = Number.isFinite(Number(cfg.tuv.requestRelay)) ? Number(cfg.tuv.requestRelay) : 0;
    const legacyRelay = Number.isFinite(Number(cfg.tuv.relay)) ? Number(cfg.tuv.relay) : 0;
    if (!cfg.tuv.requestRelay && legacyRelay) cfg.tuv.requestRelay = legacyRelay;
    cfg.tuv.relay = cfg.tuv.requestRelay;
    cfg.tuv.enabled = (typeof cfg.tuv.enabled !== "undefined") ? !!cfg.tuv.enabled : false;
  };

  const optLabel = (arr, idx, fallback) => {
    const n = String(arr?.[idx] || "").trim();
    return n ? n : fallback;
  };

  const renderTuvConfig = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return;
    ensureTuv(cfg);

    const selIn = $("#tuvDemandInput");
    const selOut = $("#tuvRequestRelay");
    const chk = $("#tuvManualEnable");
    const st = $("#tuvCfgStatus");
    if (!selIn || !selOut || !chk) return;

    const inNames = Array.isArray(cfg.inputNames) ? cfg.inputNames : [];
    const outNames = Array.isArray(cfg.relayNames) ? cfg.relayNames : [];

    selIn.innerHTML = `<option value="0">— (nepoužít)</option>` + Array.from({ length: 8 }, (_, i) =>
      `<option value="${i + 1}">DI${i + 1} – ${optLabel(inNames, i, "Vstup " + (i + 1))}</option>`
    ).join("");

    selOut.innerHTML = `<option value="0">— (nepoužít)</option>` + Array.from({ length: 8 }, (_, i) =>
      `<option value="${i + 1}">DO${i + 1} – ${optLabel(outNames, i, "Relé " + (i + 1))}</option>`
    ).join("");

    selIn.value = String(cfg.tuv.demandInput || 0);
    selOut.value = String(cfg.tuv.requestRelay || 0);
    chk.checked = !!cfg.tuv.enabled;

    if (st) {
      st.textContent =
        (cfg.tuv.requestRelay ? `Požadavek TUV bude posílán na DO${cfg.tuv.requestRelay}` : "Nenastavené relé pro TUV request.")
        + (cfg.tuv.demandInput ? ` · Vstup DI${cfg.tuv.demandInput} je demand.` : " · Demand vstup není nastaven.");
    }
  };

  const collectTuvConfig = (cfg) => {
    ensureTuv(cfg);
    const selIn = $("#tuvDemandInput");
    const selOut = $("#tuvRequestRelay");
    const chk = $("#tuvManualEnable");
    if (selIn) cfg.tuv.demandInput = Number(selIn.value || 0);
    if (selOut) cfg.tuv.requestRelay = Number(selOut.value || 0);
    cfg.tuv.relay = cfg.tuv.requestRelay;
    if (chk) cfg.tuv.enabled = !!chk.checked;
  };

  // ----- UI grouping (intervals) -----
  // interval is represented by 2 schedule items in config:
  //  - start: value={enable:true, pair:<id>, role:"start"} at=FROM
  //  - end  : value={enable:false,pair:<id>, role:"end"}   at=TO
  let uiItems = []; // [{type:"single", item}, {type:"interval", ...}]

  const buildUiItems = (cfg) => {
    ensure(cfg);
    ensureTuv(cfg);

    const pairMap = new Map();
    const normals = [];

    for (const s of cfg.schedules) {
      const pair = s?.value?.pair;
      const role = String(s?.value?.role || "");
      const kind = String(s?.kind || "");
      if (pair && (role === "start" || role === "end") && BOOL_KINDS.has(kind)) {
        const g = pairMap.get(pair) || {};
        g[role] = s;
        pairMap.set(pair, g);
      } else {
        normals.push({ type: "single", item: s });
      }
    }

    const grouped = [];
    for (const [pair, g] of pairMap.entries()) {
      const a = g.start, b = g.end;
      if (!a || !b) {
        if (a) normals.push({ type: "single", item: a });
        if (b) normals.push({ type: "single", item: b });
        continue;
      }
      grouped.push({
        type: "interval",
        pair,
        startId: a.id,
        endId: b.id,
        enabled: (a.enabled !== false) && (b.enabled !== false),
        name: a.name || "Interval",
        days: Array.isArray(a.days) ? a.days : [1,2,3,4,5,6,7],
        kind: String(a.kind),
        from: String(a.at || "06:00"),
        to: String(b.at || "07:00"),
      });
    }

    // stable order: intervals first, then singles (as in cfg)
    return [...grouped, ...normals];
  };

  const writeBackUiItems = (cfg, items) => {
    const out = [];
    for (const u of items) {
      if (u.type === "single") { out.push(u.item); continue; }

      const pair = u.pair || makeId();
      const days = (Array.isArray(u.days) && u.days.length) ? u.days : [1,2,3,4,5,6,7];
      const startId = u.startId || (pair + "-start");
      const endId = u.endId || (pair + "-end");

      out.push({
        id: startId,
        enabled: u.enabled !== false,
        name: u.name || "Interval",
        days,
        at: u.from || "06:00",
        kind: u.kind,
        value: { enable: true, pair, role: "start" }
      });
      out.push({
        id: endId,
        enabled: u.enabled !== false,
        name: u.name || "Interval",
        days,
        at: u.to || "07:00",
        kind: u.kind,
        value: { enable: false, pair, role: "end" }
      });
    }
    cfg.schedules = out;
  };

  // ----- Days modal -----
  let daysModalCtx = null; // { getDays, setDays, onApply }
  const openDaysModal = (ctx) => {
    const m = $("#modalDays");
    if (!m) return;
    daysModalCtx = ctx;

    const set = new Set(ctx.getDays() || []);
    m.querySelectorAll('input[type="checkbox"][data-day]').forEach(ch => {
      const d = Number(ch.getAttribute("data-day"));
      ch.checked = set.has(d);
    });

    m.classList.remove("hidden");
    m.setAttribute("aria-hidden", "false");
  };

  const closeDaysModal = () => {
    const m = $("#modalDays");
    if (!m) return;
    m.classList.add("hidden");
    m.setAttribute("aria-hidden", "true");
    daysModalCtx = null;
  };

  const readDaysModal = () => {
    const m = $("#modalDays");
    if (!m) return [1,2,3,4,5,6,7];
    const days = [];
    m.querySelectorAll('input[type="checkbox"][data-day]').forEach(ch => {
      if (ch.checked) days.push(Number(ch.getAttribute("data-day")));
    });
    days.sort((a, b) => a - b);
    return days.length ? Array.from(new Set(days)) : [1,2,3,4,5,6,7];
  };

  const wireDaysModal = () => {
    const m = $("#modalDays");
    if (!m) return;

    m.querySelector("[data-close]")?.addEventListener("click", closeDaysModal);
    $("#daysClose")?.addEventListener("click", closeDaysModal);
    $("#daysCancel")?.addEventListener("click", closeDaysModal);

    $("#daysAll")?.addEventListener("click", () => m.querySelectorAll('input[data-day]').forEach(ch => ch.checked = true));
    $("#daysWeek")?.addEventListener("click", () => {
      m.querySelectorAll('input[data-day]').forEach(ch => {
        const d = Number(ch.getAttribute("data-day"));
        ch.checked = d >= 1 && d <= 5;
      });
    });
    $("#daysWeekend")?.addEventListener("click", () => {
      m.querySelectorAll('input[data-day]').forEach(ch => {
        const d = Number(ch.getAttribute("data-day"));
        ch.checked = (d === 6 || d === 7);
      });
    });
    $("#daysNone")?.addEventListener("click", () => m.querySelectorAll('input[data-day]').forEach(ch => ch.checked = false));

    $("#daysOk")?.addEventListener("click", () => {
      if (!daysModalCtx) { closeDaysModal(); return; }
      const days = readDaysModal();
      daysModalCtx.setDays(days);
      try { daysModalCtx.onApply && daysModalCtx.onApply(); } catch (_) {}
      closeDaysModal();
    });
  };

  const dayLabel = (d) => ["Po","Út","St","Čt","Pá","So","Ne"][d - 1] || "?";
  const daysStr = (days) => (days && days.length === 7) ? "Každý den" : (days || []).slice().sort((a,b)=>a-b).map(dayLabel).join(",");

  const kindOptions = [
    ["set_mode","Přepnout režim (MODE)"],
    ["set_control_mode","Řízení MANUAL/AUTO"],
    ["dhw_enable","Ohřev TUV ON/OFF"],
    ["night_mode","Noční útlum ON/OFF"],
  ];

  const refreshTuvStatus = async () => {
    const st = $("#tuvCfgStatus");
    if (!st) return;
    try {
      const s = await apiGetJson("/api/status");
      const t = s?.tuv;
      if (!t) return;
      const parts = [];
      if (typeof t.modeActive !== "undefined") parts.push(`Aktivní: ${t.modeActive ? "ANO" : "NE"}`);
      if (typeof t.scheduleEnabled !== "undefined") parts.push(`Plán/ručně: ${t.scheduleEnabled ? "ON" : "OFF"}`);
      if (typeof t.demandActive !== "undefined") parts.push(`Vstup: ${t.demandActive ? "ON" : "OFF"}`);
      st.textContent = parts.length ? parts.join(" · ") : "—";
    } catch (_) {}
  };

  const render = () => {
    const tbl = $("#tblSchedules");
    if (!tbl) return;

    const cfg = App.getConfig?.();
    if (!cfg) return;

    uiItems = buildUiItems(cfg);
    renderTuvConfig();

    tbl.innerHTML = "";
    const head = document.createElement("div");
    head.className = "row2 head";
    head.innerHTML = `<div>Název</div><div>Aktivní</div><div>Kdy</div><div>Akce</div><div></div>`;
    tbl.appendChild(head);

    uiItems.forEach((u, i) => {
      const isInterval = u.type === "interval";
      const s = isInterval ? u : u.item;

      const row = document.createElement("div");
      row.className = "row2";
      row.dataset.uix = String(i);

      const kindSel = kindOptions.map(([v, t]) => `<option value="${v}" ${String(s.kind) === v ? "selected" : ""}>${t}</option>`).join("");

      const modeVal = String(s.value?.mode || "MODE1");
      const modeSel = MODE_IDS.map(m => `<option value="${m}" ${modeVal === m ? "selected" : ""}>${m}</option>`).join("");

      const ctrlVal = String(s.value?.control || "auto");
      const ctrlSel = `<option value="auto" ${ctrlVal === "auto" ? "selected" : ""}>AUTO</option>
                       <option value="manual" ${ctrlVal === "manual" ? "selected" : ""}>MANUAL</option>`;

      const boolSel = (val) =>
        `<option value="true" ${val ? "selected" : ""}>ON</option><option value="false" ${!val ? "selected" : ""}>OFF</option>`;

      row.innerHTML = `
        <div>
          <input type="text" class="pName" value="${String(s.name || "Plán").replace(/"/g,"&quot;")}">
          <div class="muted">${isInterval ? ("pair:" + u.pair) : (s.id || "")}</div>
        </div>

        <div>
          <label class="check"><input type="checkbox" class="pEn" ${s.enabled ? "checked" : ""}> <span></span></label>
        </div>

        <div>
          <div class="inline">
            ${isInterval
              ? `<span class="pill">Od</span><input type="time" class="pFrom" value="${u.from}">
                 <span class="pill">Do</span><input type="time" class="pTo" value="${u.to}">`
              : `<input type="time" class="pAt" value="${s.at}">`
            }
            <span class="pill pDays">${daysStr(s.days)}</span>
            <button class="btn ghost pEditDays">Dny...</button>
          </div>
        </div>

        <div>
          <div class="inline">
            <select class="pKind">${kindSel}</select>
            <span class="pParams"></span>
          </div>
        </div>

        <div><button class="btn ghost pDel">✕</button></div>
      `;

      tbl.appendChild(row);

      const params = row.querySelector(".pParams");

      const rebuildParams = () => {
        const kind = row.querySelector(".pKind").value;

        if (kind === "set_mode") {
          params.innerHTML = `<select class="pMode">${modeSel}</select>`;
        } else if (kind === "set_control_mode") {
          params.innerHTML = `<select class="pControl">${ctrlSel}</select>`;
        } else if (kind === "dhw_enable" || kind === "night_mode") {
          if (isInterval) {
            params.innerHTML = `<span class="pill">Interval</span>
                                <span class="muted">ON od→do</span>
                                <button class="btn ghost pToSingle">Jednorázově</button>`;
          } else {
            const enVal = !!s.value?.enable;
            params.innerHTML = `<select class="pBool">${boolSel(enVal)}</select>
                                <button class="btn ghost pToInterval">Od–Do...</button>`;
          }
        } else {
          params.innerHTML = `<span class="muted">—</span>`;
        }

        // If kind changed and interval is no longer allowed -> convert to single
        if (isInterval && !BOOL_KINDS.has(kind)) {
          // convert in-memory and re-render
          const cfg = App.getConfig?.();
          if (!cfg) return;
          const idx = Number(row.dataset.uix);
          const it = uiItems[idx];
          const single = {
            id: it.startId || makeId(),
            enabled: it.enabled !== false,
            name: it.name || "Plán",
            days: it.days || [1,2,3,4,5,6,7],
            at: it.from || "06:00",
            kind,
            value: {},
          };
          uiItems[idx] = { type: "single", item: single };
          render();
        }
      };

      rebuildParams();
      row.querySelector(".pKind").addEventListener("change", () => {
        // Keep current kind in model
        const idx = Number(row.dataset.uix);
        const kind = row.querySelector(".pKind").value;
        const it = uiItems[idx];
        if (it?.type === "interval") it.kind = kind;
        else if (it?.type === "single") it.item.kind = kind;
        rebuildParams();
      });
    });
  };

  const collect = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return null;

    ensure(cfg);
    collectTuvConfig(cfg);

    const tbl = $("#tblSchedules");
    const rows = $$(".row2", tbl).filter(r => !r.classList.contains("head"));

    // First update uiItems from visible inputs
    rows.forEach(r => {
      const idx = Number(r.dataset.uix);
      const u = uiItems[idx];
      if (!u) return;

      const enabled = !!r.querySelector(".pEn")?.checked;
      const name = r.querySelector(".pName")?.value || "Plán";
      const kind = r.querySelector(".pKind")?.value || "set_mode";
      const daysText = r.querySelector(".pDays")?.textContent; // just for UI, real days live in item

      if (u.type === "interval") {
        u.enabled = enabled;
        u.name = name;
        u.kind = kind;
        u.from = r.querySelector(".pFrom")?.value || u.from || "06:00";
        u.to = r.querySelector(".pTo")?.value || u.to || "07:00";
      } else {
        const s = u.item;
        s.enabled = enabled;
        s.name = name;
        s.kind = kind;
        s.at = r.querySelector(".pAt")?.value || s.at || "06:00";

        // params by kind
        if (kind === "set_mode") {
          s.value = { mode: r.querySelector(".pMode")?.value || "MODE1" };
        } else if (kind === "set_control_mode") {
          s.value = { control: r.querySelector(".pControl")?.value || "auto" };
        } else if (kind === "dhw_enable" || kind === "night_mode") {
          s.value = { enable: (r.querySelector(".pBool")?.value || "false") === "true" };
        } else {
          s.value = s.value || {};
        }
      }
    });

    // Then write back to cfg.schedules (firmware-friendly)
    writeBackUiItems(cfg, uiItems);
    return cfg;
  };

  const toInterval = (idx) => {
    const u = uiItems[idx];
    if (!u || u.type !== "single") return;
    const s = u.item;
    const kind = String(s.kind);
    if (!BOOL_KINDS.has(kind)) return;

    const pair = makeId();
    const from = s.at || "06:00";
    const to = addMinutes(from, 60);

    uiItems[idx] = {
      type: "interval",
      pair,
      startId: pair + "-start",
      endId: pair + "-end",
      enabled: s.enabled !== false,
      name: s.name || "Interval",
      days: Array.isArray(s.days) ? s.days : [1,2,3,4,5,6,7],
      kind,
      from,
      to,
    };
    render();
  };

  const toSingle = (idx) => {
    const u = uiItems[idx];
    if (!u || u.type !== "interval") return;

    // Convert to single schedule: keep "from" as at, keep kind, set enable=true by default
    uiItems[idx] = {
      type: "single",
      item: {
        id: u.startId || makeId(),
        enabled: u.enabled !== false,
        name: u.name || "Plán",
        days: Array.isArray(u.days) ? u.days : [1,2,3,4,5,6,7],
        at: u.from || "06:00",
        kind: u.kind,
        value: { enable: true },
      }
    };
    render();
  };

  const openDaysFor = (idx) => {
    const u = uiItems[idx];
    if (!u) return;

    const getDays = () => (u.type === "interval") ? (u.days || [1,2,3,4,5,6,7]) : (u.item.days || [1,2,3,4,5,6,7]);
    const setDays = (days) => {
      if (u.type === "interval") u.days = days;
      else u.item.days = days;
    };

    openDaysModal({ getDays, setDays, onApply: () => render() });
  };

  const bind = () => {
    if (!$("#cfg-sched")) return;

    wireDaysModal();

    // Update config live for TUV mapping
    $("#tuvDemandInput")?.addEventListener("change", () => { const cfg = App.getConfig?.(); if (cfg) collectTuvConfig(cfg); });
    $("#tuvRequestRelay")?.addEventListener("change", () => { const cfg = App.getConfig?.(); if (cfg) collectTuvConfig(cfg); });
    $("#tuvManualEnable")?.addEventListener("change", () => { const cfg = App.getConfig?.(); if (cfg) collectTuvConfig(cfg); });

    $("#btnAddSchedule")?.addEventListener("click", () => {
      const cfg = App.getConfig?.();
      if (!cfg) return;

      ensure(cfg);
      uiItems = buildUiItems(cfg);
      uiItems.push({
        type: "single",
        item: {
          id: makeId(),
          enabled: true,
          name: "Plán",
          days: [1,2,3,4,5,6,7],
          at: "06:00",
          kind: "set_mode",
          value: { mode: "MODE1" }
        }
      });
      render();
    });

    $("#tblSchedules")?.addEventListener("click", (ev) => {
      const row = ev.target.closest(".row2");
      if (!row || row.classList.contains("head")) return;

      const idx = Number(row.dataset.uix);
      const u = uiItems[idx];
      if (!u) return;

      if (ev.target.closest(".pDel")) {
        uiItems.splice(idx, 1);
        render();
        return;
      }
      if (ev.target.closest(".pEditDays")) {
        openDaysFor(idx);
        return;
      }
      if (ev.target.closest(".pToInterval")) {
        toInterval(idx);
        return;
      }
      if (ev.target.closest(".pToSingle")) {
        toSingle(idx);
        return;
      }
    });

    $("#btnSaveSchedules")?.addEventListener("click", async () => {
      const cfg = collect();
      if (!cfg) return;
      try {
        await apiPostJson("/api/config", cfg);
        toast("Plánování uloženo");
        refreshTuvStatus();
      } catch (err) {
        toast("Chyba ukládání: " + (err?.message || err), "bad");
      }
    });

    refreshTuvStatus();
    if (!intervalId) intervalId = setInterval(refreshTuvStatus, 3000);
  };

  // Hook render after config load
  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    try { prev && prev(cfg); } catch (_) {}
    updateInputHints(cfg);
    render();
  };
  if (App.getConfig?.()) render();
    initialized = true;
  };

  const destroy = () => {
    if (intervalId) {
      clearInterval(intervalId);
      intervalId = null;
    }
  };

  window.Modules = window.Modules || {};
  window.Modules.schedules = { init, destroy };
})();
