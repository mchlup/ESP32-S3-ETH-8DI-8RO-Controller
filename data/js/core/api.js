(function(){
  const withTimeout = async (promise, ms=8000) => {
    let timer;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Timeout')), ms);
    });
    try {
      return await Promise.race([promise, timeout]);
    } finally {
      clearTimeout(timer);
    }
  };

  const request = async (url, opts = {}) => {
    const res = await withTimeout(fetch(url, opts), opts.timeoutMs || 8000);
    const ct = res.headers.get('content-type') || '';
    const isJson = ct.includes('application/json');
    const body = isJson ? await res.json().catch(() => null) : await res.text().catch(() => '');
    if (!res.ok) {
      const err = (body && body.error) ? body.error : `${res.status} ${res.statusText}`;
      throw new Error(err);
    }
    return body;
  };

  const getJson = (url) => request(url, { cache: 'no-store' });
  const getText = (url) => request(url, { cache: 'no-store' });
  const postJson = (url, body) => request(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  });
  const postText = (url, text) => request(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: text,
  });

  window.Core = window.Core || {};
  window.Core.api = { getJson, getText, postJson, postText };
})();
