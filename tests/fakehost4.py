#!/usr/bin/env python3
# Stand-in for ddmhost.exe in browser tests: logs each message, answers like DDM.
import sys, json, struct, time
LOG = "/tmp/ddm4host.log"
def rd():
    h = sys.stdin.buffer.read(4)
    if len(h) < 4: return None
    n = struct.unpack('<I', h)[0]
    return json.loads(sys.stdin.buffer.read(n))
def wr(o):
    b = json.dumps(o).encode()
    sys.stdout.buffer.write(struct.pack('<I', len(b)) + b); sys.stdout.buffer.flush()
m = rd()
keep = {k: (v if not isinstance(v, str) or len(v) < 90 else v[:70] + '…') for k, v in m.items() if k in ('type', 'url', 'page', 'direct', 'filename', 'title', 'kind', 'favicon', 'size', 'mime', 'token', 'index', 'prefetch', 'browser')}
if m.get('type') == 'links': keep['items'] = len(m.get('items', []))
keep['hasCookies'] = bool(m.get('cookies'))
keep['streams'] = len(m.get('streams', []))
with open(LOG, 'a') as f: f.write(json.dumps(keep) + '\n')
if m.get('streams'):   # what the add-on noted about each stream the page played
    with open('/tmp/ddm4streams.log', 'a') as f: f.write(json.dumps({'type': m.get('type'), 'height': m.get('height'), 'topUrl': m.get('topUrl'), 'title': m.get('title'), 'url': m.get('url'), 'streams': [{'url': s.get('url'), 'kind': s.get('kind'), 'mime': s.get('mime'), 'size': s.get('size'), 'headers': s.get('headers'), 'text': len(s.get('text', ''))} for s in m['streams']]}) + '\n')
if m.get('type') == 'hello': wr({'ok': True, 'catch': True, 'types': 'ZIP EXE MP4', 'minKB': 0, 'skip': [], 'videoButton': True})
elif m.get('type') == 'formats':
    time.sleep(0.3)   # like reading a video site
    wr({'ok': True, 'token': 'tok1', 'title': 'Test clip', 'formats': [{'label': '1080p HD', 'detail': 'MP4 · 48 MB', 'group': 'Video'}, {'label': '720p', 'detail': 'MP4 · 22 MB', 'group': 'Video'}, {'label': 'Sound', 'detail': 'MP3', 'group': 'Audio'}]})
else: wr({'ok': True})
