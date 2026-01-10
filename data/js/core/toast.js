(function(){
  const toast = (msg, kind = "good", opts = {}) => {
    const host = document.getElementById('toasts');
    if (!host) return;
    const el = document.createElement('div');
    el.className = `toast ${kind || ""}`;
    el.setAttribute('role', 'status');
    el.innerHTML = `
      <div class="toastMsg">${window.Core?.util?.escapeHtml(msg)}</div>
      ${opts.actionLabel ? `<button class="toastBtn" type="button">${window.Core?.util?.escapeHtml(opts.actionLabel)}</button>` : ""}
      <button class="toastClose" type="button" aria-label="Zavřít">×</button>
    `;
    host.appendChild(el);

    const remove = () => {
      if (!el.isConnected) return;
      el.classList.add('hide');
      setTimeout(() => el.remove(), 220);
    };

    const btn = el.querySelector('.toastBtn');
    if (btn && typeof opts.onAction === 'function') {
      btn.addEventListener('click', () => {
        opts.onAction();
        remove();
      });
    }
    el.querySelector('.toastClose')?.addEventListener('click', remove);
    const timeout = Number.isFinite(opts.timeoutMs) ? opts.timeoutMs : 3800;
    if (timeout > 0) setTimeout(remove, timeout);
  };

  window.Core = window.Core || {};
  window.Core.toast = { show: toast };
})();
