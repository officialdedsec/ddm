#!/usr/bin/env python3
"""tools/tr/<code>.txt ("N|translation", N = line of strings.txt; "EN|..." = line N of strings-extra.txt) + its "@" headers
-> res/lang/<code>.txt ("English => translation"). Checks that every {k} is kept."""
import re, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load(name):
    p = os.path.join(ROOT, 'tools', name)
    return [l for l in open(p, encoding='utf-8').read().split('\n') if l and not l.startswith('#')]

def main(code):
    base = load('strings.txt')
    extra = load('strings-extra.txt')
    src = os.path.join(ROOT, 'tools/tr', code + '.txt')
    heads, out, bad, seen = [], [], 0, set()
    for n, l in enumerate(open(src, encoding='utf-8').read().split('\n'), 1):
        l = l.rstrip('\r')
        if not l.strip() or l.startswith('#'):
            continue
        if l.startswith('@'):
            heads.append(l); continue
        m = re.match(r'^(E?)(\d+)\|(.*)$', l)
        if not m:
            print('%s:%d: not "N|text": %s' % (code, n, l[:60])); bad += 1; continue
        idx = int(m.group(2)) - 1
        table = extra if m.group(1) else base
        if idx < 0 or idx >= len(table):
            print('%s:%d: no line %s' % (code, n, m.group(1) + m.group(2))); bad += 1; continue
        en, to = table[idx], m.group(3).strip()
        if not to or (to == en and not re.search(r'\{\d\}', en)):   # (a template kept as it is still matters: its parts get translated)
            continue
        if sorted(re.findall(r'\{\d\}', en)) != sorted(re.findall(r'\{\d\}', to)):
            print('%s:%d: {k} differ: %s  =>  %s' % (code, n, en[:50], to[:50])); bad += 1; continue
        if en.count('\\n') != to.count('\\n') and en.count('\\n') > 0:
            pass   # line breaks may move; fine
        if (m.group(1), idx) in seen:
            continue
        seen.add((m.group(1), idx))
        if ' => ' in en:
            continue
        out.append(en + ' => ' + to)
    os.makedirs(os.path.join(ROOT, 'res/lang'), exist_ok=True)
    with open(os.path.join(ROOT, 'res/lang', code + '.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# DDM in this language. Lines: English => translation. {1}, {2}: the changing parts (keep them).\n')
        f.write('# Your own corrections: copy this file to DDM\'s data folder\\lang\\ and edit it; it wins over the built-in one.\n')
        for h in heads:
            f.write(h + '\n')
        for o in out:
            f.write(o + '\n')
    print(code, len(out), 'translations,', bad, 'problems')

if __name__ == '__main__':
    for c in sys.argv[1:]:
        main(c)
