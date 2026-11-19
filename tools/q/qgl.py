"""qgl: text-level readers the AST index cannot give (S103 lane QGL). All read THIS checkout's own files (always current, no index, a worktree is
answered from its own tree).

    py tools/q/q.py ours glsl NAME            which of our .glsl / .fx.json shader sources read / write / declare / #define / #if-test NAME, with file:line and the
                                              #if guard each line sits under. NAME = a uniform (Sweat), an interpolator (inUvFog.w = only the lines that touch
                                              that component, a swizzle counts: .xyzw), a struct field (fc.Sweat.y -> Sweat.y) or a define (FX_WRINKLES)
    py tools/q/q.py ours fxreg [--dupes]      src/renderer/shaders/spv/fx_registry.h: counts; --dupes = hashes that map to more than one variant, grouped by
                                              owner (the shader family before the first `_`), cross-owner ones flagged; always: twins (X_fragment / X_pl_fragment)
                                              where only one side has a constants table, and variants no hash row names
    py tools/q/q.py ours str LITERAL          where our code uses a string literal containing LITERAL ("g_fWklLv%d", "g_mNormalWklPrm"): file:line, the whole
                                              literal, the enclosing function. A printf-style LITERAL also matches by its text before the first %-spec and
                                              by the literal split in two ("g_fWklLv" "%d"); --tools adds scripts / json / toml text
Selftest: py -I tools/q/qgl_selftest.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qtext  # noqa: E402

GLSL_EXT = ('.glsl', '.vert', '.frag', '.fx.json', '.hlsl')
REG_REL = 'src/renderer/shaders/spv/fx_registry.h'


class GError(Exception):
    pass


def _read(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read()


# ------------------------------------------------------------------ glsl

SW = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3, 's': 0, 't': 1, 'p': 2, 'q': 3}


def _split_name(name):
    """'fc.Sweat.y' -> ('Sweat', 'y'); 'inUvFog.w' -> ('inUvFog', 'w'); 'FX_WRINKLES' -> ('FX_WRINKLES', None)"""
    parts = [p for p in re.split(r'\.', name.strip()) if p]
    if not parts:
        raise GError('glsl: give a name: a uniform, interpolator, define or struct field (Sweat, inUvFog.w, FX_WRINKLES)')
    if len(parts) == 1:
        return parts[0], None
    return parts[-2], parts[-1]


def find_hits(code, base, member):
    """-> [(start, end, whole)] of the hits of `base` (optionally `.member`) on one comment-free code line. member None: every use of the word. A swizzle member (w, xy, rgb):
    only uses of those components, plus WHOLE-vector uses (`inUvFog = ..`, `f(inUvFog)`: whole=True, they touch every component); a use of another component is skipped. A
    struct-field member: only `base.member` (an index between is fine). `end` is the end of the access (base, index, swizzle / field)."""
    out = []
    b = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(base) + r'(?![A-Za-z0-9_])')
    acc = re.compile(r'\s*(?:\[[^\]]*\])?\s*(?:\.\s*([A-Za-z_]\w*))?')
    swz = re.compile(r'[xyzwrgbastpq]{1,4}')
    comps = {SW[c] for c in member} if member and swz.fullmatch(member) else None
    for m in b.finditer(code):
        am = acc.match(code, m.end())
        fld = am.group(1)
        end = am.end() if (fld is not None or am.group(0).strip()) else m.end()
        if member is None:
            out.append((m.start(), end, False))
        elif comps is not None:
            if fld is None:
                out.append((m.start(), end, True))
            elif swz.fullmatch(fld):
                if comps & {SW[c] for c in fld}:
                    out.append((m.start(), end, False))
        elif fld == member:
            out.append((m.start(), end, False))
    return out


INC_GUARD = re.compile(r'!\s*defined\s*\(?\s*\w+_GLSL\s*\)?|!defined\(\w+_GLSL\)|!defined\(\w+_H\)')   # a file's own include guard is not a feature guard
BSL = chr(92)
COND_START = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b\s*(.*)$')
COND_ELIF = re.compile(r'^\s*#\s*elif\b\s*(.*)$')
COND_ELSE = re.compile(r'^\s*#\s*else\b')
COND_END = re.compile(r'^\s*#\s*endif\b')
DEFINE = re.compile(r'^\s*#\s*(define|undef)\s+([A-Za-z_]\w*)')
ASSIGN = re.compile(r'(?<![=!<>+\-*/%&|^])(?:[-+*/%&|^]|<<|>>)?=(?!=)')
DECL = re.compile(r'^\s*(?:layout\s*\([^)]*\)\s*)?(?:(?:uniform|const|in|out|flat|highp|mediump|lowp|varying|attribute|buffer|shared)\s+)*'
                  r'(?:float|u?int|bool|[iub]?vec[234]|mat[234](?:x[234])?|sampler\w*|[A-Za-z_]\w*)\s+(?:[A-Za-z_]\w*\s+)*(\w+)\s*(?:\[[^\]]*\])?\s*(?:[;=,]|$)')


def _strip_comment(ln, in_block):
    """-> (code, in_block): // and /* */ comments removed (a comment-only line is a blank)"""
    out, i = [], 0
    while i < len(ln):
        if in_block:
            j = ln.find('*/', i)
            if j < 0:
                return ''.join(out), True
            i, in_block = j + 2, False
            continue
        a, b = ln.find('//', i), ln.find('/*', i)
        if a >= 0 and (b < 0 or a < b):
            out.append(ln[i:a])
            return ''.join(out), False
        if b >= 0:
            out.append(ln[i:b])
            i, in_block = b + 2, True
            continue
        out.append(ln[i:])
        break
    return ''.join(out), in_block


def codes_of(lines):
    """the lines with // and /* */ comments removed (block state carried across lines): a directive inside a block comment is not a directive"""
    out, blk = [], False
    for raw in lines:
        code, blk = _strip_comment(raw, blk)
        out.append(code)
    return out


def guards_of(codes):
    """-> per line, the active #if guard stack as one readable string ('' = none). `codes` are comment-stripped lines (codes_of). #else = the negation of every earlier test
    of its chain; #elif B = !(A) && B (the earlier tests negated, then its own); a directive continued over lines with a backslash is read whole; a file's own
    include guard is dropped."""
    res, stack = [], []          # entry: {'cur': tests of the active branch, 'prior': positive tests of the earlier branches, 'pos': this branch's positive test}
    i, n = 0, len(codes)
    while i < n:
        ln = codes[i]
        span = 1
        if ln.lstrip().startswith('#') and ln.rstrip().endswith(BSL):     # a continued directive: join its lines
            full = ln.rstrip()[:-1]
            while i + span < n and codes[i + span - 1].rstrip().endswith(BSL):
                full += ' ' + codes[i + span].rstrip().rstrip(BSL)
                span += 1
            ln = full
        m = COND_START.match(ln)
        if m:
            kind, expr = m.group(1), ' '.join(m.group(2).split())
            t = expr if kind == 'if' else (f'defined({expr})' if kind == 'ifdef' else f'!defined({expr})')
            stack.append({'cur': [t], 'prior': [], 'pos': t})
            tag = None
        elif COND_ELIF.match(ln) and stack:
            top = stack[-1]
            expr = ' '.join(COND_ELIF.match(ln).group(1).split())
            top['prior'].append(top['pos'])
            top['pos'] = expr
            top['cur'] = ['!(' + ' || '.join(top['prior']) + ')', expr]
            tag = None
        elif COND_ELSE.match(ln) and stack:
            top = stack[-1]
            top['prior'].append(top['pos'])
            top['cur'] = ['!(' + ' || '.join(top['prior']) + ')']
            tag = None
        elif COND_END.match(ln) and stack:
            stack.pop()
            tag = None
        else:
            tag = ''
        cur = ' && '.join(t for e in stack for t in e['cur'] if not INC_GUARD.fullmatch(t))
        res.append(None if tag is None else cur)
        for _ in range(span - 1):          # the continuation lines of a directive
            res.append(None)
        i += span
    return res


def _sources(root, tops=('src', 'tools', 'assets')):
    out = []
    for top in tops:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in qtext.SKIP and not d.startswith('.') and d != 'spv']
            for f in fn:
                if f.endswith(GLSL_EXT):
                    out.append(os.path.join(dp, f))
    return sorted(out)


WRITE_AFTER = re.compile(r'^\s*(?:\[[^\]]*\]\s*)?(?:\.\s*[A-Za-z_]\w*\s*)*(?:(?:[-+*/%&|^]|<<|>>)=|=(?!=))')


def _kind(code, m_start, m_end, base, member, whole=False):
    """classify one hit on one code line: define / undef / guard (a #if / #ifdef test) / decl / write / read. A WRITE = the statement (cut at the first ';' after the hit)
    continues after the hit with nothing but whitespace, an index or a swizzle / field before an assignment operator (=, +=, <<= ...; never ==), or the hit is the operand of ++ / --."""
    d = DEFINE.match(code)
    if d and d.group(2) == base:
        return d.group(1)
    if re.match(r'^\s*#\s*(?:el)?if', code) and re.search(r'(?<![A-Za-z0-9_])' + re.escape(base) + r'(?![A-Za-z0-9_])', code):
        return 'guard'
    if code.lstrip().startswith('#'):
        return 'preproc'
    dm = DECL.match(code)
    if dm and dm.group(1) == base and (member is None or whole) and not re.search(r'[=]\s*[^=]', re.sub(r'layout\s*\([^)]*\)', '', code[:m_start])):
        return 'decl'
    semi = code.find(';', m_end)
    seg = code[m_end:semi if semi >= 0 else len(code)]
    if WRITE_AFTER.match(seg):
        return 'write'
    if re.search(r'(\+\+|--)\s*$', code[:m_start]) or re.match(r'\s*(\+\+|--)', seg):
        return 'write'
    return 'read'


def cmd_glsl(root, name, limit=400):
    base, member = _split_name(name)
    hits, scanned = [], 0
    for p in _sources(root):
        text = _read(p)
        if base not in text:
            continue
        scanned += 1
        rel = qtext.rel_of(root, p)
        lines = text.split('\n')
        codes = codes_of(lines)
        gs = guards_of(codes)
        for i, code in enumerate(codes):
            if not code.strip():
                continue
            best = None
            for st, en, whole in find_hits(code, base, member):
                kind = _kind(code, st, en, base, member, whole)
                if best is None or (kind == 'write' and best[0] != 'write'):
                    best = (kind, whole)
            if best:
                hits.append({'file': rel, 'line': i + 1, 'kind': best[0], 'whole': best[1], 'guard': gs[i] or '', 'text': raw_line(lines, i)})
    hits.sort(key=lambda h: (h['file'], h['line']))
    kinds = {}
    for h in hits:
        kinds[h['kind']] = kinds.get(h['kind'], 0) + 1
    return {'name': name, 'base': base, 'member': member, 'files_scanned': scanned, 'n': len(hits), 'kinds': kinds, 'hits': hits}


def raw_line(lines, i):
    return lines[i].strip()[:150]


def print_glsl(r, args):
    lim = getattr(args, 'limit', 30)
    ks = ', '.join(f'{v} {k}' for k, v in sorted(r['kinds'].items(), key=lambda kv: -kv[1]))
    print(f'glsl {r["name"]}: {r["n"]} lines in {len({h["file"] for h in r["hits"]})} shader sources ({ks or "none"})')
    if not r['n']:
        print('  no shader source names it (the .glsl files under src / tools / assets; generated SPIR-V headers are not read)')
        return
    by = {}
    for h in r['hits']:
        by.setdefault(h['file'], []).append(h)
    shown = 0
    for f, hs in by.items():
        print(f'  {f}  ({len(hs)})')
        for h in (hs if getattr(args, 'all', False) else hs[:12]):
            print(f'    {h["line"]:>5} [{h["kind"]:<6}] {h["text"][:100]}' + ('   (whole vector)' if h.get('whole') else '') + (f'    #if {h["guard"]}' if h['guard'] else ''))
        if not getattr(args, 'all', False) and len(hs) > 12:
            print(f'    ... {len(hs) - 12} more lines (--all)')
        shown += 1
        if shown >= lim:
            print(f'  ... {len(by) - shown} more files (--limit)')
            break


# ------------------------------------------------------------------ fxreg

VAR_RX = re.compile(r'^  \{"(\w+)",(.*)\},?\s*$')
HASH_RX = re.compile(r'^  \{0x([0-9A-Fa-f]+)ull, (\d+)\}')


def parse_registry(path):
    """-> {'variants': [{'name','owner','constants','const_count','samplers'}], 'hashes': [(hash, variant_index)]}"""
    text = _read(path)
    vs, hs = [], []
    sect = None
    for ln in text.split('\n'):
        if ln.startswith('inline constexpr Variant kVariants'):
            sect = 'v'
            continue
        if ln.startswith('inline constexpr HashEntry kHashes'):
            sect = 'h'
            continue
        if ln.startswith('};'):
            sect = None
            continue
        if sect == 'v':
            m = VAR_RX.match(ln)
            if m:
                f = [x.strip() for x in m.group(2).split(',')]
                # fields after the name: spirv, sizeof(..), spirv_alpha, bytes_alpha, constants, constant_count, block_bytes, bools_offset, attrs, attr_count,
                # samplers, sampler_count, out_alpha, vc_fetch   (the first field is empty: the name is followed by a comma)
                f = [x for x in f]
                if f and f[0] == '':
                    f = f[1:]
                consts = f[4] if len(f) > 5 else ''
                cc = f[5] if len(f) > 5 else '0'
                vs.append({'name': m.group(1), 'owner': m.group(1).split('_')[0], 'constants': consts, 'const_count': int(cc) if cc.isdigit() else -1})
        elif sect == 'h':
            m = HASH_RX.match(ln)
            if m:
                hs.append((int(m.group(1), 16), int(m.group(2))))
    return {'variants': vs, 'hashes': hs}


def twin_name(n):
    """X_fragment <-> X_pl_fragment (the analytic-lighting twin); -> the other name, or None"""
    if n.endswith('_pl_fragment'):
        return n[:-len('_pl_fragment')] + '_fragment'
    if n.endswith('_fragment'):
        return n[:-len('_fragment')] + '_pl_fragment'
    return None


def cmd_fxreg(root, dupes=False, path=None):
    p = path or os.path.join(root, REG_REL)
    if not os.path.isfile(p):
        raise GError(f'fxreg: no {REG_REL} in this checkout ({p})')
    reg = parse_registry(p)
    vs, hs = reg['variants'], reg['hashes']
    if not vs or not hs:
        raise GError(f'fxreg: could not read variants / hashes out of {p} (format changed? tools/q/qgl.py parse_registry)')
    byname = {v['name']: i for i, v in enumerate(vs)}
    by_hash = {}
    for h, vi in hs:
        by_hash.setdefault(h, []).append(vi)
    dup = []
    for h, ix0 in sorted(by_hash.items()):
        if len(ix0) < 2:
            continue
        ix = sorted(set(ix0))
        owners = sorted({vs[i]['owner'] for i in ix})
        dup.append({'hash': f'0x{h:016X}', 'variants': [vs[i]['name'] for i in ix], 'owners': owners, 'cross_owner': len(owners) > 1,
                    'same_variant_twice': len(ix) != len(ix0)})
    groups = {}
    for d in dup:
        groups.setdefault(','.join(d['owners']), []).append(d)
    named = {vi for _, vi in hs}
    unhashed = [vs[i]['name'] for i in range(len(vs)) if i not in named]
    notable, pairs = [], 0
    for d in dup:     # hash twins: variants sharing one hash; a member without a constants table next to members that have one
        ms = [vs[byname[n]] for n in d['variants']]
        if any(m['const_count'] > 0 for m in ms) and any(m['const_count'] <= 0 for m in ms):
            notable.append({'kind': 'hash twins', 'hash': d['hash'], 'members': [(m['name'], m['const_count']) for m in ms],
                            'missing_table_on': [m['name'] for m in ms if m['const_count'] <= 0]})
    for v in vs:
        t = twin_name(v['name'])
        if t is None or v['name'].endswith('_pl_fragment') or t not in byname:
            continue
        pairs += 1
        a, b = v, vs[byname[t]]
        if (a['const_count'] > 0) != (b['const_count'] > 0):
            notable.append({'kind': '_pl twins', 'twin_a': a['name'], 'a_constants': a['const_count'], 'twin_b': b['name'], 'b_constants': b['const_count'],
                            'missing_table_on': (a if a['const_count'] <= 0 else b)['name']})
    return {'path': os.path.relpath(p, root).replace('\\', '/') if root else p, 'variants': len(vs), 'hash_rows': len(hs), 'distinct_hashes': len(by_hash),
            'dup_hashes': len(dup), 'cross_owner': sum(1 for d in dup if d['cross_owner']), 'groups': groups, 'show_dupes': dupes,
            'unhashed_variants': unhashed, 'twin_pairs': pairs, 'twins_without_table': notable}


def print_fxreg(r, args):
    lim = getattr(args, 'limit', 30)
    print(f'fxreg {r["path"]}: {r["variants"]} variants, {r["hash_rows"]} hash rows, {r["distinct_hashes"]} distinct hashes; '
          f'{r["dup_hashes"]} duplicate hashes ({r["cross_owner"]} across owners), {r["twin_pairs"]} _fragment / _pl_fragment twin pairs, '
          f'{len(r["twins_without_table"])} twin groups with a constants table on one side only, {len(r["unhashed_variants"])} variants no hash row names')
    if r['show_dupes']:
        for own, ds in sorted(r['groups'].items(), key=lambda kv: (',' not in kv[0], kv[0])):
            tag = 'CROSS-OWNER' if ',' in own else 'same owner'
            print(f'  owner {own}  ({len(ds)} hashes, {tag})')
            for d in ds[:lim]:
                print(f'    {d["hash"]}  ->  {", ".join(d["variants"])}' + ('  [the same variant listed twice]' if d['same_variant_twice'] else ''))
            if len(ds) > lim:
                print(f'    ... {len(ds) - lim} more (--limit)')
    else:
        print('  (--dupes lists the duplicate hashes by owner)')
    if r['twins_without_table']:
        print('  twins without a table (one side has a constants table, the other none):')
        for t in r['twins_without_table'][:lim]:
            if t['kind'] == 'hash twins':
                print(f'    hash twins {t["hash"]}: ' + ', '.join(f'{n} [{c}]' for n, c in t['members']) + f'   -> no table: {", ".join(t["missing_table_on"])}')
            else:
                print(f'    _pl twins {t["twin_a"]} [{t["a_constants"]} constants]  /  {t["twin_b"]} [{t["b_constants"]}]   -> no table: {t["missing_table_on"]}')
        if len(r['twins_without_table']) > lim:
            print(f'    ... {len(r["twins_without_table"]) - lim} more (--limit)')
    if r['unhashed_variants'] and r['show_dupes']:
        u = r['unhashed_variants']
        print(f'  variants no hash row names ({len(u)}): ' + ', '.join(u[:lim]) + (' ...' if len(u) > lim else ''))


# ------------------------------------------------------------------ str

SPEC_RX = re.compile(r'%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|I64|I)?[diuxXfFgGeEsScp%]')


def _unesc(s):
    return s.replace('\\"', '"').replace('\\\\', '\\')


def cmd_str(root, lit, encl, tools=False, cap=2000):
    lit = lit.strip()
    if len(lit) >= 2 and lit[0] == lit[-1] == '"':
        lit = lit[1:-1]
    if not lit:
        raise GError('str: give the text of a string literal ("g_fWklLv%d", g_mNormalWklPrm)')
    want = [('exact', lit)]
    pre = SPEC_RX.split(lit)[0]
    if pre != lit and len(pre) >= 3:
        want.append(('prefix', pre))        # "g_fWklLv%d" also matches code that formats it from the text before the %d
    hits = []
    exts = qtext.CPP + (qtext.TOOL_EXTS if tools else ())
    tops = ('src', 'probes') + (('tools', 'android', 'config') if tools else ())
    exts = exts if tools else qtext.CPP
    seen_files = 0
    for p in qtext.walk(root, exts, tops):
        try:
            text = _read(p)
        except OSError:
            continue
        if not any(w[1] in text for w in want):
            continue
        seen_files += 1
        rel = qtext.rel_of(root, p)
        is_cpp = rel.endswith(qtext.CPP)
        for i, ln in enumerate(text.split('\n'), 1):
            if not any(w[1] in ln for w in want):
                continue
            if is_cpp:
                ln = qtext.strip_comment(ln)      # a literal named only in a comment is not a use
                if not any(w[1] in ln for w in want):
                    continue
                lits = [_unesc(m.group(1)) for m in qtext.LIT_RX.finditer(ln)]
                # adjacent literals ("g_fWklLv" "%d") are one literal to the compiler
                joined = [_unesc(''.join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', run.group(0)))) for run in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*){2,}', ln)]
                cand = lits + joined
                how = None
                for kind, w in want:
                    for c in cand:
                        if w in c:
                            how = 'literal' if (kind == 'exact' and c == w) else ('literal contains it' if kind == 'exact' else 'prefix of a format')
                            break
                    if how:
                        break
                if not how:
                    continue
                hits.append({'file': rel, 'line': i, 'how': how, 'func': encl(rel, i) if encl else '', 'text': ln.strip()[:170]})
            else:
                hits.append({'file': rel, 'line': i, 'how': 'script text', 'func': '', 'text': ln.strip()[:170]})
            if len(hits) >= cap:
                break
        if len(hits) >= cap:      # a GLOBAL cap (the old check only left the line loop of one file)
            break
    hits.sort(key=lambda h: (h['how'] == 'prefix of a format', h['file'], h['line']))
    return {'literal': lit, 'forms': [f'{k}: {w}' for k, w in want], 'n': len(hits), 'files': len({h['file'] for h in hits}), 'hits': hits,
            'truncated': len(hits) >= cap}


def print_str(r, args):
    lim = getattr(args, 'limit', 30)
    print(f'str "{r["literal"]}": {r["n"]} string-literal sites in {r["files"]} files  ({"; ".join(r["forms"])})')
    if not r['n']:
        print('  not used as a string literal in src / probes (--tools adds tools / android / config text). `q ours lit` searches any text.')
        return
    for h in r['hits'][:lim * 4]:
        print(f'  {h["file"]}:{h["line"]}  <{h["how"]}>' + (f'  [in {h["func"]}]' if h['func'] else '') + f'\n      {h["text"]}')
    if r['n'] > lim * 4:
        print(f'  ... {r["n"] - lim * 4} more (--limit)')
    if r.get('truncated'):
        print(f'  (stopped at {r["n"]} sites: narrow the literal)')
