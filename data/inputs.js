// inputs.js
window.App = window.App || {};
const INPUT_LABELS = [
  'Požadavek TUV',
  'Den / noc',
  'Požadavek cirkulace',
  'Volné','Volné','Volné','Volné','Volné'
];

function inputBitmaskToBool(mask, idx){
  if (mask==null) return false;
  return (Number(mask) & (1 << idx)) !== 0;
}

App.registerWidget({
  id:'inputs',
  title:'Vstupy',
  defaultSpan:6,
  render(root){
    const card = document.createElement('div');
    card.className = 'card span6';
    card.innerHTML = `<div class="card__head"><div><div class="card__title">Vstupy</div></div></div><div class="inputs" id="w_inputs"></div>`;
    root.append(card);
    const list = card.querySelector('#w_inputs');
    for (let i=0;i<8;i++){
      const el = document.createElement('div');
      el.className='item';
      el.innerHTML=`<div class="label"><div class="id">IN${i+1}</div><div class="desc">${INPUT_LABELS[i]}</div></div><div class="value" id="in_${i}">—</div>`;
      list.append(el);
    }
  },
  update(fast){
    const mask = fast?.i ?? 0;
    for(let i=0;i<8;i++){
      const on = inputBitmaskToBool(mask,i);
      const t = (i===0) ? (on ? 'požadavek' : 'bez požadavku')
            : (i===1) ? (on ? 'noc' : 'den')
            : (i===2) ? (on ? 'cirkulace ON' : 'cirkulace OFF')
            : (on ? 'active' : 'inactive');
      const el = document.getElementById(`in_${i}`);
      if (el) el.textContent = t;
    }
  }
});
