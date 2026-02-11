// thermometers_page.js
// Full page UI for configuring thermometer roles & sources.

window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  function roleTitle(role){
    const o = (App.roles?.ROLE_OPTIONS || []).find(x => x.v === role);
    return o ? o.t : String(role);
  }

  function fmtTemp(v){
    if (v == null || isNaN(v)) return '—';
    return `${Number(v).toFixed(2)} °C`;
  }

  function isTempSource(src){
    return /^temp[1-8]$/.test(String(src || ''));
  }

  function parseTempIdx(src){
    const m = String(src || '').match(/^temp([1-8])$/);
    if (!m) return -1;
    return Number(m[1]) - 1;
  }

  function buildSourceOptions(){
    const out = [
      { v:'none', t:'(nenastaveno)' },
      ...Array.from({length:8}).map((_,i)=>({ v:`temp${i+1}`, t:`Dallas T${i+1}` })),
      { v:'ble', t:'BLE' },
      { v:'mqtt1', t:'MQTT1' },
      { v:'mqtt2', t:'MQTT2' },
    ];
    return out;
  }

  function buildSelect(value, options){
    const sel = document.createElement('select');
    sel.className = 'sel';
    for (const o of options) {
      const opt = document.createElement('option');
      opt.value = o.v;
      opt.textContent = o.t;
      if (o.v === value) opt.selected = true;
      sel.appendChild(opt);
    }
    return sel;
  }

  function findAssignedSourceForRole(role){
    const r = String(role || '');
    const temp = App.roles?.tempRoles || [];
    for (let i=0;i<8;i++) {
      if (String(temp[i] || 'none') === r) return `temp${i+1}`;
    }
    if (String(App.roles?.bleRole || 'none') === r) return 'ble';
    const m = App.roles?.mqtt || [];
    for (let i=0;i<2;i++) {
      if (String(m[i]?.role || 'none') === r) return `mqtt${i+1}`;
    }
    return 'none';
  }

  function clearRoleEverywhere(role){
    const r = String(role || '');
    for (let i=0;i<8;i++) {
      if (String(App.roles.tempRoles[i] || 'none') === r) App.roles.tempRoles[i] = 'none';
    }
    if (String(App.roles.bleRole || 'none') === r) App.roles.bleRole = 'none';
    for (let i=0;i<2;i++) {
      if (String(App.roles.mqtt?.[i]?.role || 'none') === r) App.roles.mqtt[i].role = 'none';
    }
  }

  function assignRoleToSource(role, src){
    const r = String(role || '');
    const s = String(src || 'none');

    clearRoleEverywhere(r);

    if (s === 'none') return;
    if (isTempSource(s)) {
      const idx = parseTempIdx(s);
      if (idx >= 0 && idx < 8) App.roles.tempRoles[idx] = r;
      return;
    }
    if (s === 'ble') {
      App.roles.bleRole = r;
      return;
    }
    if (s === 'mqtt1' || s === 'mqtt2') {
      const mi = (s === 'mqtt1') ? 0 : 1;
      App.roles.mqtt[mi].role = r;
      return;
    }
  }

  function countRoleAssignments(){
    const map = new Map();
    const inc = (role, src) => {
      const r = String(role || 'none');
      if (!r || r === 'none') return;
      if (!map.has(r)) map.set(r, []);
      map.get(r).push(src);
    };
    for (let i=0;i<8;i++) inc(App.roles.tempRoles[i], `temp${i+1}`);
    inc(App.roles.bleRole, 'ble');
    for (let i=0;i<2;i++) inc(App.roles.mqtt?.[i]?.role, `mqtt${i+1}`);
    return map;
  }

  function makeTag(text, cls){
    const s = document.createElement('span');
    s.className = `tag ${cls || ''}`.trim();
    s.textContent = text;
    return s;
  }

  App.pages.thermometers = {
    _root: null,
    _els: {},
    _dallas: null,

    async mount(root){
      this._root = root;
      root.innerHTML = `
        <div class="grid12">
          <div class="col12">
            <div class="card">
              <div class="card__head">
                <div class="card__title">Teploměry</div>
                <div class="card__actions">
                  <button class="btn" data-act="load">Načíst config</button>
                  <button class="btn btn--ghost" data-act="save" disabled>Uložit</button>
                  <button class="btn" data-act="refresh">Obnovit stav</button>
                </div>
              </div>
              <div class="card__body">
                <div class="muted">Tahle stránka mapuje <b>role</b> na zdroje (Dallas T1..T8, BLE, MQTT). Role se ukládají do <span class="mono">/config.json</span> a používají se napříč celým firmwarem (fallback pro Equitherm atd.).</div>
                <div class="row" style="margin-top:10px">
                  <div class="muted" data-msg></div>
                </div>

                <div class="tblWrap" style="margin-top:10px">
                  <table class="tbl">
                    <thead>
                      <tr>
                        <th style="width:240px">Role</th>
                        <th style="width:220px">Zdroj</th>
                        <th style="width:220px">Aktuální stav</th>
                        <th>ID</th>
                      </tr>
                    </thead>
                    <tbody data-rows></tbody>
                  </table>
                </div>
              </div>
            </div>
          </div>

<div class="col12">
  <div class="card">
    <div class="card__head">
      <div class="card__title">BLE venkovní senzor</div>
      <div class="card__actions">
        <span class="tag" data-bleLive>—</span>
      </div>
    </div>
    <div class="card__body">
      <div class="muted">Nastavení se ukládá do <span class="mono">/config.json</span> v sekcích <span class="mono">ble</span> (příjem) a <span class="mono">bleThermometer</span> (role/ID). Pokud nastavíš roli <b>Venkovní teplota</b>, ekviterm ji může použít jako venkovní zdroj.</div>

      <div class="kv" style="grid-template-columns: 220px 1fr; margin-top:10px">
        <div class="k">Povolit BLE příjem</div>
        <div class="v"><label class="switch"><input type="checkbox" data-ble="enabled"><span></span></label></div>

        <div class="k">Typ dekodéru</div>
        <div class="v">
          <select class="sel" data-ble="type">
            <option value="auto">auto</option>
            <option value="esp_meteo_mfg">esp_meteo_mfg</option>
            <option value="atc_mitherm">atc_mitherm</option>
          </select>
        </div>

        <div class="k">Filtr MAC (volitelně)</div>
        <div class="v"><input class="inp mono" style="max-width:260px" placeholder="AA:BB:CC:DD:EE:FF" data-ble="mac"></div>

        <div class="k">Max stáří dat</div>
        <div class="v"><input class="inp mono" style="max-width:160px" type="number" min="1000" step="1000" data-ble="maxAgeMs"> ms</div>

        <div class="k">Role BLE teploměru</div>
        <div class="v"><select class="sel" data-bleT="role"></select></div>

        <div class="k">BLE ID (volitelně)</div>
        <div class="v">
          <input class="inp mono" style="max-width:260px" placeholder="meteo / meteo.tempC" data-bleT="id">
          <span class="muted" style="margin-left:8px">doporučeno: <span class="mono">meteo</span></span>
        </div>
      </div>

      <details style="margin-top:10px">
        <summary class="muted" style="cursor:pointer">Pokročilé: BLE Mesh relay</summary>
        <div class="kv" style="grid-template-columns: 220px 1fr; margin-top:10px">
          <div class="k">Mesh enabled</div>
          <div class="v"><label class="switch"><input type="checkbox" data-mesh="enabled"><span></span></label></div>

          <div class="k">Relay</div>
          <div class="v"><label class="switch"><input type="checkbox" data-mesh="relay"><span></span></label></div>

          <div class="k">TTL</div>
          <div class="v"><input class="inp mono" style="max-width:120px" type="number" min="1" max="10" step="1" data-mesh="ttl"></div>

          <div class="k">Adv interval</div>
          <div class="v"><input class="inp mono" style="max-width:160px" type="number" min="100" max="10000" step="50" data-mesh="advIntervalMs"> ms</div>

          <div class="k">Min relay RSSI</div>
          <div class="v"><input class="inp mono" style="max-width:160px" type="number" min="-120" max="0" step="1" data-mesh="minRelayRssi"> dBm</div>

          <div class="k">Prefer direct</div>
          <div class="v"><label class="switch"><input type="checkbox" data-mesh="preferDirect"><span></span></label></div>
        </div>
      </details>

      <div class="hint" style="margin-top:10px" data-bleHint>—</div>
    </div>
  </div>
</div>

          <div class="col12">
            <div class="card">
              <div class="card__head">
                <div class="card__title">Dallas diagnostika (GPIO0..GPIO3)</div>
              </div>
              <div class="card__body" data-dallas></div>
            </div>
          </div>
        </div>
      `;

      this._els.msg = root.querySelector('[data-msg]');
      this._els.tbody = root.querySelector('[data-rows]');
      this._els.dallas = root.querySelector('[data-dallas]');
      this._els.bleLive = root.querySelector('[data-bleLive]');
      this._els.bleHint = root.querySelector('[data-bleHint]');
      this._els.bleEnabled = root.querySelector('[data-ble="enabled"]');
      this._els.bleType = root.querySelector('[data-ble="type"]');
      this._els.bleMac = root.querySelector('[data-ble="mac"]');
      this._els.bleMaxAge = root.querySelector('[data-ble="maxAgeMs"]');
      this._els.bleTRole = root.querySelector('[data-bleT="role"]');
      this._els.bleTId = root.querySelector('[data-bleT="id"]');
      this._els.meshEnabled = root.querySelector('[data-mesh="enabled"]');
      this._els.meshRelay = root.querySelector('[data-mesh="relay"]');
      this._els.meshTtl = root.querySelector('[data-mesh="ttl"]');
      this._els.meshAdv = root.querySelector('[data-mesh="advIntervalMs"]');
      this._els.meshMinRssi = root.querySelector('[data-mesh="minRelayRssi"]');
      this._els.meshPreferDirect = root.querySelector('[data-mesh="preferDirect"]');
      this._els.btnLoad = root.querySelector('[data-act=load]');
      this._els.btnSave = root.querySelector('[data-act=save]');
      this._els.btnRefresh = root.querySelector('[data-act=refresh]');

      this._els.btnLoad.onclick = async () => {
        this._els.btnLoad.disabled = true;
        this._setMsg('Načítám…');
        try {
          await App.roles.loadConfig();
          await App.api.getJson('/api/ble/status').then(x=>{App.state.bleStatus=x;}).catch(_=>{App.state.bleStatus=null;});
          await this._loadDallas();
          this._render();
          this._els.btnSave.disabled = false;
          this._setMsg('Načteno.');
        } catch(e) {
          this._setMsg(`Chyba načtení: ${e.message || e}`);
        } finally {
          this._els.btnLoad.disabled = false;
        }
      };

      this._els.btnSave.onclick = async () => {
        this._els.btnSave.disabled = true;
        this._setMsg('Ukládám…');
        try {
          await App.roles.saveConfig();
          this._setMsg('Uloženo.');
        } catch(e) {
          this._setMsg(`Chyba uložení: ${e.message || e}`);
        } finally {
          this._els.btnSave.disabled = false;
        }
      };

      this._els.btnRefresh.onclick = async () => {
        this._els.btnRefresh.disabled = true;
        try {
          await App.api.getJson('/api/ble/status').then(x=>{App.state.bleStatus=x;}).catch(_=>{App.state.bleStatus=null;});
          await this._loadDallas();
          this._renderBleConfig();
          this._renderDallasDiag();
          this._renderStateOnly();
        } catch(_) {
        } finally {
          this._els.btnRefresh.disabled = false;
        }
      };

      // Initial paint (without forcing a config read)
      try {
        if (!App.roles.loaded) await App.roles.loadConfig();
      } catch(_) { /* config.json might not exist yet */ }
      await this._loadDallas();
      this._render();
      this._els.btnSave.disabled = false;
    },

    update(){
      // live update of the "Aktuální stav" column via /api/fast (SSE)
      this._renderStateOnly();
    },

    _setMsg(text){
      if (this._els.msg) this._els.msg.textContent = text || '';
    },

    async _loadDallas(){
      try {
        this._dallas = await App.api.getJson('/api/dallas');
      } catch(e) {
        this._dallas = null;
      }
    },

    _render(){
      this._renderRows();
      this._renderBleConfig();
      this._renderDallasDiag();
      this._renderStateOnly();
    },

    _renderBleConfig(){
  const r = App.roles || {};
  // populate role select (same options as roles.js)
  if (this._els.bleTRole) {
    this._els.bleTRole.innerHTML = '';
    for (const o of (r.ROLE_OPTIONS || [])) {
      const opt = document.createElement('option');
      opt.value = o.v;
      opt.textContent = o.t;
      if (o.v === (r.bleRole || 'outdoor')) opt.selected = true;
      this._els.bleTRole.appendChild(opt);
    }
  }
  if (this._els.bleEnabled) this._els.bleEnabled.checked = (r.ble?.enabled != null) ? !!r.ble.enabled : true;
  if (this._els.bleType) this._els.bleType.value = String(r.ble?.type || 'auto');
  if (this._els.bleMac) this._els.bleMac.value = String(r.ble?.mac || '');
  if (this._els.bleMaxAge) this._els.bleMaxAge.value = Number(r.ble?.maxAgeMs ?? 600000);
  if (this._els.bleTId) this._els.bleTId.value = String(r.bleId || '');

  if (this._els.meshEnabled) this._els.meshEnabled.checked = !!(r.ble?.mesh?.enabled);
  if (this._els.meshRelay) this._els.meshRelay.checked = !!(r.ble?.mesh?.relay);
  if (this._els.meshTtl) this._els.meshTtl.value = Number(r.ble?.mesh?.ttl ?? 3);
  if (this._els.meshAdv) this._els.meshAdv.value = Number(r.ble?.mesh?.advIntervalMs ?? 1200);
  if (this._els.meshMinRssi) this._els.meshMinRssi.value = Number(r.ble?.mesh?.minRelayRssi ?? -95);
  if (this._els.meshPreferDirect) this._els.meshPreferDirect.checked = (r.ble?.mesh?.preferDirect != null) ? !!r.ble.mesh.preferDirect : true;

  const bindDirty = () => { if(this._els.btnSave) this._els.btnSave.disabled = false; };

  const onChange = () => {
    r.ble = r.ble || {};
    r.ble.mesh = r.ble.mesh || {};
    r.ble.enabled = !!this._els.bleEnabled?.checked;
    r.ble.type = String(this._els.bleType?.value || 'auto');
    r.ble.mac = String(this._els.bleMac?.value || '');
    r.ble.maxAgeMs = Number(this._els.bleMaxAge?.value || 0);

    r.bleRole = String(this._els.bleTRole?.value || 'outdoor');
    r.bleId = String(this._els.bleTId?.value || '');

    r.ble.mesh.enabled = !!this._els.meshEnabled?.checked;
    r.ble.mesh.relay = !!this._els.meshRelay?.checked;
    r.ble.mesh.ttl = Number(this._els.meshTtl?.value || 3);
    r.ble.mesh.advIntervalMs = Number(this._els.meshAdv?.value || 1200);
    r.ble.mesh.minRelayRssi = Number(this._els.meshMinRssi?.value || -95);
    r.ble.mesh.preferDirect = !!this._els.meshPreferDirect?.checked;
    bindDirty();
  };

  const wire = (el, ev='change') => { if(el) el.addEventListener(ev, onChange); };
  wire(this._els.bleEnabled);
  wire(this._els.bleType);
  wire(this._els.bleMac, 'input');
  wire(this._els.bleMaxAge, 'input');
  wire(this._els.bleTRole);
  wire(this._els.bleTId, 'input');
  wire(this._els.meshEnabled);
  wire(this._els.meshRelay);
  wire(this._els.meshTtl, 'input');
  wire(this._els.meshAdv, 'input');
  wire(this._els.meshMinRssi, 'input');
  wire(this._els.meshPreferDirect);

  // live status hint
  const ble = App.state?.bleStatus?.data;
  if (this._els.bleLive) {
    const ok = !!(App.state?.bleStatus?.ok);
    const scanning = !!ble?.scanning;
    this._els.bleLive.className = 'tag ' + (ok && scanning ? 'ok' : 'bad');
    this._els.bleLive.textContent = ok ? (scanning ? 'scanning' : 'stopped') : 'offline';
  }
  if (this._els.bleHint) {
    if (!ble) this._els.bleHint.textContent = 'BLE status není dostupný (zkontroluj /api/ble/status).';
    else this._els.bleHint.textContent = `RSSI ${ble.lastRssi ?? '—'} dBm, age ${ble.ageMs ?? '—'} ms, T ${ble.reading?.tempC ?? '—'} °C (${ble.reading?.src || '—'})`;
  }
},

_renderRows(){
      const rows = this._els.tbody;
      if (!rows) return;
      rows.innerHTML = '';

      const roleList = (App.roles?.ROLE_OPTIONS || []).map(x => x.v).filter(r => r !== 'none');
      const srcOpts = buildSourceOptions();
      const byRole = countRoleAssignments();

      for (const role of roleList) {
        const tr = document.createElement('tr');
        tr.dataset.role = role;

        // Role
        const tdRole = document.createElement('td');
        tdRole.innerHTML = `<div class="cellStack"><div>${roleTitle(role)}</div><div class="muted mono">${role}</div></div>`;

        // Source select
        const tdSrc = document.createElement('td');
        const assigned = findAssignedSourceForRole(role);
        const sel = buildSelect(assigned, srcOpts);
        sel.onchange = () => {
          assignRoleToSource(role, sel.value);
          // re-render to keep other rows consistent (uniqueness)
          this._renderRows();
          this._renderStateOnly();
        };
        const stack = document.createElement('div');
        stack.className = 'cellStack';
        stack.appendChild(sel);

        const srcs = byRole.get(role) || [];
        if (srcs.length > 1) {
          stack.appendChild(makeTag(`Pozor: role je přiřazená vícekrát (${srcs.join(', ')})`, 'bad'));
        }
        tdSrc.appendChild(stack);

        // State
        const tdState = document.createElement('td');
        tdState.innerHTML = `<div class="cellStack"><div data-state></div></div>`;

        // ID / details
        const tdId = document.createElement('td');
        tdId.appendChild(this._buildIdCellForRole(role, assigned));

        tr.appendChild(tdRole);
        tr.appendChild(tdSrc);
        tr.appendChild(tdState);
        tr.appendChild(tdId);
        rows.appendChild(tr);
      }
    },

    _buildIdCellForRole(role, src){
      const wrap = document.createElement('div');
      wrap.className = 'cellStack';

      if (isTempSource(src)) {
        const idx = parseTempIdx(src);
        const rom = this._dallas?.slots?.[idx]?.romHex || '';
        if (rom) {
          const d = document.createElement('div');
          d.className = 'mono';
          d.textContent = rom;
          wrap.appendChild(d);
        } else {
          const d = document.createElement('div');
          d.className = 'muted';
          d.textContent = 'ROM: —';
          wrap.appendChild(d);
        }
        return wrap;
      }

      if (src === 'ble') {
        const row = document.createElement('div');
        row.className = 'cellStack';
        const inp = document.createElement('input');
        inp.type = 'text';
        inp.placeholder = 'BLE ID (volitelné)';
        inp.value = App.roles.bleId || '';
        inp.oninput = () => App.roles.bleId = inp.value;
        row.appendChild(inp);
        const note = document.createElement('div');
        note.className = 'muted';
        note.textContent = 'Prázdné = výchozí BLE meteo.';
        row.appendChild(note);
        return row;
      }

      if (src === 'mqtt1' || src === 'mqtt2') {
        const mi = (src === 'mqtt1') ? 0 : 1;
        const inpTopic = document.createElement('input');
        inpTopic.type = 'text';
        inpTopic.placeholder = 'MQTT topic';
        inpTopic.value = App.roles.mqtt?.[mi]?.topic || '';
        inpTopic.oninput = () => App.roles.mqtt[mi].topic = inpTopic.value;
        wrap.appendChild(inpTopic);

        const inpKey = document.createElement('input');
        inpKey.type = 'text';
        inpKey.placeholder = 'JSON klíč (vol.)';
        inpKey.value = App.roles.mqtt?.[mi]?.jsonKey || '';
        inpKey.oninput = () => App.roles.mqtt[mi].jsonKey = inpKey.value;
        wrap.appendChild(inpKey);

        return wrap;
      }

      wrap.innerHTML = `<div class="muted">—</div>`;
      return wrap;
    },

    _renderStateOnly(){
      const tbody = this._els.tbody;
      if (!tbody) return;
      const fast = App.state?.fast || null;
      const b = fast?.b || {};

      tbody.querySelectorAll('tr[data-role]').forEach(tr => {
        const role = tr.dataset.role;
        const src = findAssignedSourceForRole(role);
        const host = tr.querySelector('[data-state]');
        if (!host) return;

        // Default
        host.innerHTML = '';

        if (src === 'none') {
          host.appendChild(makeTag('nenastaveno', ''));
          return;
        }

        if (isTempSource(src)) {
          const idx = parseTempIdx(src);
          const v = fast?.t?.[idx];
          const ok = !!fast?.tv?.[idx];
          const tag = makeTag(ok ? 'OK' : 'invalid', ok ? 'ok' : 'bad');
          const val = document.createElement('span');
          val.textContent = fmtTemp(v);
          const row = document.createElement('div');
          row.className = 'cellRow';
          row.appendChild(tag);
          row.appendChild(val);
          host.appendChild(row);
          return;
        }

        if (src === 'ble') {
          const ok = !!b.ok;
          const fresh = !!b.fr;
          const tag = makeTag(ok ? (fresh ? 'fresh' : 'stale') : 'no data', (ok && fresh) ? 'ok' : 'bad');
          const val = document.createElement('span');
          val.textContent = fmtTemp(b.t);
          const age = document.createElement('span');
          age.className = 'muted';
          age.textContent = (b.a != null) ? `${Math.round(Number(b.a)/1000)} s` : '';

          const row = document.createElement('div');
          row.className = 'cellRow';
          row.appendChild(tag);
          row.appendChild(val);
          if (age.textContent) row.appendChild(age);
          host.appendChild(row);
          return;
        }

        if (src === 'mqtt1' || src === 'mqtt2') {
          host.appendChild(makeTag('bez live dat v UI', ''));
          return;
        }
      });
    },

    _renderDallasDiag(){
      const host = this._els.dallas;
      if (!host) return;
      if (!this._dallas || !Array.isArray(this._dallas.gpios)) {
        host.innerHTML = '<div class="muted">/api/dallas není dostupné.</div>';
        return;
      }

      host.innerHTML = '';
      for (const g of this._dallas.gpios) {
        const card = document.createElement('div');
        card.className = 'tile';
        const h = document.createElement('div');
        h.className = 't';
        const head = document.createElement('div');
        head.className = 'head';
        head.innerHTML = `<div class="id">GPIO${g.gpio}</div><div class="desc">${g.status} • zařízení: ${g.deviceCount}</div>`;
        const badges = document.createElement('div');
        badges.className = 'badges';
        const st = String(g.status || '').toUpperCase();
        badges.appendChild(makeTag(st, (st === 'OK') ? 'ok' : (st === 'NO_DEVICES' ? '' : 'bad')));
        h.appendChild(head);
        h.appendChild(badges);

        const body = document.createElement('div');
        body.className = 'cellStack';
        const devs = Array.isArray(g.devices) ? g.devices : [];
        if (!devs.length) {
          const d = document.createElement('div');
          d.className = 'muted';
          d.textContent = '—';
          body.appendChild(d);
        } else {
          for (const d of devs) {
            const row = document.createElement('div');
            row.className = 'cellRow';
            const ok = !!d.valid;
            row.appendChild(makeTag(ok ? 'OK' : 'invalid', ok ? 'ok' : 'bad'));
            const rom = document.createElement('span');
            rom.className = 'mono';
            rom.textContent = d.romHex || '—';
            const val = document.createElement('span');
            val.textContent = fmtTemp(d.tempC);
            row.appendChild(rom);
            row.appendChild(val);
            body.appendChild(row);
          }
        }

        card.appendChild(h);
        card.appendChild(body);
        host.appendChild(card);
      }
    },
  };
})();
