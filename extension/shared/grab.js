/*
 * DDM browser extension — page helper for the right-click extras (3.6.22).
 * Answers the background only when you use a DDM menu item:
 *  - "ddm-collect": the links / images of this page (or of the selected text), so DDM can show a picker;
 *  - "ddm-snapshot": a copy of the page exactly as it looks now (signed in, expanded, scrolled), which DDM turns into a PDF.
 * Reads the page; never changes it.
 */
(function () {
  'use strict';
  if (window.__ddmGrab) return;
  window.__ddmGrab = true;
  const api = typeof browser !== 'undefined' ? browser : chrome;

  const MAX_ITEMS = 3000;

  function abs(u) {
    try { return new URL(u, document.baseURI).href; } catch (e) { return ''; }
  }

  function okUrl(u) { return /^(https?:|magnet:)/i.test(u); }

  /** The biggest picture of an <img>/<source> srcset. */
  function bestFromSrcset(set) {
    let best = '', bestW = -1;
    for (const part of (set || '').split(/,\s+/)) {
      const m = part.trim().match(/^(\S+)(?:\s+(\d+(?:\.\d+)?)([wx]))?$/);
      if (!m) continue;
      const w = m[2] ? parseFloat(m[2]) * (m[3] === 'x' ? 1000 : 1) : 0;
      if (w > bestW) { bestW = w; best = m[1]; }
    }
    return best;
  }

  function textOf(el) {
    const t = (el.innerText || el.textContent || el.getAttribute('title') || el.getAttribute('aria-label') || '').replace(/\s+/g, ' ').trim();
    return t.length > 200 ? t.slice(0, 200) : t;
  }

  function collectFrom(root, mode, out, seen, inRange) {
    const add = (url, text, kind) => {
      if (out.length >= MAX_ITEMS) return;
      url = abs(url);
      if (!url || !okUrl(url) || seen.has(url)) return;
      seen.add(url);
      out.push({ url, text: text || '', kind });
    };
    const want = (el) => !inRange || inRange(el);
    if (mode !== 'images') {
      for (const a of root.querySelectorAll('a[href], area[href]')) if (want(a)) add(a.getAttribute('href'), textOf(a), 'a');
      for (const m of root.querySelectorAll('video[src], audio[src], video source[src], audio source[src]')) {
        if (!want(m)) continue;
        const k = m.closest('audio') ? 'audio' : 'video';
        add(m.getAttribute('src'), '', k);
      }
    }
    if (mode !== 'links' || inRange) {
      for (const img of root.querySelectorAll('img')) {
        if (!want(img)) continue;
        if (mode === 'images' && (img.naturalWidth || img.width) > 0 && (img.naturalWidth || img.width) < 48 && (img.naturalHeight || img.height) < 48) continue; // icons, spacers
        const src = bestFromSrcset(img.getAttribute('srcset')) || img.currentSrc || img.getAttribute('data-src') || img.getAttribute('src');
        if (src) add(src, img.getAttribute('alt') || '', 'img');
      }
      if (mode === 'images') {
        for (const s of root.querySelectorAll('picture source[srcset]')) add(bestFromSrcset(s.getAttribute('srcset')), '', 'img');
        const og = document.querySelector('meta[property="og:image"][content]');
        if (og && !inRange) add(og.getAttribute('content'), 'Page picture', 'img');
      }
    }
  }

  /** Links in the selected text: <a> elements inside the selection, plus links written as plain text. */
  function collectSelection(out, seen) {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0) return;
    for (let i = 0; i < sel.rangeCount; i++) {
      const r = sel.getRangeAt(i);
      let root = r.commonAncestorContainer;
      if (root.nodeType !== 1) root = root.parentElement;
      if (!root) continue;
      const a0 = root.closest && root.closest('a[href]');
      if (a0) collectFrom(a0.parentElement || document, 'selection', out, seen, (el) => el === a0);
      collectFrom(root, 'selection', out, seen, (el) => { try { return r.intersectsNode(el); } catch (e) { return false; } });
    }
    const text = sel.toString();
    const re = /\b(?:https?:\/\/[^\s<>"'`]+|magnet:\?[^\s<>"'`]+)/gi;
    let m;
    while ((m = re.exec(text)) && out.length < MAX_ITEMS) {
      const u = m[0].replace(/[),.;:!?\]]+$/, '');
      if (!seen.has(u)) { seen.add(u); out.push({ url: u, text: '', kind: 'a' }); }
    }
  }

  function collect(mode) {
    const out = [], seen = new Set();
    if (mode === 'selection') collectSelection(out, seen);
    else collectFrom(document, mode, out, seen, null);
    return out;
  }

  // ---------- page snapshot for "Save page as PDF" ----------

  function cssText(sheet) {
    try { return [...sheet.cssRules].map((r) => r.cssText).join('\n'); } catch (e) { return null; }
  }

  function snapshot() {
    const liveAll = [document.documentElement, ...document.documentElement.querySelectorAll('*')];
    const clone = document.documentElement.cloneNode(true);
    const cloneAll = [clone, ...clone.querySelectorAll('*')];
    const same = liveAll.length === cloneAll.length;
    if (same) {
      for (let i = 0; i < liveAll.length; i++) {
        const a = liveAll[i], b = cloneAll[i];
        const tag = a.tagName;
        try {
          if (tag === 'STYLE' && a.sheet) {
            // Sites that build their styles in script (CSS-in-JS) leave <style> empty: write the real rules into the copy.
            const t = cssText(a.sheet);
            if (t != null && t.length > (a.textContent || '').length) b.textContent = t;
          } else if (tag === 'IMG') {
            const src = a.currentSrc || a.src;
            if (src && !/^blob:/i.test(src)) b.setAttribute('src', src);
            b.removeAttribute('srcset'); b.removeAttribute('sizes'); b.removeAttribute('loading');
            if (!src && a.getAttribute('data-src')) b.setAttribute('src', abs(a.getAttribute('data-src')));
          } else if (tag === 'SOURCE' && a.parentElement && a.parentElement.tagName === 'PICTURE') {
            b.remove();
            continue;
          } else if (tag === 'CANVAS') {
            const img = document.createElement('img');
            img.src = a.toDataURL();
            img.setAttribute('style', a.getAttribute('style') || '');
            img.width = a.width; img.height = a.height;
            img.className = a.className;
            b.replaceWith(img);
            continue;
          } else if (tag === 'INPUT') {
            if (a.type === 'checkbox' || a.type === 'radio') { if (a.checked) b.setAttribute('checked', ''); else b.removeAttribute('checked'); }
            else if (a.type !== 'password' && a.type !== 'file') b.setAttribute('value', a.value);
          } else if (tag === 'TEXTAREA') {
            b.textContent = a.value;
          } else if (tag === 'SELECT') {
            [...b.options].forEach((o, k) => { if (a.options[k] && a.options[k].selected) o.setAttribute('selected', ''); else o.removeAttribute('selected'); });
          } else if (tag === 'VIDEO') {
            if (a.poster) b.setAttribute('poster', abs(a.poster));
          }
          // Headers / banners pinned to the screen would repeat on every printed page: keep them where they are, once.
          if (a.nodeType === 1 && tag !== 'HTML' && tag !== 'BODY' && tag !== 'HEAD') {
            const pos = getComputedStyle(a).position;
            if (pos === 'fixed') b.style.setProperty('position', 'absolute', 'important');
            else if (pos === 'sticky') b.style.setProperty('position', 'relative', 'important');
          }
        } catch (e) {}
      }
    }
    // No scripts: the copy must show exactly this moment, not run the site again.
    for (const s of clone.querySelectorAll('script, link[rel="preload"], link[rel="modulepreload"], link[rel="prefetch"]')) s.remove();
    for (const el of clone.querySelectorAll('*')) {
      for (const at of [...el.attributes]) if (/^on/i.test(at.name)) el.removeAttribute(at.name);
    }
    for (const el of clone.querySelectorAll('[href^="javascript:" i]')) el.removeAttribute('href');
    let head = clone.querySelector('head');
    if (!head) { head = document.createElement('head'); clone.insertBefore(head, clone.firstChild); }
    for (const b of head.querySelectorAll('base, meta[charset], meta[http-equiv]')) b.remove();
    const meta = document.createElement('meta'); meta.setAttribute('charset', 'utf-8');
    const base = document.createElement('base'); base.setAttribute('href', document.baseURI);
    head.prepend(meta, base);
    // Styles added by script as objects (adoptedStyleSheets) aren't in the page's HTML.
    let adopted = '';
    try { for (const sh of document.adoptedStyleSheets || []) { const t = cssText(sh); if (t) adopted += t + '\n'; } } catch (e) {}
    const st = document.createElement('style');
    st.textContent = adopted + '\n* { -webkit-print-color-adjust: exact !important; print-color-adjust: exact !important; }\n' +
      'img, svg, video, canvas { max-width: 100%; }\n@media print { html, body { height: auto !important; overflow: visible !important; } }';
    head.appendChild(st);
    const dt = document.doctype ? '<!DOCTYPE ' + document.doctype.name + '>' : '<!DOCTYPE html>';
    return dt + '\n' + clone.outerHTML;
  }

  // ---------- "Download with DDM" anywhere (3.6.24) ----------
  // Chrome only offers "image" or "link" menus when the right-click lands exactly on an <img> or <a>. Icons drawn with
  // CSS, SVG icons and pictures under an invisible layer (Instagram, Pinterest, shops) get the plain page menu, so the
  // page remembers where you right-clicked and finds what is really there, under any overlay.

  let lastClick = null;
  document.addEventListener('contextmenu', (e) => {
    lastClick = { x: e.clientX, y: e.clientY, path: (e.composedPath ? e.composedPath() : [e.target]).filter((n) => n && n.nodeType === 1).slice(0, 12), at: Date.now() };
  }, true);

  const PICTURE_EXT = /\.(jpe?g|png|gif|webp|avif|bmp|svg|ico|tiff?)(?:[?#]|$)/i;

  function cssUrls(value) {
    const out = [];
    const re = /url\(\s*(['"]?)(.*?)\1\s*\)/g;
    let m;
    while ((m = re.exec(value || ''))) if (m[2]) out.push(m[2]);
    return out;
  }

  function nameOfUrl(u, fallback) {
    try { const n = decodeURIComponent(new URL(u).pathname.split('/').pop() || ''); if (n) return n; } catch (e) {}
    return fallback;
  }

  function svgItem(svg) {
    const copy = svg.cloneNode(true);
    if (!copy.getAttribute('xmlns')) copy.setAttribute('xmlns', 'http://www.w3.org/2000/svg');
    const r = svg.getBoundingClientRect();
    if (!copy.getAttribute('width') && r.width) copy.setAttribute('width', Math.round(r.width));
    if (!copy.getAttribute('height') && r.height) copy.setAttribute('height', Math.round(r.height));
    // <use href="#icon"> draws a symbol kept elsewhere in the page: copy it into the file's <defs>.
    const defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
    const added = new Set();
    for (const u of copy.querySelectorAll('use')) {
      const ref = u.getAttribute('href') || u.getAttribute('xlink:href') || '';
      if (!ref.startsWith('#') || added.has(ref) || copy.querySelector(ref.replace(/([^\w#-])/g, '\\$1'))) continue;
      const src = document.getElementById(ref.slice(1));
      if (src) { defs.appendChild(src.cloneNode(true)); added.add(ref); }
    }
    if (defs.childNodes.length) copy.insertBefore(defs, copy.firstChild);
    const label = (svg.getAttribute('aria-label') || (svg.querySelector('title') || {}).textContent || 'icon').trim().replace(/[\\/:*?"<>|]+/g, ' ').slice(0, 60) || 'icon';
    return { kind: 'inline', data: 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(new XMLSerializer().serializeToString(copy)), name: label + '.svg' };
  }

  /** The picture / video / icon / link at the point you right-clicked (also beneath overlays). */
  function whatIsHere() {
    if (!lastClick || Date.now() - lastClick.at > 5 * 60 * 1000) return null;
    let stack = [];
    try { stack = document.elementsFromPoint(lastClick.x, lastClick.y); } catch (e) {}
    const seen = new Set();
    const els = [];
    for (const n of lastClick.path.concat(stack)) if (n && !seen.has(n) && els.length < 30) { seen.add(n); els.push(n); }
    for (const el of els) {
      const tag = el.tagName;
      if (tag === 'HTML' || tag === 'BODY') continue;
      try {
        if (tag === 'IMG') {
          const src = bestFromSrcset(el.getAttribute('srcset')) || el.currentSrc || el.src || el.getAttribute('data-src');
          if (src && (el.naturalWidth || 3) > 2) return { kind: /^data:/i.test(src) ? 'inline' : 'file', url: abs(src), data: /^data:/i.test(src) ? src : undefined, name: nameOfUrl(abs(src), 'picture') };
        }
        if (tag === 'VIDEO' || tag === 'AUDIO') {
          const src = el.currentSrc || el.src || '';
          if (/^https?:/i.test(src)) return { kind: 'video', url: src };
          if (tag === 'VIDEO' && el.poster) return { kind: 'file', url: abs(el.poster), name: nameOfUrl(abs(el.poster), 'poster') };
          return { kind: 'video', url: '' }; // a stream (blob:) — DDM finds it from the page
        }
        if (tag === 'svg' || tag === 'SVG' || el instanceof SVGElement) {
          const svg = el instanceof SVGSVGElement ? el : el.ownerSVGElement;
          if (svg) {
            const img = svg.querySelector('image');
            const href = img && (img.getAttribute('href') || img.getAttribute('xlink:href'));
            if (href && !/^data:/i.test(href)) return { kind: 'file', url: abs(href), name: nameOfUrl(abs(href), 'picture') };
            const r = svg.getBoundingClientRect();
            if (r.width >= 8 && r.height >= 8) return svgItem(svg);
          }
        }
        if (tag === 'PICTURE') {
          const img = el.querySelector('img');
          if (img && (img.currentSrc || img.src)) return { kind: 'file', url: abs(img.currentSrc || img.src), name: nameOfUrl(abs(img.currentSrc || img.src), 'picture') };
        }
        // Pictures drawn with CSS: background-image, and ::before / ::after icons.
        for (const pseudo of [null, '::before', '::after']) {
          const cs = getComputedStyle(el, pseudo);
          for (const u of cssUrls(cs.backgroundImage).concat(pseudo ? cssUrls(cs.content) : [])) {
            if (/^data:/i.test(u)) return { kind: 'inline', data: u, name: 'picture' + (u.startsWith('data:image/svg') ? '.svg' : u.startsWith('data:image/png') ? '.png' : '.jpg') };
            const full = abs(u);
            if (/^https?:/i.test(full)) return { kind: 'file', url: full, name: nameOfUrl(full, 'picture') };
          }
        }
      } catch (e) {}
    }
    // No picture: a link under the pointer (even beneath a layer).
    for (const el of els) {
      const a = el.closest && el.closest('a[href], area[href]');
      if (a) { const u = abs(a.getAttribute('href')); if (okUrl(u)) return { kind: 'file', url: u, name: nameOfUrl(u, ''), link: true, picture: PICTURE_EXT.test(u) }; }
    }
    return null;
  }

  api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (!msg || typeof msg.type !== 'string') return;
    if (msg.type === 'ddm-target') {
      try { sendResponse({ ok: true, item: whatIsHere(), title: document.title, url: location.href }); } catch (e) { sendResponse({ ok: false, error: String(e) }); }
      return;
    }
    if (msg.type === 'ddm-collect') {
      try { sendResponse({ ok: true, items: collect(msg.mode || 'links'), title: document.title, url: location.href }); } catch (e) { sendResponse({ ok: false, error: String(e) }); }
      return;
    }
    if (msg.type === 'ddm-snapshot') {
      try { sendResponse({ ok: true, html: snapshot(), title: document.title, url: location.href }); } catch (e) { sendResponse({ ok: false, error: String(e) }); }
    }
  });
})();
