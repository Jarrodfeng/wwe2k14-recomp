"""Minimal unifdef for one macro (Session 95, retire NATIVE stage 4; no unifdef binary on this PC).

Usage: py unifdef_lite.py MACRO VALUE(0|1) file [file ...]   (rewrites in place)
Resolves only conditionals whose condition is exactly `MACRO`, `!MACRO`, `defined(MACRO)` or `!defined(MACRO)`
(also `#ifdef` / `#ifndef`); every other conditional is copied through, with nesting tracked.
Prints removed-line counts per file.
"""
import re
import sys

def classify(line, macro):
    m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$', line)
    if not m:
        return None
    kw, rest = m.group(1), m.group(2)
    rest = re.sub(r'//.*$', '', rest)
    rest = re.sub(r'/\*.*?\*/', '', rest).strip()
    if kw in ('else', 'endif'):
        return (kw, None)
    neg = False
    cond = None
    if kw == 'ifdef' and rest == macro:
        cond = True
    elif kw == 'ifndef' and rest == macro:
        cond = False
    elif kw in ('if', 'elif'):
        r = rest
        if r.startswith('!'):
            neg, r = True, r[1:].strip()
        if r == macro or r == 'defined(%s)' % macro or r == 'defined %s' % macro:
            cond = not neg
    return (kw, cond)

def process(path, macro, val):
    src = open(path, encoding='utf-8', newline='').read()
    lines = src.splitlines(keepends=True)
    out = []
    stack = []  # entries: dict(mine, keep_cur, taken, parent_keep)
    removed = 0
    def keeping():
        return all(e['keep'] for e in stack)
    for ln in lines:
        c = classify(ln, macro)
        if c is None:
            if keeping():
                out.append(ln)
            else:
                removed += 1
            continue
        kw, cond = c
        if kw in ('if', 'ifdef', 'ifndef'):
            if cond is not None:
                keep = (cond == bool(val))
                stack.append({'mine': True, 'keep': keep, 'taken': keep})
                removed += 1
            else:
                stack.append({'mine': False, 'keep': True})
                if keeping():
                    out.append(ln)
                else:
                    removed += 1
        elif kw == 'elif':
            e = stack[-1]
            if e['mine']:
                raise SystemExit('%s: #elif on a resolved conditional not supported' % path)
            if keeping():
                out.append(ln)
            else:
                removed += 1
        elif kw == 'else':
            e = stack[-1]
            if e['mine']:
                e['keep'] = not e['taken']
                removed += 1
            else:
                if keeping():
                    out.append(ln)
                else:
                    removed += 1
        elif kw == 'endif':
            e = stack.pop()
            if e['mine']:
                removed += 1
            else:
                if keeping():
                    out.append(ln)
                else:
                    removed += 1
    if stack:
        raise SystemExit('%s: unbalanced' % path)
    open(path, 'w', encoding='utf-8', newline='').write(''.join(out))
    return len(lines), removed

if __name__ == '__main__':
    macro, val = sys.argv[1], int(sys.argv[2])
    for p in sys.argv[3:]:
        total, rem = process(p, macro, val)
        print('%s: %d -> %d lines (%d removed)' % (p, total, total - rem, rem))
