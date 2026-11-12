/*
 * DDM browser extension (native DDM 4) — background. No buttons, no pop-ups in pages.
 * - Hands downloads to DDM (with the page's cookies, so signed-in downloads work).
 * - Puts a "Download this video" button on videos (YouTube, TikTok, Facebook, Reels, Instagram, X…): click it to
 *   see the qualities DDM found and pick one. Nothing opens by itself.
 * - The toolbar icon turns DDM off for one tab (black icon with a red X) and back on.
 * - Right-click: Download with DDM (link, picture, video, anything under the pointer), selected links, all links, all pictures.
 * Talks to DDM through native messaging (host "com.dedseec.ddm").
 */
'use strict';

const api = typeof browser !== 'undefined' ? browser : chrome;
const HOST = 'com.dedseec.ddm';
const IS_FIREFOX = typeof browser !== 'undefined' && navigator.userAgent.includes('Firefox');
const action = api.action || api.browserAction;

let config = {
  catch: true,
  types: '3GP 7Z AAC APK AVI BIN BZ2 EXE GZ ISO M4A M4V MKV MOV MP3 MP4 MSI OGG PDF RAR TAR WAV WEBM WMA WMV ZIP FLAC DMG DEB TORRENT',
  minKB: 0,
  skip: [],
  button: true,
};
const offTabs = new Set();          // tabs where DDM is switched off
const reqTab = new Map();           // request url -> { tabId, at }  (which tab a download came from)
const tabPage = new Map();          // tabId -> the page's address without its #part
const freshTabs = new Set();        // tabs that went to another page since the add-on woke up: nothing kept from before belongs to them
const tabStreams = new Map();       // tabId -> [{ url, kind, at, mime, size }]  (video/audio streams the tab played, while its page is open)
const sentVideos = new Map();       // tabId -> Set(keys) already handed to DDM
const bypass = new Map();           // url -> time: let the browser keep it (DDM didn't answer / Alt-click)

// ---------------------------------------------------------------- DDM
function native(msg) {
  return new Promise((resolve, reject) => {
    try {
      msg = Object.assign({ ext: api.runtime.getManifest().version }, msg);
      Promise.resolve(api.runtime.sendNativeMessage(HOST, msg)).then((res) => (res ? resolve(res) : reject(new Error('No answer from DDM'))), reject);
    } catch (e) { reject(e); }
  });
}

async function hello() {
  try {
    const r = await native({ type: 'hello', browser: browserName() });
    config = { catch: r.catch !== false, types: r.types || config.types, minKB: r.minKB || 0, skip: Array.isArray(r.skip) ? r.skip : [], button: r.videoButton !== false };
  } catch (e) { /* DDM isn't running yet: the helper starts it with the first download */ }
}

function browserName() {
  const ua = navigator.userAgent;
  if (IS_FIREFOX) return 'firefox';
  if (/Edg\//.test(ua)) return 'edge';
  if (/OPR\//.test(ua)) return 'opera';
  if (/Vivaldi/.test(ua)) return 'vivaldi';
  if (navigator.brave) return 'brave';
  return 'chrome';
}

async function cookieHeader(url, storeId) {
  if (!/^https?:/i.test(url || '')) return '';
  try {
    const q = { url };
    if (storeId) q.storeId = storeId;
    let list = await api.cookies.getAll(q).catch(() => null);
    if (list === null && IS_FIREFOX) list = await api.cookies.getAll(Object.assign({ firstPartyDomain: null }, q)).catch(() => []);
    return (list || []).map((c) => c.name + '=' + c.value).join('; ');
  } catch (e) { return ''; }
}

// ---------------------------------------------------------------- the toolbar icon: DDM on / off for this tab
function drawIcon(size, off) {
  const c = new OffscreenCanvas(size, size);
  const g = c.getContext('2d');
  const r = size * 0.22;
  g.beginPath();
  g.moveTo(r, 0); g.arcTo(size, 0, size, size, r); g.arcTo(size, size, 0, size, r); g.arcTo(0, size, 0, 0, r); g.arcTo(0, 0, size, 0, r);
  g.closePath();
  g.fillStyle = off ? '#000000' : '#16131C';
  g.fill();
  g.lineCap = 'round'; g.lineJoin = 'round';
  const s = size / 24;
  if (off) {
    g.strokeStyle = '#FF2D2D'; g.lineWidth = 3.4 * s;
    g.beginPath(); g.moveTo(7 * s, 7 * s); g.lineTo(17 * s, 17 * s); g.moveTo(17 * s, 7 * s); g.lineTo(7 * s, 17 * s); g.stroke();
  } else {
    g.strokeStyle = '#C6F432'; g.lineWidth = 2.6 * s;
    g.beginPath(); g.moveTo(12 * s, 5 * s); g.lineTo(12 * s, 14.5 * s); g.moveTo(8 * s, 10.5 * s); g.lineTo(12 * s, 14.5 * s); g.lineTo(16 * s, 10.5 * s);
    g.moveTo(7 * s, 19 * s); g.lineTo(17 * s, 19 * s); g.stroke();
  }
  return g.getImageData(0, 0, size, size);
}

function paintTab(tabId) {
  const off = offTabs.has(tabId);
  try {
    action.setIcon({ tabId, imageData: { 16: drawIcon(16, off), 32: drawIcon(32, off) } });
    action.setTitle({ tabId, title: off ? 'DDM is off on this tab — click to turn it on' : 'DDM — click to turn it off on this tab' });
  } catch (e) {}
}

function saveOff() {
  const store = (api.storage && (api.storage.session || api.storage.local));
  try { store && store.set({ offTabs: Array.from(offTabs) }); } catch (e) {}
}

async function loadOff() {
  const store = (api.storage && (api.storage.session || api.storage.local));
  try {
    const v = store ? await store.get('offTabs') : null;
    const tabs = await api.tabs.query({});
    const alive = new Set(tabs.map((t) => t.id));
    for (const id of (v && v.offTabs) || []) if (alive.has(id)) offTabs.add(id);
    for (const t of tabs) paintTab(t.id);
  } catch (e) {}
}

function toggleTab(tab) {
  if (!tab || tab.id === undefined) return;
  if (offTabs.has(tab.id)) offTabs.delete(tab.id); else offTabs.add(tab.id);
  saveOff();
  paintTab(tab.id);
  try { Promise.resolve(api.tabs.sendMessage(tab.id, { type: 'ddm-state', off: offTabs.has(tab.id) })).catch(() => {}); } catch (e) {}
}
action.onClicked.addListener(toggleTab);

api.tabs.onUpdated.addListener((tabId, info) => {
  if (info.status === 'loading') { paintTab(tabId); if (info.url) sentVideos.delete(tabId); }
  if (info.url) {   // another video on the same site without a reload: only what was loaded just now belongs to it
    const page = info.url.split('#')[0], at = Date.now();
    streamsReady.then(() => {
      const before = tabPage.get(tabId);
      tabPage.set(tabId, page);
      if (before === page) return;   // (a jump inside the same page, #t=30, changes nothing)
      // players in frames don't see the tab going to another video: told, so they ask DDM again
      try { Promise.resolve(api.tabs.sendMessage(tabId, { type: 'ddm-newpage' })).catch(() => {}); } catch (e) {}
      const list = tabStreams.get(tabId);
      if (list && list.length) tabStreams.set(tabId, list.filter((s) => s.at > at - 4000));
      saveStreams();
    });
  }
});
api.tabs.onRemoved.addListener((tabId) => { offTabs.delete(tabId); tabPage.delete(tabId); if (tabStreams.delete(tabId)) saveStreams(); sentVideos.delete(tabId); saveOff(); });
api.tabs.onReplaced && api.tabs.onReplaced.addListener((added, removed) => { if (offTabs.delete(removed)) { offTabs.add(added); saveOff(); paintTab(added); } });

// ---------------------------------------------------------------- remember which tab requested what
const ADS = /doubleclick\.net|googlesyndication|googleadservices|\/pagead\/|imasdk\.googleapis|adservice|\/ads?\/|[?&]ad_type=|moatads|innovid|springserve|adnxs|teads\.tv/i;

function streamKind(url, type) {
  const u = url.split('?')[0].toLowerCase();
  if (/\.m3u8$/.test(u)) return 'hls';
  if (/\.mpd$/.test(u)) return 'dash';
  if (type === 'media' && /\.(mp4|webm|m4a|mp3|mov|m4v)$/.test(u)) return 'file';
  return '';
}

// One piece of a stream (never a whole video): .ts / .m4s pieces, or an address that names a byte range or a piece number
const PIECE = /\.(ts|m4s|m4f|cmfv|cmfa|aac|vtt|webvtt|key|jpg|jpeg|png|gif|webp)$|[?&](range|bytes|bytestart|byteend|seg|segment|chunk|frag|sq)=[\d-]+/i;

// what a server says a file is: a stream's list of qualities, or a whole video / sound file
function kindByHeaders(d) {
  let type = '', length = -1, total = -1;
  for (const h of d.responseHeaders || []) {
    const n = (h.name || '').toLowerCase(), v = h.value || '';
    if (n === 'content-type') type = v.split(';')[0].trim().toLowerCase();
    else if (n === 'content-length') length = parseInt(v, 10);
    else if (n === 'content-range') { const m = /\/(\d+)\s*$/.exec(v); if (m) total = parseInt(m[1], 10); }
  }
  if (d.statusCode !== 200 && d.statusCode !== 206) return null;
  if (/mpegurl/.test(type)) return { kind: 'hls', mime: type, size: -1 };
  if (type === 'application/dash+xml') return { kind: 'dash', mime: type, size: -1 };
  if (!/^(video|audio)\//.test(type) || type === 'video/mp2t' || /mpegurl/.test(type)) return null;
  const size = total > 0 ? total : (d.statusCode === 200 && length > 0 ? length : -1);
  const path = d.url.split('?')[0];
  if (PIECE.test(path) || PIECE.test(d.url)) return null;
  // the player's own file is trusted as it is; anything else must be big enough to be a video, not a sound effect
  if (d.type !== 'media' && !(size >= (type.startsWith('audio/') ? 300000 : 1000000))) return null;
  return { kind: 'file', mime: type, size };
}

function noteStream(tabId, url, kind, mime, size, headers) {
  if (tabId < 0 || !/^https?:/i.test(url) || url.length > 4000 || ADS.test(url)) return;
  const list = tabStreams.get(tabId) || [];
  const hit = list.find((s) => s.url === url);
  const now = Date.now();
  if (hit) {   // seen again: what its server said about it is added
    hit.at = now;
    if (mime && !hit.mime) hit.mime = mime;
    if (size > 0 && !(hit.size > 0)) hit.size = size;
    if (kind && kind !== 'file' && hit.kind === 'file') hit.kind = kind;
    if (headers) hit.headers = headers;
  } else list.push({ url, kind, at: now, mime: mime || '', size: size > 0 ? size : -1, headers: headers || sentFor(url) });
  while (list.length > 40) list.shift();
  tabStreams.set(tabId, list);
  saveStreams();
}

api.webRequest.onBeforeRequest.addListener((d) => {
  if (d.tabId < 0) return;
  const now = Date.now();
  if (d.type === 'main_frame') { freshTabs.add(d.tabId); tabPage.set(d.tabId, d.url.split('#')[0]); if (tabStreams.delete(d.tabId)) saveStreams(); }   // a new page: its own streams only
  if (d.type === 'main_frame' || d.type === 'sub_frame' || d.type === 'other') {
    reqTab.set(d.url, { tabId: d.tabId, at: now });
    if (reqTab.size > 400) { for (const [k, v] of reqTab) if (now - v.at > 120000) reqTab.delete(k); }
  }
  const kind = streamKind(d.url, d.type);
  if (kind) noteStream(d.tabId, d.url, kind, '', -1);
}, { urls: ['<all_urls>'] });

// Streams whose address doesn't say what they are (no .m3u8 / .mp4): their server's answer does.
try {
  api.webRequest.onHeadersReceived.addListener((d) => {
    if (d.tabId < 0 || d.type === 'main_frame' || d.type === 'image' || d.type === 'stylesheet' || d.type === 'script' || d.type === 'font') return;
    const k = kindByHeaders(d);
    if (k) noteStream(d.tabId, d.url, k.kind, k.mime, k.size, (sent.get(d.requestId) || {}).h);
  }, { urls: ['<all_urls>'] }, ['responseHeaders']);
} catch (e) { /* a browser without it: addresses alone are used */ }

// How the browser itself asked for each file (its own User-Agent, Referer, Origin, Accept, the site's own extra
// headers — never cookies, which travel their own way). DDM asks the same way, so a server that answers the
// browser's player answers DDM too, whatever it checks.
const sent = new Map();   // requestId -> { h, url, at }
const WANT = /^(user-agent|referer|origin|accept|accept-language|authorization|sec-ch-ua[\w-]*|sec-fetch-(site|mode|dest)|x-[\w-]+)$/i;
function keepHeaders(list) {
  const o = {};
  let total = 0, n = 0;
  for (const h of list || []) {
    const name = h.name || '', v = h.value || '';
    if (!WANT.test(name) || /^x-client-data$/i.test(name) || v.length > 3000 || /[\r\n]/.test(v)) continue;
    total += name.length + v.length;
    if (total > 12000 || ++n > 24) break;
    o[name] = v;
  }
  return o;
}
function sentFor(url) { for (const v of sent.values()) if (v.url === url) return v.h; return undefined; }
function onSend(d) {
  if (d.tabId < 0 || !(d.type === 'media' || d.type === 'xmlhttprequest' || d.type === 'other' || d.type === 'object')) return;
  const h = keepHeaders(d.requestHeaders), now = Date.now();
  sent.set(d.requestId, { h, url: d.url, at: now });
  if (sent.size > 500) { for (const [k, v] of sent) if (now - v.at > 60000) sent.delete(k); if (sent.size > 800) sent.clear(); }
  const list = tabStreams.get(d.tabId);   // a stream already noted by its address
  const hit = list && list.find((s) => s.url === d.url);
  if (hit) { hit.headers = h; saveStreams(); }
}
try { api.webRequest.onSendHeaders.addListener(onSend, { urls: ['<all_urls>'] }, ['requestHeaders', 'extraHeaders']); }   // Chrome: Referer and friends need "extraHeaders"
catch (e) { try { api.webRequest.onSendHeaders.addListener(onSend, { urls: ['<all_urls>'] }, ['requestHeaders']); } catch (e2) { /* addresses and types alone */ } }

// The browser may put the add-on to sleep between two clicks: what the tabs played is kept for when it wakes up.
const streamStore = api.storage && (api.storage.session || api.storage.local);
let saveTimer = 0;
function saveStreams() {
  if (!streamStore || saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = 0;
    const o = {};
    for (const [id, list] of tabStreams) if (list.length) o[id] = list;
    const pages = {};
    for (const [id, url] of tabPage) pages[id] = url.slice(0, 2000);
    try { Promise.resolve(streamStore.set({ tabStreams: o, tabPages: pages })).catch(() => {}); } catch (e) {}
  }, 700);
}
const streamsReady = (async () => {
  try {
    if (!streamStore) return;
    const v = await streamStore.get(['tabStreams', 'tabPages']);
    const tabs = await api.tabs.query({});
    const alive = new Set(tabs.map((t) => t.id));
    const now = Date.now();
    for (const [id, list] of Object.entries((v && v.tabStreams) || {})) {
      const tabId = parseInt(id, 10);
      if (!alive.has(tabId) || freshTabs.has(tabId) || !Array.isArray(list)) continue;
      const have = tabStreams.get(tabId) || [];
      for (const s of list) if (s && typeof s.url === 'string' && now - s.at < 6 * 3600000 && !have.some((x) => x.url === s.url)) have.push(s);
      have.sort((a, b) => a.at - b.at);
      tabStreams.set(tabId, have.slice(-40));
    }
    for (const [id, url] of Object.entries((v && v.tabPages) || {})) {
      const tabId = parseInt(id, 10);
      if (alive.has(tabId) && !tabPage.has(tabId) && typeof url === 'string') tabPage.set(tabId, url);
    }
  } catch (e) {}
})();

// What a tab played, oldest first (at most the 16 newest, from the last six hours).
async function streamsOf(tabId) {
  await streamsReady;
  const now = Date.now();
  return (tabStreams.get(tabId) || []).filter((s) => now - s.at < 6 * 3600000).slice(-16)
    .map((s) => ({ url: s.url, kind: s.kind, mime: s.mime || '', size: s.size > 0 ? s.size : -1, headers: s.headers || undefined }));
}

// A stream's list of qualities, read by the browser itself (some sites answer such a request only from a real
// browser): first the way the page's own player reads it, then with the add-on's own rights. At most the four
// newest lists, 1.5 MB each, three seconds.
const listTexts = new Map();   // url -> text (kept ten minutes)
async function readLists(streams, tab, frameId) {
  const want = streams.filter((s) => s.kind === 'hls' || s.kind === 'dash').slice(-4);
  if (!want.length) return;
  const looksRight = (t) => typeof t === 'string' && t.length < 1500000 && (t.includes('#EXTM3U') || t.includes('<MPD'));
  const need = want.filter((s) => { const h = listTexts.get(s.url); if (h && Date.now() - h.at < 600000) { s.text = h.text; return false; } return true; });
  if (need.length) {
    let got = {};
    try {
      const opt = typeof frameId === 'number' ? { frameId } : undefined;
      const r = await Promise.race([
        Promise.resolve(api.tabs.sendMessage(tab.id, { type: 'ddm-read-lists', urls: need.map((s) => s.url) }, opt)),
        new Promise((done) => setTimeout(() => done(null), 3500)),
      ]);
      if (r && typeof r === 'object') got = r;
    } catch (e) {}
    await Promise.all(need.map(async (s) => {
      let text = got[s.url];
      if (!looksRight(text)) {
        try {
          const ctl = new AbortController();
          const timer = setTimeout(() => ctl.abort(), 3000);
          const res = await fetch(s.url, { credentials: 'include', signal: ctl.signal });
          text = res.ok ? await res.text() : '';
          clearTimeout(timer);
        } catch (e) { text = ''; }
      }
      if (looksRight(text)) { s.text = text; listTexts.set(s.url, { text, at: Date.now() }); }
    }));
    if (listTexts.size > 40) { const now = Date.now(); for (const [k, v] of listTexts) if (now - v.at > 600000) listTexts.delete(k); if (listTexts.size > 40) listTexts.clear(); }
  }
}

function tabOfDownload(item) {
  const hit = reqTab.get(item.finalUrl || item.url) || reqTab.get(item.url);
  return hit ? hit.tabId : -1;
}

// ---------------------------------------------------------------- downloads
function extOf(name) {
  const clean = (name || '').split(/[?#]/)[0];
  const base = clean.substring(Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\')) + 1);
  const i = base.lastIndexOf('.');
  return i > 0 ? base.substring(i + 1).toUpperCase() : '';
}

function typeWanted(ext) {
  if (!ext) return false;
  return (config.types || '').toUpperCase().split(/[\s,;]+/).filter(Boolean).some((t) => (t.endsWith('*') ? ext.startsWith(t.slice(0, -1)) : ext === t));
}

function siteSkipped(url) {
  try {
    const h = new URL(url).hostname.toLowerCase();
    return (config.skip || []).some((s) => { s = String(s).toLowerCase().trim(); return s && (h === s || h.endsWith('.' + s)); });
  } catch (e) { return false; }
}

function shouldCatch(item, fileName) {
  if (!config.catch) return false;
  const url = item.finalUrl || item.url || '';
  if (!/^(https?|ftp):/i.test(url)) return false;            // blob:, data:, file: stay in the browser
  if (item.byExtensionId) return false;
  const b = bypass.get(url);
  if (b && Date.now() - b < 120000) return false;
  const tab = tabOfDownload(item);
  if (tab >= 0 && offTabs.has(tab)) return false;            // switched off on that tab
  if (siteSkipped(url) || siteSkipped(item.referrer || '')) return false;
  if (config.minKB > 0 && item.totalBytes > 0 && item.totalBytes < config.minKB * 1024) return false;
  const ext = extOf(fileName || item.filename || '') || extOf(url);
  if (typeWanted(ext)) return true;
  const mime = (item.mime || '').toLowerCase();
  return /^(video|audio)\//.test(mime) || /(octet-stream|zip|compressed|x-msdownload|x-iso9660|pdf|x-bittorrent|vnd\.android|x-rar|x-7z)/.test(mime);
}

async function handOver(item, fileName) {
  const url = item.finalUrl || item.url;
  try { await api.downloads.cancel(item.id); } catch (e) {}
  try { await api.downloads.erase({ id: item.id }); } catch (e) {}
  let favicon = '';
  const tab = tabOfDownload(item);
  if (tab >= 0) { try { const t = await api.tabs.get(tab); favicon = t.favIconUrl || ''; } catch (e) {} }
  const msg = {
    type: 'download', url, referrer: item.referrer || '', page: item.referrer || '',
    filename: (fileName || item.filename || '').split(/[\\/]/).pop(), mime: item.mime || '', size: item.totalBytes > 0 ? item.totalBytes : -1,
    cookies: await cookieHeader(url, item.cookieStoreId), ua: navigator.userAgent, favicon, source: 'browser',
  };
  try { await native(msg); } catch (e) {
    bypass.set(url, Date.now());                               // DDM didn't answer: the browser downloads it after all
    try { api.downloads.download({ url }); } catch (x) {}
  }
}

const handled = new Set();   // download ids already decided (never handed over twice)
function decide(item, fileName) {
  if (handled.has(item.id)) return;
  handled.add(item.id);
  if (handled.size > 500) handled.clear();
  if (shouldCatch(item, fileName)) handOver(item, fileName);
}
if (api.downloads.onDeterminingFilename && !IS_FIREFOX) {
  // Chromium: decide before the browser writes anything.
  api.downloads.onDeterminingFilename.addListener((item, suggest) => { suggest(); decide(item, item.filename); });
}
// Every browser: when the name step is skipped (policies, Firefox), decide as soon as the download appears.
api.downloads.onCreated.addListener((item) => setTimeout(() => decide(item, item.filename), IS_FIREFOX ? 0 : 400));

// ---------------------------------------------------------------- videos (from content.js)
async function videoPlaying(msg, tab, chosen, frameId) {
  if (!tab || tab.id === undefined) return;
  const page = msg.pageUrl || tab.url || '';
  const framed = typeof frameId === 'number' && frameId !== 0;
  if (!chosen) {   // found by itself: respect the switch, the site list and "once per video"
    if (offTabs.has(tab.id) || !config.catch || siteSkipped(page)) return;
    const key = page.split('#')[0] + '|' + (msg.src || '').split('?')[0];
    const seen = sentVideos.get(tab.id) || new Map();
    if (seen.has(key) && Date.now() - seen.get(key) < 110000) return;
    seen.set(key, Date.now());
    sentVideos.set(tab.id, seen);
  }
  const streams = await streamsOf(tab.id);
  if (chosen) await readLists(streams, tab);
  native({
    type: 'video', kind: 'video', url: page, page: tab.url || page, topUrl: tab.url || '', direct: msg.src || '', streams, title: videoTitle(msg, tab, framed),
    duration: msg.duration || 0, width: msg.width || 0, height: msg.height || 0, tab: tab.id,
    cookies: await cookieHeader(page, tab.cookieStoreId), directCookies: msg.src ? await cookieHeader(msg.src, tab.cookieStoreId) : '',
    ua: navigator.userAgent, favicon: tab.favIconUrl || '', source: 'browser', quality: msg.quality || '', pageData: typeof msg.pageData === 'string' ? msg.pageData : '',
  }).catch(() => {});
}

// The "Download this video" button: the qualities DDM finds for the video, then the one you pick.
// The video's name. A player inside another site's page is called "Player" or "Embed" by its own little page:
// the page around it (the tab) names the video.
function videoTitle(msg, tab, framed) {
  const own = (msg.title || '').trim(), top = (tab.title || '').trim();
  if (framed && top) return top;
  return own || top;
}
const isYt = (u) => /^https?:\/\/([^/]*\.)?(youtube\.com|youtu\.be|youtube-nocookie\.com)\//i.test(u || '');
async function videoFormats(msg, tab, frameId) {
  if (!tab || tab.id === undefined) return { ok: false, error: 'No tab' };
  if (offTabs.has(tab.id)) return { ok: false, error: 'DDM is turned off on this tab (click its icon to turn it on).' };
  const page = msg.pageUrl || tab.url || '';
  const framed = typeof frameId === 'number' && frameId !== 0;
  const streams = await streamsOf(tab.id);
  await readLists(streams, tab, frameId);
  try {
    return await native({
      type: 'formats', prefetch: !!msg.prefetch, url: page, page: tab.url || page, topUrl: tab.url || '', direct: msg.src || '', streams, title: videoTitle(msg, tab, framed),
      duration: msg.duration || 0, width: msg.width || 0, height: msg.height || 0, tab: tab.id,
      cookies: await cookieHeader(isYt(page) ? 'https://www.youtube.com/' : page, tab.cookieStoreId), directCookies: msg.src ? await cookieHeader(msg.src, tab.cookieStoreId) : '',
      ua: navigator.userAgent, favicon: tab.favIconUrl || '', source: 'browser', pageData: typeof msg.pageData === 'string' ? msg.pageData : '',
    });
  } catch (e) { return { ok: false, error: whyNoAnswer(e) }; }
}

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || typeof msg.type !== 'string') return;
  if (msg.type === 'ddm-status?') {   // the welcome page: is DDM on this PC and talking?
    native({ type: 'hello', browser: browserName() }).then((r) => sendResponse({ ok: true, version: (r && r.version) || '' }), (e) => sendResponse({ ok: false, why: String((e && e.message) || e || '') }));
    return true;
  }
  if (msg.type === 'ddm-state?') { sendResponse({ off: sender.tab ? offTabs.has(sender.tab.id) : false, button: config.button !== false }); return; }
  if (msg.type === 'ddm-formats') { videoFormats(msg, sender.tab, sender.frameId).then(sendResponse, () => sendResponse({ ok: false })); return true; }
  if (msg.type === 'ddm-pick') {
    if (!msg.token) {   // "Best quality" / "Sound only" before the list arrived: Download Video opens with that already chosen
      const quality = msg.quick === 'best' || msg.quick === 'mp3' ? msg.quick : '';
      videoPlaying({ pageUrl: msg.pageUrl, src: msg.src || '', title: msg.title || (sender.tab && sender.tab.title), duration: msg.duration || 0, quality, pageData: msg.pageData || '', width: msg.width || 0, height: msg.height || 0 }, sender.tab, true, sender.frameId);
      sendResponse({ ok: true });
      return;
    }
    native({ type: 'pick', token: msg.token, index: typeof msg.index === 'number' ? msg.index : -1 }).then(sendResponse, (e) => sendResponse({ ok: false, error: whyNoAnswer(e) }));
    return true;
  }
});

// ---------------------------------------------------------------- right-click menu
// Chrome can start the add-on with onInstalled and onStartup at once: the menu is rebuilt one at a time, and an item
// that already exists is never an error.
let menuJob = Promise.resolve();
function menus() {
  menuJob = menuJob.then(() => new Promise((done) => {
    try {
      api.contextMenus.removeAll(() => {
        void api.runtime.lastError;
        const items = [
          { id: 'ddm-link', title: 'Download with DDM', contexts: ['link'] },
          { id: 'ddm-media', title: 'Download this video with DDM', contexts: ['video', 'audio'] },
          { id: 'ddm-image', title: 'Download image with DDM', contexts: ['image'] },
          { id: 'ddm-selection', title: 'Download selected links with DDM', contexts: ['selection'] },
          { id: 'ddm-here', title: 'Download with DDM', contexts: ['page', 'frame'] },
          { id: 'ddm-all-links', title: 'Download all links with DDM…', contexts: ['page', 'frame'] },
          { id: 'ddm-all-images', title: 'Download all images with DDM…', contexts: ['page', 'frame'] },
          { id: 'ddm-pdf', title: 'Save this page as PDF with DDM', contexts: ['page', 'frame'] },
        ];
        let left = items.length;
        for (const it of items) api.contextMenus.create(it, () => { void api.runtime.lastError; if (--left === 0) done(); });
      });
    } catch (e) { done(); }
  }));
  return menuJob;
}

// what to tell the user when the browser can't reach DDM
function whyNoAnswer(e) {
  const m = String((e && e.message) || e || '').toLowerCase();
  if (m.includes('not found')) return 'DDM isn\u2019t set up for this browser yet. Start DDM once (or Options \u2192 Browser \u2192 Repair), then try again.';
  if (m.includes('forbidden')) return 'DDM doesn\u2019t know this copy of the add-on yet. Start DDM, wait a few seconds, then try again.';
  if (m.includes('exited') || m.includes('disconnected')) return 'DDM\u2019s helper stopped. Start DDM, then try again.';
  return 'DDM isn\u2019t answering. Start DDM, then try again.';
}

function ask(tabId, msg, frameId) {
  return Promise.resolve(api.tabs.sendMessage(tabId, msg, frameId === undefined ? {} : { frameId })).catch(() => null);
}

async function sendLink(url, tab, extra) {
  const page = (tab && tab.url) || '';
  return native(Object.assign({
    type: 'link', url, page, referrer: page, cookies: await cookieHeader(url, tab && tab.cookieStoreId), ua: navigator.userAgent,
    favicon: (tab && tab.favIconUrl) || '', source: 'browser',
  }, extra || {})).catch(() => {});
}

// every link / picture at once: DDM opens ONE window listing them (tick what you want), never a window per link
async function sendMany(items, tab, mode) {
  if (!items || !items.length) return;
  const page = (tab && tab.url) || '';
  native({
    type: 'links', mode: mode || 'links', page,
    items: items.slice(0, 5000).map((i) => ({ url: i.url, text: String(i.text || i.name || '').slice(0, 300), kind: i.kind || '' })),
    cookies: await cookieHeader(page, tab && tab.cookieStoreId), ua: navigator.userAgent, source: 'browser',
  }).catch(() => {});
}

api.contextMenus.onClicked.addListener(async (info, tab) => {
  if (!tab) return;
  switch (info.menuItemId) {
    case 'ddm-link': sendLink(info.linkUrl, tab); break;
    case 'ddm-image': sendLink(info.srcUrl, tab); break;
    case 'ddm-media':
      if (/^https?:/i.test(info.srcUrl || '')) sendLink(info.srcUrl, tab);
      else videoPlaying({ pageUrl: info.pageUrl || tab.url, src: '', title: tab.title }, tab, true);   // a stream: DDM reads the page
      break;
    case 'ddm-selection': { const r = await ask(tab.id, { type: 'ddm-collect', mode: 'selection' }, info.frameId); sendMany(r && r.items, tab, 'selection'); break; }
    case 'ddm-all-links': { const r = await ask(tab.id, { type: 'ddm-collect', mode: 'links' }, info.frameId); sendMany(r && r.items, tab, 'links'); break; }
    case 'ddm-all-images': { const r = await ask(tab.id, { type: 'ddm-collect', mode: 'images' }, info.frameId); sendMany(r && r.items, tab, 'images'); break; }
    case 'ddm-pdf': {   // the page as you see it (signed in, scrolled, expanded) goes to DDM, which prints it to a PDF
      const url = info.frameUrl || info.pageUrl || tab.url || '';
      if (!/^https?:/i.test(url)) break;
      const snap = await ask(tab.id, { type: 'ddm-snapshot' }, info.frameId || 0);
      const html = snap && snap.ok && typeof snap.html === 'string' && snap.html.length < 12000000 ? snap.html : '';   // a huge page: DDM prints its address instead
      native({ type: 'pdf', url, title: (snap && snap.title) || tab.title || '', html }).catch(() => {});
      break;
    }
    case 'ddm-here': {
      const r = await ask(tab.id, { type: 'ddm-target' }, info.frameId);
      const it = r && r.item;
      if (it && it.url) sendLink(it.url, tab, { filename: it.name || '' });
      else if (it && it.kind === 'video') videoPlaying({ pageUrl: tab.url, src: '', title: tab.title }, tab, true);
      break;
    }
  }
});

// The first time the add-on is installed (never again, not on updates): a welcome page in a new tab — it says whether
// DDM is on this PC and how to get it.
api.runtime.onInstalled.addListener((details) => {
  menus(); hello(); loadOff();
  if (!details || details.reason !== 'install') return;
  Promise.resolve(api.storage.local.get('welcomed')).then((v) => {
    if (v && v.welcomed) return;
    api.storage.local.set({ welcomed: true });
    api.tabs.create({ url: api.runtime.getURL('welcome.html'), active: true });
  }).catch(() => {});
});
api.runtime.onStartup && api.runtime.onStartup.addListener(() => { hello(); loadOff(); });   // the menu items are kept by the browser
hello();
loadOff();
