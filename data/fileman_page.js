// fileman_page.js
window.App = window.App || {};
App.pages = App.pages || {};

(function(){
  const api = App.api;
  const util = App.util;

  function el(html){const t=document.createElement('template');t.innerHTML=html.trim();return t.content.firstChild;}
  function esc(s){return util && util.esc ? util.esc(s) : String(s);}

  function normalizePath(p){
    p = String(p||'').trim();
    if (!p) return '';
    if (!p.startsWith('/')) p = '/' + p;
    return p;
  }

  App.pages.fileman = {
    _root:null,
    _els:{},
    _selected:'',

    mount(container){
      this._root = container;
      container.innerHTML = '';
      container.appendChild(el(`
        <div class="grid" style="grid-template-columns: 1fr 1fr; gap: 16px">
          <div class="card">
            <div class="card__head">
              <div class="card__title">Souborový systém (LittleFS)</div>
              <div class="muted" data-fsinfo>—</div>
            </div>
            <div class="card__body">
              <div class="row" style="gap:10px; flex-wrap:wrap">
                <button class="btn" data-act="refresh">Obnovit seznam</button>
                <input class="inp mono" style="max-width: 240px" data-newpath placeholder="/notes.txt">
                <button class="btn btn--ghost" data-act="create">Vytvořit prázdný</button>
                <label class="btn btn--ghost" style="cursor:pointer">
                  Upload… <input type="file" style="display:none" data-upload>
                </label>
              </div>
              <div class="muted" style="margin-top:8px" data-msg></div>
              <div style="margin-top:12px" data-list></div>
            </div>
          </div>

          <div class="card">
            <div class="card__head">
              <div class="card__title">Editor</div>
              <div class="muted" data-cur>—</div>
            </div>
            <div class="card__body">
              <textarea class="inp mono" style="width:100%; min-height: 360px" data-editor placeholder="Vyber soubor vlevo…"></textarea>
              <div class="row" style="gap:10px; flex-wrap:wrap; margin-top:10px">
                <button class="btn" data-act="save">Uložit</button>
                <button class="btn btn--ghost" data-act="download">Stáhnout</button>
                <button class="btn btn--ghost" data-act="delete">Smazat</button>
              </div>
            </div>
          </div>
        </div>
      `));

      this._els.info = container.querySelector('[data-fsinfo]');
      this._els.msg = container.querySelector('[data-msg]');
      this._els.list = container.querySelector('[data-list]');
      this._els.newpath = container.querySelector('[data-newpath]');
      this._els.upload = container.querySelector('[data-upload]');
      this._els.cur = container.querySelector('[data-cur]');
      this._els.editor = container.querySelector('[data-editor]');

      container.querySelector('[data-act=refresh]').onclick = ()=>this._refresh();
      container.querySelector('[data-act=create]').onclick = ()=>this._createEmpty();
      container.querySelector('[data-act=save]').onclick = ()=>this._save();
      container.querySelector('[data-act=delete]').onclick = ()=>this._delete();
      container.querySelector('[data-act=download]').onclick = ()=>this._download();

      this._els.upload.onchange = (e)=>this._upload(e.target.files && e.target.files[0]);

      this._refresh();
    },

    async _refresh(){
      try{
        const info = await api.getJson('/api/fs/info');
        if (this._els.info && info && info.ok) {
          const t = Number(info.totalBytes)||0;
          const u = Number(info.usedBytes)||0;
          this._els.info.textContent = `Použito ${u} / ${t} B`;
        }
      }catch(_){
        if (this._els.info) this._els.info.textContent='—';
      }

      try{
        const j = await api.getJson('/api/fs/list');
        const files = Array.isArray(j.files) ? j.files : [];
        files.sort((a,b)=> String(a.name).localeCompare(String(b.name)));
        this._renderList(files);
        this._setMsg('');
      }catch(e){
        this._setMsg('Chyba: '+(e.message||e));
      }
    },

    _renderList(files){
      const host = this._els.list;
      host.innerHTML='';
      if (!files.length) {
        host.innerHTML='<div class="muted">(žádné soubory)</div>';
        return;
      }
      const table = document.createElement('table');
      table.className='tbl';
      table.innerHTML = `<thead><tr><th>Soubor</th><th>Velikost</th><th></th></tr></thead>`;
      const tb = document.createElement('tbody');
      for (const f of files){
        const tr = document.createElement('tr');
        const name = String(f.name||'');
        const size = Number(f.size)||0;
        tr.innerHTML = `<td class="mono">${esc(name)}</td><td class="mono">${size}</td><td></td>`;
        const btn = document.createElement('button');
        btn.className='btn btn--ghost';
        btn.textContent='Otevřít';
        btn.onclick = ()=>this._open(name);
        tr.children[2].appendChild(btn);
        tb.appendChild(tr);
      }
      table.appendChild(tb);
      host.appendChild(table);
    },

    async _open(path){
      path = normalizePath(path);
      if (!path) return;
      try{
        const txt = await api.getText('/api/fs/read?path='+encodeURIComponent(path));
        this._selected = path;
        this._els.cur.textContent = path;
        this._els.editor.value = txt;
        this._setMsg('');
      }catch(e){
        this._setMsg('Chyba otevření: '+(e.message||e));
      }
    },

    async _createEmpty(){
      const p = normalizePath(this._els.newpath.value);
      if (!p) { this._setMsg('Zadej cestu (např. /notes.txt)'); return; }
      try{
        await api.postText('/api/fs/write?path='+encodeURIComponent(p), '', 'text/plain');
        await this._refresh();
        await this._open(p);
      }catch(e){
        this._setMsg('Chyba vytvoření: '+(e.message||e));
      }
    },

    async _save(){
      if (!this._selected) { this._setMsg('Nejprve vyber soubor.'); return; }
      try{
        await api.postText('/api/fs/write?path='+encodeURIComponent(this._selected), this._els.editor.value, 'text/plain');
        this._setMsg('Uloženo.');
        await this._refresh();
      }catch(e){
        this._setMsg('Chyba uložení: '+(e.message||e));
      }
    },

    async _delete(){
      if (!this._selected) { this._setMsg('Nejprve vyber soubor.'); return; }
      if (!confirm('Smazat '+this._selected+' ?')) return;
      try{
        await api.postJson('/api/fs/delete?path='+encodeURIComponent(this._selected), {});
        this._selected='';
        this._els.cur.textContent='—';
        this._els.editor.value='';
        await this._refresh();
        this._setMsg('Smazáno.');
      }catch(e){
        this._setMsg('Chyba mazání: '+(e.message||e));
      }
    },

    _download(){
      if (!this._selected) { this._setMsg('Nejprve vyber soubor.'); return; }
      window.open('/api/fs/download?path='+encodeURIComponent(this._selected), '_blank');
    },

    async _upload(file){
      if (!file) return;
      const p = normalizePath(this._els.newpath.value || ('/'+file.name));
      if (!p) { this._setMsg('Zadej cestu.'); return; }

      const fd = new FormData();
      fd.append('file', file, file.name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/fs/upload?path='+encodeURIComponent(p), true);
      try{
        const tok = (App.api && App.api.getToken) ? App.api.getToken() : '';
        if (tok) xhr.setRequestHeader('X-Api-Token', tok);
      }catch(_){ }
      xhr.onload = async ()=>{
        if (xhr.status>=200 && xhr.status<300){
          this._setMsg('Upload OK: '+p);
          await this._refresh();
        } else {
          this._setMsg('Upload chyba: '+(xhr.responseText||xhr.status));
        }
      };
      xhr.onerror = ()=>this._setMsg('Upload chyba (XHR)');
      xhr.send(fd);
    },

    _setMsg(t){
      if (this._els.msg) this._els.msg.textContent = t || '';
    },

    update(){ /* no live update */ }
  };
})();
