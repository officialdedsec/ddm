// Welcome page: is DDM on this PC? (asked through the add-on; nothing leaves the computer)
(() => {
  const api = typeof browser !== 'undefined' ? browser : chrome;
  const box = document.getElementById('status'), title = document.getElementById('status-title'), detail = document.getElementById('status-detail');
  const get = document.getElementById('get'), retry = document.getElementById('retry'), steps = document.getElementById('steps');
  function show(state, t, d) {
    box.dataset.state = state; title.textContent = t; detail.textContent = d;
    get.hidden = state !== 'missing'; retry.hidden = state !== 'missing';
    steps.dataset.done = state === 'ready' ? 'true' : 'false';
    const h = document.getElementById('headline');
    if (state === 'ready') { h.textContent = 'The add-on is in. '; const s = document.createElement('span'); s.className = 'hl'; s.textContent = 'You\u2019re all set.'; h.appendChild(s); }
  }
  function check() {
    show('checking', 'Looking for DDM on this PC…', 'This takes a second.');
    Promise.resolve(api.runtime.sendMessage({ type: 'ddm-status?' })).then((r) => {
      if (r && r.ok) {
        show('ready', 'DDM is installed and ready.', 'You’re all set: downloads and videos now go to DDM' + (r.version ? ' ' + r.version : '') + '.');
        return;
      }
      const why = String((r && r.why) || '').toLowerCase();
      if (why.includes('forbidden')) show('missing', 'DDM is on this PC but doesn’t know this add-on yet.', 'Start DDM, wait a few seconds, then click Check again.');
      else show('missing', 'DDM for Windows isn’t installed yet.', 'Install it, start it once, then click Check again.');
    }).catch(() => show('missing', 'DDM for Windows isn’t installed yet.', 'Install it, start it once, then click Check again.'));
  }
  retry.addEventListener('click', check);
  check();
})();
