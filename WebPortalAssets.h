#pragma once
#include <Arduino.h>

// Fallback UI shown when /index.html is not available on LittleFS.
// It acts as a minimal Firmware and File Manager so the device remains serviceable.

static const char WEB_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="cs">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Firmware and File Manager</title>
  <style>
    :root{--bg:#0b1220;--panel:#121a2b;--panel2:#0f172a;--muted:#9fb0d1;--text:#edf3ff;--line:#22314f;--line2:#31508d;--ok:#22c55e;--bad:#ef4444;--acc:#60a5fa;--sel:#17345f;--shadow:0 14px 34px rgba(0,0,0,.28)}
    *{box-sizing:border-box} body{margin:0;font:14px/1.45 system-ui,Segoe UI,Arial,sans-serif;background:linear-gradient(180deg,#0b1220,#111827);color:var(--text)}
    .wrap{max-width:1380px;margin:0 auto;padding:18px}.hero{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;flex-wrap:wrap}
    .card{background:rgba(18,26,43,.94);border:1px solid var(--line);border-radius:16px;padding:16px;box-shadow:var(--shadow)}
    h1,h2,h3{margin:0 0 10px} h1{font-size:28px} h2{font-size:18px} h3{font-size:15px}.muted{color:var(--muted)}
    .grid{display:grid;grid-template-columns:minmax(560px,1.6fr) minmax(340px,1fr);gap:16px;margin-top:16px}
    .sideStack{display:grid;gap:16px}
    label{display:block;margin:10px 0 6px;color:var(--muted)} input[type=text],input[type=file],textarea{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:#0a1020;color:var(--text)}
    button{margin-top:12px;padding:10px 14px;border-radius:10px;border:1px solid var(--line2);background:#16325f;color:#fff;cursor:pointer}
    button.secondary{background:#0f172a;border-color:var(--line)} button.danger{background:#5b1620;border-color:#8d3141} button.small{margin-top:0;padding:7px 10px;font-size:12px}
    .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.status{margin-top:10px;padding:10px 12px;border-radius:10px;background:#0a1020;border:1px solid var(--line);white-space:pre-wrap}
    code,pre{font-family:ui-monospace,SFMono-Regular,Consolas,monospace} .tag{display:inline-block;padding:3px 8px;border-radius:999px;background:#13213d;border:1px solid #28406f;color:#cfe2ff}
    a{color:#9ec5ff;text-decoration:none}.small{font-size:12px} pre{margin:0;white-space:pre-wrap;word-break:break-word;max-height:420px;overflow:auto}
    .progress{height:10px;border-radius:999px;overflow:hidden;background:#0a1020;border:1px solid var(--line);margin-top:10px}.progress i{display:block;height:100%;width:0;background:linear-gradient(90deg,#2563eb,#60a5fa)}
    .drop{margin-top:10px;padding:10px;border:1px dashed var(--line2);border-radius:10px;background:#0b1426;color:var(--muted)}
    .fmHead{display:flex;justify-content:space-between;gap:14px;align-items:flex-start;flex-wrap:wrap;margin-bottom:12px}
    .fmStats{display:flex;gap:8px;flex-wrap:wrap}.chip{padding:5px 10px;border-radius:999px;background:#13213d;border:1px solid #28406f;color:#cfe2ff;font-size:12px}
    .fmShell{display:grid;grid-template-columns:minmax(260px,320px) 1fr;gap:14px;min-height:540px}
    .fmPane,.fmPreview{border:1px solid var(--line);border-radius:14px;background:linear-gradient(180deg,rgba(15,23,42,.98),rgba(10,16,32,.96));overflow:hidden}
    .paneHead{padding:10px 12px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;gap:8px;align-items:center;background:rgba(255,255,255,.02)}
    .paneBody{padding:10px 12px}.treePath{font:12px ui-monospace,SFMono-Regular,Consolas,monospace;color:#cfe2ff;word-break:break-all}
    .treeList,.fileList{display:grid;gap:2px;max-height:460px;overflow:auto;padding:6px}
    .treeItem,.fileRow{display:grid;align-items:center;gap:10px;border:1px solid transparent;border-radius:10px;padding:8px 10px;cursor:pointer}
    .treeItem{grid-template-columns:20px 1fr auto}.treeItem:hover,.fileRow:hover{background:rgba(255,255,255,.03);border-color:rgba(96,165,250,.14)}
    .treeItem.active,.fileRow.active{background:var(--sel);border-color:#2d5ea8}
    .treeItem .name,.fileRow .name{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
    .treeItem .meta,.fileRow .meta{font-size:12px;color:var(--muted)}
    .fileToolbar{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 10px}
    .fileList{border:1px solid var(--line);border-radius:12px;background:#0a1020;max-height:402px}
    .fileHeader,.fileRow{grid-template-columns:minmax(0,1.4fr) 84px 90px minmax(170px,.9fr)}
    .fileHeader{display:grid;gap:10px;padding:8px 10px;border-bottom:1px solid var(--line);font-size:12px;color:var(--muted);background:#10192d}
    .acts{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}.acts button{margin:0}.mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}
    textarea{min-height:320px;resize:vertical}
    @media (max-width: 1080px){.grid{grid-template-columns:1fr}.fmShell{grid-template-columns:1fr}.treeList,.fileList{max-height:none}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div>
        <h1>Firmware and File Manager</h1>
        <div class="muted">Fallback rozhraní z firmware. Zobrazí se automaticky, pokud na LittleFS není dostupný <code>/index.html</code>.</div>
      </div>
      <div class="card small">
        <div><span class="tag">Service Mode</span></div>
        <div id="basicInfo" class="muted" style="margin-top:10px">Načítám stav zařízení…</div>
      </div>
    </div>

    <div class="grid">
      <section class="card">
        <div class="fmHead">
          <div>
            <h2>LittleFS správce souborů</h2>
            <div class="muted">Rozložení ve stylu správce souborů: vlevo strom složek, vpravo obsah aktuální složky a rychlé akce.</div>
          </div>
          <div class="fmStats">
            <span class="chip" id="fsMountedChip">LittleFS: --</span>
            <span class="chip" id="fsCountChip">Položky: --</span>
            <span class="chip" id="fsUsedChip">Obsazeno: --</span>
            <span class="chip" id="fsPathChip">Složka: /</span>
          </div>
        </div>
        <div class="row" style="margin-bottom:12px">
          <button class="secondary" onclick="refreshFiles()">Obnovit seznam</button>
          <button class="secondary" onclick="promptCreateFolder()">Vytvořit složku</button>
          <button class="secondary" onclick="promptCreateTextFile()">Nový textový soubor</button>
        </div>
        <div class="fmShell">
          <div class="fmPane">
            <div class="paneHead">
              <strong>Strom složek</strong>
              <span class="muted small" id="dirCountLbl">0 složek</span>
            </div>
            <div class="paneBody">
              <div class="treePath" id="currentPath">/</div>
              <div id="dirTree" class="treeList"></div>
            </div>
          </div>
          <div class="fmPreview">
            <div class="paneHead">
              <div>
                <strong id="folderTitle">Obsah složky /</strong>
                <div class="muted small">Řazení: složky nahoře, pak soubory. Dvojklik není potřeba.</div>
              </div>
              <span class="muted small" id="folderMeta">--</span>
            </div>
            <div class="paneBody">
              <div class="fileToolbar">
                <input id="fsPath" type="text" placeholder="/index.html nebo /assets/app.js" style="flex:1;min-width:220px;margin:0" />
              </div>
              <div class="fileHeader"><div>Název</div><div>Typ</div><div>Velikost</div><div style="text-align:right">Akce</div></div>
              <div id="filesBody" class="fileList"></div>
            </div>
          </div>
        </div>
      </section>

      <div class="sideStack">
        <section class="card">
          <h2>Nahrání souboru do LittleFS</h2>
          <label for="fsFile">Soubor</label>
          <input id="fsFile" type="file" />
          <div class="drop">Přetáhni soubor do LittleFS sem nebo použij výběr níže.</div>
          <button onclick="uploadFsFile()">Nahrát do LittleFS</button>
          <div class="progress"><i id="fsProg"></i></div>
          <div id="uploadFsStatus" class="status">Připraveno.</div>
        </section>

        <section class="card">
          <h2>Textový soubor: náhled a editace</h2>
          <div class="muted">Můžeš otevřít textový soubor, upravit ho a uložit zpět do LittleFS, nebo vytvořit nový textový soubor přímo v prohlížeči.</div>
          <label for="editorPath">Cesta souboru</label>
          <input id="editorPath" type="text" placeholder="/notes.txt nebo /assets/app.js" />
          <div class="row">
            <button class="secondary" onclick="viewCurrentEditorPath()">Načíst</button>
            <button class="secondary" onclick="promptCreateTextFile()">Nový</button>
            <button onclick="saveEditorFile()">Uložit</button>
          </div>
          <div id="viewerStatus" class="status">Vyber soubor ze seznamu nebo vytvoř nový.</div>
          <textarea id="viewerContent" rows="18" spellcheck="false" placeholder="Obsah textového souboru"></textarea>
        </section>

        <section class="card">
          <h2>Aktualizace firmware (back-end)</h2>
          <div class="muted">Nahraj <code>.bin</code> pro aplikaci / firmware. Proběhne kontrola, zda se image vejde do firmware partition.</div>
          <label for="fwFile">Firmware .bin</label>
          <input id="fwFile" type="file" accept=".bin,application/octet-stream" />
          <div class="drop">Přetáhni soubor sem nebo použij výběr níže.</div>
          <button onclick="uploadFirmware()">Nahrát firmware</button>
          <div class="progress"><i id="fwProg"></i></div>
          <div id="fwStatus" class="status">Připraveno.</div>
        </section>

        <section class="card">
          <h2>Aktualizace front-endu / filesystem image</h2>
          <div class="muted">Nahraj image filesystemu. Proběhne kontrola shody s velikostí filesystem partition.</div>
          <label for="fsImageFile">Filesystem image .bin</label>
          <input id="fsImageFile" type="file" accept=".bin,application/octet-stream" />
          <div class="drop">Přetáhni filesystem image sem nebo použij výběr níže.</div>
          <button onclick="uploadFsImage()">Nahrát filesystem image</button>
          <div class="progress"><i id="fsImgProg"></i></div>
          <div id="fsImageStatus" class="status">Připraveno.</div>
        </section>
      </div>
    </div>
  </div>
<script>
let lastFsInfo = null;
let currentDir = '/';
function setStatus(id, msg){ const el=document.getElementById(id); if(!el) return; if(typeof msg==='string'){ el.textContent=msg; return; } if(msg && typeof msg==='object'){ const parts=[]; if(msg.msg) parts.push(String(msg.msg)); else if(msg.err) parts.push(String(msg.err)); else if(msg.ok===true) parts.push('OK'); else if(msg.ok===false) parts.push('Chyba'); if(msg.path) parts.push(String(msg.path)); if(msg.from && msg.to) parts.push(String(msg.from)+' → '+String(msg.to)); if(msg.size!=null) parts.push('velikost: '+msg.size+' B'); if(msg.loadedBytes!=null) parts.push('načteno: '+human(msg.loadedBytes)); if(msg.receivedBytes!=null) parts.push('přijato: '+human(msg.receivedBytes)); if(msg.partitionBytes!=null) parts.push('partition: '+human(msg.partitionBytes)); el.textContent=parts.join(' | ') || 'Hotovo.'; return; } el.textContent=String(msg||''); }
function human(n){ n=Number(n||0); if(n<1024) return n+' B'; if(n<1048576) return (n/1024).toFixed(1)+' KB'; return (n/1048576).toFixed(2)+' MB'; }
function esc(s){ return String(s).replace(/[&<>"']/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m])); }
async function getJson(url, init){ const r = await fetch(url, init); const t = await r.text(); let j=null; try{j=JSON.parse(t)}catch(e){} if(!r.ok) throw new Error((j&&j.msg)||(j&&j.err)||t||('HTTP '+r.status)); return j||{}; }
function normDir(path){ path=String(path||'/').trim()||'/'; if(!path.startsWith('/')) path='/'+path; path=path.replace(/\/+/g,'/'); if(path.length>1 && path.endsWith('/')) path=path.slice(0,-1); return path||'/'; }
function parentDir(path){ path=normDir(path); if(path==='/') return null; const i=path.lastIndexOf('/'); return i<=0?'/':path.slice(0,i); }
function pathDir(path){ path=normDir(path); if(path==='/') return '/'; const i=path.lastIndexOf('/'); return i<=0?'/':path.slice(0,i); }
function pathBase(path){ path=normDir(path); if(path==='/') return '/'; const i=path.lastIndexOf('/'); return path.slice(i+1)||'/'; }
function buildDirs(files){ const dirs=new Set(['/']); (files||[]).forEach(f=>{ const p=normDir(f.path||'/'); if(f.dir) dirs.add(p); let d=pathDir(p); while(d){ dirs.add(d); d=parentDir(d); } }); return Array.from(dirs).sort((a,b)=>a.localeCompare(b)); }
function listCurrent(files){ const dir=normDir(currentDir); const out=[]; (files||[]).forEach(f=>{ const p=normDir(f.path||'/'); if(p===dir) return; if(pathDir(p)===dir) out.push(Object.assign({},f,{path:p})); }); out.sort((a,b)=>{ if(!!b.dir!==!!a.dir) return a.dir?-1:1; return pathBase(a.path).localeCompare(pathBase(b.path)); }); return out; }
function updateFsSummary(j){ document.getElementById('fsMountedChip').textContent='LittleFS: '+(j.fsMounted?'mounted':'not mounted'); document.getElementById('fsCountChip').textContent='Položky: '+((j.files||[]).length||0); document.getElementById('fsUsedChip').textContent='Obsazeno: '+human(j.totalBytes||0); document.getElementById('fsPathChip').textContent='Složka: '+normDir(currentDir); }
async function loadBasic(){ try{ const fast = await getJson('/api/fast'); const fs = lastFsInfo || await getJson('/api/fs/list'); document.getElementById('basicInfo').innerHTML = 'IP: <code>'+(fast.ip||'-')+'</code><br>Wi‑Fi: <code>'+(fast.wifi?'OK':'OFF')+'</code><br>LittleFS: <code>'+(fs.fsMounted?'mounted':'not mounted')+'</code><br>Firmware partition: <code>'+human(fs.firmwarePartitionBytes)+'</code><br>Filesystem partition: <code>'+human(fs.fsPartitionBytes)+'</code>'; }catch(e){ document.getElementById('basicInfo').textContent='Nepodařilo se načíst stav zařízení: '+e.message; } }
function renderDirTree(j){ const dirs=buildDirs(j.files||[]); const host=document.getElementById('dirTree'); document.getElementById('dirCountLbl').textContent=dirs.length+' složek'; document.getElementById('currentPath').textContent=normDir(currentDir); host.innerHTML=''; dirs.forEach(dir=>{ const depth=dir==='/'?0:dir.split('/').filter(Boolean).length; const item=document.createElement('div'); item.className='treeItem'+(normDir(currentDir)===dir?' active':''); item.style.paddingLeft=(10+depth*14)+'px'; item.innerHTML='<div>'+(dir==='/'?'🖴':'📁')+'</div><div class="name">'+esc(dir==='/'?'/':pathBase(dir))+'</div><div class="meta">'+esc(dir)+'</div>'; item.onclick=()=>{ currentDir=dir; renderFiles(lastFsInfo); renderDirTree(lastFsInfo); }; host.appendChild(item); }); }
function renderFiles(j){ const body=document.getElementById('filesBody'); const folder=listCurrent((j&&j.files)||[]); const dir=normDir(currentDir); document.getElementById('folderTitle').textContent='Obsah složky '+dir; document.getElementById('folderMeta').textContent=folder.length+' položek'; const fsPathEl=document.getElementById('fsPath'); if(fsPathEl && !fsPathEl.value.trim()) fsPathEl.value=(dir==='/'?'/':(dir+'/'));  body.innerHTML=''; const up=parentDir(dir); if(up){ const row=document.createElement('div'); row.className='fileRow'; row.innerHTML='<div class="name">↰ ..</div><div class="meta">složka</div><div class="mono">-</div><div class="acts"><button class="secondary small">Otevřít</button></div>'; row.querySelector('button').onclick=()=>{ currentDir=up; renderFiles(lastFsInfo); renderDirTree(lastFsInfo); }; body.appendChild(row); }
 folder.forEach(f=>{ const canView=!f.dir && /\.(txt|log|json|js|css|html|htm|md|csv|xml|ini|yaml|yml|svg)$/i.test(f.path); const row=document.createElement('div'); row.className='fileRow'; row.innerHTML='<div class="name">'+(f.dir?'📁 ':'📄 ')+esc(pathBase(f.path))+'<div class="meta mono">'+esc(f.path)+'</div></div><div class="meta">'+(f.dir?'složka':'soubor')+'</div><div class="mono">'+(f.dir?'-':human(f.size))+'</div><div class="acts"></div>'; const acts=row.querySelector('.acts'); const open=document.createElement('button'); open.type='button'; open.className='secondary small'; open.textContent=f.dir?'Otevřít':'Vybrat'; open.onclick=()=>{ if(f.dir){ currentDir=normDir(f.path); renderFiles(lastFsInfo); renderDirTree(lastFsInfo); } else { document.getElementById('fsPath').value=f.path; document.getElementById('editorPath').value=f.path; } }; acts.appendChild(open); if(canView){ const bView=document.createElement('button'); bView.type='button'; bView.className='secondary small'; bView.textContent='Editovat'; bView.onclick=()=>viewFile(f.path); acts.appendChild(bView);} const bRename=document.createElement('button'); bRename.type='button'; bRename.className='secondary small'; bRename.textContent='Přejm.'; bRename.onclick=()=>renamePath(f.path); acts.appendChild(bRename); const bDelete=document.createElement('button'); bDelete.type='button'; bDelete.className='danger small'; bDelete.textContent='Smazat'; bDelete.onclick=()=>deletePath(f.path); acts.appendChild(bDelete); body.appendChild(row); }); if(!up && !folder.length){ body.innerHTML='<div class="muted" style="padding:12px">LittleFS je prázdné.</div>'; } updateFsSummary(j); }
async function refreshFiles(){ try{ const j = await getJson('/api/fs/list'); lastFsInfo = j; const dirs=buildDirs(j.files||[]); if(!dirs.includes(normDir(currentDir))) currentDir='/'; renderDirTree(j); renderFiles(j); await loadBasic(); }catch(e){ alert('Chyba při načtení seznamu souborů: '+e.message); } }
async function promptCreateFolder(){ const base=normDir(currentDir); const path = prompt('Cesta nové složky v LittleFS', base==='/'?'/assets':(base+'/new-folder')); if(!path) return; try{ await getJson('/api/fs/mkdir?path='+encodeURIComponent(path), {method:'POST'}); currentDir=pathDir(path); await refreshFiles(); }catch(e){ alert('Chyba: '+e.message); } }
async function renamePath(from){ const to = prompt('Nová cesta / nový název', from); if(!to || to===from) return; try{ await getJson('/api/fs/rename?from='+encodeURIComponent(from)+'&to='+encodeURIComponent(to), {method:'POST'}); currentDir=pathDir(to); await refreshFiles(); }catch(e){ alert('Chyba: '+e.message); } }
async function deletePath(path){ if(!confirm('Opravdu smazat '+path+' ?')) return; try{ await getJson('/api/fs/delete?path='+encodeURIComponent(path), {method:'POST'}); if(normDir(currentDir)===normDir(path)) currentDir=parentDir(path)||'/'; await refreshFiles(); }catch(e){ alert('Chyba: '+e.message); } }
async function viewFile(path){ try{ const limit = 16384; let offset = 0; let content = ''; let meta = null; for(let i=0;i<32;i++){ setStatus('viewerStatus', 'Načítám '+path+'… '+human(offset)); const j = await getJson('/api/fs/read?path='+encodeURIComponent(path)+'&offset='+offset+'&limit='+limit); if(!meta) meta = j||{}; content += (j && typeof j.content==='string') ? j.content : ''; const nextOffset = Number(j && j.nextOffset); const truncated = !!(j && j.truncated); if(!truncated || !Number.isFinite(nextOffset) || nextOffset <= offset){ meta = Object.assign({}, meta, j||{}); break; } offset = nextOffset; meta = Object.assign({}, meta, j||{}); } document.getElementById('editorPath').value = (meta && meta.path) || path || ''; document.getElementById('fsPath').value=(meta&&meta.path)||path||''; setStatus('viewerStatus', {path:(meta&&meta.path)||path,size:meta&&meta.size,loadedBytes:content.length,truncated:!!(meta&&meta.truncated)}); document.getElementById('viewerContent').value = content; }catch(e){ setStatus('viewerStatus','Chyba: '+e.message); document.getElementById('viewerContent').value=''; } }
function viewCurrentEditorPath(){ const path=document.getElementById('editorPath').value.trim(); if(!path){ setStatus('viewerStatus','Zadej cestu souboru.'); return; } viewFile(path); }
async function saveEditorFile(){ const path=document.getElementById('editorPath').value.trim(); if(!path){ setStatus('viewerStatus','Zadej cestu souboru.'); return; } try{ const content=document.getElementById('viewerContent').value; const j = await getJson('/api/fs/write?path='+encodeURIComponent(path), {method:'POST', headers:{'Content-Type':'text/plain; charset=utf-8'}, body:content}); setStatus('viewerStatus', j); currentDir=pathDir(path); await refreshFiles(); }catch(e){ setStatus('viewerStatus','Chyba: '+e.message); } }
function promptCreateTextFile(){ const base=normDir(currentDir); const path = prompt('Cesta nového textového souboru v LittleFS', base==='/'?'/notes.txt':(base+'/notes.txt')); if(!path) return; document.getElementById('editorPath').value = path; document.getElementById('fsPath').value = path; document.getElementById('viewerContent').value = ''; setStatus('viewerStatus', 'Připraven nový soubor: '+path); }
function setProg(id, loaded, total){ const el=document.getElementById(id); if(!el) return; const pct = total>0 ? Math.max(0, Math.min(100, Math.round((loaded*100)/total))) : 0; el.style.width = pct + '%'; }
async function uploadForm(url, fileInputId, statusId, extraFields, progId){ const inp=document.getElementById(fileInputId); if(!inp.files||!inp.files[0]){ setStatus(statusId,'Vyber soubor.'); return; } const f=inp.files[0]; const fd=new FormData(); fd.append('file', f); if(extraFields){ for(const [k,v] of Object.entries(extraFields)){ fd.append(k,v); } } setStatus(statusId,'Nahrávám '+f.name+' …'); setProg(progId,0,f.size||0); return await new Promise((resolve,reject)=>{ const xhr=new XMLHttpRequest(); xhr.open('POST', url, true); xhr.upload.onprogress=(e)=>{ setProg(progId,e.loaded||0,e.total||f.size||0); setStatus(statusId,'Nahrávám '+f.name+' … '+(e.total?Math.round((e.loaded*100)/e.total):0)+'%'); }; xhr.onerror=()=>reject(new Error('Network error')); xhr.onabort=()=>reject(new Error('Upload aborted')); xhr.onload=()=>{ let j={}; try{ j=JSON.parse(xhr.responseText||'{}'); }catch(e){} if(xhr.status>=200&&xhr.status<300){ setStatus(statusId,j); setProg(progId, Number(j.receivedBytes||f.size||0), Number(j.partitionBytes||f.size||0)); resolve(j);} else reject(new Error((j&&j.msg)||(j&&j.err)||xhr.responseText||('HTTP '+xhr.status))); }; xhr.send(fd); }); }
async function uploadFsFile(){ const path=document.getElementById('fsPath').value.trim(); try{ await uploadForm('/api/fs/upload','fsFile','uploadFsStatus', path?{path}:{}, 'fsProg'); if(path) currentDir=pathDir(path); await refreshFiles(); }catch(e){ setStatus('uploadFsStatus','Chyba: '+e.message); } }
async function uploadFirmware(){ try{ if(lastFsInfo) setStatus('fwStatus', 'Firmware partition: '+human(lastFsInfo.firmwarePartitionBytes)+'\nKontrola velikosti proběhne při uploadu.'); const j = await uploadForm('/api/update/firmware','fwFile','fwStatus', null, 'fwProg'); setStatus('fwStatus',j); }catch(e){ setStatus('fwStatus','Chyba: '+e.message); } }
async function uploadFsImage(){ try{ if(lastFsInfo) setStatus('fsImageStatus', 'Filesystem partition: '+human(lastFsInfo.fsPartitionBytes)+'\nImage musí odpovídat velikosti partition.'); const j = await uploadForm('/api/update/filesystem','fsImageFile','fsImageStatus', null, 'fsImgProg'); setStatus('fsImageStatus',j); }catch(e){ setStatus('fsImageStatus','Chyba: '+e.message); } }
loadBasic(); refreshFiles();
</script>
</body>
</html>
)HTML";

static const char WEB_APP_CSS[] PROGMEM = "/* fallback UI uses inline CSS */\n";
static const char WEB_APP_JS[] PROGMEM  = "/* fallback UI uses inline JS */\n";
