(function () {
  const withTimeout = (promise, timeoutMs = 6000) => {
    let t;
    const timeout = new Promise((_, reject) => {
      t = setTimeout(() => reject(new Error("Timeout")), timeoutMs);
    });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(t));
  };

  const normalizeError = (err) => {
    if (err && err.message) return err;
    return new Error(String(err || "Neznámá chyba"));
  };

  const getJson = async (url, opts = {}) => {
    try {
      const res = await withTimeout(fetch(url, { cache: "no-store", ...opts }), opts.timeoutMs);
      if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
      return await res.json();
    } catch (e) {
      throw normalizeError(e);
    }
  };

  const getText = async (url, opts = {}) => {
    try {
      const res = await withTimeout(fetch(url, { cache: "no-store", ...opts }), opts.timeoutMs);
      if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
      return await res.text();
    } catch (e) {
      throw normalizeError(e);
    }
  };

  const postJson = async (url, body, opts = {}) => {
    try {
      const res = await withTimeout(fetch(url, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(body),
        ...opts,
      }), opts.timeoutMs);
      const ct = res.headers.get("content-type") || "";
      const payload = ct.includes("application/json") ? await res.json().catch(() => null) : await res.text().catch(() => null);
      if (!res.ok) throw new Error(payload && payload.error ? payload.error : `${res.status} ${res.statusText}`);
      return payload;
    } catch (e) {
      throw normalizeError(e);
    }
  };

  window.Core = window.Core || {};
  window.Core.api = { getJson, getText, postJson };
})();
