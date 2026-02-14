window.App=window.App||{};
App.api={
  async getJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);return await r.json();},
  async getText(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);return await r.text();},
  // Raw text POST helper (used by /api/config/apply which expects the whole JSON as body)
  async postText(url, text, contentType){
    const r = await fetch(url,{
      method:'POST',
      headers:{'Content-Type': contentType || 'text/plain'},
      body: (text==null ? '' : String(text)),
      cache:'no-store'
    });
    if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    return await r.text();
  },
  async postJson(url,obj){const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj||{}),cache:'no-store'});if(!r.ok) throw new Error(`${r.status} ${r.statusText}`);return await r.text();},
  startSSE(onFast,onState){try{const es=new EventSource('/api/events');onState&&onState('connecting');es.onopen=()=>onState&&onState('connected');es.onmessage=(ev)=>{try{onFast&&onFast(JSON.parse(ev.data));}catch(_){}};es.onerror=()=>onState&&onState('disconnected');return es;}catch(e){onState&&onState('unsupported');return null;}}
};
