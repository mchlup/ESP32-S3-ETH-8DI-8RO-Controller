/* schematic.paths.js
   Definuje instance komponent a propojení (potrubí).
*/
(function () {
  "use strict";

  // Pozice jsou středy komponent v souřadnicích SVG viewBox.
  const INSTANCES = [
    { id: "floor", tpl: "tplFloor", x: 100, y: 160 },
    { id: "dhw", tpl: "tplDHW", x: 220, y: 310, vars: { tempC: 48, recircAngle: 0, recircOn: true, recircMoving: true } },
    { id: "aku", tpl: "tplAKU", x: 720, y: 165 },

    // Boiler + pump rotation (deg)
    { id: "boiler", tpl: "tplBoiler", x: 330, y: 47, vars: { pumpAngle: 0 } },
    { id: "outdoor", tpl: "tplOutdoorSensor", x: 250, y: 21, vars: { tempC: -2.3 } },
    // Thermometers (ESP32 logika: Ta + klíčové teploty na uzlech)
    { id: "ts_boiler_T", tpl: "tplThermometer", x: 305, y: 125, name: "T_kotel", vars: { tempC: 55.0 } },
    { id: "ts_boiler_V", tpl: "tplThermometer", x: 355, y: 125, name: "V_kotel", vars: { tempC: 35.0 } },
    { id: "ts_floor_in", tpl: "tplThermometer", x: 210, y: 140, name: "PT_IN",   vars: { tempC: 32.0 } },
    { id: "ts_floor_out",tpl: "tplThermometer", x: 175, y: 80, name: "PT_OUT",  vars: { tempC: 28.0 } },
    { id: "ts_dhw",      tpl: "tplThermometer", x: 280, y: 270, name: "TUV",     vars: { tempC: 48.0 } },
    { id: "ts_aku_T",    tpl: "tplThermometer", x: 640, y: 70, name: "AKU_T",   vars: { tempC: null } },
    { id: "ts_aku_M",    tpl: "tplThermometer", x: 610, y: 150, name: "AKU_M",   vars: { tempC: null } },
    { id: "ts_aku_V",    tpl: "tplThermometer", x: 580, y: 230, name: "AKU_V",   vars: { tempC: null } },

    // Valves: geomRotateDeg + labelsTpl + name
    { id: "v1", type: "valve", x: 269, y: 160, geomRotateDeg: 180, labelsTpl: "tplValve_labels_V1", name: "V1", nameY: 20 },
    { id: "v3", type: "valve", x: 420, y: 125, geomRotateDeg: 0,  labelsTpl: "tplValve_labels_V2V3", name: "V3", nameY: -20 },
    { id: "v2", type: "valve", x: 530, y: 125, geomRotateDeg: 0,  labelsTpl: "tplValve_labels_V2V3", name: "V2", nameY: -20 },
    { id: "j1", tpl: "tplJunction3", rotateDeg: 90, x: 232, y: 200 },
    { id: "j2", tpl: "tplJunction3", x: 420, y: 200 },
    { id: "j3", tpl: "tplJunction3", x: 530, y: 200 },
  ];

  // Port-to-port propojení

  // =========================
  // Helpers pro snadné propoje
  // =========================
  function P(c, p){ return { c, p }; } // port reference

  // Normalize via list: accepts ["comp:PORT", {c,p}, ["comp","PORT"]]
  function normVia(via){
    if (!via) return [];
    if (!Array.isArray(via)) throw new Error("via must be array");
    return via.map(v => {
      if (typeof v === "string") return v; // parsed later in main (parsePortRef)
      if (Array.isArray(v) && v.length === 2) return `${v[0]}:${v[1]}`;
      if (typeof v === "object" && v.c && v.p) return `${v.c}:${v.p}`;
      throw new Error("Invalid via item: " + JSON.stringify(v));
    });
  }

  function connect(fromC, fromP, toC, toP, opts = {}){
    return {
      from: P(fromC, fromP),
      to:   P(toC, toP),
      cls: opts.cls || "pipe",
      marker: opts.marker || null,
      route: opts.route || "auto",
      via: normVia(opts.via),
      midX: opts.midX,
      router: opts.router || undefined
    };
  }

  function connectSupply(fromC, fromP, toC, toP, opts = {}){
    return connect(fromC, fromP, toC, toP, {
      ...opts,
      cls: opts.cls || "pipe supply",
      marker: opts.marker || "url(#arrow_supply)"
    });
  }

  function connectReturn(fromC, fromP, toC, toP, opts = {}){
    return connect(fromC, fromP, toC, toP, {
      ...opts,
      cls: opts.cls || "pipe return dashed",
      marker: opts.marker || "url(#arrow_return)"
    });
  }
  
  function connectPreheat(fromC, fromP, toC, toP, opts = {}){
    return connect(fromC, fromP, toC, toP, {
      ...opts,
      cls: opts.cls || "pipe preheat dashed",
      marker: opts.marker || "url(#arrow_preheat)"
    });
  }

  function connectWire(fromC, fromP, toC, toP, opts = {}){
    return connect(fromC, fromP, toC, toP, {
      ...opts,
      cls: opts.cls || "pipe dashed",
      marker: opts.marker || null
    });
  }

  window.SchemaHelpers = { P, connect, connectSupply, connectReturn, connectPreheat, connectWire };

  const CONNECTIONS = [
    window.SchemaHelpers.connectSupply("boiler", "T", "v1", "AB", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    // NOVÉ: teplá větev z V1-A do podlahovky (IN)
    window.SchemaHelpers.connectSupply("v1", "A", "floor", "IN", {
        route: "manhattan",
        router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectSupply("v1", "B", "dhw", "IN", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("floor", "OUT", "j1", "L", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("dhw", "OUT", "j1", "R", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("j1", "U", "j2", "L", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("j2", "R", "j3", "L", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("j2", "U", "v3", "A", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("j3", "U", "v2", "A", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectReturn("j3", "L", "aku", "V", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectPreheat("aku", "T", "v2", "B", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectPreheat("v2", "AB", "v3", "B", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    }),
    window.SchemaHelpers.connectPreheat("v3", "AB", "boiler", "V", {
      route: "manhattan",
      router: { corridors: { x: [], y: [] } }
    })
    
    // --- sensor wiring (signál) ---
    ,window.SchemaHelpers.connectWire("outdoor", "SIG", "boiler", "TA", { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_boiler_T", "SIG", "boiler", "T", { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_boiler_V", "SIG", "boiler", "V", { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_floor_in", "SIG", "floor", "IN", { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_floor_out","SIG", "floor", "OUT",{ cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_dhw",      "SIG", "dhw", "OUT",  { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_aku_T",    "SIG", "aku", "T",    { cls: "wire dashed", route: "manhattan" })
    ,window.SchemaHelpers.connectWire("ts_aku_V",    "SIG", "aku", "V",    { cls: "wire dashed", route: "auto" })
  ];


  // Volitelné "ruční" cesty (např. koncept vratky), které nejsou navázané na porty:
  const MANUAL_PATHS = [];


  // Preferované "koridory" pro router (v souřadnicích SVG):
  // - x/y: seznam souřadnic, kolem kterých má router tendenci vést potrubí
  // - corridorPenalty: jak moc penalizovat vzdálenost od koridoru (nižší = slabší přitažlivost)
  // - corridorSnap: vzdálenost (v px), do které router koridor "zvýhodní" nejvíc
  const ROUTER_OPTIONS = {
    grid: 10,
    maxIter: 60000,
    corridors: {
      x: [],   // svislý koridor u kotle (příklad)
      y: [],   // vodorovný koridor pro "vratku" (příklad)
    },
    corridorPenalty: 0.08,
    corridorSnap: 40,
    preferEdges: false
  };

  window.SchemaPaths = { INSTANCES, CONNECTIONS, MANUAL_PATHS, ROUTER_OPTIONS };
})();

