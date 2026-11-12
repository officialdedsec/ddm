// DDM (Firefox): puts ttpage.js into TikTok's own page, where TikTok loads its posts (see ttpage.js).
(() => {
  try {
    const s = document.createElement('script');
    s.src = (typeof browser !== 'undefined' ? browser : chrome).runtime.getURL('ttpage.js');
    s.onload = () => s.remove();
    (document.head || document.documentElement).appendChild(s);
  } catch (e) {}
})();
