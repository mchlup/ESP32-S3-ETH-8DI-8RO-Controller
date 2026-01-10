(function () {
  const cache = new Map();
  let currentId = null;
  let currentUnmount = null;

  const normalize = (hash) => {
    const raw = String(hash || "").replace("#", "").trim();
    return raw || "dashboard";
  };

  const fetchFragment = async (pageId) => {
    if (cache.has(pageId)) return cache.get(pageId);
    const html = await window.Core.api.getText(`/pages/${pageId}.html`);
    cache.set(pageId, html);
    return html;
  };

  const setActiveNav = (pageId) => {
    const items = document.querySelectorAll("#nav .navItem");
    items.forEach((el) => {
      const href = el.getAttribute("href") || "";
      el.classList.toggle("active", href === `#${pageId}`);
    });
  };

  const setTitle = (pageId) => {
    const h1 = document.querySelector(".h1");
    if (!h1) return;
    const activeNav = document.querySelector("#nav .navItem.active");
    const label = activeNav?.textContent?.trim();
    if (pageId === "dashboard") h1.textContent = "Řídicí panel";
    else h1.textContent = label || "Heat Controller";
  };

  const mountPage = async (pageId) => {
    const view = document.getElementById("view");
    if (!view) return;

    if (currentUnmount) {
      try { currentUnmount(); } catch (_) {}
      currentUnmount = null;
    }

    let html = "";
    try {
      html = await fetchFragment(pageId);
    } catch (e) {
      window.Core.toast?.show?.(`Nelze načíst stránku ${pageId}: ${e.message}`, "bad");
    }
    view.innerHTML = `<section class="page" data-page="${pageId}">${html}</section>`;

    if (window.Core?.legacy?.mountLegacySections) {
      await window.Core.legacy.mountLegacySections();
    }
    if (window.Core?.legacy?.onPageLoad) {
      window.Core.legacy.onPageLoad();
    }

    const page = window.Pages?.[pageId];
    if (page && typeof page.mount === "function") {
      const ctx = {
        root: view,
        store: window.Core.store,
        api: window.Core.api,
        toast: window.Core.toast,
        roles: window.Core.roles,
        dirty: window.Core.dirty,
        navigate: (hash) => navigate(hash),
      };
      page.mount(ctx);
      if (page.unmount) currentUnmount = () => page.unmount();
    }

    currentId = pageId;
    setActiveNav(pageId);
    setTitle(pageId);
    document.getElementById("sidebar")?.classList.remove("open");
  };

  const navigate = (hash) => {
    const pageId = normalize(hash);
    if (pageId === currentId) return;
    history.replaceState(null, "", `#${pageId}`);
    mountPage(pageId);
  };

  const start = () => {
    const pageId = normalize(location.hash);
    mountPage(pageId);
    window.addEventListener("hashchange", () => {
      mountPage(normalize(location.hash));
    });
  };

  const prefetch = (pageIds = []) => {
    pageIds.forEach((id) => {
      if (cache.has(id)) return;
      fetchFragment(id).catch(() => {});
    });
  };

  window.Core = window.Core || {};
  window.Core.router = { start, navigate, prefetch };
})();
