/* schematic.templates.js
   Obsahuje SVG <defs> šablony + markery.
   Použití: SchemaTemplates.apply(svg);
*/
(function () {
  "use strict";

  const DEFS_HTML = `
    <marker id="arrow_supply" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="4" markerHeight="4" orient="auto-start-reverse">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="var(--bad)" opacity="0.9"/>
    </marker>
    <marker id="arrow_return" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="2" markerHeight="8" orient="auto-start-reverse">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="var(--accent)" opacity="0.9"/>
    </marker>
    <marker id="arrow_preheat" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="3" markerHeight="3" orient="auto-start-reverse">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="var(--violet)" opacity="0.9"/>
    </marker>

    <pattern id="bgGrid" width="20" height="20" patternUnits="userSpaceOnUse">
      <path d="M 20 0 H 0 V 20" fill="none" stroke="rgba(255,255,255,.04)" stroke-width="1"/>
    </pattern>

    <marker id="arrow_internal" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="4" markerHeight="4" orient="auto">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="currentColor" opacity="0.9"/>
    </marker>
    
     <!-- === Junction / T-spoj (3-bodový) – porty L/R/U === -->
    <g id="tplJunction3" class="junctionTpl">
        <path class="portLine" d="M -10 0 H 10"/>
        <path class="portLine" d="M 0 -10 V 0"/>
        <circle class="portDot" data-port="L" data-dir="left"  cx="-10" cy="0"  r="3"/>
        <circle class="portDot" data-port="R" data-dir="right" cx="10"  cy="0"  r="3"/>
        <circle class="portDot" data-port="U" data-dir="up"    cx="0"   cy="-10" r="3"/>
    </g>
    
    <!-- === Junction / spojka (pro slévání/rozbočení větví) === -->
    <g id="tplJunction4" class="junctionTpl">
        <path class="portLine" d="M -10 0 H 10"/>
        <path class="portLine" d="M 0 -10 V 10"/>
        <circle class="portDot" data-port="L" data-dir="left"  cx="-10" cy="0"  r="3"/>
        <circle class="portDot" data-port="R" data-dir="right" cx="10"  cy="0"  r="3"/>
        <circle class="portDot" data-port="U" data-dir="up"    cx="0"   cy="-10" r="3"/>
        <circle class="portDot" data-port="D" data-dir="down"  cx="0"   cy="10"  r="3"/>
    </g>

    <!-- === 3c ventil: porty pojmenované podle zobrazení (AB vlevo, B vpravo, A dole) === -->
    <g id="tplValve3Tri_geom" class="valve3mix">
      <path class="v3tri-port" d="M -32 0 H -50"/>
      <circle class="v3tri-dot" data-port="AB" data-dir="left" cx="-50" cy="0" r="3"/>

      <path class="v3tri-port" d="M 50 0 H 32"/>
      <circle class="v3tri-dot" data-port="B" data-dir="right" cx="50" cy="0" r="3"/>

      <path class="v3tri-port" d="M 0 32 V 50"/>
      <circle class="v3tri-dot" data-port="A" data-dir="down" cx="0" cy="50" r="3"/>

      <polygon class="v3tri-stroke v3tri-nofill" points="-10,0  -40,-12  -40,12" transform="translate(9 0)"/>
      <polygon class="v3tri-fill" points="10,0   40,-12   40,12" transform="translate(-9 0)"/>
      <polygon class="v3tri-fill" points="0,10   -12,40   12,40" transform="translate(1 -9)"/>
    </g>

    <!-- popisky – V1 (po rotaci geometrie o +90°: AB nahoře, A vlevo, B dole) -->
    <g id="tplValve_labels_V1" class="valve3mix">
      <text class="v3tri-label" x="0"   y="-54" text-anchor="middle">AB</text>
      <text class="v3tri-label" x="-50" y="-10" text-anchor="middle">A</text>
      <text class="v3tri-label" x="50"   y="-10"  text-anchor="middle">B</text>
    </g>

    <!-- popisky – V2/V3 (AB vlevo, A dole, B vpravo) -->
    <g id="tplValve_labels_V2V3" class="valve3mix">
      <text class="v3tri-label" x="-50" y="-10" text-anchor="middle">AB</text>
      <!-- A: blíž k dolnímu portu, zleva -->
      <text class="v3tri-label" x="-12" y="60" text-anchor="end">A</text>
      <text class="v3tri-label" x="50"  y="-10" text-anchor="middle">B</text>
    </g>

    <!-- === indikátor polohy 3c ventilu (kruhové pole) === -->
    <g id="tplValveGauge" class="valveGauge">
      <circle class="gaugeBg" cx="0" cy="0" r="20"/>
      <circle class="gaugeTrack" cx="0" cy="0" r="14"/>
      <circle class="gaugeArc" cx="0" cy="0" r="14" pathLength="100" transform="rotate(-90)"/>
      <text class="gaugeText" data-bind="valvePos" x="0" y="4" text-anchor="middle">0%</text>
    </g>

    <!-- === kotel: 60x90, porty T/V, vnitřní pumpa (rotace přes --pumpAngle) === -->
    <g id="tplBoiler" class="boilerTpl">
      <rect class="boilerBody" x="-30" y="-45" width="60" height="90" rx="12"/>

      <!-- levý spodní port: T (teplá voda / supply) -->
      <path class="boilerPort boilerPortSupply" d="M -15 45 V 60"/>
      <circle class="boilerDot boilerDotSupply" data-port="T" data-dir="down" cx="-15" cy="60" r="3"/>
      <text class="boilerLabel" x="-25" y="60" text-anchor="middle">T</text>

      <!-- pravý spodní port: V (vratka / return) -->
      <path class="boilerPort boilerPortReturn" d="M 15 45 V 60"/>
      <circle class="boilerDot boilerDotReturn" data-port="V" data-dir="down" cx="15" cy="60" r="3"/>
      <text class="boilerLabel" x="25" y="60" text-anchor="middle">V</text>

      <!-- port venkovního čidla (Ta) – levá strana, cca 1/4 od spodu -->
      <path class="boilerPort boilerPortSensor" d="M -30 22 H -46"/>
      <circle class="boilerDot boilerDotSensor" data-port="TA" data-dir="left" cx="-46" cy="22" r="3"/>
      <text class="boilerLabel" x="-46" y="12" text-anchor="middle">Ta</text>

      <text class="boilerTitle" x="0" y="-18" text-anchor="middle">Kotel</text>
      <text class="boilerSub" x="0" y="2" text-anchor="middle">—</text>

      <!-- vnitřní tok: V (vratka) -> pumpa -> T (teplá voda) -->
      <path class="boilerFlow boilerFlowReturn" d="M 15 60 V 22 H 10" marker-end="url(#arrow_internal)"/>
      <path class="boilerFlow boilerFlowSupply" d="M -10 22 H -15 V 60" marker-end="url(#arrow_supply)"/>

      <g class="pumpSym" transform="translate(0 22)">
        <!-- statická rotace (nastavení směru) -->
        <g class="pumpRotStatic">
          <!-- animované otáčení (proti směru hodinových ručiček) -->
          <g class="pumpRotSpin">
            <!-- základní směr otočený o 180° -->
            <g transform="rotate(180)">
              <circle class="pumpCircle" cx="0" cy="0" r="16"/>
              <path class="pumpTri" d="M -6 -8 L 10 0 L -6 8 Z"/>
            </g>
          </g>
        </g>
      </g>
    </g>

    <!-- === Venkovní čidlo teploty: kruh + hodnota, port zespod === -->
    <g id="tplOutdoorSensor" class="outdoorTpl">
      <circle class="outdoorBody" cx="0" cy="0" r="18"/>
      <text class="outdoorValue" data-bind="temp" x="0" y="5" text-anchor="middle">—</text>

      <path class="portLine outdoorPort" d="M 0 18 V 32"/>
      <circle class="portDot outdoorDot" data-port="SIG" data-dir="down" cx="0" cy="32" r="3"/>
      <circle class="portRing" cx="0" cy="32" r="7"/>
    </g>
    
    <!-- === Teploměr (Dallas/NTC/MQTT) – hodnota + jméno, port SIG zespodu === -->
    <g id="tplThermometer" class="thermoTpl">
      <circle class="thermoBody" cx="0" cy="0" r="14"/>
      <text class="thermoValue" data-bind="temp" x="0" y="4" text-anchor="middle">—</text>
      <text class="thermoLabel" data-bind="name" x="0" y="24" text-anchor="middle">T</text>
      <path class="portLine thermoPort" d="M 0 14 V 26"/>
      <circle class="portDot thermoDot" data-port="SIG" data-dir="down" cx="0" cy="26" r="3"/>
      <circle class="portRing" cx="0" cy="26" r="7"/>
    </g>

    <!-- === Podlahovka: porty IN/OUT vpravo napojené na symbol smyčky === -->
    <g id="tplFloor" class="floorTpl">
      <rect class="comp" x="-60" y="-45" width="120" height="90" rx="14"/>
      <text class="label" x="0" y="-8" text-anchor="middle"> </text>
      <text class="label" x="0" y="12" text-anchor="middle"> </text>
      <text class="sublabel" x="0" y="32" text-anchor="middle">—</text>

      <g class="floorSymbol" opacity="0.95">
        <!-- supply (teplá) -->
        <path class="floorLoop floorLoopSupply"
              d="M 44 -20
                 H -44
                 V 26
                 H 34
                 V -16
                 H -34
                 V 16
                 H 24
                 V -6
                 H -24
                 V 6
                 H 14
                 V -2
                 H -14" />
        <!-- return (vratka) -->
        <path class="floorLoop floorLoopReturn"
              d="M 44 20
                 H -44
                 V 32
                 H 40
                 V -14
                 H -40
                 V 20
                 H 30
                 V -4
                 H -30
                 V 10
                 H 20
                 V 2
                 H -20" />
      </g>

      <!-- IN (nahoře vpravo) -->
      <path class="portLine" d="M 44 -20 H 60"/>
      <circle class="portDot" data-port="IN" data-dir="right" cx="60" cy="-20" r="3"/>
      <circle class="portRing" cx="60" cy="-20" r="7"/>

      <!-- OUT (dole vpravo) -->
      <path class="portLine" d="M 44 20 H 60"/>
      <circle class="portDot" data-port="OUT" data-dir="right" cx="60" cy="20" r="3"/>
      <circle class="portRing" cx="60" cy="20" r="7"/>
    </g>

    <!-- === DHW: horní IN/OUT + boční RECIRC_OUT/RECIRC_IN (vlevo) + integrované recirc čerpadlo === -->
    <g id="tplDHW" class="dhwTpl">
      <!-- Fill (barva dle teploty) -->
      <rect class="dhwFill" x="-35" y="-85" width="70" height="170" rx="18"/>

      <!-- Body -->
      <rect class="comp" x="-35" y="-85" width="70" height="170" rx="18"/>
      <text class="label" x="0" y="-30" text-anchor="middle">TUV</text>
      <text class="sublabel" x="0" y="12" text-anchor="middle">—</text>

      <!-- TOP ports -->
      <path class="portLine" d="M -12 -67 V -85"/>
      <circle class="portDot" data-port="IN" data-dir="up" cx="-12" cy="-85" r="3"/>
      <circle class="portRing" cx="-12" cy="-85" r="7"/>

      <path class="portLine" d="M 12 -67 V -85"/>
      <circle class="portDot" data-port="OUT" data-dir="up" cx="12" cy="-85" r="3"/>
      <circle class="portRing" cx="12" cy="-85" r="7"/>

      <!-- RECIRC porty: z levé strany doprostřed nádrže -->
      <path class="portLine" d="M -17 -8 H -35"/>
      <circle class="portDot" data-port="RECIRC_OUT" data-dir="left" cx="-35" cy="-8" r="3"/>
      <circle class="portRing" cx="-35" cy="-8" r="7"/>

      <path class="portLine" d="M -17 8 H -35"/>
      <circle class="portDot" data-port="RECIRC_IN" data-dir="left" cx="-35" cy="8" r="3"/>
      <circle class="portRing" cx="-35" cy="8" r="7"/>

      <!-- Integrované cirkulační čerpadlo vlevo od nádrže -->
      <g class="recircInDhw" transform="translate(-62 0)">
        <!-- pump symbol: statická rotace (--recircAngle) + volitelná animace při běhu -->
        <g class="pumpRotStatic">
          <g class="pumpRotSpin">
            <circle class="recircBody" cx="0" cy="0" r="14"/>
            <path class="recircTri" d="M -4 -6 L 8 0 L -4 6 Z"/>
          </g>
        </g>

        <text class="recircLbl" x="0" y="30" text-anchor="middle">recirc</text>

        <!-- pump porty směrem doprava (napojení na nádrž) -->
        <path class="portLine" d="M 14 -6 H 26"/>
        <circle class="portDot" data-port="RECIRC_PUMP_IN" data-dir="right" cx="26" cy="-6" r="3"/>

        <path class="portLine" d="M 14 6 H 26"/>
        <circle class="portDot" data-port="RECIRC_PUMP_OUT" data-dir="right" cx="26" cy="6" r="3"/>
      </g>

      <!-- interní vizuální propojení: nádrž <-> recirc pumpa -->
      <path class="dhwInternal" d="M -35 -8 L -36 -6"/>
      <path class="dhwInternal" d="M -35 8 L -36 6"/>
    </g>

    <!-- === Recirc čerpadlo: samostatná šablona (volitelně) === -->
    <g id="tplRecircPump" class="recircTpl">
      <circle class="recircBody" cx="0" cy="0" r="14"/>
      <path class="recircTri" d="M -4 -6 L 8 0 L -4 6 Z"/>

      <path class="portLine" d="M 14 -6 H 26"/>
      <circle class="portDot" data-port="IN" data-dir="right" cx="26" cy="-6" r="3"/>

      <path class="portLine" d="M 14 6 H 26"/>
      <circle class="portDot" data-port="OUT" data-dir="right" cx="26" cy="6" r="3"/>

      <text class="recircLbl" x="0" y="30" text-anchor="middle">recirc</text>
    </g>

    <!-- === AKU: porty T/V + topné těleso (HEAT) === -->
    <g id="tplAKU" class="akuTpl">
      <rect class="comp" x="-105" y="-150" width="210" height="300" rx="22"/>
      <text class="label" x="0" y="-95" text-anchor="middle">AKU Nádrž</text>
      <text class="label" x="0" y="-75" text-anchor="middle">(akumulační)</text>
      <text class="sublabel" x="0" y="-52" text-anchor="middle">—</text>

      <!-- horní: T (výstup teplé / předehřáté vody) -->
      <path class="portLine akuPortSupply" d="M -87 -40 H -105"/>
      <circle class="portDot akuDotSupply" data-port="T" data-dir="left" cx="-105" cy="-40" r="3"/>
      <circle class="portRing" cx="-105" cy="-40" r="7"/>
      <text class="akuPortLabel" x="-105" y="-52" text-anchor="middle">T</text>

      <!-- spodní: V (vratka) -->
      <path class="portLine akuPortReturn" d="M -87 40 H -105"/>
      <circle class="portDot akuDotReturn" data-port="V" data-dir="left" cx="-105" cy="40" r="3"/>
      <circle class="portRing" cx="-105" cy="40" r="7"/>
      <text class="akuPortLabel" x="-105" y="58" text-anchor="middle">V</text>

      <!-- Topné těleso / spirála (V -> T) -->
      <g class="akuHeater">
        <path class="akuHeaterStroke"
              d="M -87 40
                 H -55
                 C -15 40 -15 28 25 28
                 C 65 28 65 16 25 16
                 C -15 16 -15 4 25 4
                 C 65 4 65 -8 25 -8
                 C -15 -8 -15 -20 25 -20
                 C 65 -20 65 -32 25 -32
                 C -15 -32 -15 -40 -55 -40
                 H -87"
        />
      </g>
</g>
  `;

  function apply(svg) {
    const defs = svg.querySelector("defs");
    if (!defs) throw new Error("SVG neobsahuje <defs>.");
    defs.innerHTML = DEFS_HTML;
  }

  window.SchemaTemplates = { apply };
})();
