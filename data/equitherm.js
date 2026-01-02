/* Ekviterm UI module (v3) – slope/shift + chart (local canvas, Chart-like style) */
(() => {
  const App = window.App;
  if (!App) return;

  const $ = App.$;
  const $$ = App.$$;
  const toast = App.toast;
  const apiPostJson = App.apiPostJson;

  const num = (v, d=0) => {
    const x = Number(v);
    return Number.isFinite(x) ? x : d;
  };
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

  const ensureShape = (cfg) => {
    if (!cfg || typeof cfg !== "object") return;
    cfg.equitherm = (cfg.equitherm && typeof cfg.equitherm === "object") ? cfg.equitherm : {};
    const e = cfg.equitherm;

    e.enabled = !!e.enabled;
    e.model = String(e.model || "slope"); // "slope" (current UI)

    e.outdoor = (e.outdoor && typeof e.outdoor === "object") ? e.outdoor : { source: "none", topic: "" };
    e.outdoor.source = String(e.outdoor.source || "none");
    e.outdoor.topic = String(e.outdoor.topic || "");

    // Slopes/shifts – compatible with reference UI
    e.slopeDay = num(e.slopeDay, 1.0);
    e.shiftDay = num(e.shiftDay, 0);
    e.slopeNight = num(e.slopeNight, 0.7);
    e.shiftNight = num(e.shiftNight, -5);

    // Limits
    e.minFlow = num(e.minFlow, 22);
    e.maxFlow = num(e.maxFlow, 50);
    if (e.minFlow > e.maxFlow) [e.minFlow, e.maxFlow] = [e.maxFlow, e.minFlow];

    // Optional reference points (2 points / curve)
    e.refs = (e.refs && typeof e.refs === "object") ? e.refs : {};
    const r = e.refs;
    r.day = (r.day && typeof r.day === "object") ? r.day : { tout1:-10, tflow1:45, tout2:15, tflow2:30 };
    r.night = (r.night && typeof r.night === "object") ? r.night : { tout1:-10, tflow1:40, tout2:15, tflow2:25 };
    ["tout1","tflow1","tout2","tflow2"].forEach(k=>{
      r.day[k]=num(r.day[k], r.day[k]);
      r.night[k]=num(r.night[k], r.night[k]);
    });
  };

  // Reference formula (same as attached UI):
  // Tflow = (20 - Tout) * slope + 20 + shift  (then clamped)
  const compute = (tout, slope, shift, minFlow, maxFlow) => {
    const raw = (20 - tout) * slope + 20 + shift;
    return clamp(raw, minFlow, maxFlow);
  };

  const getCfg = () => {
    const cfg = App.getConfig?.();
    if (!cfg) return null;
    ensureShape(cfg);
    return cfg;
  };

  const syncText = (rangeEl, textEl, fmt) => {
    if (!rangeEl || !textEl) return;
    const v = rangeEl.value;
    textEl.value = fmt ? fmt(v) : v;
  };

  const syncRangeFromText = (rangeEl, textEl, clampMin, clampMax) => {
    if (!rangeEl || !textEl) return;
    const v = num(textEl.value, num(rangeEl.value, 0));
    const vv = clamp(v, clampMin, clampMax);
    rangeEl.value = String(vv);
    textEl.value = String(vv);
  };

  // --- Chart (local canvas) ---
  const cssVar = (name, fallback) => {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  };

  const drawChart = () => {
    const canvas = $("#ekvChart");
    if (!canvas) return;

    const cfg = getCfg();
    if (!cfg) return;
    const e = cfg.equitherm;

    const xMin = -20, xMax = 20;
    const temps = Array.from({ length: 41 }, (_, i) => i - 20);

    // Build datasets
    const dayData = temps.map(t => compute(t, e.slopeDay, e.shiftDay, e.minFlow, e.maxFlow));
    const nightData = temps.map(t => compute(t, e.slopeNight, e.shiftNight, e.minFlow, e.maxFlow));

    const all = dayData.concat(nightData);
    const suggestedMin = Math.min(...all);
    const suggestedMax = Math.max(...all);

    // mimic script.js: auto y-range with padding
    const yMin = suggestedMin - 10;
    const yMax = suggestedMax + 10;

    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = Math.max(10, Math.floor(rect.width));
    const h = Math.max(10, Math.floor(rect.height));
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);

    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const padL = 46, padR = 12, padT = 12, padB = 36;
    const plotW = w - padL - padR;
    const plotH = h - padT - padB;

    const xToPx = (x) => padL + ((x - xMin) / (xMax - xMin)) * plotW;
    const yToPx = (y) => padT + (1 - ((y - yMin) / (yMax - yMin))) * plotH;

    // background (match card)
    ctx.fillStyle = cssVar("--panel2", "#0f141b");
    ctx.fillRect(0, 0, w, h);

    // grid
    const grid = cssVar("--border", "rgba(255,255,255,.10)");
    ctx.strokeStyle = grid;
    ctx.lineWidth = 1;

    const drawGridX = (step) => {
      for (let x=xMin; x<=xMax; x+=step){
        const px = xToPx(x);
        ctx.beginPath();
        ctx.moveTo(px, padT);
        ctx.lineTo(px, padT + plotH);
        ctx.stroke();
      }
    };
    const drawGridY = (step) => {
      // find step start
      const start = Math.ceil(yMin / step) * step;
      for (let y=start; y<=yMax; y+=step){
        const py = yToPx(y);
        ctx.beginPath();
        ctx.moveTo(padL, py);
        ctx.lineTo(padL + plotW, py);
        ctx.stroke();
      }
    };

    drawGridX(5);
    drawGridY(5);

    // axes
    const axis = cssVar("--muted", "rgba(255,255,255,.55)");
    ctx.strokeStyle = axis;
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(padL, padT);
    ctx.lineTo(padL, padT + plotH);
    ctx.lineTo(padL + plotW, padT + plotH);
    ctx.stroke();

    // labels
    ctx.fillStyle = axis;
    ctx.font = "12px system-ui, -apple-system, Segoe UI, Roboto, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let x=-20; x<=20; x+=10){
      const px = xToPx(x);
      ctx.fillText(String(x), px, padT + plotH + 8);
    }
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let y=Math.ceil(yMin/10)*10; y<=yMax; y+=10){
      const py = yToPx(y);
      ctx.fillText(String(y), padL - 8, py);
    }

    const drawLine = (data, color) => {
      ctx.strokeStyle = color;
      ctx.lineWidth = 2.2;
      ctx.beginPath();
      data.forEach((v, i) => {
        const x = temps[i];
        const px = xToPx(x);
        const py = yToPx(v);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      });
      ctx.stroke();

      // points
      ctx.fillStyle = color;
      data.forEach((v, i) => {
        if (i % 4 !== 0) return; // not too dense
        const x = temps[i];
        const px = xToPx(x);
        const py = yToPx(v);
        ctx.beginPath();
        ctx.arc(px, py, 3.2, 0, Math.PI*2);
        ctx.fill();
      });
    };

    drawLine(dayData, cssVar("--accent", "#4ca3ff"));
    drawLine(nightData, cssVar("--warn", "#ffb020"));

    // preview marker
    const tout = num($("#ekvPreviewTout")?.value, 0);
    const px = xToPx(clamp(tout, xMin, xMax));
    ctx.strokeStyle = "rgba(255,255,255,.35)";
    ctx.lineWidth = 1.2;
    ctx.setLineDash([4,4]);
    ctx.beginPath();
    ctx.moveTo(px, padT);
    ctx.lineTo(px, padT + plotH);
    ctx.stroke();
    ctx.setLineDash([]);
  };

  const updatePreview = () => {
    const cfg = getCfg();
    if (!cfg) return;
    const e = cfg.equitherm;

    const tout = num($("#ekvPreviewTout")?.value, 0);
    const d = compute(tout, e.slopeDay, e.shiftDay, e.minFlow, e.maxFlow);
    const n = compute(tout, e.slopeNight, e.shiftNight, e.minFlow, e.maxFlow);

    if ($("#ekvPreviewDay")) $("#ekvPreviewDay").textContent = d.toFixed(1) + " °C";
    if ($("#ekvPreviewNight")) $("#ekvPreviewNight").textContent = n.toFixed(1) + " °C";
  };

  const pullFromUI = () => {
    const cfg = getCfg();
    if (!cfg) return null;
    const e = cfg.equitherm;

    e.enabled = !!$("#ekvEnabled")?.checked;

    const src = $("#ekvOutdoorSource")?.value || "none";
    e.outdoor.source = src;
    e.outdoor.topic = String($("#ekvOutdoorTopic")?.value || "");

    e.slopeDay = num($("#ekvSlopeDay")?.value, e.slopeDay);
    e.shiftDay = num($("#ekvShiftDay")?.value, e.shiftDay);
    e.slopeNight = num($("#ekvSlopeNight")?.value, e.slopeNight);
    e.shiftNight = num($("#ekvShiftNight")?.value, e.shiftNight);

    e.minFlow = num($("#ekvMinFlow")?.value, e.minFlow);
    e.maxFlow = num($("#ekvMaxFlow")?.value, e.maxFlow);
    if (e.minFlow > e.maxFlow) [e.minFlow, e.maxFlow] = [e.maxFlow, e.minFlow];

    // refs (if present)
    if ($("#ekvDayTout1")){
      e.refs.day.tout1 = num($("#ekvDayTout1").value, e.refs.day.tout1);
      e.refs.day.tflow1 = num($("#ekvDayTflow1").value, e.refs.day.tflow1);
      e.refs.day.tout2 = num($("#ekvDayTout2").value, e.refs.day.tout2);
      e.refs.day.tflow2 = num($("#ekvDayTflow2").value, e.refs.day.tflow2);
    }
    if ($("#ekvNightTout1")){
      e.refs.night.tout1 = num($("#ekvNightTout1").value, e.refs.night.tout1);
      e.refs.night.tflow1 = num($("#ekvNightTflow1").value, e.refs.night.tflow1);
      e.refs.night.tout2 = num($("#ekvNightTout2").value, e.refs.night.tout2);
      e.refs.night.tflow2 = num($("#ekvNightTflow2").value, e.refs.night.tflow2);
    }

    return cfg;
  };

  const pushToUI = () => {
    const cfg = getCfg();
    if (!cfg) return;
    const e = cfg.equitherm;

    if ($("#ekvEnabled")) $("#ekvEnabled").checked = !!e.enabled;

    if ($("#ekvOutdoorSource")) $("#ekvOutdoorSource").value = e.outdoor.source || "none";
    if ($("#ekvOutdoorTopic")) $("#ekvOutdoorTopic").value = e.outdoor.topic || "";
    if ($("#ekvOutdoorTopicWrap")) $("#ekvOutdoorTopicWrap").style.display = (e.outdoor.source === "mqtt") ? "" : "none";

    const setRangePair = (rid, tid, v) => {
      const r = $("#" + rid), t = $("#" + tid);
      if (r) r.value = String(v);
      if (t) t.value = String(v);
    };

    setRangePair("ekvSlopeDay","ekvSlopeDayValue", e.slopeDay);
    setRangePair("ekvShiftDay","ekvShiftDayValue", e.shiftDay);
    setRangePair("ekvSlopeNight","ekvSlopeNightValue", e.slopeNight);
    setRangePair("ekvShiftNight","ekvShiftNightValue", e.shiftNight);
    setRangePair("ekvMinFlow","ekvMinFlowValue", e.minFlow);
    setRangePair("ekvMaxFlow","ekvMaxFlowValue", e.maxFlow);

    // refs
    if ($("#ekvDayTout1")){
      $("#ekvDayTout1").value = String(e.refs.day.tout1);
      $("#ekvDayTflow1").value = String(e.refs.day.tflow1);
      $("#ekvDayTout2").value = String(e.refs.day.tout2);
      $("#ekvDayTflow2").value = String(e.refs.day.tflow2);
    }
    if ($("#ekvNightTout1")){
      $("#ekvNightTout1").value = String(e.refs.night.tout1);
      $("#ekvNightTflow1").value = String(e.refs.night.tflow1);
      $("#ekvNightTout2").value = String(e.refs.night.tout2);
      $("#ekvNightTflow2").value = String(e.refs.night.tflow2);
    }

    updatePreview();
    drawChart();
  };

  // compute slope/shift from two reference points
  const applyRefs = (which) => {
    const cfg = getCfg();
    if (!cfg) return;
    const e = cfg.equitherm;
    const r = (which === "day") ? e.refs.day : e.refs.night;

    const tout1 = num(r.tout1, -10);
    const tout2 = num(r.tout2, 15);
    const tflow1 = num(r.tflow1, 45);
    const tflow2 = num(r.tflow2, 30);
    if (tout1 === tout2){
      toast("Body mají stejnou venkovní teplotu – nelze spočítat sklon", "bad");
      return;
    }

    // line: Tflow = m*Tout + b, where m = (t2 - t1)/(x2 - x1)
    const m = (tflow2 - tflow1) / (tout2 - tout1);
    const b = tflow1 - m*tout1;

    // our slope/shift mapping: Tflow = -slope*Tout + (20*slope + 20 + shift)
    // => m = -slope, b = 20*slope + 20 + shift
    const slope = -m;
    const shift = b - (20*slope + 20);

    if (which === "day"){
      e.slopeDay = clamp(slope, 0.1, 6.0);
      e.shiftDay = clamp(shift, -10, 20);
    } else {
      e.slopeNight = clamp(slope, 0.1, 6.0);
      e.shiftNight = clamp(shift, -10, 20);
    }

    pushToUI();
    toast("Přepočteno z referenčních bodů");
  };

  
  // --- Outdoor temp sources: TEMP1..TEMP8 are logical temperature channels filled by sensors configured in "Funkce I/O".
  const _tempRoleLabel = (role) => {
    if (role === "temp_dallas") return "Dallas";
    if (role === "temp_ntc10k") return "NTC 10k";
    return "";
  };

  const rebuildOutdoorSourceOptions = () => {
    const cfg = App.getConfig?.();
    const sel = $("#ekvOutdoorSource");
    if (!cfg || !sel) return;

    const inputs = cfg?.iofunc?.inputs || [];
    const inNames = Array.isArray(cfg.inputNames) ? cfg.inputNames : [];
    const mkName = (i) => {
      const n = String(inNames[i] || "").trim();
      return n ? n : `Vstup ${i+1}`;
    };

    const cur = sel.value || (cfg?.equitherm?.outdoorSource || "none");
    const opts = [];
    opts.push(`<option value="none">—</option>`);

    for (let i = 0; i < 8; i++) {
      const role = String(inputs?.[i]?.role || "none");
      const extra = _tempRoleLabel(role);
      const base = extra ? `Teploměr ${i+1} (${mkName(i)} · ${extra})` : `Teploměr ${i+1} (${mkName(i)})`;
      const enabled = (role === "temp_dallas" || role === "temp_ntc10k");
      const label = enabled ? base : (base + " — nenastaveno");
      opts.push(`<option value="temp${i+1}" ${enabled ? "" : "disabled"}>${label}</option>`);
    }

    opts.push(`<option value="mqtt">MQTT (topic)</option>`);
    sel.innerHTML = opts.join("");
    // keep selection if possible (fallback to none if disabled)
    const opt = sel.querySelector(`option[value="${cur}"]`);
    if (opt && !opt.disabled) sel.value = cur;
    else sel.value = "none";
  };

  const _fmtC = (v) => (typeof v === "number" && isFinite(v)) ? (v.toFixed(1) + " °C") : "—";

  const renderEqStatus = () => {
    const st = App.getStatus?.();
    const cfg = App.getConfig?.();
    if (!st || !cfg) return;

    const pill = $("#ekvActivePill");
    const hint = $("#ekvStatusHint");
    const outNow = $("#ekvOutdoorNow");
    const tgtNow = $("#ekvTargetNow");

    const eq = st.equitherm || {};
    const enabled = !!eq.enabled;
    const active = !!eq.active;

    if (pill) {
      pill.textContent = active ? "aktivní" : (enabled ? "čeká na data" : "neaktivní");
      pill.classList.toggle("ok", active);
      pill.classList.toggle("warn", enabled && !active);
    }
    if (outNow) outNow.textContent = _fmtC(eq.outdoorC);
    if (tgtNow) tgtNow.textContent = _fmtC(eq.targetFlowC);

    if (!hint) return;

    const reasons = [];
    if (!enabled) reasons.push("Ekviterm není povolen");
    else {
      const src = String(cfg?.equitherm?.outdoorSource || "none");
      if (src === "none") reasons.push("Není zvolen zdroj venkovní teploty");
      else if (src.startsWith("temp")) {
        const idx = Number(src.substring(4)) - 1;
        const role = String(cfg?.iofunc?.inputs?.[idx]?.role || "none");
        if (role !== "temp_dallas" && role !== "temp_ntc10k") reasons.push(`Teploměr ${idx+1} nemá nastavenou roli (Dallas/NTC) ve Funkce I/O`);
        if (!isFinite(eq.outdoorC)) reasons.push("Venkovní teplota není platná");
      } else if (src === "mqtt") {
        if (!String(cfg?.equitherm?.outdoorTopic || "").trim()) reasons.push("MQTT topic pro venkovní teplotu je prázdný");
        if (!isFinite(eq.outdoorC)) reasons.push("Z MQTT ještě nepřišla platná teplota");
      }
    }

    hint.textContent = active ? "Výpočet běží" : (reasons.length ? reasons.join(" · ") : "Neznámý stav");
  };

const wire = () => {
    if (!$("#cfg-ekviterm")) return;

    rebuildOutdoorSourceOptions();


    rebuildOutdoorSourceOptions();


    // show/hide mqtt topic
    $("#ekvOutdoorSource")?.addEventListener("change", () => {
      const v = $("#ekvOutdoorSource").value;
      const wrap = $("#ekvOutdoorTopicWrap");
      if (wrap) wrap.style.display = (v === "mqtt") ? "" : "none";
    });

    // range -> text, update config + chart
    const bindPair = (rid, tid, min, max, stepIsFloat=false) => {
      const r = $("#" + rid);
      const t = $("#" + tid);
      if (!r || !t) return;
      const fmt = (v) => stepIsFloat ? String(num(v,0).toFixed(1)) : String(num(v,0));
      syncText(r, t, fmt);

      r.addEventListener("input", () => {
        syncText(r, t, fmt);
        pullFromUI();
        updatePreview();
        drawChart();
      });
      t.addEventListener("change", () => {
        syncRangeFromText(r, t, min, max);
        pullFromUI();
        updatePreview();
        drawChart();
      });
    
    // status refresh
    renderEqStatus();
    if (!wire._eqTimer) wire._eqTimer = setInterval(renderEqStatus, 2000);
};

    bindPair("ekvSlopeDay","ekvSlopeDayValue", 0.1, 6.0, true);
    bindPair("ekvShiftDay","ekvShiftDayValue", -10, 20, false);
    bindPair("ekvSlopeNight","ekvSlopeNightValue", 0.1, 6.0, true);
    bindPair("ekvShiftNight","ekvShiftNightValue", -10, 20, false);
    bindPair("ekvMinFlow","ekvMinFlowValue", 0, 70, false);
    bindPair("ekvMaxFlow","ekvMaxFlowValue", 30, 80, false);

    $("#ekvPreviewTout")?.addEventListener("input", () => { updatePreview(); drawChart(); });

    // buttons
    $("#btnEkvUpdateChart")?.addEventListener("click", () => {
      pullFromUI();
      updatePreview();
      drawChart();
    });

    $("#btnEkvApplyDayRefs")?.addEventListener("click", () => {
      pullFromUI();
      applyRefs("day");
    });
    $("#btnEkvApplyNightRefs")?.addEventListener("click", () => {
      pullFromUI();
      applyRefs("night");
    });

    $("#btnSaveEquitherm")?.addEventListener("click", async () => {
      const cfg = pullFromUI();
      if (!cfg) return;
      try{
        await apiPostJson("/api/config", cfg);
        toast("Ekviterm uložen");
      }catch(err){
        toast("Chyba ukládání: " + (err?.message || err), "bad");
      }
    });

    // redraw on resize
    window.addEventListener("resize", () => {
      // cheap debounce
      clearTimeout(wire._rt);
      wire._rt = setTimeout(drawChart, 120);
    });
  };

  
  // hook after status load
  const prevS = App.onStatusLoaded;
  App.onStatusLoaded = (st) => {
    try { prevS && prevS(st); } catch(_) {}
    renderEqStatus();
  };

// hook after config load
  const prev = App.onConfigLoaded;
  App.onConfigLoaded = (cfg) => {
    try { prev && prev(cfg); } catch(_) {}
    ensureShape(cfg);
    rebuildOutdoorSourceOptions();
    pushToUI();
    renderEqStatus();
  };

  window.addEventListener("DOMContentLoaded", () => {
    wire();
    renderEqStatus();
    // if config already loaded, render now
    const cfg = App.getConfig?.();
    if (cfg) { ensureShape(cfg); pushToUI(); }
  });
})();