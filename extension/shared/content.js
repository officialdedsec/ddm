/*
 * DDM browser extension — the "Download this video" button. When you point at a video (or the main video of a
 * page starts playing) a small button appears over it; click it to see every quality the site offers and pick one —
 * DDM opens with that quality. Nothing downloads, and no window opens, until you click. Works for each new video
 * when a feed scrolls (Facebook Reels, TikTok, YouTube Shorts, Instagram Reels…): each video has its own address.
 * Built only with DOM calls (no HTML strings, no style tags), so strict pages (Trusted Types, CSP) can't block it.
 */
(function () {
  'use strict';
  if (window.__ddmVideoButton) return;
  window.__ddmVideoButton = true;
  const api = typeof browser !== 'undefined' ? browser : chrome;
  let off = false, enabled = true;

  try { Promise.resolve(api.runtime.sendMessage({ type: 'ddm-state?' })).then((r) => { off = !!(r && r.off); enabled = !(r && r.button === false); }).catch(() => {}); } catch (e) {}
  api.runtime.onMessage.addListener((m, sender, reply) => {
    if (m && m.type === 'ddm-state') { off = !!m.off; if (typeof m.button === 'boolean') enabled = m.button; if (off || !enabled) hideAll(); return; }
    if (m && m.type === 'ddm-newpage') { pageTurn++; answers.clear(); prefetched.clear(); return; }
    if (m && m.type === 'ddm-read-lists' && Array.isArray(m.urls)) {
      // a stream's list of qualities, read the way the page's own player reads it (same address, same site)
      const out = {};
      Promise.all(m.urls.slice(0, 4).filter((u) => /^https?:/i.test(u)).map(async (u) => {
        try {
          const ctl = new AbortController();
          const timer = setTimeout(() => ctl.abort(), 3000);
          const res = await fetch(u, { signal: ctl.signal });
          const t = res.ok ? await res.text() : '';
          clearTimeout(timer);
          if (t.length < 1500000 && (t.includes('#EXTM3U') || t.includes('<MPD'))) out[u] = t;
        } catch (e) {}
      })).then(() => reply(out), () => reply(out));
      return true;
    }
  });

  const host = location.hostname.replace(/^www\./, '').replace(/^m\./, '');
  const abs = (u) => { try { return new URL(u, location.href).href; } catch (e) { return ''; } };

  // An advert playing instead of the video. YouTube marks its player while an ad runs; elsewhere only whole class
  // names / ids that say "ad" count (YouTube's player always carries "ad-created", which is not an ad).
  function inAd(v) {
    if (/youtube/.test(host)) {
      const p = v.closest('.html5-video-player') || document.querySelector('#movie_player');
      return !!(p && (p.classList.contains('ad-showing') || p.classList.contains('ad-interrupting')));
    }
    const words = new Set(['ad', 'ads', 'advert', 'advertisement', 'sponsored', 'preroll', 'ima-ad-container', 'video-ads']);
    for (let el = v; el && el !== document.body; el = el.parentElement) {
      if (el.id && words.has(el.id.toLowerCase())) return true;
      for (const c of el.classList || []) if (words.has(c.toLowerCase())) return true;
    }
    return false;
  }

  // The page is about this one video (a watch page, a reel, a short, a post), or the video fills most of the window.
  function isMainVideo(v, r) {
    const vw = window.innerWidth || 1, vh = window.innerHeight || 1;
    const visW = Math.max(0, Math.min(r.right, vw) - Math.max(r.left, 0)), visH = Math.max(0, Math.min(r.bottom, vh) - Math.max(r.top, 0));
    const share = (visW * visH) / (vw * vh);
    const videoPageUrl = /\/(watch|shorts|reel|reels|video|videos|status|tv|p)\b/.test(location.pathname) || /[?&]v=/.test(location.search) || /tiktok\.com/.test(host);
    return share >= 0.45 || (videoPageUrl && (share >= 0.18 || visH >= vh * 0.5));
  }

  function firstLink(root, re) {
    if (!root) return '';
    for (const a of root.querySelectorAll('a[href]')) { const u = abs(a.getAttribute('href')); if (re.test(u)) return u; }
    return '';
  }

  // The address of THIS video (what DDM needs to list its formats), not just the page's.
  function videoPage(v) {
    const here = location.href;
    if (/youtube\.com|youtube-nocookie\.com/.test(host)) {
      const emb = location.pathname.match(/\/embed\/([\w-]{6,})/);
      if (emb) return 'https://www.youtube.com/watch?v=' + emb[1];
      return here;
    }
    if (/tiktok\.com/.test(host)) {
      // the video under the pointer, not the address bar: when you swipe, the address changes late (or not at all)
      let id = '';
      for (let el = v; el && !id; el = el.parentElement) {   // player wrappers carry the video id: "xgwrapper-0-7421…"
        const m = (el.id || '').match(/(\d{15,})$/);
        if (m) id = m[1];
      }
      const box = v.closest('[data-e2e="recommend-list-item-container"], article, [class*="ItemContainer"], [class*="DivItem"], section') || v.parentElement;
      const link = firstLink(box, /tiktok\.com\/@[^/]*\/(video|photo)\/\d+/);
      if (link && (!id || link.indexOf(id) >= 0)) return link.split('?')[0];
      const at = location.pathname.match(/\/@([^/]*)\/video\/(\d+)/);
      if (id) {
        if (at && at[2] === id) return here.split('?')[0];
        const who = box && box.querySelector('[data-e2e="video-author-uniqueid"], [data-e2e="browse-username"], a[href^="/@"]');
        const user = who ? ((who.getAttribute('href') || '').match(/^\/@([^/?]+)/) || [, who.textContent.trim().replace(/^@/, '')])[1] : '';
        return 'https://www.tiktok.com/@' + (user || '') + '/video/' + id;
      }
      if (at) return here.split('?')[0];
      return here;
    }
    if (/facebook\.com|fb\.watch/.test(host)) {
      if (/\/(reel|reels|watch|videos|share\/v|share\/r)\//.test(location.pathname) || /[?&]v=\d+/.test(location.search)) return here;
      const box = v.closest('[role="article"], [data-pagelet], [aria-posinset]') || v.parentElement;
      const link = firstLink(box, /facebook\.com\/(reel\/\d+|[^/]+\/videos\/|watch\/?\?v=|share\/[vr]\/)/);
      return link || here;
    }
    if (/instagram\.com/.test(host)) {
      if (/\/(reel|reels|p|tv)\/[\w-]+/.test(location.pathname)) return here.split('?')[0];
      const box = v.closest('article') || v.parentElement;
      return firstLink(box, /instagram\.com\/(reel|p|tv)\/[\w-]+/) || here;
    }
    if (/twitter\.com|x\.com/.test(host)) {
      const box = v.closest('article');
      return firstLink(box, /\/status\/\d+/) || here;
    }
    return here;
  }

  function titleOf(v, page) {
    if (/youtube/.test(host)) {
      const t = document.querySelector('h1.ytd-watch-metadata, h1.title, #title h1, yt-formatted-string.ytd-reel-player-header-renderer');
      if (t && t.textContent.trim()) return t.textContent.trim();
    }
    if (/tiktok\.com/.test(host)) {   // the caption next to this video (a feed shows many)
      const box = v.closest('[data-e2e="recommend-list-item-container"], article, [class*="ItemContainer"], [class*="DivItem"], section');
      const d = (box && box.querySelector('[data-e2e="video-desc"], [data-e2e="browse-video-desc"]')) || (/\/video\/\d+/.test(location.pathname) ? document.querySelector('[data-e2e="browse-video-desc"], [data-e2e="video-desc"]') : null);
      if (d && d.textContent.trim()) return d.textContent.trim();
    }
    const label = (v.getAttribute('aria-label') || v.getAttribute('title') || '').trim();
    if (label && !/^(video|media|the video|untitled)?\s*(player|content|element)?$/i.test(label) && !/player$/i.test(label)) return label;
    return pageName();
  }
  // The video's own name on an ordinary page: the page's heading when the tab's title is that heading plus the
  // site's slogan ("Woman Swiping On A Smartphone - Free HD Video Clips & Stock Footage at SiteName" -> the heading).
  function pageName() {
    const full = (document.title || '').trim();
    try {
      const norm = (s) => s.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ' ').trim();
      const nf = norm(full);
      const heads = [...document.querySelectorAll('h1')].map((h) => (h.textContent || '').replace(/\s+/g, ' ').trim()).filter((t) => t.length >= 3 && t.length <= 200);
      for (const t of heads) { const n = norm(t); if (n.length >= 3 && nf.includes(n)) return t; }
      const og = document.querySelector('meta[property="og:title"], meta[name="og:title"], meta[name="twitter:title"]');
      const o = og ? (og.getAttribute('content') || '').replace(/\s+/g, ' ').trim() : '';
      if (o.length >= 3 && o.length <= 200) { const n = norm(o); if (!nf || nf.includes(n) || n.length < nf.length) return o; }
      if (!full && heads.length) return heads[0];
    } catch (e) {}
    return full;
  }


  // ------------------------------------------------------------------------------------------ the button and menu
  const LIME = '#C6F432', INK = '#16131C', INK2 = '#25212D', TEXT = '#F4F4F6', MUTED = '#A9A6B2';
  let ui = null, shadow = null, btn = null, label = null, panel = null, list = null;
  let current = null, hideTimer = 0, panelOpen = false, busyToken = 0, lastMove = 0, mouse = { x: -1, y: -1 };
  let overBtn = false;
  const REST = 0.45;   // see-through until the pointer is on it
  const hiddenFor = new Set();      // videos the user closed the button on
  const prefetched = new Set();

  function css(el, styles) { for (const k in styles) el.style.setProperty(k, styles[k], 'important'); return el; }
  function el(tag, styles, text) { const e = document.createElement(tag); if (styles) css(e, styles); if (text !== undefined) e.textContent = text; return e; }
  function svg(paths, size, color, width) {
    const ns = 'http://www.w3.org/2000/svg', s = document.createElementNS(ns, 'svg');
    s.setAttribute('viewBox', '0 0 24 24'); s.setAttribute('width', size); s.setAttribute('height', size);
    for (const d of paths) {
      const p = document.createElementNS(ns, 'path');
      p.setAttribute('d', d); p.setAttribute('fill', 'none'); p.setAttribute('stroke', color); p.setAttribute('stroke-width', width || '2.4');
      p.setAttribute('stroke-linecap', 'round'); p.setAttribute('stroke-linejoin', 'round');
      s.appendChild(p);
    }
    return s;
  }
  const FONT = '600 13px/1.2 "Segoe UI Variable Text","Segoe UI",system-ui,-apple-system,Roboto,Arial,sans-serif';

  function ensureUI() {
    if (ui && ui.isConnected) return;
    ui = document.createElement('ddm-video-button');
    css(ui, { all: 'initial', position: 'absolute', left: '0', top: '0', width: '0', height: '0', 'z-index': '2147483647', display: 'block', 'pointer-events': 'none' });
    shadow = ui.attachShadow({ mode: 'closed' });
    // the pill
    btn = el('div', { position: 'fixed', display: 'none', 'align-items': 'center', gap: '7px', height: '32px', padding: '0 12px 0 10px', 'border-radius': '16px',
      background: INK, color: TEXT, font: FONT, cursor: 'pointer', 'pointer-events': 'auto', 'box-shadow': '0 4px 14px rgba(0,0,0,.35)', border: '1px solid rgba(255,255,255,.12)',
      'user-select': 'none', 'white-space': 'nowrap', transition: 'background .12s, opacity .15s', opacity: String(REST) });
    btn.appendChild(svg(['M12 4v11', 'M7.5 10.5L12 15l4.5-4.5', 'M5 19.5h14'], 17, LIME));
    label = el('span', { font: FONT, color: TEXT }, 'Download this video');
    btn.appendChild(label);
    const close = el('span', { display: 'inline-flex', 'margin-left': '4px', opacity: '.65', 'border-radius': '9px', padding: '2px' });
    close.appendChild(svg(['M7 7l10 10', 'M17 7L7 17'], 13, TEXT));
    close.title = 'Hide the button for this video';
    btn.appendChild(close);
    btn.title = 'DDM: download this video';
    btn.addEventListener('mouseenter', () => { overBtn = true; css(btn, { background: INK2, opacity: '1' }); clearTimeout(hideTimer); });
    btn.addEventListener('mouseleave', () => { overBtn = false; css(btn, { background: INK, opacity: panelOpen ? '1' : String(REST) }); scheduleHide(); });
    btn.addEventListener('click', (e) => {
      e.preventDefault(); e.stopPropagation();
      if (e.target === close || close.contains(e.target)) { if (current) hiddenFor.add(keyOf(current)); hideAll(); return; }
      panelOpen ? closePanel() : openPanel();
    }, true);
    for (const t of ['mousedown', 'mouseup', 'pointerdown', 'pointerup', 'dblclick']) btn.addEventListener(t, (e) => e.stopPropagation(), true);
    // the quality menu
    panel = el('div', { position: 'fixed', display: 'none', width: '290px', 'max-height': '360px', 'overflow-y': 'auto', 'overscroll-behavior': 'contain', background: INK, color: TEXT, 'border-radius': '12px',
      'box-shadow': '0 10px 30px rgba(0,0,0,.45)', border: '1px solid rgba(255,255,255,.12)', 'pointer-events': 'auto', padding: '6px', font: FONT,
      'scrollbar-width': 'thin', 'scrollbar-color': 'rgba(255,255,255,.28) transparent' });
    const sb = document.createElement('style');   // the same dark, thin scroll bar in Chrome, Edge and Opera
    sb.textContent = 'div::-webkit-scrollbar{width:8px}div::-webkit-scrollbar-track{background:transparent}div::-webkit-scrollbar-thumb{background:rgba(255,255,255,.28);border-radius:4px}div::-webkit-scrollbar-button{display:none;height:0}';
    shadow.appendChild(sb);
    panel.addEventListener('mouseenter', () => clearTimeout(hideTimer));
    panel.addEventListener('mouseleave', () => scheduleHide());
    for (const t of ['mousedown', 'mouseup', 'click', 'pointerdown', 'pointerup', 'wheel', 'dblclick']) panel.addEventListener(t, (e) => e.stopPropagation(), false);   // bubbling: the rows get their clicks first
    list = el('div');
    panel.appendChild(list);
    shadow.appendChild(btn);
    shadow.appendChild(panel);
    attach();
  }

  function attach() {
    const parent = document.fullscreenElement || document.webkitFullscreenElement || document.documentElement;
    if (ui.parentNode !== parent) { try { parent.appendChild(ui); } catch (e) { document.documentElement.appendChild(ui); } }
  }
  document.addEventListener('fullscreenchange', () => { if (ui) attach(); place(); }, true);
  document.addEventListener('webkitfullscreenchange', () => { if (ui) attach(); place(); }, true);

  // the file the player plays: its address, or (when the player feeds it piece by piece) one of its <source> files
  function fileSrc(v) {
    const s = v.currentSrc || v.src || '';
    if (/^https?:/i.test(s)) return s;
    try { for (const x of v.querySelectorAll('source')) if (/^https?:/i.test(x.src || '')) return x.src; } catch (e) {}
    return s;
  }
  // which video this is: its page, its file — and, for players that play video after video on one address, its length
  let pageTurn = 0;   // counts the tab going to another video while this frame stays (a player inside another page)
  function keyOf(v) {
    const src = v.currentSrc || v.src || '';
    const d = isFinite(v.duration) && v.duration > 0 ? Math.round(v.duration) : 0;
    return videoPage(v).split('#')[0] + '|' + (/^https?:/i.test(src) ? src.split('?')[0] : src.slice(0, 80)) + '|' + d + '|' + pageTurn;
  }

  function usable(v) {
    if (!(v instanceof HTMLVideoElement) || !v.isConnected) return false;
    const r = v.getBoundingClientRect();
    if (r.width < 200 || r.height < 110) return false;               // thumbnails, hover previews
    const vw = window.innerWidth, vh = window.innerHeight;
    if (r.bottom < 40 || r.top > vh - 40 || r.right < 40 || r.left > vw - 40) return false;
    if (inAd(v)) return false;
    return !hiddenFor.has(keyOf(v));
  }

  // Just above the video's top-right corner, outside the picture (below it when there's no room above; inside its top
  // edge when there's no room either). The side is chosen when the button appears; after that the button and its menu
  // stay fixed to the video: scrolling the page moves them with the video, never across the screen.
  let side = 'above', fixedMode = false;
  function inFixed(v) {
    if (document.fullscreenElement || document.webkitFullscreenElement) return true;
    for (let e = v; e && e !== document.documentElement; e = e.parentElement) { const p = getComputedStyle(e).position; if (p === 'fixed' || p === 'sticky') return true; }
    return false;
  }
  function chooseSide() {
    if (!btn || !current) return;
    const r = current.getBoundingClientRect(), vh = window.innerHeight, h = btn.offsetHeight || 32, gap = 6;
    fixedMode = inFixed(current);
    side = r.top - h - gap >= 4 ? 'above' : (r.bottom + gap + h <= vh - 4 ? 'below' : 'inside');
  }
  function place() {
    if (!btn || !current) return;
    if (!current.isConnected) { hideAll(); return; }
    const r = current.getBoundingClientRect();
    const vw = document.documentElement.clientWidth || window.innerWidth, w = btn.offsetWidth || 190, h = btn.offsetHeight || 32, gap = 6;
    const ox = fixedMode ? 0 : window.scrollX, oy = fixedMode ? 0 : window.scrollY, pos = fixedMode ? 'fixed' : 'absolute';
    const x = Math.max(6, Math.min(vw - w - 6, r.right - w));
    const y = side === 'above' ? r.top - h - gap : side === 'below' ? r.bottom + gap : r.top + 10;
    css(btn, { position: pos, left: (x + ox) + 'px', top: (y + oy) + 'px' });
    if (panelOpen) {
      const px = Math.max(6, Math.min(vw - 296, r.right - 290));
      const mh = 360;
      const py = side === 'below' ? y - 6 - Math.min(mh, panel.scrollHeight || mh) : y + h + 6;
      css(panel, { position: pos, left: (px + ox) + 'px', top: (py + oy) + 'px', 'max-height': mh + 'px' });
    }
  }

  function show(v, ms) {
    if (off || !enabled || !usable(v)) return;
    if (current && v !== current && btn && btn.style.display !== 'none' && (panelOpen || nearBtn(28))) return;   // (another video started playing while you reach for the button: it stays)
    ensureUI();
    attach();
    const fresh = current !== v || btn.style.display === 'none';
    if (current !== v) { closePanel(); setLabel('Download this video'); }
    current = v;
    css(btn, { display: 'inline-flex' });
    // The button takes its place when it appears and keeps it: moving the pointer never moves it. Only the video
    // itself moving on the page (its layout changed) takes the button along — and never while you reach for it.
    const r = v.getBoundingClientRect();
    const moved = !anchor || Math.abs(r.right - anchor.right) > 24 || Math.abs(r.top - anchor.top) > 24 || Math.abs(r.bottom - anchor.bottom) > 24;
    if (fresh) chooseSide();
    if (fresh || (moved && !nearBtn(60))) { place(); anchor = { right: r.right, top: r.top, bottom: r.bottom }; }
    clearTimeout(hideTimer);
    if (ms) hideTimer = setTimeout(() => { if (!pointerOver()) hideAll(); }, ms);
    maybePrefetch(v);
  }
  let anchor = null;       // where the video was when the button was placed
  let pending = null;      // another video the pointer went over: taken only once the pointer rests on it
  function nearBtn(pad) {  // the pointer is on the button, or on its way to it
    if (!btn || btn.style.display === 'none') return false;
    if (overBtn) return true;
    const b = btn.getBoundingClientRect();
    return mouse.x >= b.left - pad && mouse.x <= b.right + pad && mouse.y >= b.top - pad && mouse.y <= b.bottom + pad;
  }
  function hideAll() { if (panelOpen) closePanel(); if (btn) css(btn, { display: 'none' }); current = null; anchor = null; }
  function scheduleHide() { clearTimeout(hideTimer); hideTimer = setTimeout(() => { if (!panelOpen && !pointerOver()) hideAll(); }, 1200); }
  function pointerOver() {
    if (!current) return false;
    if (overBtn) return true;
    const r = current.getBoundingClientRect();
    return mouse.x >= r.left && mouse.x <= r.right && mouse.y >= r.top && mouse.y <= r.bottom;
  }
  function setLabel(t) { if (label) label.textContent = t; place(); }

  // the video under the pointer (players lay controls over the video, so the event target is rarely the video itself)
  function videoAt(x, y) {
    let best = null, area = 0;
    for (const v of document.querySelectorAll('video')) {
      const r = v.getBoundingClientRect();
      if (x < r.left || x > r.right || y < r.top || y > r.bottom) continue;
      const a = r.width * r.height;
      if (a > area) { area = a; best = v; }
    }
    return best;
  }
  document.addEventListener('pointermove', (e) => {
    mouse = { x: e.clientX, y: e.clientY };
    const now = Date.now();
    if (now - lastMove < 120) return;
    lastMove = now;
    if (off || !enabled) return;
    // reaching for the button (or its menu): nothing changes under the pointer
    if (current && (panelOpen || nearBtn(28))) { clearTimeout(hideTimer); pending = null; return; }
    const v = videoAt(e.clientX, e.clientY);
    if (!v) { pending = null; if (current && !panelOpen) scheduleHide(); return; }
    if (!current || v === current || btn.style.display === 'none') { pending = null; show(v); return; }
    // another video (a preview, the next reel) under the pointer: the button goes there only once the pointer rests on it
    if (!pending || pending.v !== v) { pending = { v, at: now }; return; }
    if (now - pending.at >= 350) { pending = null; show(v); }
  }, true);
  window.addEventListener('scroll', () => { if (fixedMode || panelOpen || (current && btn && btn.style.display !== 'none')) { place(); if (current) { const r = current.getBoundingClientRect(); anchor = { right: r.right, top: r.top, bottom: r.bottom }; } } }, true);   // (same spot on the page: no jump)
  window.addEventListener('resize', () => { chooseSide(); place(); }, true);
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && panelOpen) closePanel(); }, true);
  document.addEventListener('mousedown', (e) => { if (panelOpen && !(ui && ui.contains(e.target))) closePanel(); }, true);

  // the main video of a page (a watch page, a reel, a TikTok) shows the button for a few seconds when it starts
  document.addEventListener('playing', (e) => {
    const v = e.target;
    if (!(v instanceof HTMLVideoElement)) return;
    if (usable(v) && isMainVideo(v, v.getBoundingClientRect())) maybePrefetch(v);   // read its qualities now, before any click
    setTimeout(() => { if (!v.paused && usable(v) && isMainVideo(v, v.getBoundingClientRect())) show(v, 4000); }, 700);
  }, true);
  // feeds that swap the address without reloading (Reels, Shorts, TikTok)
  let lastHref = location.href;
  setInterval(() => {
    if (location.href === lastHref) return;
    lastHref = location.href;
    if (panelOpen) closePanel();
    for (const v of document.querySelectorAll('video')) if (!v.paused && usable(v) && isMainVideo(v, v.getBoundingClientRect())) { show(v, 4000); break; }
  }, 700);

  // One reading per video: asked for as soon as the video plays (or the pointer is on it), kept for 10 minutes, so the
  // menu shows every quality at once when it's clicked.
  const answers = new Map();   // key -> { at, promise }
  // TikTok often hides a post's data from programs: the browser reads it (the page it opened with, or the post's own
  // page fetched here with your session) and hands it to DDM with the request.
  const ttData = new Map();   // post id -> promise of its data (JSON text, '' when unreadable)
  const ttSeen = new Map();   // post id -> its data, as TikTok's page loaded it (feed, search, profiles: see ttpage.js)
  if (/tiktok\.com/.test(host)) {
    window.addEventListener('message', (e) => {
      try {
        if (e.source !== window || !e.data || !Array.isArray(e.data.__ddmTikTok)) return;
        for (const t of e.data.__ddmTikTok.slice(0, 60)) {
          if (typeof t !== 'string' || t.length > 400000) continue;
          const id = (t.match(/^\{"id":"(\d{15,})"/) || t.match(/"id":"(\d{15,})"/) || [])[1];
          if (!id) continue;
          ttSeen.delete(id);
          ttSeen.set(id, t);
          if (ttData.has(id)) ttData.get(id).then((r) => { if (!r) ttData.delete(id); });
        }
        while (ttSeen.size > 400) ttSeen.delete(ttSeen.keys().next().value);
      } catch (err) {}
    });
  }
  function tiktokData(page) {
    if (!/tiktok\.com/.test(host)) return Promise.resolve('');
    const id = (page.match(/\/(?:video|photo)\/(\d{15,})/) || [])[1];
    if (!id) return Promise.resolve('');
    if (ttSeen.has(id)) return Promise.resolve(ttSeen.get(id));
    if (ttData.has(id)) return ttData.get(id);
    const pick = (txt) => {
      try {
        const sc = (JSON.parse(txt) || {}).__DEFAULT_SCOPE__ || {};
        for (const k of ['webapp.video-detail', 'webapp.reflow.video.detail']) {
          const it = sc[k] && sc[k].itemInfo && sc[k].itemInfo.itemStruct;
          if (it && String(it.id) === id) return JSON.stringify(it);
        }
      } catch (e) {}
      return '';
    };
    const job = (async () => {
      const own = document.getElementById('__UNIVERSAL_DATA_FOR_REHYDRATION__');
      const mine = own ? pick(own.textContent || '') : '';
      if (mine) return mine;
      try {
        const get = (typeof content !== 'undefined' && content && content.fetch) ? content.fetch.bind(content) : fetch;   // Firefox: as the page
        const ctl = new AbortController(), t = setTimeout(() => ctl.abort(), 8000);
        const res = await get(page.split('?')[0].replace(/\/@\/(video|photo)\//, '/@_/$1/'), { credentials: 'include', signal: ctl.signal });
        clearTimeout(t);
        if (!res.ok) return '';
        const html = await res.text();
        const m = html.match(/<script[^>]*id="__UNIVERSAL_DATA_FOR_REHYDRATION__"[^>]*>([\s\S]*?)<\/script>/);
        return m ? pick(m[1]) : '';
      } catch (e) { return ''; }
    })();
    ttData.set(id, job);
    job.then((r) => { if (!r) setTimeout(() => ttData.delete(id), 30000); });
    if (ttData.size > 80) ttData.delete(ttData.keys().next().value);
    return job;
  }

  function request(v, prefetch) {
    const k = keyOf(v), now = Date.now(), hit = answers.get(k);
    if (hit && now - hit.at < 600000) return hit.promise;
    const src = fileSrc(v);
    const page = videoPage(v);
    const entry = { at: now, promise: null, done: null };
    const promise = tiktokData(page).then((pageData) => api.runtime.sendMessage({
      type: 'ddm-formats', prefetch: !!prefetch, pageUrl: page, src: /^https?:/i.test(src) ? src : '', title: titleOf(v, page),
      duration: isFinite(v.duration) ? Math.round(v.duration) : 0, width: v.videoWidth || 0, height: v.videoHeight || 0, pageData,
    })).then((res) => {
      // an answer without any quality isn't kept: the player may not have loaded its stream yet, so the next look asks again
      if (!res || !res.ok || !(res.formats || []).length) { if (answers.get(k) === entry) answers.delete(k); }
      return res;
    }, (e) => { if (answers.get(k) === entry) answers.delete(k); throw e; });
    answers.set(k, entry);
    entry.promise = promise;
    promise.then((r) => { entry.done = r; }, () => {});
    if (answers.size > 60) answers.delete(answers.keys().next().value);
    return promise;
  }
  function maybePrefetch(v) {
    const k = keyOf(v);
    if (prefetched.has(k) || off || !enabled || !usable(v)) return;
    prefetched.add(k);
    // read ahead, so the click shows every quality at once. Nothing found yet (the player hasn't loaded its stream):
    // looked at again a little later, a few times, while the video is still on the page
    const tries = [0, 2500, 6000, 12000];
    const go = (i) => {
      if (off || !enabled || !v.isConnected || keyOf(v) !== k) { prefetched.delete(k); return; }
      request(v, true).then((res) => {
        if (res && res.ok && (res.formats || []).length) return;
        if (i + 1 < tries.length) setTimeout(() => go(i + 1), tries[i + 1] - tries[i]); else prefetched.delete(k);
      }).catch(() => { prefetched.delete(k); });
    };
    go(0);
  }

  function row(text, detail, onClick, opts) {
    const o = opts || {};
    const r = el('div', { display: 'flex', 'align-items': 'center', 'justify-content': 'space-between', gap: '10px', padding: o.head ? '8px 10px 4px' : '8px 10px',
      'border-radius': '8px', cursor: onClick ? 'pointer' : 'default', color: o.head ? MUTED : (o.bad ? '#FF8A8A' : TEXT), font: o.head ? '600 11px/1.2 "Segoe UI",system-ui,sans-serif' : FONT,
      'text-transform': o.head ? 'uppercase' : 'none', 'letter-spacing': o.head ? '.06em' : 'normal' });
    const a = el('span', o.wrap ? { font: 'inherit', color: 'inherit', 'white-space': 'normal', 'line-height': '1.35' } : { font: 'inherit', color: 'inherit', overflow: 'hidden', 'text-overflow': 'ellipsis', 'white-space': 'nowrap' }, text);
    r.appendChild(a);
    if (detail) r.appendChild(el('span', { font: '400 12px/1.2 "Segoe UI",system-ui,sans-serif', color: MUTED, 'white-space': 'nowrap' }, detail));
    if (onClick) {
      r.addEventListener('mouseenter', () => css(r, { background: INK2 }));
      r.addEventListener('mouseleave', () => css(r, { background: 'transparent' }));
      r.addEventListener('click', (e) => { e.preventDefault(); e.stopPropagation(); onClick(); });
    }
    list.appendChild(r);
    return r;
  }
  function clearList() { while (list.firstChild) list.removeChild(list.firstChild); }

  function fillFormats(res) {
    clearList();
    if (res.title) row(res.title, '', null, { head: true });
    let group = '';
    (res.formats || []).forEach((f, i) => {
      if (f.group !== group) { group = f.group; row(group === 'Audio' ? 'Sound only' : 'Video', '', null, { head: true }); }
      row(f.label, f.detail || 'size unknown', () => pick(res.token, i));
    });
    if (!(res.formats || []).length) row('Open in DDM', 'choose there', () => pick(res.token, -1));
  }
  // Opens at once: with every quality when DDM already read them, otherwise with "Best quality" and "Sound only" to
  // click right away while the full list arrives (usually within a second).
  function openPanel() {
    if (!current) return;
    const v = current, my = ++busyToken;
    panelOpen = true;
    css(btn, { opacity: '1' });
    clearList();
    let ready = null;
    const k = keyOf(v), hit = answers.get(k);
    if (hit && hit.done && hit.done.ok && (hit.done.formats || []).length) ready = hit.done;
    if (ready && ready.ok) fillFormats(ready);
    else {
      row('Best quality', 'video + sound', () => pick('', -1, 'best'));
      row('Sound only', 'MP3', () => pick('', -1, 'mp3'));
      const head = row('Getting every quality…', '', null, { head: true });
      setTimeout(() => { if (my === busyToken && panelOpen && head.isConnected) head.firstChild.textContent = 'Still reading this site… Best quality works now'; }, 20000);
    }
    css(panel, { display: 'block' });
    place();
    if (ready && ready.ok) return;
    request(v, false).then((res) => {
      const h2 = answers.get(k); if (h2) h2.done = res;
      if (my !== busyToken || !panelOpen) return;
      if (!res || !res.ok) {
        clearList();
        row(res && res.error ? res.error : 'DDM isn’t answering. Start DDM, then try again.', '', null, { bad: true, wrap: true });
        row('Open in DDM anyway', '', () => pick(res && res.token, -1));
        return;
      }
      fillFormats(res);
      place();
    }).catch(() => {
      if (my !== busyToken) return;
      clearList();
      row('DDM isn’t answering. Start DDM, then try again.', '', null, { bad: true, wrap: true });
    });
  }
  function closePanel() { panelOpen = false; busyToken++; if (panel) css(panel, { display: 'none' }); if (btn && !overBtn) css(btn, { opacity: String(REST) }); }
  function pick(token, index, quick) {
    closePanel();
    setLabel('Sent to DDM');
    setTimeout(() => setLabel('Download this video'), 2500);
    const v = current, page = v ? videoPage(v) : location.href, src = v ? fileSrc(v) : '';
    const title = v ? titleOf(v, page) : '', duration = v && isFinite(v.duration) ? Math.round(v.duration) : 0;
    (token ? Promise.resolve('') : tiktokData(page)).then((pageData) => api.runtime.sendMessage({ type: 'ddm-pick', token: token || '', index, quick: quick || '', pageUrl: page,
      src: /^https?:/i.test(src) ? src : '', title, duration, pageData, width: v ? v.videoWidth || 0 : 0, height: v ? v.videoHeight || 0 : 0 })).catch(() => {});
  }
})();
