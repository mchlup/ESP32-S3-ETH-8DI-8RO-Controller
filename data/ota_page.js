// ota_page.js
window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  const api = App.api;
  const util = App.util;

  function el(html){
    const t = document.createElement('template');
    t.innerHTML = html.trim();
    return t.content.firstChild;
  }

  function fmtPct(v){
    const n = Number(v);
    if (!Number.isFinite(n)) return '—';
    const p = Math.max(0, Math.min(100, Math.round(n * 100)));
    return p + '%';
  }

  App.pages.ota = {
    _root: null,
    _els: {},

    mount(container){
      this._root = container;
      container.innerHTML = '';
      container.appendChild(el(`
        <div class="card">
          <div class="card__head">
            <div class="card__title">OTA aktualizace firmware (web)</div>
          </div>
          <div class="card__body">
            <div class="muted" style="margin-bottom:10px">
              Nahraj <span class="mono">.bin</span> soubor. Po úspěšném nahrání se zařízení samo restartuje.
            </div>

            <div class="row" style="display:flex; gap:10px; flex-wrap:wrap; align-items:center">
              <input type="file" data-file accept=".bin,application/octet-stream" />
              <button class="btn" data-upload>Nahrát firmware</button>
            </div>

            <div style="margin-top:12px">
              <div class="muted">Průběh</div>
              <div style="height:10px; background:rgba(255,255,255,0.08); border-radius:999px; overflow:hidden">
                <div data-bar style="height:10px; width:0%; background:var(--ok)"></div>
              </div>
              <div class="muted" style="margin-top:6px" data-pct>—</div>
            </div>

            <pre class="mono" data-status style="margin-top:12px; white-space:pre-wrap">—</pre>
          </div>
        </div>
      `));

      this._els.file = container.querySelector('[data-file]');
      this._els.upload = container.querySelector('[data-upload]');
      this._els.bar = container.querySelector('[data-bar]');
      this._els.pct = container.querySelector('[data-pct]');
      this._els.status = container.querySelector('[data-status]');

      this._els.upload.onclick = () => this._doUpload();
      this._refreshStatus();
    },

    async _refreshStatus(){
      if (!this._els.status) return;
      try{
        const j = await api.getJson('/api/ota/status');
        const p = (j && j.total) ? (Number(j.written) / Number(j.total)) : (Number(j.progress) || 0);
        this._els.bar.style.width = Math.max(0, Math.min(100, p * 100)) + '%';
        this._els.pct.textContent = fmtPct(p);
        this._els.status.textContent =
          `inProgress: ${!!j.inProgress}\n`+
          `written: ${j.written||0} / ${j.total||0} bytes\n`+
          (j.lastError ? `error: ${j.lastError}\n` : '')+
          (j.rebootPending ? `reboot: pending\n` : '');
      } catch(e){
        this._els.status.textContent = 'Chyba načtení /api/ota/status';
      }
    },

    _doUpload(){
      const f = this._els.file && this._els.file.files ? this._els.file.files[0] : null;
      if (!f){
        alert('Vyber .bin soubor');
        return;
      }

      this._els.upload.disabled = true;
      this._els.status.textContent = 'Nahrávám…';
      this._els.bar.style.width = '0%';
      this._els.pct.textContent = '0%';

      const fd = new FormData();
      fd.append('firmware', f, f.name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota/update', true);
// Include API token if configured (matches server-side OTA auth)
try{
  const t = (api && api.getToken) ? api.getToken() : '';
  if(t) xhr.setRequestHeader('X-Api-Token', t);
}catch(_){}


      xhr.upload.onprogress = (ev)=>{
        if (!ev.lengthComputable) return;
        const p = ev.loaded / ev.total;
        this._els.bar.style.width = Math.max(0, Math.min(100, p * 100)) + '%';
        this._els.pct.textContent = fmtPct(p);
      };

      xhr.onload = async ()=>{
        this._els.upload.disabled = false;
        if (xhr.status >= 200 && xhr.status < 300) {
          this._els.status.textContent = 'Nahráno. Za chvíli dojde k restartu…';
        } else {
          let msg = (xhr.responseText || xhr.status);
try{
  const j = JSON.parse(xhr.responseText||'');
  if(j && (j.err||j.error||j.message)) msg = (j.err||j.error||j.message);
}catch(_){}
this._els.status.textContent = 'Chyba OTA: ' + msg;
        }
        // status will turn offline during reboot
        setTimeout(()=>this._refreshStatus(), 1000);
      };

      xhr.onerror = ()=>{
        this._els.upload.disabled = false;
        this._els.status.textContent = 'Chyba přenosu (XHR)';
      };

      xhr.send(fd);
    },

    update(){
      // periodic refresh while on page
      this._refreshStatus();
    }
  };
})();
