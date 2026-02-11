window.App=window.App||{};App.widgets=App.widgets||{};
(function(){
const DEF=[
 {id:'status',on:true,span:6},{id:'equitherm',on:true,span:6},{id:'dhw',on:true,span:6},{id:'recirc',on:true,span:6},{id:'aku',on:true,span:6},{id:'relays',on:true,span:6},{id:'inputs',on:true,span:6},{id:'temps',on:true,span:6},
 {id:'roles',on:false,span:6},{id:'ble',on:true,span:6},{id:'diag',on:true,span:12}
];
function load(){
  try{
    const r=localStorage.getItem('dashLayout');
    if(!r) return DEF.slice();
    const l=JSON.parse(r);
    if(Array.isArray(l)&&l.length){
      // forward-compat: append any newly introduced widgets (keep user order)
      const have = new Set(l.map(x=>x.id));
      for(const d of DEF){ if(!have.has(d.id)) l.push({...d}); }
      return l;
    }
  }catch(_){ }
  return DEF.slice();
}
function save(l){localStorage.setItem('dashLayout',JSON.stringify(l));}
function card(t,body){return `<div class="card"><h2>${t}</h2>${body}</div>`;}
function w(id){return App.widgets[id]||{title:id,render:()=>`<div class="hint">Widget '${id}' nenalezen.</div>`,update:()=>{}};}
let _hostSelector = '#pageContent';
function render(l){
 const root=App.util.$(_hostSelector);
 if(!root) return;
 root.innerHTML=`<div class="grid12">${l.filter(x=>x.on).map(x=>`<section class="col${x.span===12?12:6}" data-w="${x.id}">${card(w(x.id).title||x.id,`<div id="w_${x.id}"></div>`)}</section>`).join('')}</div>`;
 // mount widgets into their containers
 for(const it of l.filter(x=>x.on)){
   const host = document.getElementById(`w_${it.id}`);
   const ww = w(it.id);
   try{ if(host && typeof ww.render==='function') ww.render(host); }catch(_){ }
 }
}
function updateAll(fast){
  // Widgets in this project use a few different update() signatures:
  //   update(fast)
  //   update(fast, host)
  //   update(host, fast)
  // Support all variants so no widget silently breaks.
  for (const id in App.widgets) {
    const ww = App.widgets[id];
    if (typeof ww.update !== 'function') continue;

    const host = document.getElementById(`w_${id}`);
    try {
      if (ww.update.length <= 1) {
        ww.update(fast);
      } else {
        // Prefer (fast, host) (used by relays.js), fallback to (host, fast) (used by ble.js)
        try { ww.update(fast, host); }
        catch (_) { ww.update(host, fast); }
      }
    } catch (_) {}
  }
}
function editor(){const ed=App.util.$('#layoutEditor');const l=App.dashboard.getLayout();ed.innerHTML=l.map((it,i)=>{
 const ww=w(it.id);
 return `<div class="layoutRow"><label class="chk"><input type="checkbox" data-i="${i}" ${it.on?'checked':''}/> <span>${ww.title||it.id}</span></label><select class="sel" data-i="${i}" data-k="span"><option value="6" ${it.span===6?'selected':''}>1/2</option><option value="12" ${it.span===12?'selected':''}>1/1</option></select><div class="layoutBtns"><button class="btn sm" data-i="${i}" data-act="up">▲</button><button class="btn sm" data-i="${i}" data-act="down">▼</button></div></div>`;
}).join('');
 ed.querySelectorAll('input[type=checkbox]').forEach(inp=>inp.addEventListener('change',()=>{const i=+inp.dataset.i;const l2=App.dashboard.getLayout();l2[i].on=!!inp.checked;App.dashboard.setLayout(l2);}));
 ed.querySelectorAll('select').forEach(sel=>sel.addEventListener('change',()=>{const i=+sel.dataset.i;const l2=App.dashboard.getLayout();l2[i].span=+sel.value;App.dashboard.setLayout(l2);}));
 ed.querySelectorAll('button[data-act]').forEach(b=>b.addEventListener('click',()=>{const i=+b.dataset.i;const act=b.dataset.act;const l2=App.dashboard.getLayout();const j=act==='up'?i-1:i+1; if(j<0||j>=l2.length) return; const tmp=l2[i]; l2[i]=l2[j]; l2[j]=tmp; App.dashboard.setLayout(l2); editor();}));
}
App.dashboard={
 _layout:load(),
 register(w){if(w&&w.id) App.widgets[w.id]=w;},
 getLayout(){return this._layout.slice();},
 setLayout(l){this._layout=l.slice();},
 mount(hostSelector){
   if(hostSelector) _hostSelector = hostSelector;
   render(this._layout);
 },
 updateAll,
 openEditor(){App.util.$('#layoutPanel').hidden=false;editor();},
 closeEditor(){App.util.$('#layoutPanel').hidden=true;},
 saveReload(){save(this._layout);location.reload();}
};

// Legacy/compat: most widget files register via App.registerWidget(...)
App.registerWidget = App.registerWidget || function(widget){
  App.dashboard.register(widget);
};
})();
