// relays.js
window.App = window.App || {};
const RELAY_LABELS = [
  'Směšovací ventil A (OPEN)',
  'Směšovací ventil B (CLOSE)',
  'TUV ventil (přepínací)',
  'Cirkulace TUV',
  'Požadavek kotle: TUV',
  'Křivka: den / noc',
  'Omezení výkonu kotle',
  'Topná tyč AKU',
];

function relayBitmaskToBool(mask, idx){
  if (mask==null) return false;
  return (Number(mask) & (1 << idx)) !== 0;
}

async function relayCmd(id, cmd){
  const payload = { id:Number(id) };
  if(cmd === 'toggle') payload.toggle = true;
  else payload.on = cmd === 'on';
  await App.api.postJson('/api/relay', payload);
}

App.registerWidget({
  id:'relays',
  title:'Relé',
  defaultSpan:6,
  render(card){
    card.innerHTML = `
      <div class="hint">Popisky odpovídají pevné HW konfiguraci ve firmwaru.</div>
      <div class="relays" id="relaysGrid"></div>
    `;
    const grid = card.querySelector('#relaysGrid');
    for (let i=0;i<8;i++){
      const tile = document.createElement('div');
      tile.className = 'tile';
      tile.dataset.relay = String(i+1);
      tile.innerHTML = `
        <div class="t">
          <div class="head">
            <div class="id">R${i+1}</div>
            <div class="desc">${RELAY_LABELS[i]||''}</div>
          </div>
          <div class="badges">
            <span class="badge fixed">FIXED</span>
            <span class="badge off" data-state>OFF</span>
          </div>
        </div>
        <div class="row">
          <button class="btn" data-on>ON</button>
          <button class="btn" data-off>OFF</button>
          <button class="btn" data-tgl>Toggle</button>
        </div>
      `;
      if(i === 7){
        tile.querySelector('[data-on]').disabled = true;
        tile.querySelector('[data-tgl]').disabled = true;
        tile.querySelector('[data-on]').title = 'R8 topná tyč vyžaduje servisní bezpečnostní příkaz';
        tile.querySelector('[data-tgl]').title = 'R8 topná tyč vyžaduje servisní bezpečnostní příkaz';
      }
      tile.querySelector('[data-on]').onclick = ()=>relayCmd(i+1,'on');
      tile.querySelector('[data-off]').onclick = ()=>relayCmd(i+1,'off');
      tile.querySelector('[data-tgl]').onclick = ()=>relayCmd(i+1,'toggle');
      grid.appendChild(tile);
    }
  },
  update(state, root){
    const mask = state?.r;
    root.querySelectorAll('[data-relay]').forEach(tile=>{
      const id = Number(tile.dataset.relay);
      const on = relayBitmaskToBool(mask, id-1);
      const badge = tile.querySelector('[data-state]');
      badge.className = 'badge ' + (on ? 'on' : 'off');
      badge.textContent = on ? 'ON' : 'OFF';
    });
  }
});
