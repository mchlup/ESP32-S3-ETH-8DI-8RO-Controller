/* schematic.router.js
   Autorouter (A* na mřížce) s podporou "koridorů" (preferovaných os X/Y).
   - Vstup: start/end body + seznam překážek (AABB)
   - Výstup: SVG path "d" (polyline z bodů)

   Koridory:
     opts.corridors = { x:[...], y:[...] }  // v px souřadnicích SVG
     opts.corridorPenalty (default 0.08)    // penalizace vzdálenosti od nejbližšího koridoru
     opts.corridorSnap (default 40)         // do této vzdálenosti (px) je "přitažlivost" největší
     opts.preferEdges (default false)       // pokud true, přidá koridory u okrajů viewBox
*/
(function () {
  "use strict";

  function aabbFromGroup(svg, g, pad = 12) {
    const bb = g.getBBox();
    const ctm = g.getCTM();
    if (!ctm) return null;

    const pts = [
      { x: bb.x, y: bb.y },
      { x: bb.x + bb.width, y: bb.y },
      { x: bb.x, y: bb.y + bb.height },
      { x: bb.x + bb.width, y: bb.y + bb.height },
    ].map(p => {
      const sp = svg.createSVGPoint(); sp.x = p.x; sp.y = p.y;
      const gp = sp.matrixTransform(ctm);
      return { x: gp.x, y: gp.y };
    });

    const xs = pts.map(p => p.x), ys = pts.map(p => p.y);
    let minX = Math.min(...xs), maxX = Math.max(...xs);
    let minY = Math.min(...ys), maxY = Math.max(...ys);

    return { minX: minX - pad, minY: minY - pad, maxX: maxX + pad, maxY: maxY + pad };
  }

  function buildObstacles(svg, comps, ignoreIds = new Set(), pad = 14) {
    const obs = [];
    for (const [id, g] of Object.entries(comps)) {
      if (ignoreIds.has(id)) continue;
      const aabb = aabbFromGroup(svg, g, pad);
      if (aabb) obs.push(aabb);
    }
    return obs;
  }

  function pointInAabb(p, a) { return p.x >= a.minX && p.x <= a.maxX && p.y >= a.minY && p.y <= a.maxY; }
  function anyObstacle(p, obstacles) { for (const o of obstacles) if (pointInAabb(p, o)) return true; return false; }
  function toKey(ix, iy) { return ix + "|" + iy; }

  function reconstruct(cameFrom, current) {
    const pts = [current];
    let key = toKey(current.ix, current.iy);
    while (cameFrom.has(key)) {
      current = cameFrom.get(key);
      pts.push(current);
      key = toKey(current.ix, current.iy);
    }
    pts.reverse();
    return pts;
  }

  function simplify(points) {
    if (points.length <= 2) return points;
    const out = [points[0]];
    for (let i = 1; i < points.length - 1; i++) {
      const a = out[out.length - 1], b = points[i], c = points[i + 1];
      const abx = b.x - a.x, aby = b.y - a.y;
      const bcx = c.x - b.x, bcy = c.y - b.y;
      if ((abx === 0 && bcx === 0) || (aby === 0 && bcy === 0)) continue; // collinear
      out.push(b);
    }
    out.push(points[points.length - 1]);
    return out;
  }

  function pointsToPath(points) {
    if (!points.length) return "";
    let d = `M ${points[0].x} ${points[0].y}`;
    for (let i = 1; i < points.length; i++) d += ` L ${points[i].x} ${points[i].y}`;
    return d;
  }

  function nearestDistToLinesPx(p, lines, axis) {
    if (!lines || !lines.length) return Infinity;
    let best = Infinity;
    if (axis === "x") {
      for (const lx of lines) best = Math.min(best, Math.abs(p.x - lx));
    } else {
      for (const ly of lines) best = Math.min(best, Math.abs(p.y - ly));
    }
    return best;
  }

  function corridorCostPx(p, corridors, corridorPenalty, corridorSnap) {
    if (!corridors) return 0;
    const dx = nearestDistToLinesPx(p, corridors.x, "x");
    const dy = nearestDistToLinesPx(p, corridors.y, "y");
    const d = Math.min(dx, dy); // přitažlivost k nejbližšímu koridoru (X nebo Y)
    if (!isFinite(d)) return 0;

    // "snap" oblast: v blízkosti koridoru je náklad minimální, dál roste
    const dd = Math.max(0, d - corridorSnap);
    return dd * corridorPenalty;
  }

  function routeAStar(svg, start, end, obstacles, opts) {
    const grid = opts.grid ?? 10;
    const maxIter = opts.maxIter ?? 60000;

    const vb = svg.viewBox.baseVal;
    const minX = vb.x, minY = vb.y, maxX = vb.x + vb.width, maxY = vb.y + vb.height;

    // corridors
    const corridorPenalty = opts.corridorPenalty ?? 0.08;
    const corridorSnap = opts.corridorSnap ?? 40;
    const corridors = opts.corridors ? { x: [...(opts.corridors.x || [])], y: [...(opts.corridors.y || [])] } : { x: [], y: [] };

    if (opts.preferEdges) {
      const margin = 30;
      corridors.x.push(minX + margin, maxX - margin);
      corridors.y.push(minY + margin, maxY - margin);
    }

    function toIdx(p) { return { ix: Math.round((p.x - minX) / grid), iy: Math.round((p.y - minY) / grid) }; }
    function toPoint(ix, iy) { return { x: minX + ix * grid, y: minY + iy * grid }; }

    const s = toIdx(start), e = toIdx(end);

    function h(ix, iy) { return Math.abs(ix - e.ix) + Math.abs(iy - e.iy); } // Manhattan heuristic

    const open = new Map(); // key -> {ix,iy,f,g}
    const cameFrom = new Map();
    const gScore = new Map();

    const startKey = toKey(s.ix, s.iy);
    open.set(startKey, { ix: s.ix, iy: s.iy, f: h(s.ix, s.iy), g: 0 });
    gScore.set(startKey, 0);

    const dirs = [{ dx: 1, dy: 0 }, { dx: -1, dy: 0 }, { dx: 0, dy: 1 }, { dx: 0, dy: -1 }];

    let iter = 0;
    while (open.size && iter++ < maxIter) {
      // pick node with smallest f
      let currentKey = null, current = null;
      for (const [k, v] of open) {
        if (!current || v.f < current.f) { current = v; currentKey = k; }
      }
      if (!currentKey) break;

      if (current.ix === e.ix && current.iy === e.iy) {
        const raw = reconstruct(cameFrom, current).map(n => toPoint(n.ix, n.iy));
        return simplify(raw);
      }

      open.delete(currentKey);

      for (const d of dirs) {
        const nix = current.ix + d.dx;
        const niy = current.iy + d.dy;

        const p = toPoint(nix, niy);
        if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) continue;
        if (anyObstacle(p, obstacles)) continue;

        const nk = toKey(nix, niy);

        // krok + penalizace vzdálenosti od koridoru
        const stepCost = 1 + corridorCostPx(p, corridors, corridorPenalty, corridorSnap);
        const tentativeG = current.g + stepCost;

        const bestG = gScore.get(nk);
        if (bestG === undefined || tentativeG < bestG) {
          cameFrom.set(nk, current);
          gScore.set(nk, tentativeG);
          const f = tentativeG + h(nix, niy);
          open.set(nk, { ix: nix, iy: niy, g: tentativeG, f });
        }
      }
    }
    return null;
  }

  function route(svg, start, end, obstacles, opts = {}) {
    const pts = routeAStar(svg, start, end, obstacles, opts);
    if (pts && pts.length >= 2) {
      // force endpoints to be exact (avoid grid snapping drift)
      pts[0] = { x: start.x, y: start.y };
      pts[pts.length - 1] = { x: end.x, y: end.y };
      return pointsToPath(simplify(pts));
    }
    return `M ${start.x} ${start.y} L ${end.x} ${start.y} L ${end.x} ${end.y}`;
  }

  window.SchemaRouter = { aabbFromGroup, buildObstacles, route, pointsToPath, simplify };
})();
