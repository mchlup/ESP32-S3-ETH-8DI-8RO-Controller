(function(){
  const loaded = new Map();

  const loadScript = (src) => {
    const url = String(src || "").trim();
    if (!url) return Promise.reject(new Error("Missing script src"));
    if (loaded.has(url)) return loaded.get(url);

    const p = new Promise((resolve, reject) => {
      const s = document.createElement("script");
      s.src = url;
      s.async = false; // keep execution order
      s.onload = () => resolve(true);
      s.onerror = () => reject(new Error(`Failed to load ${url}`));
      document.head.appendChild(s);
    });

    loaded.set(url, p);
    return p;
  };

  window.Core = window.Core || {};
  window.Core.loader = { loadScript };
})();
