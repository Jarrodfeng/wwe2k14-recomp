"""S110 PLANTONCE: compile a selftest unit ONCE and pick the planted fault at run time (design: docs/research/s110/PLANTONCE.md).

A plant is (name, header, regex pattern, replacement), applied as the scripts do: re.subn(pat, rep, text, count=1). The change span is found by
trimming the common prefix/suffix; when it lies strictly inside ONE function body, that function is emitted as
    if (::plant_once::Id() == i) { <body with plant i> } else if (...) { ... } else { <shipped body> }
and the unit is compiled once (force-including a generated plant_rt.h whose Id() reads env PLANT_ONCE_ID; 0 = shipped). Anything else (a
constant or declaration outside a body), or anything the combined compile refuses, is DEMOTED to the recompile path. One process per plant.

Entry (a library; no CLI):

    results, fallback = run_once(spec, verdict, recompile)

  spec      Spec (below). Fields: unit_cpp (the unit's .cpp), headers {name: path} (the shipped headers that carry plants), siblings
            {name: path} (headers copied beside the planted ones, quote-includes look there first), includes [dirs, after the temp inc dir],
            flags [the script's compile flags WITH its warning flags, e.g. -std=c++20 -Wall -D...; no -O], plants [Plant(name, header, pat, rep,
            literal=False)] (literal=True: rep is not a template, as letter_ui), run_args(tag)->[argv...] (default none; seats pass a work dir),
            tmp (scratch dir, default a fresh temp dir), log (callable(str), default silent), jobs (parallel processes/recompiles, default 4),
            clang (default: found like the scripts do), stats (OUT dict, filled by run_once: compiles, shipped_out, shipped_warn,
            shipped_pass, id0_pass, abandoned, rounds, demoted).
  verdict(rc, out) -> bool   True when the unit FAILED the way the script requires of a caught plant (e.g. rc != 0 and a FAIL line).
                             A run "passes" when rc == 0 and verdict is False.
  recompile(plant) -> (caught: bool, detail)   builds and runs ONE plant the old way (whole header text compiled alone) and returns the
                             script's verdict for it; detail is the script's own wording (e.g. 'did not compile', 'still PASSES').
  results   {name: (how, caught, detail)}; how = 'run-time' | 'recompile' | 'no-match'. detail: run-time = (rc, out); recompile = what the
            callback returned; no-match = 'the pattern matched N times'.
  fallback  names with how == 'recompile', in plant order.

Honesty rule: a plant is reported 'run-time' only when the combined exe CAUGHT it (verdict true) with a normal exit (not a crash code or a
timeout) and id 0 of the same exe passed. Any other outcome is re-run through `recompile` and THAT verdict is the report.

Where the combined exe could differ from the per-plant compile (S110 PLANTONCE review), the plant goes to the recompile path:
  - its alternative body does not close lexically or does not balance its braces (it would re-nest inside the if-chain);
  - it lies in a consteval function (the keyword must go, so a body that is no constant expression would compile);
  - the combined compile warns, at a line of its alternative, of an uninitialised read (stack garbage differs between the two builds) or of
    a declaration at the alternative's top level that shadows a local (a parameter redeclared in a body's outermost block is an error alone,
    a legal shadow inside an if-branch).
"""
import os
import re
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

RT_HEADER = '''#pragma once
#include <cstdlib>
namespace plant_once {
inline int Id() {
  static const int v = [] { const char* e = std::getenv("PLANT_ONCE_ID"); return e ? std::atoi(e) : 0; }();
  return v;
}
}
'''


@dataclass
class Plant:
    name: str
    header: str
    pat: str
    rep: str
    literal: bool = False


@dataclass
class Spec:
    unit_cpp: str
    headers: dict
    plants: list
    flags: list = field(default_factory=list)
    siblings: dict = field(default_factory=dict)
    includes: list = field(default_factory=list)
    run_args: object = None
    tmp: str = ''
    log: object = None
    jobs: int = 4
    clang: str = ''
    stats: dict = field(default_factory=dict)


# ---------------------------------------------------------------- the brace walk

def _blank(text):
    """Same length: comments, string/char literals and preprocessor lines become spaces (newlines kept)."""
    n = len(text)
    out = list(text)
    i = 0
    bol = True
    while i < n:
        c = text[i]
        if c == '\n':
            bol = True
            i += 1
            continue
        if bol and c in ' \t\r':
            i += 1
            continue
        if bol and c == '#':
            j = i
            while j < n and text[j] != '\n':
                if text[j] == '\\' and j + 1 < n and text[j + 1] in '\r\n':
                    j += 2 if text[j + 1] == '\n' or text[j + 1:j + 3] != '\r\n' else 3
                    continue
                j += 1
            for k in range(i, j):
                if out[k] not in '\r\n':
                    out[k] = ' '
            i = j
            continue
        bol = False
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                if out[k] != '\r':
                    out[k] = ' '
            i = j
            continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] not in '\r\n':
                    out[k] = ' '
            i = j
            continue
        if c == '"':
            m = re.compile(r'R"([^()\\ ]{0,16})\(').match(text, i - 1) if i > 0 and text[i - 1] == 'R' else None
            if m:
                end = text.find(')' + m.group(1) + '"', i)
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = i + 1
                while j < n and text[j] != '"' and text[j] != '\n':
                    j += 2 if text[j] == '\\' else 1
                j = min(j + 1, n)
            for k in range(i, j):
                if out[k] not in '\r\n':
                    out[k] = ' '
            i = j
            continue
        if c == "'":
            k = i
            while k > 0 and (text[k - 1].isalnum() or text[k - 1] in "_'."):
                k -= 1
            if k < i and text[k].isdigit() and i + 1 < n and text[i + 1].isalnum():
                i += 1  # a digit separator: inside a number (L'a' / u8'a' are char literals)
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] not in '\r\n':
                    out[k] = ' '
            i = j
            continue
        i += 1
    return ''.join(out)


def _match_close(b, o):
    d = 0
    for k in range(o, len(b)):
        ch = b[k]
        if ch == '{':
            d += 1
        elif ch == '}':
            d -= 1
            if d == 0:
                return k
    raise ValueError('unbalanced braces from offset %d' % o)


def _strip_templates(h):
    h = h.lstrip()
    while h.startswith('template'):
        m = re.match(r'template\s*<', h)
        if not m:
            break
        d, k = 1, m.end()
        while k < len(h) and d:
            d += (h[k] == '<') - (h[k] == '>')
            k += 1
        h = h[k:].lstrip()
    return h


_FUNC_TAIL = re.compile(r'\)\s*((const|volatile|noexcept|override|final|mutable|&&?)\s*)*$')
_KEYS = re.compile(r'\b(struct|class|union|enum)\b')


def _has_top_assign(h):
    h = re.sub(r'operator\s*(\(\s*\)|[^\s(]+)', ' ', h)
    d = 0
    for k, ch in enumerate(h):
        if ch in '([<{':
            d += 1 if ch in '([{' else 0
        elif ch in ')]}':
            d -= 1
        elif ch == '=' and d == 0:
            prev = h[k - 1] if k else ''
            nxt = h[k + 1] if k + 1 < len(h) else ''
            if nxt == '=' or prev in '=!<>':
                continue
            return True
    return False


def _scan(text):
    """[(open, close, head_start)] of the outermost function bodies, by the rules of the note (rule 3)."""
    b = _blank(text)
    n = len(b)
    bodies = []
    stack = []
    head = 0
    i = 0
    while i < n:
        ch = b[i]
        if ch == ';':
            head = i + 1
        elif ch == '}':
            if stack:
                stack.pop()
            head = i + 1
        elif ch == '{':
            h = _strip_templates(b[head:i]).rstrip()
            m = _KEYS.search(h)
            if re.search(r'\b(namespace|extern)\b', h) and '(' not in h:
                stack.append('ns')
                head = i + 1
            elif m and '(' not in h[:m.start()]:
                stack.append('class')
                head = i + 1
            elif _has_top_assign(h):
                i = _match_close(b, i)  # an initialiser: skip it, the head goes on
            elif _FUNC_TAIL.search(h) or re.search(r'\)[^;{}]*->[^;{}]*$', h) or (h.endswith('}') and ')' in h):
                c = _match_close(b, i)
                bodies.append((i, c, head))
                i = c
                head = c + 1
            else:
                i = _match_close(b, i)
        i += 1
    return bodies


def function_bodies(text):
    """[(open, close)] indices of the '{' and '}' of every outermost function body of a header text."""
    return [(o, c) for o, c, _ in _scan(text)]


# ---------------------------------------------------------------- the generator

@dataclass
class Combined:
    texts: dict          # header name -> combined text (headers without a body plant keep the shipped text)
    id_of: dict          # plant name -> id (1-based list position, stable across rounds)
    line_map: dict       # header name -> [(first_line, last_line, 'alt'|'ship', id | func key)]
    funcs: list          # [{'key', 'header', 'name', 'ids', 'lost_constexpr'}]
    fallback: list       # plant names that stay on the recompile path (outside a body, or skipped)
    nomatch: dict        # plant name -> number of matches (n != 1)


def _func_name(head):
    h = _strip_templates(head)
    for m in re.finditer(r'([~\w]+|operator\s*\S+?)\s*\(', h):
        if m.group(1) not in ('constexpr', 'consteval', 'noexcept', 'decltype', 'alignas', 'if', 'while', 'for', 'switch', 'throw'):
            return m.group(1)
    return '?'


def _closed_block(alt):
    """True when an alternative body text ends lexically closed (no open comment or raw string) and its braces balance without going below 0."""
    b = _blank(alt + '\n}')
    if not b.endswith('}'):
        return False
    d = 0
    for ch in b[:-1]:
        if ch == '{':
            d += 1
        elif ch == '}':
            d -= 1
            if d < 0:
                return False
    return d == 0


def combine(headers, plants, skip=()):
    """headers {name: shipped text}; plants [Plant]; skip: plant names forced to the fallback. Rules 1-6 of the note."""
    dup = sorted({p.name for p in plants if sum(q.name == p.name for q in plants) > 1})
    if dup:
        raise ValueError('plant names must be unique (results and run dirs are keyed by name): %s' % ', '.join(dup))
    id_of = {p.name: k + 1 for k, p in enumerate(plants)}
    nomatch, fallback = {}, []
    per_header = {}      # header -> {body index: [(id, alt text)]}
    scans = {name: _scan(text) for name, text in headers.items()}
    blanks = {name: _blank(text) for name, text in headers.items()}
    for p in plants:
        text = headers[p.header]
        if p.literal:
            new, n = re.subn(p.pat, lambda m, r=p.rep: r, text, count=1)
        else:
            new, n = re.subn(p.pat, p.rep, text, count=1)
        if n != 1:
            nomatch[p.name] = n
            continue
        if p.name in skip:
            fallback.append(p.name)
            continue
        pre = 0
        lim = min(len(text), len(new))
        while pre < lim and text[pre] == new[pre]:
            pre += 1
        suf = 0
        while suf < lim - pre and text[len(text) - 1 - suf] == new[len(new) - 1 - suf]:
            suf += 1
        a, e = pre, len(text) - suf
        delta = len(new) - len(text)
        hit = None
        for k, (o, c, _) in enumerate(scans[p.header]):
            if o < a and e <= c:
                hit = (k, o, c)
                break
        if hit is None:
            fallback.append(p.name)
            continue
        k, o, c = hit
        assert new[:o + 1] == text[:o + 1] and new[c + delta:] == text[c:], p.name
        alt = new[o + 1:c + delta]
        if not _closed_block(alt) or re.search(r'\bconsteval\b', blanks[p.header][scans[p.header][k][2]:o]):
            fallback.append(p.name)
            continue
        per_header.setdefault(p.header, {}).setdefault(k, []).append((id_of[p.name], alt))
    texts, line_map, funcs = {}, {}, []
    for name, text in headers.items():
        by_body = per_header.get(name)
        if not by_body:
            texts[name] = text
            continue
        b = _blank(text)
        edits = []   # (start, end, replacement, kind, payload)
        for k, alts in by_body.items():
            o, c, hs = scans[name][k]
            fkey = len(funcs)
            lost = False
            for m in re.finditer(r'\b(constexpr|consteval)\b', b[hs:o]):
                edits.append((hs + m.start(), hs + m.end(), '', 'kw', None))
                lost = True
            funcs.append({'key': fkey, 'header': name, 'name': _func_name(b[hs:o]), 'ids': [i for i, _ in alts], 'lost_constexpr': lost})
            edits.append((o + 1, c, None, 'body', (fkey, alts, text[o + 1:c])))
        edits.sort(key=lambda t: t[0])
        out, pos, line, lm = [], 0, 1, []
        for start, end, rep, kind, pay in edits:
            seg = text[pos:start]
            out.append(seg)
            line += seg.count('\n')
            if kind == 'body':
                fkey, alts, ship = pay
                for j, (i, alt) in enumerate(alts):
                    s = '\n%sif (::plant_once::Id() == %d) {\n' % ('} else ' if j else '', i)
                    out.append(s)
                    line += s.count('\n')
                    out.append(alt)
                    lm.append((line, line + alt.count('\n'), 'alt', i))
                    line += alt.count('\n')
                s = '\n} else {\n'
                out.append(s)
                line += s.count('\n')
                out.append(ship)
                lm.append((line, line + ship.count('\n'), 'ship', fkey))
                line += ship.count('\n')
                out.append('\n}\n')
                line += 2
            pos = end
        out.append(text[pos:])
        texts[name] = ''.join(out)
        line_map[name] = lm
    return Combined(texts, id_of, line_map, funcs, fallback, nomatch)


_ERR = re.compile(r'^(.+?):(\d+):(\d+): (fatal error|error): (.*)$')
_ANY = re.compile(r'^(.+?):(\d+):(\d+): (fatal error|error|warning|note): (.*)$')


def demote(stderr, comb, paths):
    """Rule 7. paths {header name: path as written}. Returns the set of plant ids to take off the combined exe (empty = no progress)."""
    where = {os.path.normcase(os.path.abspath(p)): n for n, p in paths.items()}
    ids, funcs = set(), set()
    blocks = []
    for ln in stderr.splitlines():
        if _ERR.match(ln):
            blocks.append([ln])
        elif blocks:
            blocks[-1].append(ln)
    unmapped = []
    for blk in blocks:
        m = _ERR.match(blk[0])
        hname = where.get(os.path.normcase(os.path.abspath(m.group(1))))
        line = int(m.group(2))
        hit = None
        for s, e, kind, key in comb.line_map.get(hname, ()):
            if s <= line <= e:
                hit = (kind, key)
                break
        if hit and hit[0] == 'alt':
            ids.add(hit[1])
        elif hit:
            funcs.add(hit[1])
        else:
            unmapped.append(blk)
    if unmapped:
        for f in comb.funcs:
            if f['lost_constexpr']:
                funcs.add(f['key'])
        names = set()
        for blk in unmapped:
            for q in re.findall(r"'([^']+)'", ' '.join(blk)):
                names.update(re.findall(r'[~\w]+', q))
        for f in comb.funcs:
            if f['name'] in names:
                funcs.add(f['key'])
    for f in comb.funcs:
        if f['key'] in funcs:
            ids.update(f['ids'])
    return ids


_WARN = re.compile(r'^(.+?):(\d+):(\d+): warning: (.*)$')


def warned(out, comb, paths):
    """Plant ids whose alternative draws, in a SUCCESSFUL combined compile, a warning that makes the run-time result build-dependent:
    an uninitialised read at any alt line, or a top-level declaration of the alt that shadows a local (the parameter-redeclaration case)."""
    where = {os.path.normcase(os.path.abspath(p)): n for n, p in paths.items()}
    ids = set()
    split = {}
    for ln in out.splitlines():
        m = _WARN.match(ln)
        if not m:
            continue
        msg = m.group(4)
        uninit = re.search(r'\[-W[\w-]*uninitialized[\w-]*\]', msg)
        shadow = 'shadows a local variable' in msg
        if not (uninit or shadow):
            continue
        hname = where.get(os.path.normcase(os.path.abspath(m.group(1))))
        line, col = int(m.group(2)), int(m.group(3))
        for s, e, kind, key in comb.line_map.get(hname, ()):
            if kind != 'alt' or not s <= line <= e:
                continue
            if uninit:
                ids.add(key)
            else:
                if hname not in split:
                    split[hname] = (comb.texts[hname].split('\n'), _blank(comb.texts[hname]).split('\n'))
                raw, bl = split[hname]
                upto = len(raw[line - 1].encode('utf-8')[:col - 1].decode('utf-8', 'ignore'))
                depth = sum(x.count('{') - x.count('}') for x in bl[s - 1:line - 1])
                depth += bl[line - 1][:upto].count('{') - bl[line - 1][:upto].count('}')
                if depth == 0:
                    ids.add(key)
            break
    return ids


# ---------------------------------------------------------------- the runner

def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def _read(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read()


def _write_inc(spec, inc, texts):
    os.makedirs(inc, exist_ok=True)
    for n, p in spec.siblings.items():
        if n not in texts:
            shutil.copy(p, os.path.join(inc, n))
    for n, t in texts.items():
        with open(os.path.join(inc, n), 'w', encoding='utf-8', newline='') as f:
            f.write(t)


def _compile(spec, clang, inc, extra, exe_name):
    exe = os.path.join(inc, exe_name + ('.exe' if os.name == 'nt' else ''))
    cmd = [clang] + list(spec.flags) + list(extra) + ['-I', inc] + [a for d in spec.includes for a in ('-I', d)] + [spec.unit_cpp, '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=300)
    ok = r.returncode == 0 and os.path.isfile(exe)
    return ok, exe, r.stdout + r.stderr


def _abnormal(rc):
    """A crash, not a return from main: a POSIX signal, a Windows exception code (0xC0000005 ...), or abort()'s 3 on Windows."""
    return rc < 0 or rc > 255 or (os.name == 'nt' and rc == 3)


def _run(spec, exe, tag, pid):
    """(rc, stdout, abnormal). abnormal: a crash code or a timeout; such a run never counts as a run-time catch."""
    env = dict(os.environ, PLANT_ONCE_ID=str(pid))
    args = list(spec.run_args(tag)) if spec.run_args else []
    try:
        r = subprocess.run([exe] + args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60, env=env)
    except subprocess.TimeoutExpired:
        return 1, 'FAIL timeout\n', True
    return r.returncode, r.stdout, _abnormal(r.returncode)


def run_once(spec, verdict, recompile):
    log = spec.log or (lambda s: None)
    st = spec.stats
    st.update(compiles=0, shipped_out='', shipped_rc=None, shipped_warn='', shipped_pass=False, id0_pass=False, abandoned=False, rounds=0, demoted=[])
    own_tmp = not spec.tmp
    tmp = spec.tmp or tempfile.mkdtemp(prefix='plant_once_')
    clang = spec.clang or find_clang()
    plants = list(spec.plants)
    by_name = {p.name: p for p in plants}
    headers = {n: _read(p) for n, p in spec.headers.items()}
    results = {}
    try:
        os.makedirs(tmp, exist_ok=True)
        rt = os.path.join(tmp, 'plant_rt.h')
        with open(rt, 'w', encoding='utf-8', newline='') as f:
            f.write(RT_HEADER)
        skip = set()
        comb = combine(headers, plants, skip)
        for name, n in comb.nomatch.items():
            results[name] = ('no-match', False, 'the pattern matched %d times' % n)
        ship_inc = os.path.join(tmp, 'shipped')
        _write_inc(spec, ship_inc, headers)

        def build_combined(rnd, cb):
            inc = os.path.join(tmp, 'once%d' % rnd)
            _write_inc(spec, inc, cb.texts)
            # the script's own warning flags stay (no -w: the combined compile maps diagnostics as the per-plant one does) plus -Wshadow,
            # read by warned(); warnings decide nothing else
            return inc, _compile(spec, clang, inc, ['-Wshadow', '-ferror-limit=0', '-include', rt], 'plant_once_unit')

        combined_ok, once_exe = False, None
        if comb.funcs:
            with ThreadPoolExecutor(2) as ex:
                fs = ex.submit(_compile, spec, clang, ship_inc, [], 'shipped_unit')
                fc = ex.submit(build_combined, 1, comb)
                sok, sexe, sout = fs.result()
                inc1, (cok, cexe, cout) = fc.result()
            st['compiles'] += 2
            rnd, inc, ok, exe, out = 1, inc1, cok, cexe, cout
        else:
            sok, sexe, sout = _compile(spec, clang, ship_inc, [], 'shipped_unit')
            st['compiles'] += 1
            ok = False
            rnd = 0
        if not sok:
            raise RuntimeError('the unit did not compile on the shipped headers: ' + sout[-1200:])
        st['shipped_warn'] = sout if 'warning' in sout else ''
        rc, o, _ = _run(spec, sexe, 'shipped', 0)
        st['shipped_out'], st['shipped_rc'] = o, rc
        st['shipped_pass'] = rc == 0 and not verdict(rc, o)
        while comb.funcs and not ok and rnd < 3:
            gone = demote(out, comb, {n: os.path.join(inc, n) for n in comb.texts})
            new_names = {p.name for p in plants if comb.id_of[p.name] in gone} - skip
            if not new_names:
                break
            skip |= new_names
            st['demoted'] += sorted(new_names)
            log('plant-once: trial compile %d refused; demoting %s' % (rnd, ', '.join(sorted(new_names))))
            comb = combine(headers, plants, skip)
            if not comb.funcs:
                break
            rnd += 1
            inc, (ok, exe, out) = build_combined(rnd, comb)
            st['compiles'] += 1
        st['rounds'] = rnd
        if not comb.funcs or not ok:
            if comb.funcs:
                st['abandoned'] = True
                log('plant-once: the combined unit still did not compile after %d rounds; every plant is recompiled' % rnd)
            run_ids = {}
        else:
            rc0, o0, _ = _run(spec, exe, 'once_id0', 0)
            st['id0_pass'] = rc0 == 0 and not verdict(rc0, o0)
            if not st['id0_pass']:
                log('plant-once: the combined exe does not PASS at id 0; every plant is recompiled')
            warn_ids = warned(out, comb, {n: os.path.join(inc, n) for n in comb.texts})
            warn_names = sorted(p.name for p in plants if comb.id_of[p.name] in warn_ids)
            if warn_names:
                st['demoted'] += warn_names
                log('plant-once: the combined compile warns in their alternative (uninitialised read / shadowed local); recompiled: %s'
                    % ', '.join(warn_names))
            run_ids = {p.name: comb.id_of[p.name] for p in plants
                       if st['id0_pass'] and p.name not in comb.fallback and p.name not in comb.nomatch and p.name not in warn_names}
        todo = []
        with ThreadPoolExecutor(max(1, spec.jobs)) as ex:
            futs = {n: ex.submit(_run, spec, exe, n, i) for n, i in run_ids.items()}
            for p in plants:
                if p.name in comb.nomatch:
                    continue
                if p.name in futs:
                    rc, o, abnormal = futs[p.name].result()
                    if verdict(rc, o) and not abnormal:
                        results[p.name] = ('run-time', True, (rc, o))
                        continue
                todo.append(p)
            rf = {p.name: ex.submit(recompile, p) for p in todo}
            for p in todo:
                caught, detail = rf[p.name].result()
                results[p.name] = ('recompile', bool(caught), detail)
        results = {p.name: results[p.name] for p in plants}
        fallback = [n for n, r in results.items() if r[0] == 'recompile']
        return results, fallback
    finally:
        if own_tmp:
            shutil.rmtree(tmp, ignore_errors=True)
