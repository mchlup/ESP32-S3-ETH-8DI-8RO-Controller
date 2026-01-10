/* Valve calibration module (v2) */
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

  const RELAY_COUNT = 8;

  const ensure = (cfg) => {
    cfg.iofunc = (cfg.iofunc && typeof cfg.iofunc === "object") ? cfg.iofunc : {};
    cfg.iofunc.outputs = Array.isArray(cfg.iofunc.outputs) ? cfg.iofunc.outputs : [];
    cfg.iofunc.outputs = cfg.iofunc.outputs.map(o => ({
      role: String(o?.role || "none"),
      params: (o?.params && typeof o.params === "object") ? o.params : {}
    }));
    while (cfg.iofunc.outputs.length < RELAY_COUNT) cfg.iofunc.outputs.push({ role: "none", params: {} });
    cfg.iofunc.outputs = cfg.iofunc.outputs.slice(0, RELAY_COUNT);
  };

  const relayName = (cfg, i) => {
    const n = String(cfg?.relayNames?.[i] || "").trim();
    return n ? n : `Relé ${i + 1}`;
  };

  const state = {
    timers: new Map(), // idx -> startTimeMs
  };

  const setRelayAbs = async (relay1based, on) => {
    // /api/mode_ctrl action:"relay_raw" expects { relay:1..8, on:true/false }
    await apiPostJson("/api/mode_ctrl", { action:"relay_raw", relay: relay1based, on: !!on });
  };

  const stopBoth = async (a, b) => {
    if (a) await setRelayAbs(a, false);
    if (b) await setRelayAbs(b, false);
  };

  const pulseAbs = async (relay1based, seconds) => {
    await setRelayAbs(relay1based, true);
    setTimeout(() => setRelayAbs(relay1based, false).catch(() => {}), Math.max(100, seconds * 1000));
  };

  const moveAbs = async (relay1based, seconds) => {
    // "move" = energize for full travel time
    await pulseAbs(relay1based, seconds);
  };

  const render = () => {
    const host = $("#tblValveCalib");
    if (!host) return;

    const cfg = App.getConfig?.();
    if (!cfg) return;
    ensure(cfg);

    const items = [];
    for (let i = 0; i < RELAY_COUNT; i++) {
      const o = cfg.iofunc.outputs[i];
      let role = String(o.role || "none");
      if (role === "valve_3way_spring") role = "valve_3way_2rel"; // legacy
      const singleRelay = (role === "valve_3way_tuv" || role === "valve_3way_dhw");
      if (role !== "valve_3way_mix" && role !== "valve_3way_2rel" && !singleRelay) continue;

      // 1-based DO number
      let peerRel = Number(o.params?.peerRel ?? o.params?.partnerRelay ?? (i + 2));
      if (!Number.isFinite(peerRel) || peerRel < 1 || peerRel > RELAY_COUNT) peerRel = (i + 2);
      items.push({ idx: i, peerRel: singleRelay ? 0 : peerRel, singleRelay });
    }

    if (!items.length) {
      host.innerHTML = `<div class="pad muted">Nenalezen žádný trojcestný ventil. Nastav ho v záložce „Funkce I/O“ jako „Trojcestný ventil 2‑bod“ (master) a vyber „Peer relé“.</div>`;
      return;
    }

    host.innerHTML = "";
    const head = document.createElement("div");
    head.className = "row2 head";
    head.innerHTML = `<div>Ventil</div><div>Nastavení</div><div>Akce</div><div>Stav</div>`;
    host.appendChild(head);

    items.forEach(({ idx, peerRel, singleRelay }) => {
      const cfg = App.getConfig?.();
      const o = cfg.iofunc.outputs[idx];
      o.params = o.params || {};

      const travel = Number(o.params.travelTime ?? 6);
      const pulse = Number(o.params.pulseTime ?? 0.8);
      const guard = Number(o.params.guardTime ?? 0.3);
      const minSw = Number(o.params.minSwitchS ?? 30);
      const defPos = String(o.params.defaultPos || "A");
      const inv = !!o.params.invertDir;

      const row = document.createElement("div");
      row.className = "row2";
      row.dataset.idx = String(idx);
      row.dataset.partner = String(peerRel || 0);
      row.dataset.single = singleRelay ? "1" : "0";

      const aRelay = singleRelay ? (idx + 1) : (inv ? peerRel : (idx + 1));
      const bRelay = singleRelay ? 0 : (inv ? (idx + 1) : peerRel);

      row.innerHTML = `
        <div>
          <b>${relayName(cfg, idx)}</b>
          <div class="muted">${singleRelay ? `Relé DO${aRelay} (B=ON / A=OFF)` : `A=${aRelay ? `DO${aRelay}` : "—"} · B=${bRelay ? `DO${bRelay}` : "—"}`}</div>
        </div>

        <div>
          <div class="inline" style="flex-wrap:wrap;gap:10px">
            <div class="field" style="min-width:170px">
              <label>Přeběh (s)</label>
              <input type="number" step="0.1" class="pTravel" value="${travel}">
            </div>
            <div class="field" style="min-width:170px">
              <label>Pulse (s)</label>
              <input type="number" step="0.1" class="pPulse" value="${pulse}">
            </div>
            <div class="field" style="min-width:170px">
              <label>Pauza (s)</label>
              <input type="number" step="0.1" class="pGuard" value="${guard}">
            </div>
            <div class="field" style="min-width:170px">
              <label>Min. perioda (s)</label>
              <input type="number" step="1" min="0" class="pMinSw" value="${minSw}">
            </div>
            <div class="field" style="min-width:170px">
              <label>Výchozí poloha</label>
              <select class="pDef">
                <option value="A" ${defPos==="A" ? "selected":""}>A</option>
                <option value="B" ${defPos==="B" ? "selected":""}>B</option>
              </select>
            </div>
            <label class="check"><input type="checkbox" class="pInv" ${inv?"checked":""}> <span>Prohodit A/B</span></label>
          </div>
        </div>

        <div>
          <div class="inline" style="flex-wrap:wrap;gap:8px">
            <button class="btn ghost actHoldA">A ON</button>
            <button class="btn ghost actHoldB">B ON</button>
            <button class="btn ghost actStop">STOP</button>
            <button class="btn ghost actPulseA">Pulse A</button>
            <button class="btn ghost actPulseB">Pulse B</button>
            <button class="btn actMoveA">Přestav na A</button>
            <button class="btn actMoveB">Přestav na B</button>
            <button class="btn ghost actCycle">Cyklus A→B</button>
          </div>
        </div>

        <div><span class="pill status">připraveno</span></div>
      `;

      host.appendChild(row);
    });
  };

  const updateRowStatus = (row) => {
    const idx = Number(row.dataset.idx);
    const pill = row.querySelector(".status");
    const t0 = state.timers.get(idx);
    if (!pill) return;
    if (!t0) {
      pill.textContent = "připraveno";
      return;
    }
    const dt = (Date.now() - t0) / 1000;
    pill.textContent = `běží: ${dt.toFixed(1)} s`;
  };

  const tick = () => {
    const host = $("#tblValveCalib");
    if (!host) return;
    $$(".row2", host).forEach(r => {
      if (r.classList.contains("head")) return;
      updateRowStatus(r);
    });
  };

  const bind = () => {
    if (!$("#cfg-valve")) return;

    const host = $("#tblValveCalib");
    if (!host) return;

    // persist config on input changes
    host.addEventListener("input", (ev) => {
      const row = ev.target.closest(".row2");
      if (!row || row.classList.contains("head")) return;
      const idx = Number(row.dataset.idx);
      const cfg = App.getConfig?.();
      if (!cfg) return;
      ensure(cfg);

      const o = cfg.iofunc.outputs[idx];
      o.params = o.params || {};

      o.params.travelTime = Number(row.querySelector(".pTravel")?.value ?? 6);
      o.params.pulseTime  = Number(row.querySelector(".pPulse")?.value ?? 0.8);
      o.params.guardTime  = Number(row.querySelector(".pGuard")?.value ?? 0.3);
      o.params.minSwitchS = Number(row.querySelector(".pMinSw")?.value ?? 0);
      o.params.defaultPos = row.querySelector(".pDef")?.value || "A";
      o.params.invertDir  = !!row.querySelector(".pInv")?.checked;
    });

    host.addEventListener("click", async (ev) => {
      const row = ev.target.closest(".row2");
      if (!row || row.classList.contains("head")) return;

      const idx = Number(row.dataset.idx);
      const cfg = App.getConfig?.();
      if (!cfg) return;
      ensure(cfg);

      const o = cfg.iofunc.outputs[idx];
      o.params = o.params || {};

      const partner = Number(row.dataset.partner || 0);
      const isSingle = row.dataset.single === "1";
      if (!partner && !isSingle) {
        toast("Chybí relé B (peer). Trojcestný ventil potřebuje master + peer relé.", "bad");
        return;
      }

      const travel = Number(row.querySelector(".pTravel")?.value ?? 6);
      const pulse = Number(row.querySelector(".pPulse")?.value ?? 0.8);
      const guard = Number(row.querySelector(".pGuard")?.value ?? 0.3);
      const inv = !!row.querySelector(".pInv")?.checked;

      const aRelay = isSingle ? (idx + 1) : (inv ? partner : (idx + 1));
      const bRelay = isSingle ? 0 : (inv ? (idx + 1) : partner);

      try {
        if (ev.target.closest(".actHoldA")) {
          await stopBoth(aRelay, bRelay);
          if (isSingle) {
            await setRelayAbs(aRelay, false);
            toast(`A OFF (DO${aRelay})`);
          } else {
            await setRelayAbs(aRelay, true);
            toast(`A ON (DO${aRelay})`);
          }
        } else if (ev.target.closest(".actHoldB")) {
          await stopBoth(aRelay, bRelay);
          if (isSingle) {
            await setRelayAbs(aRelay, true);
            toast(`B ON (DO${aRelay})`);
          } else {
            await setRelayAbs(bRelay, true);
            toast(`B ON (DO${bRelay})`);
          }
        } else if (ev.target.closest(".actStop")) {
          await stopBoth(aRelay, bRelay);
          state.timers.delete(idx);
          toast("STOP");
        } else if (ev.target.closest(".actPulseA")) {
          await stopBoth(aRelay, bRelay);
          if (isSingle) {
            await setRelayAbs(aRelay, false);
            toast(`A OFF`);
          } else {
            await pulseAbs(aRelay, pulse);
            toast(`Pulse A ${pulse.toFixed(1)} s`);
          }
        } else if (ev.target.closest(".actPulseB")) {
          await stopBoth(aRelay, bRelay);
          if (isSingle) {
            await pulseAbs(aRelay, pulse);
            toast(`Pulse B ${pulse.toFixed(1)} s`);
          } else {
            await pulseAbs(bRelay, pulse);
            toast(`Pulse B ${pulse.toFixed(1)} s`);
          }
        } else if (ev.target.closest(".actMoveA")) {
          await stopBoth(aRelay, bRelay);
          state.timers.set(idx, Date.now());
          if (isSingle) {
            await setRelayAbs(aRelay, false);
            setTimeout(() => state.timers.delete(idx), Math.max(100, travel * 1000));
            toast(`Přestav na A (${travel.toFixed(1)} s)`);
          } else {
            await moveAbs(aRelay, travel);
            setTimeout(() => state.timers.delete(idx), Math.max(100, travel * 1000));
            toast(`Přestav na A (${travel.toFixed(1)} s)`);
          }
        } else if (ev.target.closest(".actMoveB")) {
          await stopBoth(aRelay, bRelay);
          state.timers.set(idx, Date.now());
          if (isSingle) {
            await moveAbs(aRelay, travel);
            setTimeout(() => state.timers.delete(idx), Math.max(100, travel * 1000));
            toast(`Přestav na B (${travel.toFixed(1)} s)`);
          } else {
            await moveAbs(bRelay, travel);
            setTimeout(() => state.timers.delete(idx), Math.max(100, travel * 1000));
            toast(`Přestav na B (${travel.toFixed(1)} s)`);
          }
        } else if (ev.target.closest(".actCycle")) {
          await stopBoth(aRelay, bRelay);
          state.timers.set(idx, Date.now());
          if (isSingle) {
            await setRelayAbs(aRelay, false);
            setTimeout(() => moveAbs(aRelay, travel).catch(()=>{}), Math.max(50, (travel + guard) * 1000));
            setTimeout(() => state.timers.delete(idx), Math.max(100, (2*travel + guard) * 1000));
            toast(`Cyklus A→B (${travel.toFixed(1)} s + ${guard.toFixed(1)} s + ${travel.toFixed(1)} s)`);
          } else {
            await moveAbs(aRelay, travel);
            setTimeout(() => moveAbs(bRelay, travel).catch(()=>{}), Math.max(50, (travel + guard) * 1000));
            setTimeout(() => state.timers.delete(idx), Math.max(100, (2*travel + guard) * 1000));
            toast(`Cyklus A→B (${travel.toFixed(1)} s + ${guard.toFixed(1)} s + ${travel.toFixed(1)} s)`);
          }
        }
      } catch (err) {
        toast("Chyba: " + (err?.message || err), "bad");
      }
    });

    $("#btnSaveValveCalib")?.addEventListener("click", async () => {
      const cfg = App.getConfig?.();
      if (!cfg) return;
      try {
        await apiPostJson("/api/config", cfg);
        toast("Kalibrace uložena");
      } catch (err) {
        toast("Chyba ukládání: " + (err?.message || err), "bad");
      }
    });

    if (!intervalId) intervalId = setInterval(tick, 200);
  };

  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    try { prev && prev(cfg); } catch (_) {}
    render();
  };
  if (App.getConfig?.()) render();

  // Přepnutí záložky – udržuj synchronizaci parametrů s "Funkce I/O".
  window.addEventListener("app:cfgTabChanged", (ev) => {
    const tab = ev?.detail?.tab;
    if (tab === "valvecal") render();
  });
    initialized = true;
  };

  const destroy = () => {
    if (intervalId) {
      clearInterval(intervalId);
      intervalId = null;
    }
  };

  window.Modules = window.Modules || {};
  window.Modules.valve_calib = { init, destroy };
})();
