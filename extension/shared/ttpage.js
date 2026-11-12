// DDM: runs inside TikTok's own page. TikTok loads the posts of the feed, search and profiles in the background; this
// keeps a copy of each post's data (caption, cover, every quality) for the "Download this video" button, so DDM never
// has to ask TikTok for a post TikTok hides from programs. Nothing is changed or sent anywhere but to DDM's button.
(() => {
  if (window.__ddmTikTok) return;
  window.__ddmTikTok = true;
  const send = (items) => { try { if (items.length) window.postMessage({ __ddmTikTok: items }, location.origin); } catch (e) {} };
  const collect = (j, out, depth) => {
    if (!j || typeof j !== 'object' || depth > 8 || out.length > 60) return;
    if (Array.isArray(j)) { for (const x of j) collect(x, out, depth + 1); return; }
    if (typeof j.id === 'string' && /^\d{15,}$/.test(j.id) && (j.video || j.imagePost) && j.author) {
      try { out.push(JSON.stringify(j)); } catch (e) {}
      return;
    }
    for (const k in j) { const v = j[k]; if (v && typeof v === 'object') collect(v, out, depth + 1); }
  };
  const look = (url, text) => {
    try {
      if (!/\/api\//.test(String(url || '')) || !text || text.length > 8e6 || text[0] !== '{') return;
      const out = [];
      collect(JSON.parse(text), out, 0);
      send(out);
    } catch (e) {}
  };
  try {
    const f = window.fetch;
    if (typeof f === 'function') {
      window.fetch = function (input, init) {
        const p = f.apply(this, arguments);
        try {
          const url = typeof input === 'string' ? input : (input && input.url) || '';
          if (/\/api\//.test(url)) p.then((r) => { try { r.clone().text().then((t) => look(url, t)).catch(() => {}); } catch (e) {} }).catch(() => {});
        } catch (e) {}
        return p;
      };
    }
  } catch (e) {}
  try {
    const X = XMLHttpRequest.prototype, open = X.open, sendX = X.send;
    X.open = function (m, url) { try { this.__ddmUrl = String(url || ''); } catch (e) {} return open.apply(this, arguments); };
    X.send = function () {
      try {
        if (/\/api\//.test(this.__ddmUrl || '')) this.addEventListener('load', () => {
          try { if (!this.responseType || this.responseType === 'text') look(this.__ddmUrl, this.responseText); else if (this.responseType === 'json') look(this.__ddmUrl, JSON.stringify(this.response)); } catch (e) {}
        });
      } catch (e) {}
      return sendX.apply(this, arguments);
    };
  } catch (e) {}
})();
