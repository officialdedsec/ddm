#!/usr/bin/env python3
"""Collects every text DDM shows (string literals in the UI code and the messages of the engine), and the
sentences built from pieces ("{1} is already downloaded in this folder."), into tools/strings.txt.
Language files then translate those lines (res/lang/<code>.txt: "English => translation")."""
import re, sys, os, glob, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = glob.glob(os.path.join(ROOT, 'src/ui/*.cpp')) + glob.glob(os.path.join(ROOT, 'src/ui/*.hpp'))
CORE = glob.glob(os.path.join(ROOT, 'src/core/*.cpp')) + glob.glob(os.path.join(ROOT, 'src/common/*.cpp'))

TOK = re.compile(r'''
  (?P<str>(?:u8|L)?"(?:[^"\\\n]|\\.)*")
 |(?P<chr>'(?:[^'\\\n]|\\.)*')
 |(?P<id>[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)
 |(?P<num>\d[\w.]*)
 |(?P<op>==|!=|<=|>=|&&|\|\||<<|>>|->|::|\+=|[-+*/%<>=!&|^~?:;,.(){}\[\]#])
 |(?P<ws>\s+)
''', re.X)

def strip_comments(s):
    out, i, n = [], 0, len(s)
    while i < n:
        if s.startswith('//', i):
            j = s.find('\n', i); i = n if j < 0 else j
        elif s.startswith('/*', i):
            j = s.find('*/', i + 2); i = n if j < 0 else j + 2
        elif s[i] == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            out.append(s[i:j + 1]); i = j + 1
        elif s[i] == "'":
            j = i + 1
            while j < n and s[j] != "'":
                j += 2 if s[j] == '\\' else 1
            out.append(s[i:j + 1]); i = j + 1
        else:
            out.append(s[i]); i += 1
    return ''.join(out)

def tokens(src):
    toks = []
    for m in TOK.finditer(src):
        k = m.lastgroup
        if k == 'ws':
            continue
        toks.append((k, m.group(k)))
    return toks

def lit_value(t):
    if t.startswith('L') or t.startswith('u8'):
        return None
    v = t[1:-1]
    # keep C escapes as the program shows them: \n -> newline, \" -> ", \\ -> \
    out, i = [], 0
    while i < len(v):
        if v[i] == '\\' and i + 1 < len(v):
            c = v[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '0': '\0'}.get(c, '\\' + c)); i += 2
        else:
            out.append(v[i]); i += 1
    return ''.join(out)

STOP = {',', ';', ')', ']', '}', '?', ':', '==', '!=', '=', '&&', '||', '<<', '>>', '+=', '{', 'return', '<=', '>='}
OPEN = {'(': ')', '[': ']', '{': '}'}

def operand_end(toks, i):
    """from i, the end (exclusive) of one operand of a + chain"""
    depth = 0
    j = i
    while j < len(toks):
        k, t = toks[j]
        if depth == 0 and (t in STOP or t == '+' or (t in (')', ']', '}'))):
            break
        if t in OPEN:
            depth += 1
        elif t in (')', ']', '}'):
            depth -= 1
        j += 1
    return j

def operand_start(toks, i):
    """from i (inclusive, going left), the start of one operand"""
    depth = 0
    j = i
    while j >= 0:
        k, t = toks[j]
        if t in (')', ']', '}'):
            depth += 1
        elif t in OPEN:
            if depth == 0:
                break
            depth -= 1
        elif depth == 0 and (t in STOP or t == '+'):
            break
        j -= 1
    return j + 1

def as_literal(span):
    """an operand that is only text: "a" "b", std::string("a")"""
    vals = []
    s = span
    if len(s) >= 4 and s[0][1] in ('std::string', 'std::string_view') and s[1][1] == '(' and s[-1][1] == ')':
        s = s[2:-1]
    if s and all(k == 'str' for k, _ in s):
        for _, t in s:
            v = lit_value(t)
            if v is None:
                return None
            vals.append(v)
        return ''.join(vals)
    return None

BAD_CTX_BEFORE = {'logLine', 'GetProcAddress', 'LoadLibraryW', 'LoadLibraryA', 'getenv', 'GetEnvironmentVariableA', 'find', 'rfind', 'compare', 'startsWith',
                  'endsWith', 'jsonString', 'jsonAt', 'meta', 'headerValue', 'setHeader', 'regex', 'std::regex', 'S', 'B', 'I', 'D', 'contains', 'icon', 'iconName',
                  'openUrl', 'ShellExecuteW', 'execute', 'run', 'post', 'get', 'addHeader', 'RegSetValueExW', 'regGet', 'regSet', 'mk', 'path', 'joinPath'}

def visible(v):
    if not re.search(r'[A-Za-z]{2,}', v):
        return False
    if '\0' in v:
        return False
    if re.match(r'^(https?|ftp|magnet|file|mailto):', v) or re.search(r'\.(com|org|net|io)/', v):
        return False
    if re.match(r'^[a-z0-9_.\-/:%*?=&+#@\\]+$', v):                 # keys, ids, paths, extensions, mime types
        return False
    if re.match(r'^[A-Z0-9_]+$', v):                                  # CONSTANTS
        return False
    if re.match(r'^[MmLlHhVvCcSsQqTtAaZz0-9 .,\-]+$', v) and re.search(r'\d', v):   # SVG path data
        return False
    if re.match(r'^[A-Za-z]+(-[A-Za-z]+)+$', v) and ' ' not in v and v[0].isupper():   # HTTP headers
        return False
    if re.search(r'%[0-9.]*[sdufxl]', v) or '%lld' in v or '%I64' in v:
        return False
    if v.startswith('<') and '>' in v:
        return False
    if re.match(r'^\s*[{\[]', v):
        return False
    if '\\\\' in v or re.match(r'^[A-Z]:\\', v) or v.startswith('SOFTWARE') or v.startswith('Software\\'):
        return False
    if re.match(r'^[a-z]+[A-Z][A-Za-z]*$', v):                        # camelCase identifiers
        return False
    if re.search(r'\w\(\)', v) or ';' in v and ' ' not in v:
        return False
    return True

def ctx_bad(toks, start):
    """a literal used as a key, compared, or passed to an API that isn't shown"""
    if start >= 1 and toks[start - 1][1] in ('==', '!=', '['):
        return True
    if start >= 2 and toks[start - 1][1] == '(' and toks[start - 2][1].split('::')[-1] in BAD_CTX_BEFORE:
        return True
    if start >= 2 and toks[start - 1][1] == ',' :   # second argument of a key/API call: look back to the call
        depth = 0
        for j in range(start - 1, max(0, start - 40), -1):
            t = toks[j][1]
            if t == ')':
                depth += 1
            elif t == '(':
                if depth == 0:
                    return j >= 1 and toks[j - 1][1].split('::')[-1] in BAD_CTX_BEFORE - {'S', 'B', 'I', 'D', 'mk'}
                depth -= 1
    if start + 1 < len(toks) and toks[start + 1][1] in ('==', '!='):
        return True
    return False

def ternary_literals(span):
    """( cond ? "a" : "b" ) with literal branches (also nested: ? "a" : c2 ? "b" : "c") -> ["a", "b", "c"]; else None"""
    if len(span) < 5 or span[0][1] != '(' or span[-1][1] != ')':
        return None
    inner = span[1:-1]
    depth, q = 0, -1
    for j, (k, t) in enumerate(inner):
        if t in OPEN: depth += 1
        elif t in (')', ']', '}'): depth -= 1
        elif t == '?' and depth == 0:
            q = j
            break
    if q < 0:
        return None
    rest = inner[q + 1:]
    out = []
    while True:
        # one branch: a literal, then ':' or the end; or another "cond ? ..."
        depth, colon = 0, -1
        for j, (k, t) in enumerate(rest):
            if t in OPEN: depth += 1
            elif t in (')', ']', '}'): depth -= 1
            elif t == ':' and depth == 0:
                colon = j
                break
        branch = rest[:colon] if colon >= 0 else rest
        v = as_literal(branch)
        if v is None:
            # "c2 ? "b" : ..." nested
            depth, q2 = 0, -1
            for j, (k, t) in enumerate(branch):
                if t in OPEN: depth += 1
                elif t in (')', ']', '}'): depth -= 1
                elif t == '?' and depth == 0:
                    q2 = j
                    break
            if q2 >= 0 and colon >= 0:
                rest = rest[q2 + 1:]
                continue
            out.append(None)
        else:
            out.append(v)
        if colon < 0:
            break
        rest = rest[colon + 1:]
    return out if any(x is not None for x in out) and len(out) <= 6 else None

def collect(files, strict):
    exact, templ = {}, {}
    for f in files:
        src = strip_comments(open(f, encoding='utf-8').read())
        src = re.sub(r'^\s*#\s*(include|define|pragma|if|ifdef|ifndef|endif|else|elif).*$', '', src, flags=re.M)
        toks = tokens(src)
        i = 0
        seen_chain = set()
        while i < len(toks):
            k, t = toks[i]
            if k != 'str':
                i += 1
                continue
            # the chain around this literal
            s = operand_start(toks, i)
            e = operand_end(toks, i)
            # extend left over '+'
            parts = [(s, e)]
            L = s
            while L - 1 >= 0 and toks[L - 1][1] == '+':
                ps = operand_start(toks, L - 2)
                parts.insert(0, (ps, L - 1))
                L = ps
            R = e
            while R < len(toks) and toks[R][1] == '+':
                pe = operand_end(toks, R + 1)
                parts.append((R + 1, pe))
                R = pe
            key = (L, R)
            i = e if e > i else i + 1
            if key in seen_chain:
                continue
            seen_chain.add(key)
            if ctx_bad(toks, L):
                continue
            # each operand: its possible texts ("x" -> ["x"]; (c ? "a" : "b") -> ["a", "b"]; anything else -> [None])
            opts = []
            for (a, b) in parts:
                v = as_literal(toks[a:b])
                if v is not None:
                    opts.append([v])
                    continue
                alts = ternary_literals(toks[a:b])
                for lv in alts or []:
                    if lv and visible(lv) and lv[:1].isupper() and sum(c.isalpha() for c in lv) >= 3:
                        exact.setdefault(lv, os.path.basename(f))
                opts.append(alts if alts else [None])
            combos = [[]]
            for o in opts:
                combos = [c + [x] for c in combos for x in o]
                if len(combos) > 16:
                    break
            if len(combos) > 16:
                continue
            for combo in combos:
                pieces = []
                bad = False
                for x in combo:
                    if x is None:
                        if pieces and pieces[-1] is None:
                            bad = True
                            break
                        pieces.append(None)
                    elif pieces and isinstance(pieces[-1], str):
                        pieces[-1] += x
                    else:
                        pieces.append(x)
                if bad:
                    continue
                slots = sum(1 for p in pieces if p is None)
                if slots == 0:
                    v = ''.join(pieces)
                    if visible(v) and (not strict or ' ' in v.strip()) and not re.match(r'^[\s,.;:)(]', v):
                        exact.setdefault(v, os.path.basename(f))
                    continue
                if slots > 9:
                    continue
                n = 0
                out = ''
                for p in pieces:
                    if p is None:
                        n += 1
                        out += '{%d}' % n
                    else:
                        out += p
                letters = sum(1 for c in re.sub(r'\{\d\}', '', out) if c.isalpha())
                if letters >= 3 and visible(re.sub(r'\{\d\}', 'x', out)):
                    templ.setdefault(out, os.path.basename(f))
    return exact, templ

def main():
    ex1, tp1 = collect(UI, False)
    ex2, tp2 = collect(CORE, True)
    exact = dict(ex2); exact.update(ex1)
    templ = dict(tp2); templ.update(tp1)
    out = os.path.join(ROOT, 'tools/strings.txt')
    with open(out, 'w', encoding='utf-8') as fo:
        fo.write('# DDM: every text the program shows (made by tools/i18n_extract.py). {1}, {2}: changing parts.\n')
        for s in sorted(exact):
            fo.write(s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t') + '\n')
        for s in sorted(templ):
            fo.write(s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t') + '\n')
    print(len(exact), 'texts,', len(templ), 'sentences with changing parts ->', out)

if __name__ == '__main__':
    main()
