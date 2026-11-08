# What's new in DDM

## 4.2.2 — November 2026
**The download window**
- Its title bar now has two minimize buttons, like IDM: the green arrow (Minimize to system tray) sits next to Minimize, Maximize and Close; the usual – still puts it on the taskbar
- The speedometer and the speed graph are gone; Show details / Hide details now shows or hides the parts of the file
- Everything else stays as it was: the rows, the progress bar, Speed limiter, When done, Security, the tray icon and its menu

## 4.2.1 — November 2026
**The download window, like IDM's**
- Download status now shows one row each: URL, Status, File size, Downloaded with its percentage, Transfer rate, Time left and Resume capability
- A big progress bar with the percentage on it: stripes slide while it downloads, amber when paused, red on an error (with what went wrong written under it)
- Show details / Hide details: the speedometer, the speed graph and the parts of the file are in the details; the window now really gets smaller when they're hidden, and on a small screen it opens with them hidden
- A second minimize button, like IDM: the green arrow at the top right (also in the title bar's menu) hides the window in the tray with its own icon; the title bar's – now puts it on the taskbar (Options → General can make it go to the tray again)

**Unfinished downloads stay out of your folders**
- .ddmpart files and a video's separate picture and sound now wait in DDM's own folder for unfinished downloads; your folders only ever get the finished file
- Options → Downloads → Folder for unfinished downloads lets you choose where they wait; what's already there moves along
- Downloads started with an older DDM finish where they already are

**TikTok**
- Reels that still said "No video was found" are found: the browser add-on (4.0.5) also catches the posts TikTok's own player loads as you scroll
- Photo slideshows offer their song, and DDM says when a post has no video

## 4.2.0 — November 2026
**DDM in your language**
- DDM now speaks বাংলা (Bengali), हिन्दी (Hindi), اردو (Urdu), العربية (Arabic), Español, Français, Português (Brasil) and Bahasa Indonesia, as well as English
- Options → General → Language changes every window, menu, button, message and notification at once; "Same as Windows" follows Windows' own language
- Bengali and Hindi use Windows' own fonts for them; Arabic and Urdu read from right to left
- File names, websites, video titles and your own categories always stay exactly as they are

**Videos**
- TikTok: reels that showed only "Open in DDM" now list every quality — your browser reads the post with your own session and hands it to DDM, so TikTok can't hide it any more
- Bilibili (bilibili.com, b23.tv links, multi-part videos, series episodes, bilibili.tv): every quality up to 4K, picture and sound downloaded with many connections each and joined into one MP4; with your browser's sign-in you get the qualities your account allows

**Security**
- Your browser's cookies for a site are sent only to that site, never to another site a download link redirects to
- A downloads list file someone sends you can no longer place files in Windows' startup folders or other system folders
- "Save page as PDF" locks the page copy down before anything in it can run
- X-Ray can't be made to use too much memory or read past a damaged archive's data
- Windows parts DDM loads by name always come from Windows' own folder
- The phone remote and the local player can't be held up by a slow or broken connection

## 4.1.1 — November 2026
**TikTok**
- Swiping through the feed: the button always downloads the video you're watching, even when the address bar still shows another one
- Every TikTok video is saved under its own caption (never "TikTok Embed.mp4" again), with its creator's name when it has no caption
- The download window shows the video's cover picture, like for YouTube
- When TikTok refuses DDM's first way of reading a video, its player page is read properly: caption, cover, length and the video itself
- Every quality TikTok has for a video is offered (1080p, 720p, 540p…)

**Facebook, Instagram, Threads**
- Reels and videos are saved under the caption the creator wrote, with the video's own cover in the download window

**Windows**
- "File already downloaded" and the browser add-on questions: the window is now as wide as its buttons, so no button is ever cut off
- "Add DDM to your browser": after clicking Add, the longer button no longer covers the browser's name

## 4.1.0 — November 2026
**Videos**
- TikTok: videos from the For You feed and from shared links (also links without the creator's name) are found again; DDM tries the video page and TikTok's own player page, with the cookies TikTok hands out
- When a site refuses to show its qualities, the button's **Best quality** downloads the very file your browser is playing, with your browser's sign-in — no more "No video was found"
- The button's menu says so when a site takes long to read, instead of waiting without a word
- Every video gets its own name: the caption shown next to it (TikTok feeds), its title, or its number — never "(20+) Facebook" or another tab title again, so downloads no longer ask to replace each other

**Updates**
- DDM's updates and site rules now come from github.com/officialdedsec/ddm (the old address is still read too)

## 4.0.9 — November 2026
**Playlists, channels and subscriptions**
- Download a whole YouTube playlist or channel at once (videos, Shorts or live): paste its address or use Tasks → Download playlist or channel…, untick what you don't want, choose the quality (up to MP3) and the folder
- Long playlists are read completely, not only their first page
- Subscribe to a playlist or channel and its new videos download by themselves, in the quality you chose (Tools → Subscriptions… to check now, pause or unsubscribe)

**Phone remote**
- Tools → Phone remote…: scan the QR code with your phone and see every download with its progress and speed; add links, pause, resume or remove downloads from the phone — nothing to install
- Works only inside your own network and only with the link's secret code; wrong tries are locked out; New link makes a new code at any time

**Play while downloading**
- Right-click a video or song → Play while downloading: DDM's player plays what has arrived, and when you jump ahead, that part of the file is fetched first
- Play in DDM for finished files, and Play in your browser for formats Windows can't play

## 4.0.8 — November 2026
**Videos**
- More sites by DDM's own engine: Snapchat, and every site that describes its videos in the page's data (JSON-LD, Next.js and app data), with all their qualities
- **Adapts by itself when sites change**: DDM's site rules (signed by DedSec) update by themselves every few hours, and at once when a video can't be found — no new version needed
- No outside programs at all any more: the spare torrent program (aria2) and the full FFmpeg download are gone; torrents use DDM's built-in engine only, and File tools use DDM's own built-in tools

**DDM updates itself**
- New versions are downloaded, checked (signed by DedSec) and installed by themselves as soon as nothing is downloading; DDM comes back the way it was, in the tray if it was there (Options → General → Update DDM by itself)

**Your own categories**
- View → Add category… (or right-click the categories): your own categories with their own file types and folder; rename, delete, change file types or folder with a right-click

**Fixes**
- The Undo card after removing downloads stays inside DDM's window: minimizing or closing the window to the tray ends it (the removal is final)

## 4.0.7 — November 2026
**Videos from every site, with DDM's own engine**
- DDM now reads videos from **any site** by itself — YouTube, Facebook, Instagram, Threads, TikTok, X, Vimeo, Dailymotion, Reddit and every other site with a video player (also players embedded from another site, and YouTube videos shown in other pages). Nothing is downloaded or installed for videos any more, ever
- **Every quality** of streamed videos (HLS .m3u8 and DASH .mpd): pieces are fetched many at a time on kept-open connections, key-protected streams (AES-128) are decrypted, separate sound tracks are joined, and one normal MP4 comes out without re-encoding. Live streams are recorded until you stop them
- The quality list appears in milliseconds once the page has arrived (even very large pages)
- **Resume that doesn't give up**: stream pieces are kept one by one, and when a site's video address expires DDM reads the page again by itself and carries on from the same byte or piece
- The old video component and its helper programs are gone; DDM removes what earlier versions fetched

**Download windows**
- Minimize a download window and it gets **its own tray icon**, like IDM: it fills up as the file arrives (green / amber when paused / red on an error) and its tip shows the name, percentage, KB/s and time left. Double-click it to bring the window back; right-click it to Show, Pause / Resume, Stop or Cancel
- Speeds in **KB/s** like IDM (for example 1,536 KB/s) in the download window, its Peak and per-connection speed, the list's Transfer rate, the status bar and the tray, with MB/s and Mbit/s underneath the big number
- **X-Ray opens instantly** from Download File Info: DDM reads the archive's contents as soon as the window shows it (the start and end of the file at the same time), and X-Ray says how long it took in milliseconds

**Tools**
- **Remove image background** (File tools → Pictures, or right-click a finished picture → File tools): the subject cut out on a transparent PNG by a small neural network built into DDM, with edges refined against the picture — on your PC, nothing uploaded, in a few seconds
- File tools make H.264 videos, AAC sound and GIFs with DDM's own built-in tools (Windows' own H.264 encoder): no extra download

**Security and fixes**
- Addresses from web pages, playlists and redirects can only be web or FTP addresses (never files on your PC)
- A page's cookies are sent only to that page's own site, never to other sites its videos come from
- Thumbnail and subtitle names are cleaned before they're saved
- A dead video address is no longer retried several times a second; the waits grow
- Very large or unusual web pages can't slow DDM down or make it stop

## 4.0.6 — November 2026
**YouTube without waiting**
- DDM reads YouTube videos itself: no helper to download, nothing to install (no Python or other programs), so even the first YouTube video starts at once
- Joining video and sound, and making MP3s, is built into DDM (installed with it): no more "Preparing video merging…" on the first video
- YouTube streams download with all of DDM's connections from the first second, in pieces YouTube doesn't slow down
- An expired YouTube address is renewed by itself and the download carries on from the same byte
- Thumbnails and subtitles (.srt) for YouTube are saved by DDM itself
- Copying, cutting, MP3 conversion and metadata removal in File tools no longer need a download either
- For other sites, and for YouTube videos that need sign-in or are age-restricted, DDM's video component is fetched quietly in the background a few minutes after DDM starts, so it's ready before you need it
- Only one "Download complete" window when a video finishes while its window is still open

## 4.0.5 — November 2026
**Downloads and the list**
- One Pause / Resume button that follows what's selected, a new **Stop** button (a stopped download waits until you start it yourself), and one Pause all / Resume all button — all three now act on exactly what they say
- Removing downloads (one, several, or Delete finished) shows an **Undo** card for 3 seconds (`Ctrl+Z` works too)
- Right-click a finished file → **Move / rename…**
- View → **Hide categories** / Show categories (`Ctrl+B`): the panel slides away and back
- File Properties shows exactly how long a file took (to the second, pauses not counted)
- No more "All downloads are finished" pop-up
- Download File Info shows the file's own Windows icon (PDF, Word, programs…) and keeps downloading at full speed from the moment it opens

**Torrents**
- Right-click: Start / Pause, Stop, Force start, **Copy magnet link**, and Delete torrent (keep files, delete the .torrent file, delete torrent + data, or delete only the data)
- Share (seed) switches to Stop sharing at once

**Options and browsers**
- Options → Connection → **User agent** for links you add yourself: ready-made choices or your own (checked before it's saved)
- Options pages scroll smoothly
- Add DDM to your browser finds every browser on the PC (also Opera GX, Yandex, Chromium, Waterfox, LibreWolf, Floorp) and opens the store page in that browser, not the default one

**Licence and security**
- The 30-day trial can't be restarted by deleting files or registry keys, and setting the clock back or forward doesn't change it; after 30 days DDM opens only the activation window
- Contact: admin@dedseec.com (in About, Register DDM and the installer); setups are also signed with the renewed certificate
- DDM's helpers run in their own clean environment: programs installed on the PC (such as Python) can't change how they work
- Help search and index boxes show the text cursor again

## 4.0.4 — November 2026
**New download window**
- A redesigned download window with a live speedometer: the transfer rate in MB/s (or KB/s) and Mbit/s, with a scale that grows with your speed
- Progress, downloaded / total, time left, peak speed and resume support at a glance, plus a graph of the speed over the last seconds
- The parts of the file shown as blocks that fill in as they arrive, with the number of connections and the speed of each
- Speed limiter: pick a speed with one click (64 KB/s to 10 MB/s) and see how fast it goes right now
- When done and Security tabs laid out as cards; warnings stand out in colour
- Follows DDM's light and dark look, also while the window is open; same size as before, and every button and option is still there

## 4.0.3 — November 2026
**Dust effect**
- Removed downloads turn into fine dust that blows away, and the rows below slide up into the gap
- Download File Info and download windows turn to dust when you close or minimize them (they close at once; nothing waits)
- On/off in Options → General; skipped when Windows' "Show animations" is off and in Remote Desktop
- Download File Info can now be minimized to the taskbar

**Security**
- Save page as PDF: the page copy is locked down, so it can't run code or read files on your PC into the PDF
- DDM only opens web (http / https) addresses from download pages, never local files or other protocol handlers
- Very long file names are shortened without breaking letters of other languages

## 4.0.2 — November 2026
**New torrent engine**
- A faster BitTorrent engine built into DDM (nothing extra to download), with progress kept through restarts
- Torrent details window: General (progress, ratio, seeds and peers, availability, pieces), Files (choose files and High / Normal / Low priority), Trackers (add, remove, update now), Peers, and each torrent's own options
- Sharing (seeding) that stops by itself at a ratio or after a time, per torrent or for all; what was sharing shares again when DDM starts
- Options → Torrents: port with automatic router setup (UPnP / NAT-PMP), DHT, peer exchange, local peers, µTP, encryption, anonymous mode, IP blocklist, connection limits, first and last pieces first, move finished torrents, watch folder, trackers added to every torrent
- Tools → Create torrent: make a .torrent (v1, hybrid or v2) from a file or folder, get its magnet link, and share it at once

## 4.0.1 — October 2026
**Fixes and improvements**
- Download all links / all images / selected links: one window lists everything found on the page (never a window per link). Tick what you want, show one kind at a time (videos, pictures, music, archives, documents, programs, torrents, web pages), and save into category folders or one folder of your choice
- Links without a file name get a proper name and extension from the server before they start
- Save this page as PDF is much more reliable: it saves the page as you see it, tries a second way and the other browser before giving up, and isn't blocked by folder protection or antivirus
- Facebook and Instagram Reels and TikTok videos are named after their real caption (no links, hashtags, mentions or "| Facebook" endings)

## 4.0.0 — October 2026
A new DDM, rebuilt from the ground up as a small native Windows program.

**Security**
- Link & site check: look-alike, brand-new and malware sites, disguised programs, bait names — Safe / Be careful / Dangerous in Download File Info
- Security tab in every download window: live SHA-256, the server's checksum, antivirus result, Mark of the Web
- Programs disguised as other files are flagged when they arrive
- X-Ray: look inside ZIP, RAR, 7z, ISO, TAR, CAB, torrents and programs before downloading
- Metadata remover for photos, videos, music and Office documents

**Speed and reliability**
- Up to 32 connections, several network connections at once, every part checked
- Downloads start while the download window is open
- Automatic resume after connection drops; Refresh download link for expired links
- Speed limit lifted while nothing else uses the internet

**Videos**
- "Download this video" button above videos, with every quality; opens at once
- HLS streams, thumbnails and subtitles; sizes shown as exact, estimated or unknown

**Browser add-ons** for Chrome, Edge, Firefox, Opera, Brave and Vivaldi: Save page as PDF, download all links / images, a one-time welcome page

**Program**
- Minimize download windows to the tray; hideable tabs
- File tools from the right-click menu; statistics count finished downloads only
- Quiet automatic updates from GitHub, verified by signature and SHA-256
- Uninstaller that asks before removing your files or DDM's data
- Your DDM 3.x download list comes along on first start
