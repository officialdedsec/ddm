#!/usr/bin/env python3
"""The texts that need a translation (strings.txt + strings-extra.txt minus technical ones) -> tools/todo.txt "N|text"."""
import re, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = [r'\\r\\n', r'Mozilla/5\.0', r'User Data', r'\\\\', r'\|\*', r'^HLP_', r'Zone\.Identifier', r'^host!', r'^ddm-hw', r'\|init', r'\|\|live',
        r'ytInitialData', r"default-src", r'^text/html', r'^avi mpg', r'^doc docx', r'^exe msi', r'^mp3 wav', r'^zip rar', r'^bytes \*/', r'youtubei',
        r'SOCS=', r'^Accept-Encoding', r'^Content-Type', r'^\d\d\d [A-Z]', r'^(Bad Request|Method Not Allowed|No Content|Not Found|Range Not Satisfiable|Service Unavailable|Too Many Requests|Partial Content|unknown request|bad request|access denied|not found)$',
        r'^(facebook|instagram|tiktok|log in|log into|make your day|for you|sign in)', r'^URL:Magnet', r'^(Epic Games|OBS Studio|Opera GX|Chrome|Edge|Firefox|Google Chrome|Microsoft Edge|Mozilla Firefox|Yandex Browser|Microsoft Defender|Internet Download Manager|Free Download Manager|Neat Download Manager|Xtreme Download Manager|Tor|Ethernet|CRC-32|Ag|KB/s|MB/s|MBytes|English|DDM|DedSec Download Manager)$',
        r'^Ctrl\+|^Shift\+|^Alt\+|^Del$', r'^%zu', r'^\\xEF', r'^\{1\}\.\{2\} KB/s$', r'^\{1\} (KB/s|MB/s|kbps)$', r'^DDM \{1\}$', r'^\{1\}  \(\{2\} bytes\)$', r'^\{1\} × \{2\}', r'^\{1\}\|\{2\}',
        r'^Opera Software', r'^Microsoft Security', r'^Windows Defender\\', r'^Vivaldi', r'^Yandex\\', r'^Chromium', r'^Google\\', r'^BraveSoftware', r'^\\xEF']
lines = []
for name in ('strings.txt', 'strings-extra.txt'):
    p = os.path.join(ROOT, 'tools', name)
    if os.path.exists(p):
        for l in open(p, encoding='utf-8').read().split('\n'):
            if l and not l.startswith('#') and l not in lines:
                lines.append(l)
todo = [l for l in lines if not any(re.search(r, l) for r in SKIP)]
with open(os.path.join(ROOT, 'tools/todo.txt'), 'w', encoding='utf-8') as f:
    for i, l in enumerate(todo, 1):
        f.write('%d|%s\n' % (i, l))
print(len(lines), 'texts,', len(todo), 'to translate')
