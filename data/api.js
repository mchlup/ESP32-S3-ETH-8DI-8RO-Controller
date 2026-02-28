window.App=window.App||{};
App.api={
  _tokenKey:'apiToken',
  getToken(){
    try{ return (localStorage.getItem(this._tokenKey)||'').trim(); }catch(_){ return ''; }
  },
  setToken(t){
    try{
      const v = (t==null ? '' : String(t)).trim();
      if(v) localStorage.setItem(this._tokenKey, v);
      else localStorage.removeItem(this._tokenKey);
    }catch(_){}
    return this.getToken();
  },
  _authHeaders(extra){
    const h = Object.assign({}, extra||{});
    const t = this.getToken();
    if(t) h['X-Api-Token'] = t;
    return h;
  },

  async _throwHttpError(r){
    // Try to extract a useful error message from JSON responses like:
    //   { ok:false, err:"..." }
    // while still working with plain-text errors.
    let msg = `${r.status} ${r.statusText}`;
    try{
      const ct = (r.headers.get('content-type')||'').toLowerCase();
      if(ct.includes('application/json')){
        const j = await r.clone().json();
        const e = (j && (j.err || j.error || j.message)) ? (j.err || j.error || j.message) : null;
        if(e) msg += `: ${e}`;
      }else{
        const t = (await r.clone().text()).trim();
        if(t) msg += `: ${t}`;
      }
    }catch(_){
      // ignore
    }
    throw new Error(msg);
  },

  async _ensureOk(r){
    if(!r.ok) await this._throwHttpError(r);
    return r;
  },

  async getJson(url){
    const r=await fetch(url,{cache:'no-store', headers:this._authHeaders()});
    await this._ensureOk(r);
    return await r.json();
  },
  async getText(url){
    const r=await fetch(url,{cache:'no-store', headers:this._authHeaders()});
    await this._ensureOk(r);
    return await r.text();
  },

  // Raw text POST helper (used by /api/config/apply which expects the whole JSON as body)
  async postText(url, text, contentType){
    const r = await fetch(url,{
      method:'POST',
      headers:this._authHeaders({'Content-Type': contentType || 'text/plain'}),
      body: (text==null ? '' : String(text)),
      cache:'no-store'
    });
    await this._ensureOk(r);
    return await r.text();
  },

  async postJson(url,obj){
    const r=await fetch(url,{
      method:'POST',
      headers:this._authHeaders({'Content-Type':'application/json'}),
      body:JSON.stringify(obj||{}),
      cache:'no-store'
    });
    await this._ensureOk(r);
    return await r.text();
  },

  startSSE(onFast,onState){
    try{
      const es=new EventSource('/api/events');
      onState&&onState('connecting');
      es.onopen=()=>onState&&onState('connected');
      es.onmessage=(ev)=>{try{onFast&&onFast(JSON.parse(ev.data));}catch(_){}}; 
      es.onerror=()=>onState&&onState('disconnected');
      return es;
    }catch(e){
      onState&&onState('unsupported');
      return null;
    }
  }
};
