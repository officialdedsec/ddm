// Real-browser test: the add-on notes the streams a page plays by what their server says they are (not by their
// address), keeps them while the page is open, and reads a stream's list of qualities itself.
//   node streamtest.js <unpacked extension dir>     (the mock site on 127.0.0.1:8871, fakehost4.py as DDM)
const { chromium } = require('playwright'); const fs = require('fs');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const LOG = '/tmp/ddm4streams.log';
const read = () => (fs.existsSync(LOG) ? fs.readFileSync(LOG, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse) : []);
let pass = 0, fail = 0;
const check = (name, ok, info) => { console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); ok ? pass++ : fail++; };
(async () => {
  try { fs.unlinkSync(LOG); } catch (e) {}
  const dir = process.argv[2];
  const ctx = await chromium.launchPersistentContext('/tmp/pwud-streams', { headless: false, viewport: { width: 1000, height: 700 },
    args: ['--disable-extensions-except=' + dir, '--load-extension=' + dir] });
  const errors = [];
  const sw = ctx.serviceWorkers()[0] || await ctx.waitForEvent('serviceworker');
  sw.on('console', (m) => { if (m.type() === 'error') errors.push('SW ' + m.text()); });
  await sleep(1200);
  const B = 'http://127.0.0.1:8871';
  const page = await ctx.newPage();
  await page.goto(B + '/sp.html');
  await page.waitForFunction(() => document.title === 'loaded', null, { timeout: 15000 });
  await sleep(800);
  const tab = await sw.evaluate(async () => { const t = (await chrome.tabs.query({})).find((x) => /sp\.html/.test(x.url)); return { id: t.id, url: t.url }; });
  const kept = await sw.evaluate((id) => tabStreams.get(id) || [], tab.id);
  const by = (list, re) => list.find((s) => re.test(s.url));
  check('stream list without .m3u8 noted as HLS', (by(kept, /opaque1/) || {}).kind === 'hls', JSON.stringify(by(kept, /opaque1/)));
  check('video file without .mp4 noted with its size', (by(kept, /obj\/video1/) || {}).kind === 'file' && by(kept, /obj\/video1/).size === 22967796 && by(kept, /obj\/video1/).mime === 'video/mp4', JSON.stringify(by(kept, /obj\/video1/)));
  check('sound file noted as sound', (by(kept, /obj\/audio1/) || {}).mime === 'audio/mp4', JSON.stringify(by(kept, /obj\/audio1/)));
  check('pieces and pictures are not noted', !by(kept, /\.ts$/) && !by(kept, /thumb/), kept.map((s) => s.url).join(' '));
  // long after (the old add-on forgot them after 90 seconds): still there for the button
  await sw.evaluate((id) => { for (const s of tabStreams.get(id)) s.at -= 3600000; }, tab.id);
  const r = await sw.evaluate(async (t) => videoFormats({ pageUrl: t.url, src: '', title: 'Stream page', height: 720 }, { id: t.id, url: t.url }, 0), tab);
  const sent = read().filter((m) => m.type === 'formats').pop() || { streams: [] };
  check('DDM answered', r && r.ok === true, JSON.stringify(r).slice(0, 120));
  check('streams still sent an hour later', sent.streams.length >= 4, String(sent.streams.length));
  check('the playing height is sent', sent.height === 720, String(sent.height));
  check('list of qualities read by the browser', (by(sent.streams, /opaque1/) || {}).text > 100, JSON.stringify(by(sent.streams, /opaque1/)));
  check('a list only the page may read is read through the page', (by(sent.streams, /secret2/) || {}).text > 100, JSON.stringify(by(sent.streams, /secret2/)));
  // a player inside another page: the tab's address and title go along (they name the video, the frame's own don't)
  await sw.evaluate(async (t) => videoFormats({ pageUrl: 'https://player.example/embed.html', src: '', title: 'Player', duration: 24 }, { id: t.id, url: t.url, title: 'Holiday in Rome - TubeSite' }, 7), tab);
  const framed = read().filter((m) => m.type === 'formats').pop() || {};
  check('a framed player sends the tab as the video\'s page and name', framed.topUrl === tab.url && framed.title === 'Holiday in Rome - TubeSite' && framed.url === 'https://player.example/embed.html', JSON.stringify([framed.topUrl, framed.title, framed.url]));
  // how the browser itself asked for a file goes along, so DDM can ask the same way (never cookies)
  const hv = (by(sent.streams, /obj\/video1/) || {}).headers || {};
  const names = Object.keys(hv).map((k) => k.toLowerCase());
  check('the browser\'s own request headers are noted', names.includes('user-agent') && names.includes('referer') && !names.includes('cookie') && !names.includes('range'), names.join(','));
  // kept for when the browser puts the add-on to sleep
  await sleep(1000);
  const stored = await sw.evaluate(async () => { const a = chrome.storage.session || chrome.storage.local; return (await a.get('tabStreams')).tabStreams || {}; });
  check('kept across the add-on sleeping', (stored[tab.id] || []).length >= 4, String((stored[tab.id] || []).length));
  // a jump inside the page keeps them; another page drops them
  await page.evaluate(() => { location.hash = 't=30'; }); await sleep(600);
  check('#t=30 keeps the streams', (await sw.evaluate((id) => (tabStreams.get(id) || []).length, tab.id)) >= 4);
  await sleep(4200);   // (reading the lists counted as the page loading them again: only what is older than a few seconds goes)
  await page.evaluate(() => history.pushState({}, '', '/sp.html?v=2')); await sleep(600);
  check('another video on the same site: the old streams go', (await sw.evaluate((id) => (tabStreams.get(id) || []).length, tab.id)) === 0);
  await page.goto(B + '/blank.html'); await sleep(600);
  check('a new page starts empty', (await sw.evaluate((id) => (tabStreams.get(id) || []).length, tab.id)) === 0);
  // the button keeps its place while the pointer travels to it, also across another video
  const p2 = await ctx.newPage();
  await p2.goto(B + '/two.html');
  await p2.waitForTimeout(700);
  const boxOf = (sel) => p2.evaluate((q) => { const r = document.querySelector(q).getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }, sel);
  const v2 = await boxOf('#v2');
  await p2.mouse.move(v2.x + v2.w / 2, v2.y + v2.h / 2); await p2.waitForTimeout(250);
  await p2.mouse.move(v2.x + v2.w / 2 + 5, v2.y + v2.h / 2 + 5); await p2.waitForTimeout(400);
  const btnBox = () => p2.evaluate((v) => {   // (the button lives in a closed shadow root: found by what is under each point)
    let x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1;
    for (let y = Math.max(0, v.y - 70); y <= v.y + 40; y += 2)
      for (let x = Math.max(0, v.x + v.w - 340); x <= v.x + v.w + 12; x += 2) {
        const e = document.elementFromPoint(x, y);
        if (e && e.tagName === 'DDM-VIDEO-BUTTON') { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
      }
    return x1 < 0 ? null : { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
  }, v2);
  const b0 = await btnBox();
  check('the button shows for the video under the pointer', !!b0, JSON.stringify(b0));
  if (b0) {
    let same = true, last = b0;
    const tx = b0.x + b0.w / 2, ty = b0.y + b0.h / 2, sx = v2.x + v2.w / 2, sy = v2.y + v2.h / 2;
    for (let i = 1; i <= 24; i++) {   // a slow hand: over the other video above, 60 ms a step
      await p2.mouse.move(sx + (tx - sx) * i / 24, sy + (ty - sy) * i / 24); await p2.waitForTimeout(60);
      const b = await btnBox();
      if (!b || b.x !== b0.x || b.y !== b0.y) { same = false; last = b; break; }
    }
    check('the button stays where it is while the pointer goes to it', same, JSON.stringify([b0, last]));
  }
  check('no console errors', errors.length === 0, errors.join(' | '));
  console.log('RESULT pass=' + pass + ' fail=' + fail);
  await ctx.close();
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
