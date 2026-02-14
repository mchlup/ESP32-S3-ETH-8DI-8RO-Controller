// diag.js
window.App = window.App || {};

App.registerWidget({
  id:'diag',
  title:'Diagnostika',
  defaultSpan:12,
  render(el){
    el.innerHTML = `
      <div class="card__title">Diagnostika</div>
      <pre class="log" id="diagLog">{}</pre>
    `;
  },
  update(fast){
    const pre = document.getElementById('diagLog');
    if (!pre) return;
    try { pre.textContent = JSON.stringify(fast, null, 2); }
    catch(e){ pre.textContent = '{}'; }
  }
});
