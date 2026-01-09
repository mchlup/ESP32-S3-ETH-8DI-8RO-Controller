/* schematic.main.js
   Vykreslení instancí + automatické propojení podle data-port.
   Závislosti: schematic.templates.js, schematic.paths.js, schematic.router.js
*/
(function () {
  "use strict";

  const NS = "http://www.w3.org/2000/svg";
  function svgEl(name) { return document.createElementNS(NS, name); }

  function cloneTpl(svg, tplId) {
    const tpl = svg.getElementById(tplId) || document.getElementById(tplId);
    if (!tpl) throw new Error("Template not found: " + tplId);
    const node = tpl.cloneNode(true);
    node.removeAttribute("id");
    return node;
  }

  // Převod bodu z lokálních souřadnic elementu do SVG user-space.
  // Používá screen CTM a inverzi SVG CTM => funguje korektně i při CSS škálování SVG (width:100%, viewBox, ...).
  function elLocalToSvg(svg, el, x, y) {
    const elM = el.getScreenCTM && el.getScreenCTM();
    const svgM = svg.getScreenCTM && svg.getScreenCTM();
    if (!elM || !svgM) return { x, y };

    try {
      const pt = svg.createSVGPoint();
      pt.x = x; pt.y = y;

      const sp = pt.matrixTransform(elM);   // element-local -> screen
      const inv = svgM.inverse();           // screen -> svg

      const pt2 = svg.createSVGPoint();
      pt2.x = sp.x; pt2.y = sp.y;

      const up = pt2.matrixTransform(inv);  // -> svg user-space
      return { x: up.x, y: up.y };
    } catch (_) {
      return { x, y };
    }
  }

  function getLocalAnchor(el) {
    if (el.cx?.baseVal && el.cy?.baseVal) return { x: el.cx.baseVal.value, y: el.cy.baseVal.value };
    if (el.x?.baseVal && el.y?.baseVal) return { x: el.x.baseVal.value, y: el.y.baseVal.value };

    if (el.x1?.baseVal && el.y1?.baseVal && el.x2?.baseVal && el.y2?.baseVal) {
      return {
        x: (el.x1.baseVal.value + el.x2.baseVal.value) / 2,
        y: (el.y1.baseVal.value + el.y2.baseVal.value) / 2,
      };
    }

    try {
      const bb = el.getBBox();
      return { x: bb.x + bb.width / 2, y: bb.y + bb.height / 2 };
    } catch (_) {
      return { x: 0, y: 0 };
    }
  }

  function parsePortRef(ref) {
    if (!ref) return null;
    if (typeof ref === "string") {
      const m = ref.split(":");
      if (m.length === 2) return { c: m[0], p: m[1] };
      throw new Error("Invalid port ref string: " + ref);
    }
    if (typeof ref === "object" && ref.c && ref.p) return ref;
    throw new Error("Invalid port ref: " + JSON.stringify(ref));
  }

  function getPortGlobal(svg, compGroup, portName) {
    const el = compGroup.querySelector(`[data-port="${portName}"]`);
    if (!el) throw new Error(`Port ${portName} not found in ${compGroup.id}`);

    const a = getLocalAnchor(el);
    const g0 = elLocalToSvg(svg, el, a.x, a.y);

    const dirAttr = el.getAttribute("data-dir") || null;
    let dir = dirAttr;

    if (dirAttr) {
      const delta = 10;
      const dmap = { left: [-delta, 0], right: [delta, 0], up: [0, -delta], down: [0, delta] };
      const dv = dmap[dirAttr] || null;

      if (dv) {
        const g1 = elLocalToSvg(svg, el, a.x + dv[0], a.y + dv[1]);
        const dx = g1.x - g0.x;
        const dy = g1.y - g0.y;

        if (Math.abs(dx) >= Math.abs(dy)) dir = dx >= 0 ? "right" : "left";
        else dir = dy >= 0 ? "down" : "up";
      }
    }

    return { x: g0.x, y: g0.y, dir };
  }

  function offsetFromDir(p, dir, d = 16) {
    switch (dir) {
      case "left":  return { x: p.x - d, y: p.y };
      case "right": return { x: p.x + d, y: p.y };
      case "up":    return { x: p.x, y: p.y - d };
      case "down":  return { x: p.x, y: p.y + d };
      default:      return { x: p.x, y: p.y };
    }
  }

  function manhattanPath(a, b, midX = null) {
    if (midX === null || midX === undefined) {
      return `M ${a.x} ${a.y} L ${b.x} ${a.y} L ${b.x} ${b.y}`;
    }
    return `M ${a.x} ${a.y} L ${midX} ${a.y} L ${midX} ${b.y} L ${b.x} ${b.y}`;
  }

  function addPipe(pipesLayer, d, cls, markerEnd, attrs) {
    const p = svgEl("path");
    p.setAttribute("class", cls);
    p.setAttribute("d", d);
    if (markerEnd) p.setAttribute("marker-end", markerEnd);
    if (attrs && typeof attrs === "object") {
      for (const [k, v] of Object.entries(attrs)) {
        if (v === undefined || v === null) continue;
        p.setAttribute(k, String(v));
      }
    }
    pipesLayer.appendChild(p);
    return p;
  }

  function applyTempClass(group, tempC) {
    group.classList.remove("dhwTempCold", "dhwTempWarm", "dhwTempHot");
    if (tempC === null || tempC === undefined || isNaN(tempC)) return;
    if (tempC < 30) group.classList.add("dhwTempCold");
    else if (tempC > 45) group.classList.add("dhwTempHot");
    else group.classList.add("dhwTempWarm");
  }

  function bindTemperatureText(group, tempC) {
    const el = group.querySelector('[data-bind="temp"]');
    if (!el) return;
    if (tempC === null || tempC === undefined || isNaN(tempC)) { el.textContent = "—"; return; }
    const v = Math.round(Number(tempC) * 10) / 10;
    el.textContent = v.toFixed(1) + "°C";
  }
  
  function bindNameText(group, name) {
    const el = group.querySelector('[data-bind="name"]');
    if (!el) return;
    el.textContent = name || "";
  }

  function setValveGauge(gaugeG, pct) {
    const arc = gaugeG.querySelector(".gaugeArc");
    const txt = gaugeG.querySelector('[data-bind="valvePos"]');
    if (!arc || !txt) return;

    let v = Number(pct);
    if (!isFinite(v)) v = 0;
    v = Math.max(0, Math.min(100, v));

    // díky pathLength=100 v šabloně pracujeme přímo v procentech
    arc.setAttribute("stroke-dasharray", `${v} ${100 - v}`);
    txt.textContent = `${Math.round(v)}%`;
  }

  function addBlock(svg, instancesLayer, comps, inst) {
    const g = svgEl("g");
    g.id = inst.id;
    g.setAttribute("transform", `translate(${inst.x} ${inst.y})`);

    // host classes for state-driven styling
    if (inst.tpl === "tplBoiler") g.classList.add("boilerTplHost");
    if (inst.tpl === "tplAKU") g.classList.add("akuTplHost");

    // volitelná rotace pro bloky (např. T-spojky)
    if (typeof inst.rotateDeg === "number" && inst.rotateDeg !== 0) {
      const gr = svgEl("g");
      gr.setAttribute("transform", `rotate(${inst.rotateDeg})`);
      gr.appendChild(cloneTpl(svg, inst.tpl));
      g.appendChild(gr);
    } else {
      g.appendChild(cloneTpl(svg, inst.tpl));
    }

    if (inst.vars?.pumpAngle !== undefined) {
      g.style.setProperty("--pumpAngle", String(inst.vars.pumpAngle) + "deg");
    }
    if (inst.vars?.pumpOn) g.classList.add("pumpOn");
    if (inst.vars?.heaterOn) g.classList.add("heaterOn");
    if (inst.vars?.tempC !== undefined) {
      if (inst.tpl === "tplDHW") applyTempClass(g, inst.vars.tempC);
      bindTemperatureText(g, inst.vars.tempC);
    }
    // název pro šablony, které ho mají (např. tplThermometer)
    bindNameText(g, inst.name || inst.id);

    if (inst.vars?.recircAngle !== undefined) {
      g.style.setProperty("--recircAngle", String(inst.vars.recircAngle) + "deg");
    }
    if (inst.vars?.recircOn) g.classList.add("recircOn");
    if (inst.vars?.recircMoving) g.classList.add("recircMoving");
    if (inst.tpl === "tplDHW") g.classList.add("dhwTplHost");

    instancesLayer.appendChild(g);
    comps[inst.id] = g;
    return g;
  }

  function addValve(svg, instancesLayer, comps, inst) {
    const g = svgEl("g");
    g.id = inst.id;
    g.setAttribute("transform", `translate(${inst.x} ${inst.y})`);

    if (inst.vars?.valveMoving) g.classList.add("valveMoving");

    // zmenšený ventil (default), možnost override přes inst.scale
    const valveScale = (typeof inst.scale === "number") ? inst.scale : 0.6;

    const gs = svgEl("g");
    gs.setAttribute("transform", `scale(${valveScale})`);
    g.appendChild(gs);

    const gg = svgEl("g");
    gg.setAttribute("transform", `rotate(${inst.geomRotateDeg || 0})`);
    gg.appendChild(cloneTpl(svg, "tplValve3Tri_geom"));
    gs.appendChild(gg);

    gs.appendChild(cloneTpl(svg, inst.labelsTpl));

    // kruhové pole polohy ventilu (V2/V3 nahoře, V1 dole)
    const gauge = cloneTpl(svg, "tplValveGauge");
    const gaugePos = inst.gaugePos || (inst.id === "v1" ? "bottom" : "top");
    const gaugeY = (gaugePos === "bottom") ? 125 : -85;
    gauge.setAttribute("transform", `translate(0 ${gaugeY})`);
    gs.appendChild(gauge);

    // hodnota v procentech (0..100) – můžeš dodávat přes inst.vars.valvePos
    setValveGauge(gauge, inst.vars?.valvePos);
    if (inst.vars?.valveMoving) g.classList.add("valveMoving");

    if (inst.vars?.valveMoving) g.classList.add("valveMoving");

    const t = svgEl("text");
    t.setAttribute("class", "valveName");
    t.setAttribute("x", String(inst.nameX ?? 0));
    t.setAttribute("y", String(inst.nameY ?? 34));
    t.setAttribute("text-anchor", "middle");
    t.textContent = inst.name || inst.id.toUpperCase();
    g.appendChild(t);

    instancesLayer.appendChild(g);
    comps[inst.id] = g;
    return g;
  }

  function render() {
    const svg = document.getElementById("svg");
    if (!svg) throw new Error("Chybí SVG #svg.");

    if (!window.SchemaTemplates?.apply) throw new Error("Chybí SchemaTemplates.apply().");
    window.SchemaTemplates.apply(svg);

    const pipesLayer = svg.querySelector("#pipes");
    const instancesLayer = svg.querySelector("#instances");
    if (!pipesLayer || !instancesLayer) throw new Error("SVG musí obsahovat vrstvy #pipes a #instances.");

    pipesLayer.innerHTML = "";
    instancesLayer.innerHTML = "";

    const { INSTANCES, CONNECTIONS, MANUAL_PATHS } = window.SchemaPaths;
    const comps = {};

    for (const inst of INSTANCES) {
      if (inst.type === "valve") addValve(svg, instancesLayer, comps, inst);
      else addBlock(svg, instancesLayer, comps, inst);
    }

    const router = window.SchemaRouter;
    const haveRouter = !!router?.buildObstacles && !!router?.route;

    for (const conRaw of CONNECTIONS) {
      const con = conRaw || {};
      const from = parsePortRef(con.from);
      const to = parsePortRef(con.to);
      if (!from || !to) throw new Error("Propojení musí mít from/to.");

      const gA = comps[from.c];
      const gB = comps[to.c];
      if (!gA || !gB) throw new Error(`Neznámá komponenta v propojení: ${from.c} nebo ${to.c}`);

      const a0 = getPortGlobal(svg, gA, from.p);
      const b0 = getPortGlobal(svg, gB, to.p);
      const a1 = offsetFromDir(a0, a0.dir, con.stub ?? 16);
      const b1 = offsetFromDir(b0, b0.dir, con.stub ?? 16);

      const routeMode = con.route || "auto";
      let d;

      if (routeMode === "manhattan" || !haveRouter) {
        d = manhattanPath(a1, b1, con.midX);
      } else {
        const ignore = new Set([from.c, to.c]);
        const obstacles = router.buildObstacles(svg, comps, ignore, 18);
        const baseOpt = (window.SchemaPaths && window.SchemaPaths.ROUTER_OPTIONS)
          ? window.SchemaPaths.ROUTER_OPTIONS
          : { grid: 10, maxIter: 60000 };
        const opt = Object.assign({}, baseOpt, con.router || {});
        d = router.route(svg, a1, b1, obstacles, opt);
      }

      // Dotáhni čáru až na porty (a ne jen na stub body)
      const stripMove = (dd) => String(dd || "").replace(/^M\s*[-\d.]+\s*[-\d.]+\s*/i, "");
      const core = stripMove(d);
      const dFull = `M ${a0.x} ${a0.y} L ${a1.x} ${a1.y} ${core} L ${b0.x} ${b0.y}`;
      const fromKey = `${from.c}:${from.p}`;
      const toKey = `${to.c}:${to.p}`;
      const key = `${fromKey}->${toKey}`;
      const cls = String(con.cls || "pipe");
      let kind = "pipe";
      if (cls.includes("wire")) kind = "wire";
      else if (cls.includes("supply")) kind = "supply";
      else if (cls.includes("return")) kind = "return";
      else if (cls.includes("preheat")) kind = "preheat";
      addPipe(
        pipesLayer,
        dFull,
        cls,
        con.marker || null,
        {
          "data-from": fromKey,
          "data-to": toKey,
          "data-key": key,
          "data-kind": kind,
        }
      );
    }

    for (const mp of (MANUAL_PATHS || [])) {
      addPipe(pipesLayer, mp.d, mp.cls, mp.marker, mp.attrs || null);
    }
  }

  window.addEventListener("DOMContentLoaded", () => {
    try { render(); }
    catch (e) {
      console.error(e);
      const pre = document.createElement("pre");
      pre.style.whiteSpace = "pre-wrap";
      pre.style.color = "salmon";
      pre.textContent = String(e && e.stack ? e.stack : e);
      document.body.appendChild(pre);
    }
  });

  window.SchemaMain = { render };
})();
