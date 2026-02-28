// settings_page.js
// Stránka "Nastavení" – konfigurace zařízení (RGB LED, Buzzer, RTC, Ethernet/Wi‑Fi, čas, MQTT, BLE).
// Ukládá se přes POST /api/config/apply jako PATCH-like top-level klíče.

window.App = window.App || {};
App.pages = App.pages || {};

function _bool(v, def){ return (v==null) ? !!def : !!v; }
function _num(v, def){ const n = Number(v); return Number.isFinite(n) ? n : def; }
function _str(v, def){ return (v==null) ? (def||'') : String(v); }

App.pages.settings = {
  _host: null,
  _els: {},
  mount(host){
    this._host = host;
    host.innerHTML = [
      '<div class="pageHead">',
        '<div class="pageTitle">Nastavení</div>',
        '<div class="pageActions">',
          '<button class="btn" id="btnSettingsReload">Načíst</button>',
          '<button class="btn primary" id="btnSettingsSaveTop">Uložit</button>',
        '</div>',
      '</div>',

      '<div class="settingsLayout">',
        '<div class="settingsNav">',
          '<div class="settingsNavTitle muted">Kategorie</div>',
          '<button class="navChip isActive" data-target="secRgb">RGB LED</button>',
          '<button class="navChip" data-target="secBuz">Buzzer</button>',
          '<button class="navChip" data-target="secTime">Čas (NTP / RTC)</button>',
          '<button class="navChip" data-target="secNet">Ethernet / Wi‑Fi</button>',
          '<button class="navChip" data-target="secMqtt">MQTT</button>',
          '<button class="navChip" data-target="secBle">BLE</button>',
          '<button class="navChip" data-target="secSys">Systém</button>',
          '<div class="hr"></div>',
          '<div class="muted small">Tip: konfigurace se ukládá najednou – nahoře nebo dole.</div>',
        '</div>',

        '<div class="settingsBody">',

          // RGB LED
          '<details class="sect" open id="secRgb">',
            '<summary><div><div class="sectTitle">RGB LED</div><div class="sectHint">Indikace stavu + jas</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="formGrid">',
                '<div class="k">Povolit</div><div><label class="chk"><input id="setRgbEn" type="checkbox"> <span>Zapnuto</span></label></div>',
                '<div class="k">Jas</div><div><input id="setRgbBr" type="number" min="0" max="255" step="1" class="inp w140"> <span class="muted">0–255</span></div>',
                '<div class="k">Stavové režimy</div><div><label class="chk"><input id="setRgbSt" type="checkbox"> <span>BLE / ERROR indikace</span></label></div>',
              '</div>',
              '<div class="btnRow" style="margin-top:10px">',
                '<button class="btn" id="btnRgbOff">Vypnout LED</button>',
                '<button class="btn" id="btnRgbBlue">Test modrá</button>',
                '<span class="muted small">Test je okamžitý, uložení je tlačítkem Uložit.</span>',
              '</div>',
            '</div>',
          '</details>',

          // Buzzer
          '<details class="sect" id="secBuz">',
            '<summary><div><div class="sectTitle">Buzzer</div><div class="sectHint">Notifikace + test zvuků</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="formGrid">',
                '<div class="k">Povolit</div><div><label class="chk"><input id="setBuzzEn" type="checkbox"> <span>Zapnuto</span></label></div>',
                '<div class="k">Výchozí frekvence</div><div><input id="setBuzzF" type="number" min="10" max="10000" step="10" class="inp w140"> <span class="muted">Hz</span></div>',
                '<div class="k">Výchozí délka</div><div><input id="setBuzzD" type="number" min="10" max="2000" step="10" class="inp w140"> <span class="muted">ms</span></div>',
                '<div class="k">Hlasitost</div><div><input id="setBuzzVol" type="number" min="0" max="255" step="1" class="inp w140"> <span class="muted">0–255</span></div>',
                '<div class="k">Obálka Attack</div><div><input id="setBuzzAtk" type="number" min="0.2" max="8" step="0.1" class="inp w140"> <span class="muted">křivka</span></div>',
                '<div class="k">Obálka Release</div><div><input id="setBuzzRel" type="number" min="0.2" max="8" step="0.1" class="inp w140"> <span class="muted">křivka</span></div>',
                '<div class="k">Vibrato</div><div class="row" style="gap:10px;flex-wrap:wrap;align-items:center">'
                  + '<input id="setBuzzVibC" type="number" min="0" max="60" step="1" class="inp w120" placeholder="cents">'
                  + '<span class="muted">cents</span>'
                  + '<input id="setBuzzVibHz" type="number" min="0" max="20" step="0.1" class="inp w120" placeholder="Hz">'
                  + '<span class="muted">Hz</span>'
                + '</div>',
                '<div class="k">Micro detune</div><div><input id="setBuzzDet" type="number" min="0" max="30" step="1" class="inp w140"> <span class="muted">± cents</span></div>',

                '<div class="k">Zvuky</div><div class="muted small">Výběr zvuku je melodický (ne jen “8‑bit” beep)</div>',

                '<div class="k">INFO</div><div class="row" style="gap:8px;align-items:center;flex-wrap:wrap">'
                  + '<select id="setBuzzInfo" class="inp w180">'
                    + '<option value="chime">Chime</option>'
                    + '<option value="vag">VAG Chime</option>'
                    + '<option value="gong">Gong</option>'
                    + '<option value="alert">Alert</option>'
                    + '<option value="off">Vypnuto</option>'
                  + '</select>'
                  + '<button class="btn" id="btnBuzzPlayInfo">Přehrát</button>'
                + '</div>',

                '<div class="k">WARNING</div><div class="row" style="gap:8px;align-items:center;flex-wrap:wrap">'
                  + '<select id="setBuzzWarn" class="inp w180">'
                    + '<option value="chime">Chime</option>'
                    + '<option value="vag">VAG Chime</option>'
                    + '<option value="gong">Gong</option>'
                    + '<option value="alert">Alert</option>'
                    + '<option value="off">Vypnuto</option>'
                  + '</select>'
                  + '<button class="btn" id="btnBuzzPlayWarn">Přehrát</button>'
                + '</div>',

                '<div class="k">ALARM</div><div class="row" style="gap:8px;align-items:center;flex-wrap:wrap">'
                  + '<select id="setBuzzAlarm" class="inp w180">'
                    + '<option value="chime">Chime</option>'
                    + '<option value="vag">VAG Chime</option>'
                    + '<option value="gong">Gong</option>'
                    + '<option value="alert">Alert</option>'
                    + '<option value="off">Vypnuto</option>'
                  + '</select>'
                  + '<button class="btn" id="btnBuzzPlayAlarm">Přehrát</button>'
                + '</div>',

                '<div class="k">Notifikace</div>',
                '<div>'
                  + '<label class="chk"><input id="setBuzzN1" type="checkbox"> <span>změna AUTO/MANUAL</span></label><br>'
                  + '<label class="chk"><input id="setBuzzN2" type="checkbox"> <span>změna režimu MANUAL</span></label>'
                + '</div>',
              '</div>',

              '<div class="btnRow" style="margin-top:10px">',
                '<button class="btn" id="btnBuzzTest">Test (beep)</button>',
                '<button class="btn" id="btnBuzzTestChime">Test Chime</button>',
                '<button class="btn" id="btnBuzzTestVag">Test VAG</button>',
                '<button class="btn" id="btnBuzzTestGong">Test Gong</button>',
                '<button class="btn" id="btnBuzzTestAlert">Test Alert</button>',
              '</div>',
            '</div>',
          '</details>',

          // Time / RTC
          '<details class="sect" id="secTime">',
            '<summary><div><div class="sectTitle">Čas (NTP / RTC)</div><div class="sectHint">časové funkce + plánovač</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="formGrid">',
                '<div class="k">NTP</div><div><label class="chk"><input id="setNtpEn" type="checkbox"> <span>Povolit NTP</span></label></div>',
                '<div class="k">Server 1</div><div><input id="setNtpS1" type="text" class="inp" placeholder="pool.ntp.org"></div>',
                '<div class="k">Server 2</div><div><input id="setNtpS2" type="text" class="inp" placeholder="time.nist.gov"></div>',
                '<div class="k">TZ</div><div><input id="setTz" type="text" class="inp" placeholder="Europe/Prague nebo POSIX TZ"></div>',
                '<div class="k">Sync do RTC</div><div><label class="chk"><input id="setRtcSync" type="checkbox"> <span>průběžně zapisovat čas do RTC</span></label></div>',
                '<div class="k">Interval</div><div><input id="setRtcInt" type="number" min="5" max="1440" step="1" class="inp w140"> <span class="muted">min</span></div>',
                '<div class="k">Fallback z RTC</div><div><label class="chk"><input id="setRtcFb" type="checkbox"> <span>při nevalidním systémovém čase načíst z RTC</span></label></div>',
              '</div>',
              '<div class="muted small" id="timeHint" style="margin-top:10px"></div>',
            '</div>',
          '</details>',

          // Ethernet / Wi-Fi
          '<details class="sect" id="secNet">',
            '<summary><div><div class="sectTitle">Ethernet / Wi‑Fi</div><div class="sectHint">připojení + Wi‑Fi portál</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="formGrid">',
                '<div class="k">Wi‑Fi</div><div><label class="chk"><input id="setWifiEn" type="checkbox"> <span>Povolit</span></label></div>',
                '<div class="k">Wi‑Fi portal SSID</div><div><input id="setWifiPortalSsid" type="text" class="inp" placeholder="(auto) ESP-HeatCtrl-XXXXXX"></div>',
                '<div class="k">Wi‑Fi portal heslo</div><div><input id="setWifiPortalPass" type="text" class="inp" placeholder="(volitelné, min. 8 znaků)"></div>',
                '<div class="k">Portal timeout</div><div><input id="setWifiPortalTo" type="number" min="30" max="3600" step="10" class="inp w160"> <span class="muted">s</span></div>',
                '<div class="k">Connect timeout</div><div><input id="setWifiConnTo" type="number" min="5" max="120" step="1" class="inp w160"> <span class="muted">s</span></div>',
                '<div class="k">Connect retries</div><div><input id="setWifiRetries" type="number" min="1" max="10" step="1" class="inp w160"></div>',
                '<div class="k">Wi‑Fi konfigurace</div><div class="row" style="gap:10px;flex-wrap:wrap;align-items:center">'
                  + '<button class="btn" id="btnWifiPortal">Spustit portál (restart)</button>'
                  + '<span class="muted small">užitečné při přístupu přes Ethernet</span>'
                + '</div>',

                '<div class="k">Ethernet</div><div><label class="chk"><input id="setEthEn" type="checkbox"> <span>Povolit</span></label></div>',
                '<div class="k">Preferovat Ethernet</div><div><label class="chk"><input id="setEthPref" type="checkbox"> <span>když je RJ45 připojen</span></label></div>',
                '<div class="k">DHCP grace</div><div><input id="setEthGrace" type="number" min="0" max="120000" step="1000" class="inp w160"> <span class="muted">ms</span></div>',
                '<div class="k">Info</div><div class="muted" id="netInfo">—</div>',
              '</div>',
            '</div>',
          '</details>',

          // MQTT
          '<details class="sect" id="secMqtt">',
            '<summary><div><div class="sectTitle">MQTT</div><div class="sectHint">napojení na domácí automatizaci</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="muted small" style="margin-bottom:10px">Konfigurace MQTT klienta. Pokud je Host prázdný, MQTT se neaktivuje.</div>',
              '<div class="formGrid">',
                '<div class="k">Povolit</div><div><label class="chk"><input id="setMqttEn" type="checkbox"> <span>Zapnuto</span></label></div>',
                '<div class="k">Host</div><div><input id="setMqttHost" type="text" class="inp" placeholder="192.168.1.10"></div>',
                '<div class="k">Port</div><div><input id="setMqttPort" type="number" min="1" max="65535" step="1" class="inp w160"> <span class="muted">default 1883</span></div>',
                '<div class="k">Uživatel</div><div><input id="setMqttUser" type="text" class="inp" placeholder="(volitelné)"></div>',
                '<div class="k">Heslo</div><div><input id="setMqttPass" type="password" class="inp" placeholder="(volitelné)"></div>',
                '<div class="k">Client ID</div><div><input id="setMqttClientId" type="text" class="inp" placeholder="esp32-heat"></div>',
                '<div class="k">Base topic</div><div><input id="setMqttBase" type="text" class="inp" placeholder="esp32/heat"></div>',
              '</div>',
            '</div>',
          '</details>',

          // BLE
          '<details class="sect" id="secBle">',
            '<summary><div><div class="sectTitle">BLE zařízení</div><div class="sectHint">venkovní senzor + mesh</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="muted small" style="margin-bottom:10px">Nastavení příjmu z venkovního BLE senzoru. MAC může být prázdná (příjem z libovolného zařízení, které odpovídá zvolenému typu).</div>',
              '<div class="formGrid">',
                '<div class="k">Povolit</div><div><label class="chk"><input id="setBleEn" type="checkbox"> <span>Zapnuto</span></label></div>',
                '<div class="k">Typ</div><div>'
                  + '<select id="setBleType" class="inp w220">'
                    + '<option value="auto">Auto</option>'
                    + '<option value="esp_meteo_mfg">ESP Meteo (ADV/MFG)</option>'
                    + '<option value="esp_meteo_gatt">ESP Meteo (GATT)</option>'
                  + '</select>'
                + '</div>',
                '<div class="k">MAC</div><div><input id="setBleMac" type="text" class="inp" placeholder="AA:BB:CC:DD:EE:FF"></div>',
                '<div class="k">Max age</div><div><input id="setBleMaxAge" type="number" min="0" max="86400000" step="1000" class="inp w180"> <span class="muted">ms (0 = bez timeoutu)</span></div>',

                '<div class="k">Mesh</div><div class="muted small">volitelné – pro síťování více BLE uzlů</div>',
                '<div class="k">Mesh povolit</div><div><label class="chk"><input id="setBleMeshEn" type="checkbox"> <span>Zapnuto</span></label></div>',
                '<div class="k">Mesh relay</div><div><label class="chk"><input id="setBleMeshRelay" type="checkbox"> <span>přeposílání paketů</span></label></div>',
                '<div class="k">TTL</div><div><input id="setBleMeshTtl" type="number" min="0" max="10" step="1" class="inp w160"> <span class="muted">0 = nevysílat</span></div>',
                '<div class="k">ADV interval</div><div><input id="setBleMeshAdv" type="number" min="0" max="600000" step="50" class="inp w180"> <span class="muted">ms</span></div>',
                '<div class="k">Min RSSI</div><div><input id="setBleMeshRssi" type="number" min="-127" max="0" step="1" class="inp w160"> <span class="muted">dBm</span></div>',
                '<div class="k">Preferovat přímé</div><div><label class="chk"><input id="setBleMeshDirect" type="checkbox"> <span>když je dostupné</span></label></div>',
              '</div>',
            '</div>',
          '</details>',

          // System
          '<details class="sect" id="secSys">',
            '<summary><div><div class="sectTitle">Systém</div><div class="sectHint">restart + rychlé akce</div></div><div class="muted">▾</div></summary>',
            '<div class="sectBody">',
              '<div class="formGrid" style="margin-bottom:10px">',
                '<div class="k">API token</div>',
                '<div>',
                  '<input id="setApiToken" type="password" class="inp" placeholder="(lokálně v prohlížeči)">',
                  ' <button class="btn" id="btnApiTokenSave">Uložit token</button>',
                  ' <button class="btn" id="btnApiTokenClear">Smazat</button>',
                  ' <span class="muted small" id="apiTokenHint"></span>',
                '</div>',
              '</div>',
              '<div class="btnRow">',
                '<button class="btn danger" id="btnSysRestart">Restart zařízení</button>',
                '<span class="muted small">Použije /api/device/restart</span>',
              '</div>',
              '<div class="hr"></div>',
              '<div class="muted small">Pozn.: LED/Buzzer testy jsou v sekcích výše.</div>',
            '</div>',
          '</details>',

        '</div>',
      '</div>',

      // Save
      '<div class="card settingsSaveCard" style="margin-top:12px">',
        '<div class="card__header"><div class="card__title">Uložit</div></div>',
        '<div class="card__body">',
          '<div class="row" style="gap:10px;align-items:center">',
            '<button class="btn primary" id="btnSettingsSave">Uložit konfiguraci</button>',
            '<span id="settingsSaveMsg" class="muted">—</span>',
          '</div>',
        '</div>',
      '</div>'
    ].join('');

    const $ = (id)=>host.querySelector('#'+id);
    this._els = {
      rgbEn: $('setRgbEn'), rgbBr: $('setRgbBr'), rgbSt: $('setRgbSt'),
      buzzEn: $('setBuzzEn'), buzzF: $('setBuzzF'), buzzD: $('setBuzzD'), buzzVol: $('setBuzzVol'),
      buzzAtk: $('setBuzzAtk'), buzzRel: $('setBuzzRel'), buzzVibC: $('setBuzzVibC'), buzzVibHz: $('setBuzzVibHz'), buzzDet: $('setBuzzDet'),
      buzzN1: $('setBuzzN1'), buzzN2: $('setBuzzN2'),
      buzzInfo: $('setBuzzInfo'), buzzWarn: $('setBuzzWarn'), buzzAlarm: $('setBuzzAlarm'),
      ntpEn: $('setNtpEn'), ntpS1: $('setNtpS1'), ntpS2: $('setNtpS2'), tz: $('setTz'), rtcSync: $('setRtcSync'), rtcInt: $('setRtcInt'), rtcFb: $('setRtcFb'),
      wifiEn: $('setWifiEn'), wifiPortalSsid: $('setWifiPortalSsid'), wifiPortalPass: $('setWifiPortalPass'),
      wifiPortalTo: $('setWifiPortalTo'), wifiConnTo: $('setWifiConnTo'), wifiRetries: $('setWifiRetries'),
      ethEn: $('setEthEn'), ethPref: $('setEthPref'), ethGrace: $('setEthGrace'),
      // MQTT
      mqttEn: $('setMqttEn'), mqttHost: $('setMqttHost'), mqttPort: $('setMqttPort'), mqttUser: $('setMqttUser'), mqttPass: $('setMqttPass'),
      mqttClientId: $('setMqttClientId'), mqttBase: $('setMqttBase'),
      // BLE
      bleEn: $('setBleEn'), bleType: $('setBleType'), bleMac: $('setBleMac'), bleMaxAge: $('setBleMaxAge'),
      bleMeshEn: $('setBleMeshEn'), bleMeshRelay: $('setBleMeshRelay'), bleMeshTtl: $('setBleMeshTtl'), bleMeshAdv: $('setBleMeshAdv'),
      bleMeshRssi: $('setBleMeshRssi'), bleMeshDirect: $('setBleMeshDirect'),

      timeHint: $('timeHint'), netInfo: $('netInfo'),
      btnSave: $('btnSettingsSave'), msg: $('settingsSaveMsg'),
      btnWifiPortal: $('btnWifiPortal'),
      btnRgbOff: $('btnRgbOff'), btnRgbBlue: $('btnRgbBlue'),
      btnBuzzTest: $('btnBuzzTest'),
      btnBuzzTestChime: $('btnBuzzTestChime'), btnBuzzTestVag: $('btnBuzzTestVag'), btnBuzzTestGong: $('btnBuzzTestGong'), btnBuzzTestAlert: $('btnBuzzTestAlert'),
      btnBuzzPlayInfo: $('btnBuzzPlayInfo'), btnBuzzPlayWarn: $('btnBuzzPlayWarn'), btnBuzzPlayAlarm: $('btnBuzzPlayAlarm'),
      btnSaveTop: $('btnSettingsSaveTop'),
      btnReloadTop: $('btnSettingsReload'),
      // System
      setApiToken: $('setApiToken'),
      btnApiTokenSave: $('btnApiTokenSave'),
      btnApiTokenClear: $('btnApiTokenClear'),
      apiTokenHint: $('apiTokenHint'),
      btnSysRestart: $('btnSysRestart'),
    };

    // nav chips
    this._navBtns = Array.from(host.querySelectorAll('.navChip'));

    this._wire();
    this._renderFromConfig();
    this.update(App.state.fast);
  },


  _wire(){
    const E=this._els;


    // API token (stored locally in the browser; never read from device config)
    try{
      const curTok = (App.api && App.api.getToken) ? App.api.getToken() : '';
      if (E.setApiToken) E.setApiToken.value = curTok || '';
      if (E.apiTokenHint) E.apiTokenHint.textContent = curTok ? 'nastaveno (uloženo v prohlížeči)' : 'nenastaveno';
      if (E.btnApiTokenSave) E.btnApiTokenSave.addEventListener('click', ()=>{
        const v = E.setApiToken ? E.setApiToken.value : '';
        const t = (App.api && App.api.setToken) ? App.api.setToken(v) : '';
        if (E.apiTokenHint) E.apiTokenHint.textContent = t ? 'nastaveno (uloženo v prohlížeči)' : 'nenastaveno';
        if(App.util && App.util.toast) App.util.toast(t ? 'Token uložen' : 'Token smazán', 'ok');
      });
      if (E.btnApiTokenClear) E.btnApiTokenClear.addEventListener('click', ()=>{
        if (E.setApiToken) E.setApiToken.value = '';
        const t = (App.api && App.api.setToken) ? App.api.setToken('') : '';
        if (E.apiTokenHint) E.apiTokenHint.textContent = 'nenastaveno';
        if(App.util && App.util.toast) App.util.toast('Token smazán', 'ok');
      });
    }catch(_){}

    // Section navigation
    const activateChip = (btn)=>{
      (this._navBtns||[]).forEach(b=>b.classList.toggle('isActive', b===btn));
    };
    (this._navBtns||[]).forEach(btn=>{
      btn.addEventListener('click', ()=>{
        const id = btn.getAttribute('data-target');
        const el = id ? document.getElementById(id) : null;
        if (el) {
          // open the section if it's a <details>
          if (el.tagName === 'DETAILS') el.open = true;
          el.scrollIntoView({behavior:'smooth', block:'start'});
          activateChip(btn);
        }
      });
    });
    E.btnSave.addEventListener('click', async()=>{
      await this._save();
    });

    // Top action buttons (same behavior as bottom save)
    if(E.btnSaveTop) E.btnSaveTop.addEventListener('click', async()=>{ await this._save(); });
    if(E.btnReloadTop) E.btnReloadTop.addEventListener('click', async()=>{
      // re-load config from /config.json (if available), then re-render fields
      try{
        const txt = await App.api.getText('/config.json');
        App.state = App.state || {};
        App.state.config = JSON.parse(txt);
      }catch(_){ /* ignore */ }
      this._renderFromConfig();
      if(App.util && App.util.toast) App.util.toast('Načteno', 'ok');
    });

    E.btnRgbOff.addEventListener('click', async()=>{
      try{ await App.api.postJson('/api/device/led', {mode:'off'}); }catch(_){ alert('Test LED není dostupný (endpoint /api/device/led).'); }
    });
    E.btnRgbBlue.addEventListener('click', async()=>{
      try{ await App.api.postJson('/api/device/led', {mode:'solid', r:0, g:0, b:128}); }catch(_){ alert('Test LED není dostupný (endpoint /api/device/led).'); }
    });
    E.btnBuzzTest.addEventListener('click', async()=>{
      try{ await App.api.postJson('/api/device/beep', {}); }catch(_){ alert('Test buzzeru není dostupný (endpoint /api/device/beep).'); }
    });

    const playSound = async (sound)=>{
      try{ await App.api.postJson('/api/device/beep', {mode:'sound', sound:String(sound||'chime'), repeats:1}); }
      catch(_){ alert('Přehrání zvuku není dostupné (endpoint /api/device/beep).'); }
    };
    E.btnBuzzTestChime.addEventListener('click', ()=>playSound('chime'));
    E.btnBuzzTestVag.addEventListener('click', ()=>playSound('vag'));
    E.btnBuzzTestGong.addEventListener('click', ()=>playSound('gong'));
    E.btnBuzzTestAlert.addEventListener('click', ()=>playSound('alert'));

    E.btnBuzzPlayInfo.addEventListener('click', ()=>playSound(E.buzzInfo.value));
    E.btnBuzzPlayWarn.addEventListener('click', ()=>playSound(E.buzzWarn.value));
    E.btnBuzzPlayAlarm.addEventListener('click', ()=>playSound(E.buzzAlarm.value));

    if (E.btnWifiPortal) {
      E.btnWifiPortal.addEventListener('click', async()=>{
        if (!confirm('Zařízení se restartuje a spustí Wi‑Fi konfigurační portál. Pokračovat?')) return;
        try{
          await App.api.postJson('/api/network/portal', {});
          if(App.util && App.util.toast) App.util.toast('Restartuji…', 'ok');
        }catch(e){
          alert('Spuštění portálu selhalo: '+(e?.message||e));
        }
      });
    }

    if (E.btnSysRestart) {
      E.btnSysRestart.addEventListener('click', async()=>{
        if (!confirm('Opravdu restartovat zařízení?')) return;
        try{
          await App.api.postJson('/api/device/restart', {});
          if(App.util && App.util.toast) App.util.toast('Restartuji…', 'ok');
        }catch(e){
          alert('Restart není dostupný (endpoint /api/device/restart): '+(e?.message||e));
        }
      });
    }

    // UX helpers
    const applyMqttEnabled = ()=>{
      const en = !!E.mqttEn?.checked;
      const dis = !en;
      ['mqttHost','mqttPort','mqttUser','mqttPass','mqttClientId','mqttBase'].forEach(k=>{
        if (E[k]) E[k].disabled = dis;
      });
      if (!en) {
        if (E.mqttHost) E.mqttHost.value = '';
      } else {
        if (E.mqttPort && !String(E.mqttPort.value||'').trim()) E.mqttPort.value = 1883;
      }
    };
    if (E.mqttEn) E.mqttEn.addEventListener('change', applyMqttEnabled);

    const applyBleEnabled = ()=>{
      const en = !!E.bleEn?.checked;
      const dis = !en;
      ['bleType','bleMac','bleMaxAge','bleMeshEn','bleMeshRelay','bleMeshTtl','bleMeshAdv','bleMeshRssi','bleMeshDirect'].forEach(k=>{
        if (E[k]) E[k].disabled = dis;
      });
    };
    if (E.bleEn) E.bleEn.addEventListener('change', applyBleEnabled);
  },

  _renderFromConfig(){
    const cfg = (App.state.config && typeof App.state.config==='object') ? App.state.config : {};
    const rgb = cfg.rgbLed || {};
    const buz = cfg.buzzer || {};
    const t = cfg.time || {};
    const e = cfg.ethernet || {};
    const w = cfg.wifi || {};
    const m = cfg.mqtt || {};
    const ble = cfg.ble || {};
    const mesh = (ble.mesh && typeof ble.mesh==='object') ? ble.mesh : {};

    const E=this._els;
    E.rgbEn.checked = _bool(rgb.enabled, true);
    E.rgbBr.value = _num(rgb.brightness, 64);
    E.rgbSt.checked = _bool(rgb.allowStatus, true);

    E.buzzEn.checked = _bool(buz.enabled, true);
    E.buzzF.value = _num(buz.freqHz, 2000);
    E.buzzD.value = _num(buz.durationMs, 80);
    E.buzzVol.value = _num(buz.volume, 180);
    E.buzzAtk.value = _num(buz.envAttackCurve, 2.2);
    E.buzzRel.value = _num(buz.envReleaseCurve, 2.0);
    E.buzzVibC.value = _num(buz.vibratoCents, 7);
    E.buzzVibHz.value = _num(buz.vibratoHz, 5.5);
    E.buzzDet.value = _num(buz.detuneCents, 3);
    const nn = (buz.notifications && typeof buz.notifications==='object') ? buz.notifications : {};
    E.buzzInfo.value = _str(nn.info, 'chime');
    E.buzzWarn.value = _str(nn.warning, 'gong');
    E.buzzAlarm.value = _str(nn.alarm, 'alert');
    E.buzzN1.checked = _bool(buz.notifyMode, true);
    E.buzzN2.checked = _bool(buz.notifyManual, true);

    E.ntpEn.checked = _bool(t.ntpEnabled, true);
    E.ntpS1.value = _str(t.server1, 'pool.ntp.org');
    E.ntpS2.value = _str(t.server2, 'time.nist.gov');
    E.tz.value = _str(t.tz, 'Europe/Prague');
    E.rtcSync.checked = _bool(t.rtcSyncEnabled, true);
    E.rtcFb.checked = _bool(t.rtcFallbackEnabled, true);
    E.rtcInt.value = _num(t.syncIntervalMin, 60);

    E.ethEn.checked = _bool(e.enabled, true);
    E.ethPref.checked = _bool(e.preferEthernet, true);
    E.ethGrace.value = _num(e.dhcpGraceMs, 20000);

    E.wifiEn.checked = _bool(w.enabled, true);
    E.wifiPortalSsid.value = _str(w.portalSsid, '');
    E.wifiPortalPass.value = _str(w.portalPassword, '');
    E.wifiPortalTo.value = _num(w.portalTimeoutS, 180);
    E.wifiConnTo.value = _num(w.connectTimeoutS, 20);
    E.wifiRetries.value = _num(w.connectRetries, 3);

    // MQTT (enabled = host not empty)
    const host = _str(m.host, '').trim();
    E.mqttEn.checked = !!host;
    E.mqttHost.value = host;
    E.mqttPort.value = _num(m.port, 1883);
    E.mqttUser.value = _str(m.user, '');
    // do not auto-fill password if empty (keep UI as-is)
    E.mqttPass.value = _str(m.pass, '');
    E.mqttClientId.value = _str(m.clientId, 'esp32-heat');
    E.mqttBase.value = _str(m.baseTopic, 'esp32/heat');

    // BLE
    E.bleEn.checked = _bool(ble.enabled, true);
    E.bleType.value = _str(ble.type, 'auto');
    E.bleMac.value = _str(ble.mac, '');
    E.bleMaxAge.value = _num(ble.maxAgeMs, 10*60*1000);
    E.bleMeshEn.checked = _bool(mesh.enabled, true);
    E.bleMeshRelay.checked = _bool(mesh.relay, false);
    E.bleMeshTtl.value = _num(mesh.ttl, 3);
    E.bleMeshAdv.value = _num(mesh.advIntervalMs, 1200);
    E.bleMeshRssi.value = _num(mesh.minRelayRssi, -95);
    E.bleMeshDirect.checked = _bool(mesh.preferDirect, true);

    // hint
    E.timeHint.textContent = 'Pozn.: TZ může být IANA (např. Europe/Prague) nebo POSIX TZ (např. CET-1CEST,M3.5.0/2,M10.5.0/3).';

    // apply enabling UX
    if (E.mqttEn) E.mqttEn.dispatchEvent(new Event('change'));
    if (E.bleEn) E.bleEn.dispatchEvent(new Event('change'));
  },

  async _save(){
    const E=this._els;

    // MQTT: if disabled => keep host empty (disables module)
    const mqttHost = (E.mqttEn && !E.mqttEn.checked) ? '' : String(E.mqttHost.value||'').trim();

    const patch = {
      rgbLed: {
        enabled: !!E.rgbEn.checked,
        brightness: Math.max(0, Math.min(255, Math.round(Number(E.rgbBr.value)||0))),
        allowStatus: !!E.rgbSt.checked,
      },
      buzzer: {
        enabled: !!E.buzzEn.checked,
        freqHz: Math.max(10, Math.min(10000, Math.round(Number(E.buzzF.value)||2000))),
        durationMs: Math.max(10, Math.min(2000, Math.round(Number(E.buzzD.value)||80))),
        volume: Math.max(0, Math.min(255, Math.round(Number(E.buzzVol.value)||180))),
        envAttackCurve: Math.max(0.2, Math.min(8, Number(E.buzzAtk.value)||2.2)),
        envReleaseCurve: Math.max(0.2, Math.min(8, Number(E.buzzRel.value)||2.0)),
        vibratoCents: Math.max(0, Math.min(60, Math.round(Number(E.buzzVibC.value)||7))),
        vibratoHz: Math.max(0, Math.min(20, Number(E.buzzVibHz.value)||5.5)),
        detuneCents: Math.max(0, Math.min(30, Math.round(Number(E.buzzDet.value)||3))),
        notifyMode: !!E.buzzN1.checked,
        notifyManual: !!E.buzzN2.checked,
        notifications: {
          info: String(E.buzzInfo.value||'chime'),
          warning: String(E.buzzWarn.value||'gong'),
          alarm: String(E.buzzAlarm.value||'alert'),
        },
      },
      time: {
        ntpEnabled: !!E.ntpEn.checked,
        server1: String(E.ntpS1.value||'').trim(),
        server2: String(E.ntpS2.value||'').trim(),
        tz: String(E.tz.value||'').trim(),
        rtcSyncEnabled: !!E.rtcSync.checked,
        rtcFallbackEnabled: !!E.rtcFb.checked,
        syncIntervalMin: Math.max(5, Math.min(1440, Math.round(Number(E.rtcInt.value)||60))),
      },
      wifi: {
        enabled: !!E.wifiEn.checked,
        portalSsid: String(E.wifiPortalSsid.value||'').trim(),
        portalPassword: String(E.wifiPortalPass.value||'').trim(),
        portalTimeoutS: Math.max(30, Math.min(3600, Math.round(Number(E.wifiPortalTo.value)||180))),
        connectTimeoutS: Math.max(5, Math.min(120, Math.round(Number(E.wifiConnTo.value)||20))),
        connectRetries: Math.max(1, Math.min(10, Math.round(Number(E.wifiRetries.value)||3))),
      },
      ethernet: {
        enabled: !!E.ethEn.checked,
        preferEthernet: !!E.ethPref.checked,
        dhcpGraceMs: Math.max(0, Math.min(120000, Math.round(Number(E.ethGrace.value)||0))),
      },
      mqtt: {
        host: mqttHost,
        port: Math.max(1, Math.min(65535, Math.round(Number(E.mqttPort.value)||1883))),
        user: String(E.mqttUser.value||'').trim(),
        pass: String(E.mqttPass.value||'').trim(),
        clientId: String(E.mqttClientId.value||'').trim(),
        baseTopic: String(E.mqttBase.value||'').trim(),
      },
      ble: {
        enabled: !!E.bleEn.checked,
        type: String(E.bleType.value||'auto').trim(),
        mac: String(E.bleMac.value||'').trim(),
        maxAgeMs: Math.max(0, Math.min(86400000, Math.round(Number(E.bleMaxAge.value)||0))),
        mesh: {
          enabled: !!E.bleMeshEn.checked,
          relay: !!E.bleMeshRelay.checked,
          ttl: Math.max(0, Math.min(10, Math.round(Number(E.bleMeshTtl.value)||0))),
          advIntervalMs: Math.max(0, Math.min(600000, Math.round(Number(E.bleMeshAdv.value)||0))),
          minRelayRssi: Math.max(-127, Math.min(0, Math.round(Number(E.bleMeshRssi.value)||-95))),
          preferDirect: !!E.bleMeshDirect.checked,
        }
      }
    };

    // Keep config.json clean: empty strings -> remove keys on save.
    if (!patch.wifi.portalSsid) delete patch.wifi.portalSsid;
    if (!patch.wifi.portalPassword) delete patch.wifi.portalPassword;

    // MQTT: if disabled or empty host => remove optional fields to keep JSON shorter
    if (!patch.mqtt.host) {
      // Keep host empty to explicitly disable; strip the rest
      delete patch.mqtt.user;
      delete patch.mqtt.pass;
      delete patch.mqtt.clientId;
      delete patch.mqtt.baseTopic;
      // keep port as default (harmless)
    } else {
      if (!patch.mqtt.user) delete patch.mqtt.user;
      if (!patch.mqtt.pass) delete patch.mqtt.pass;
      if (!patch.mqtt.clientId) delete patch.mqtt.clientId;
      if (!patch.mqtt.baseTopic) delete patch.mqtt.baseTopic;
    }

    // BLE: clean empty MAC
    if (!patch.ble.mac) delete patch.ble.mac;

    E.msg.textContent = 'Ukládám…';
    try{
      await App.api.postText('/api/config/apply', JSON.stringify(patch), 'application/json');
      // refresh config so other pages/widgets see new values
      if(App.roles && typeof App.roles.loadConfig==='function') await App.roles.loadConfig();
      else App.state.config = await App.api.getJson('/api/config');
      this._renderFromConfig();
      E.msg.textContent = 'Uloženo';
    }catch(e){
      E.msg.textContent = 'Chyba';
      alert('Uložení se nezdařilo: '+(e?.message||e));
    }
  },

  update(fast){
    const E=this._els;
    if(!E || !E.netInfo) return;
    const ip = fast?.n?.ip || '0.0.0.0';
    const up = fast?.n?.up ? 'online' : 'offline';
    E.netInfo.textContent = `${up}, IP: ${ip}`;
  }
};
