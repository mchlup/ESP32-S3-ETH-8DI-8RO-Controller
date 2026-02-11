// roles.js
window.App = window.App || {};

App.roles = {
  ROLE_OPTIONS: [
    { v: 'none',       t: '(nepoužito)' },
    { v: 'outdoor',    t: 'Venkovní teplota' },
    { v: 'flow',       t: 'Topná voda za směšovačem (flow)' },
    { v: 'boiler_out', t: 'Výstup z kotle' },
    { v: 'return',     t: 'Zpátečka' },
    { v: 'dhw_tank',   t: 'Bojler TUV' },
    { v: 'dhw_return', t: 'Vratka TUV (cirkulace)' },
    { v: 'aku_top',    t: 'AKU nahoře' },
    { v: 'aku_mid',    t: 'AKU uprostřed' },
    { v: 'aku_bottom', t: 'AKU dole' },
  ],
  tempRoles: Array(8).fill('none'),
  bleRole: 'outdoor',
  bleId: '',
  ble: {
    enabled: true,
    type: 'auto',
    mac: '',
    maxAgeMs: 600000,
    mesh: { enabled:false, relay:false, ttl:3, advIntervalMs:1200, minRelayRssi:-95, preferDirect:true }
  },
  mqtt: [
    { topic:'', role:'none', jsonKey:'' },
    { topic:'', role:'none', jsonKey:'' },
  ],
  loaded: false,

  getTempRoles(){
    return this.tempRoles.slice();
  },

  buildSelect(value){
    const sel = document.createElement('select');
    for (const o of this.ROLE_OPTIONS) {
      const opt = document.createElement('option');
      opt.value = o.v;
      opt.textContent = o.t;
      if (o.v === value) opt.selected = true;
      sel.appendChild(opt);
    }
    return sel;
  },

  async loadConfig(){
    let txt = '';
    let obj = {};
    try {
      txt = await App.api.getText('/config.json');
      obj = JSON.parse(txt);
    } catch(e) {
      // pokud config neexistuje, začneme prázdným a umožníme vytvoření přes apply
      obj = {};
    }

    const tr = Array.isArray(obj.tempRoles) ? obj.tempRoles : [];
    this.tempRoles = Array.from({length:8}).map((_,i) => (tr[i] || 'none'));
    this.bleRole = obj.bleThermometer?.role || 'outdoor';
    this.bleId = obj.bleThermometer?.id || '';

// BLE receiver config (global)
const b = obj.ble || {};
this.ble = this.ble || {};
this.ble.enabled = (b.enabled != null) ? !!b.enabled : true;
this.ble.type = String(b.type || 'auto');
this.ble.mac = String(b.mac || '');
this.ble.maxAgeMs = Number.isFinite(Number(b.maxAgeMs)) ? Number(b.maxAgeMs) : 600000;

const bm = b.mesh || {};
this.ble.mesh = this.ble.mesh || {};
this.ble.mesh.enabled = (bm.enabled != null) ? !!bm.enabled : false;
this.ble.mesh.relay = (bm.relay != null) ? !!bm.relay : false;
this.ble.mesh.ttl = Number.isFinite(Number(bm.ttl)) ? Number(bm.ttl) : 3;
this.ble.mesh.advIntervalMs = Number.isFinite(Number(bm.advIntervalMs)) ? Number(bm.advIntervalMs) : 1200;
this.ble.mesh.minRelayRssi = Number.isFinite(Number(bm.minRelayRssi)) ? Number(bm.minRelayRssi) : -95;
this.ble.mesh.preferDirect = (bm.preferDirect != null) ? !!bm.preferDirect : true;

    const mt = Array.isArray(obj.mqttThermometers) ? obj.mqttThermometers : [];
    this.mqtt = [0,1].map((i)=>({
      topic: String(mt[i]?.topic || ''),
      role:  String(mt[i]?.role || 'none'),
      jsonKey: String(mt[i]?.jsonKey || ''),
    }));

    App.state.config = obj;
    this.loaded = true;
  },

  async saveConfig(){
    const obj = App.state.config && typeof App.state.config === 'object' ? App.state.config : {};
    obj.tempRoles = this.tempRoles.slice(0,8);
// Global BLE receiver config (used by /api/ble/status + BLE outdoor thermometer)
obj.ble = obj.ble || {};
obj.ble.enabled = (this.ble?.enabled != null) ? !!this.ble.enabled : true;
obj.ble.type = String(this.ble?.type || 'auto');
obj.ble.mac = String(this.ble?.mac || '');
obj.ble.maxAgeMs = Number.isFinite(Number(this.ble?.maxAgeMs)) ? Number(this.ble.maxAgeMs) : 600000;
obj.ble.mesh = obj.ble.mesh || {};
obj.ble.mesh.enabled = (this.ble?.mesh?.enabled != null) ? !!this.ble.mesh.enabled : false;
obj.ble.mesh.relay = (this.ble?.mesh?.relay != null) ? !!this.ble.mesh.relay : false;
obj.ble.mesh.ttl = Number.isFinite(Number(this.ble?.mesh?.ttl)) ? Number(this.ble.mesh.ttl) : 3;
obj.ble.mesh.advIntervalMs = Number.isFinite(Number(this.ble?.mesh?.advIntervalMs)) ? Number(this.ble.mesh.advIntervalMs) : 1200;
obj.ble.mesh.minRelayRssi = Number.isFinite(Number(this.ble?.mesh?.minRelayRssi)) ? Number(this.ble.mesh.minRelayRssi) : -95;
obj.ble.mesh.preferDirect = (this.ble?.mesh?.preferDirect != null) ? !!this.ble.mesh.preferDirect : true;

    obj.bleThermometer = obj.bleThermometer || {};
    obj.bleThermometer.role = this.bleRole || 'outdoor';
    if (this.bleId && this.bleId.trim()) obj.bleThermometer.id = this.bleId.trim();
    else if (obj.bleThermometer.id) delete obj.bleThermometer.id;

    obj.mqttThermometers = Array.from({length:2}).map((_,i)=>({
      topic: (this.mqtt?.[i]?.topic || '').trim(),
      role:  (this.mqtt?.[i]?.role  || 'none').trim(),
      jsonKey: (this.mqtt?.[i]?.jsonKey || '').trim(),
    }));

    const json = JSON.stringify(obj, null, 2);
    await App.api.postText('/api/config/apply', json, 'application/json');
  }
};

App.registerWidget({
  id:'roles',
  title:'Role teploměrů',
  defaultSpan:6,
  render(el){
    el.innerHTML = `
      <div class="card__title">Role teploměrů</div>
      <div class="muted">Role se ukládají do <span class="mono">/config.json</span> (pole <span class="mono">tempRoles</span> pro T1..T8 a <span class="mono">bleThermometer.role/id</span> pro BLE).</div>
      <div class="row" style="margin-top:10px">
        <button class="btn" data-act="load">Načíst config</button>
        <button class="btn btn--ghost" data-act="save" disabled>Uložit role</button>
      </div>
      <div class="roles" style="margin-top:10px" data-body>
        <div class="block">
          <h3>BLE senzor</h3>
          <div class="grid2" data-ble>
            <label>Role</label><div data-slot="bleRole"></div>
            <label>ID (vol.)</label><input type="text" data-slot="bleId" placeholder="např. OUTDOOR_1" />
          </div>
        </div>
        <div class="block">
          <h3>Dallas T1..T8</h3>
          <div class="grid2" data-dallas></div>
        </div>

        <div class="block">
          <h3>MQTT (2 sloty)</h3>
          <div class="grid2" data-mqtt></div>
          <div class="muted" style="margin-top:6px">MQTT se používá jako virtuální teploměr pro role (např. venkovní teplota z jiné jednotky).</div>
        </div>
        <div class="muted" data-msg></div>
      </div>
    `;

    const body = el.querySelector('[data-body]');
    const msg = el.querySelector('[data-msg]');
    const btnLoad = el.querySelector('[data-act=load]');
    const btnSave = el.querySelector('[data-act=save]');

    const bleRoleHost = el.querySelector('[data-slot=bleRole]');
    const bleIdInput = el.querySelector('[data-slot=bleId]');
    const dallasGrid = el.querySelector('[data-dallas]');
    const mqttGrid = el.querySelector('[data-mqtt]');

    function renderForm(){
      bleRoleHost.innerHTML = '';
      const bleSel = App.roles.buildSelect(App.roles.bleRole);
      bleSel.onchange = () => App.roles.bleRole = bleSel.value;
      bleRoleHost.appendChild(bleSel);

      bleIdInput.value = App.roles.bleId || '';
      bleIdInput.oninput = () => App.roles.bleId = bleIdInput.value;

      dallasGrid.innerHTML = '';
      for (let i=0;i<8;i++) {
        const lab = document.createElement('label');
        lab.textContent = `T${i+1}`;
        const slot = document.createElement('div');
        const sel = App.roles.buildSelect(App.roles.tempRoles[i]);
        sel.onchange = () => App.roles.tempRoles[i] = sel.value;
        slot.appendChild(sel);
        dallasGrid.appendChild(lab);
        dallasGrid.appendChild(slot);
      }

      mqttGrid.innerHTML = '';
      for (let i=0;i<2;i++) {
        const h = document.createElement('div');
        h.style.gridColumn = '1 / -1';
        h.innerHTML = `<div class="small" style="margin-top:6px">Slot MQTT${i+1}</div>`;
        mqttGrid.appendChild(h);

        const labRole = document.createElement('label');
        labRole.textContent = 'Role';
        const slotRole = document.createElement('div');
        const selRole = App.roles.buildSelect(App.roles.mqtt[i].role || 'none');
        selRole.onchange = () => App.roles.mqtt[i].role = selRole.value;
        slotRole.appendChild(selRole);
        mqttGrid.appendChild(labRole);
        mqttGrid.appendChild(slotRole);

        const labTopic = document.createElement('label');
        labTopic.textContent = 'Topic';
        const inpTopic = document.createElement('input');
        inpTopic.type = 'text';
        inpTopic.placeholder = 'např. home/outdoor/temp';
        inpTopic.value = App.roles.mqtt[i].topic || '';
        inpTopic.oninput = () => App.roles.mqtt[i].topic = inpTopic.value;
        mqttGrid.appendChild(labTopic);
        mqttGrid.appendChild(inpTopic);

        const labKey = document.createElement('label');
        labKey.textContent = 'JSON klíč (vol.)';
        const inpKey = document.createElement('input');
        inpKey.type = 'text';
        inpKey.placeholder = 'např. tempC';
        inpKey.value = App.roles.mqtt[i].jsonKey || '';
        inpKey.oninput = () => App.roles.mqtt[i].jsonKey = inpKey.value;
        mqttGrid.appendChild(labKey);
        mqttGrid.appendChild(inpKey);
      }
    }

    btnLoad.onclick = async () => {
      btnLoad.disabled = true;
      msg.textContent = 'Načítám…';
      try {
        await App.roles.loadConfig();
        renderForm();
        btnSave.disabled = false;
        msg.textContent = App.roles.loaded ? 'Načteno.' : 'Nelze načíst, vytvoří se nový config při uložení.';
      } catch(e) {
        msg.textContent = `Chyba načtení: ${e.message}`;
      } finally {
        btnLoad.disabled = false;
      }
    };

    btnSave.onclick = async () => {
      btnSave.disabled = true;
      msg.textContent = 'Ukládám…';
      try {
        await App.roles.saveConfig();
        msg.textContent = 'Uloženo. (config.json byl aktualizován)';
      } catch(e) {
        msg.textContent = `Chyba uložení: ${e.message}`;
      } finally {
        btnSave.disabled = false;
      }
    };

    // render empty form immediately
    renderForm();
  },
  update(){ /* form updates are event-driven */ }
});
