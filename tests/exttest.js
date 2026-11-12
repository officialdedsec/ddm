// Real-browser test of the silent extension. node exttest.js <unpacked extension dir>
const { chromium } = require('playwright'); const fs = require('fs');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const LOG = '/tmp/ddm4host.log';
const read = () => (fs.existsSync(LOG) ? fs.readFileSync(LOG, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse) : []);
let pass = 0, fail = 0;
const check = (name, ok, info) => { console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); ok ? pass++ : fail++; };
(async () => {
  try { fs.unlinkSync(LOG); } catch (e) {}
  const dir = process.argv[2];
  const ctx = await chromium.launchPersistentContext('/tmp/pwud4', { headless: false, viewport: { width: 1100, height: 760 },
    acceptDownloads: true,
    args: ['--host-resolver-rules=MAP www.tiktok.com 127.0.0.1:8833', '--disable-extensions-except=' + dir, '--load-extension=' + dir, '--autoplay-policy=no-user-gesture-required'] });
  const errors = [];
  const sw = ctx.serviceWorkers()[0] || await ctx.waitForEvent('serviceworker');
  sw.on('console', (m) => { if (m.type() === 'error') errors.push('SW ' + m.text()); });
  await sleep(1500);
  const B = 'http://127.0.0.1:8833';
  const page = await ctx.newPage();
  page.on('pageerror', (e) => errors.push('page ' + e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push('page ' + m.text()); });

  // 1) a download is handed to DDM
  await sw.evaluate(() => { self.__dlEvents = []; chrome.downloads.onCreated.addListener((i) => self.__dlEvents.push('created ' + i.url)); });
  await page.goto(B + '/dl.html'); await sleep(500);
  await page.click('#z'); await sleep(2500);
  const items = await sw.evaluate(async () => (await chrome.downloads.search({})).map((d) => d.url + ' ' + d.state));
  console.log('browser downloads: ' + JSON.stringify(items) + ' events: ' + JSON.stringify(await sw.evaluate(() => self.__dlEvents)));
  let l = read().filter((m) => m.type === 'download');
  check('download handed to DDM', l.some((m) => /sample\.zip/.test(m.url)), JSON.stringify(l[0] || {}));
  check('no extra UI injected', await page.evaluate(() => document.querySelectorAll('body *').length) === 1);

  // 2) a video playing with sound → DDM gets it once
  await page.goto(B + '/video.html'); await sleep(3500);
  l = read().filter((m) => m.type === 'video');
  check('video with sound handed to DDM', l.length === 1 && /video\.html/.test(l[0].url), JSON.stringify(l));
  check('direct address of the video sent', l.length && /clip\.webm/.test(l[0].direct || ''));
  const inPage = await page.evaluate(() => document.querySelectorAll('*').length);
  check('page untouched (no buttons)', inPage === 5, String(inPage));

  // 3) muted autoplay is ignored
  const before = read().filter((m) => m.type === 'video').length;
  await page.goto(B + '/muted.html'); await sleep(3500);
  check('muted autoplay preview ignored', read().filter((m) => m.type === 'video').length === before);

  // 4) a feed: the next video after scrolling is handed over too, each with its own address
  await page.goto('http://www.tiktok.com/feed.html'); await sleep(7000);
  const feed = read().filter((m) => m.type === 'video' && /video\/7421/.test(m.url));
  check('first feed video by its own address', feed.some((m) => /alice\/video\/7421000000000000001/.test(m.url)), feed.map((m) => m.url).join(' | '));
  check('next feed video after scrolling', feed.some((m) => /bob\/video\/7421000000000000002/.test(m.url)));

  // 5) switched off on this tab: nothing is caught
  const tabId = await sw.evaluate(async () => (await chrome.tabs.query({ active: true }))[0].id);
  await sw.evaluate((id) => toggleTab({ id }), tabId); await sleep(400);
  const n0 = read().length;
  await page.goto(B + '/video.html?again'); await sleep(3000);
  await page.goto(B + '/dl.html'); await page.click('#z'); await sleep(2500);
  const after = read().slice(n0).filter((m) => m.type === 'video' || m.type === 'download');
  check('off on this tab: no video, no download', after.length === 0, JSON.stringify(after));
  const title = await sw.evaluate((id) => chrome.action.getTitle({ tabId: id }), tabId);
  check('icon title says off', /off on this tab/.test(title), title);
  await sw.evaluate((id) => toggleTab({ id }), tabId); await sleep(400);
  const title2 = await sw.evaluate((id) => chrome.action.getTitle({ tabId: id }), tabId);
  check('clicking again turns it back on', /click to turn it off/.test(title2), title2);

  check('no console errors', errors.length === 0, errors.join(' | '));
  console.log('RESULT pass=' + pass + ' fail=' + fail);
  await ctx.close();
})().catch((e) => { console.error('ERR', e); process.exit(1); });
