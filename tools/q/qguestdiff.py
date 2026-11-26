r"""q guestdiff: every Material field the STOCK loader fills against what the GUEST (created superstar / CAW / edit-part) build fills (S111 GUESTDIFF).

    py tools/q/q.py guestdiff [--field GLOB] [--verdict V ...] [--all] [--json] [--ref REF]

One row per assets::Material field (src/assets/asset_model.h, with its default) that either path fills:
  stock  - src/assets/jboy.h LoadJboy, from `Material mat;` to `m.materials.push_back(mat);`: every write to `mat.F` (`mat.F = ..`,
           `mat.F[i] = ..`, `slot = &mat.F`, an arm `pname == "X" ? mat.F` of the vec4 chain) with the parameter NAMES that select it
           (`pname == "X"`, `pname.rfind("X", 0) == 0` = X*) on its own line, else on the block that holds it, else through the field
           the line tests (`slot == &mat.G` / `dst == mat.G`: G's names); a write no name selects is DERIVED (the record word / the
           effect name). A write only an arena / prop load reaches (a positive `o.arena` / `o.prop_slot` / `skin_streams` on the line
           or its blocks) is ARENA; one behind a positive `...Plant()` is a plant (not a fill).
  guest  - src/renderer_scene/guest_model_draw.h BuildModel, from `assets::Material mat;` to `out.materials.push_back(mat);`: the
           same writes; a write takes the string literals of its line /
           block (a by-name parameter read); a line or block naming `TexSlot::X` (a case arm, an `if (slot == TexSlot::X)` block) takes
           SlotOfParam's names for X. A write behind `flat_diffuseless` or `if (g_draw_plant == N)` is a debug plant (not a fill).
  calls  - on BOTH paths, a free call that hands `mat` itself on (`F(.., mat, ..)`, also behind an `if (` or inside another call) is
           followed into its definition in src that takes a writable `Material&` / `assets::Material&` (a const one is named, not
           followed), and that body's own such calls too (up to MAX_FOLLOW levels: S111 GUESTHAIR's ONE float table
           src/assets/material_floats.h ApplyFloat is reached from LoadJboy directly and from BuildModel through ApplyRows); a followed
           write takes the string literals of its line / block (`is("X")`; `NameStarts(n, "X", ..)` = X*) and the call line's arena /
           plant condition. The guest reader (src/scene_live/guest_model_reader.h) is read for its sampler loop's kind filter.
  read   - ResolveChar (src/renderer_scene/char_effects.cpp): each `if (.. name == "X" ..)` statement's `mt.F` -> constant X, whose
           effects come from the fx registry (src/renderer/shaders/fx/*.fx.json); the character bind switch (front_end.cpp, through
           qtexdiff.load_ours: role -> `tex(mat.F)` / `cube_of(mat.F)`, roles -> slots) -> the effects whose pixel stage samples that
           slot. Only the effects a guest part can be drawn with count: PipelineEffect's kFx list that some CAW record uses.
           A field neither reads is searched in src as text (`.F` not written; [L]).
  CAW    - the game's created-superstar part records: the asset index's `material` rows of the edit pacs (pac/edit/edNN.pac, the DLC
           edNN_dlcMM.pac): per (effect, name) how many records carry the name and how many with a value other than the guest default
           (a sampler: any texture name).
  NOREAD - a CAW name no path reads into a field is printed with its reason when tools/q/qguestdiff_noread.py has one (S111
           UNREADNAMES): the line of bare names lists only the rest (a new unread name); --all prints every reason.
Verdicts (rows printed in this order; ARENA / BOTH only with --all):
  LOOK       stock fills it, guest keeps the default, a guest effect's constant / texture slot reads it, and CAW records of that effect
             carry the name with a value other than the default: the guest part draws differently from the stock one;
  PARTIAL    both fill it, but CAW records carry (non-default) a name only the stock loader reads into it;
  READ       as LOOK, but read only outside ResolveChar / the bind switch (text search, [L]);
  DERIVED    stock fills it with no parameter name (record word / effect rule), guest does not, and something reads it;
  SAME       a LOOK candidate, but every CAW record carries the default value: invisible;
  NOREC      a LOOK candidate no CAW record names: not a hole (the game has no value either);
  UNREAD     stock-only, and nothing reads it for a guest effect;
  GUEST-ONLY guest fills it, the stock loader never does (other src writers listed);
  BOTH       both fill it, every CAW-carried name covered;
  ARENA      only an arena / prop load fills it (never a character part).
Exit 2 on a PROBLEM (a block or function not found, a merge marker). Notation and limits: docs/research/s111/GUESTDIFF.md.
"""
import argparse
import atexit
import fnmatch
import io
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402  (masking, bracket matching, the fx constant registry)
import qtexdiff as T      # noqa: E402  (the character bind switch, the role functions, the fx sampler registry)
import qguestdiff_noread as NR   # noqa: E402  (S111 UNREADNAMES: the reason each unread CAW name needs no field)

REPO = CD.REPO
JBOY = 'src/assets/jboy.h'
MODEL = 'src/assets/asset_model.h'
GUEST = 'src/renderer_scene/guest_model_draw.h'
READER = 'src/scene_live/guest_model_reader.h'
CHAR = T.CHAR
CAW_PAC = re.compile(r'(?:^|/)pac/edit/ed\d+(?:_dlc\d+)?\.pac$')
VERDICTS = ('LOOK', 'PARTIAL', 'READ', 'DERIVED', 'SAME', 'NOREC', 'UNREAD', 'GUEST-ONLY', 'BOTH', 'ARENA')
SHOWN = VERDICTS[:-2]
EMPTY_TEX = ('', '""')
# a DERIVED stock fill that reads the mesh record word `r.u32(rec + 0xNN)`: the asset index column holding that word per record
RECORD_WORDS = {0x90: 'technique_word', 0x8C: 'cull_word'}


class GuestDiffError(Exception):
    pass


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


# ------------------------------------------------------------------ Material: fields and defaults
def _split_top(s, sep):
    out, d, cur = [], 0, []
    for ch in s:
        if ch in '{([':
            d += 1
        elif ch in '})]':
            d -= 1
        if ch == sep and d == 0:
            out.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append(''.join(cur))
    return out


def _num(t):
    t = t.strip()
    if t in ('true', 'false'):
        return 1.0 if t == 'true' else 0.0
    t = re.sub(r'(?<=[\d.])[fFuU]$', '', t)
    try:
        return float(t)
    except ValueError:
        return None


def parse_defaults(text):
    """-> {field: {'type', 'n', 'default' (text), 'vals' (floats or None), 'line'}} from `struct Material { ... };`."""
    ms = CD._mask(text)
    m = re.search(r'^struct Material\s*\{', ms, re.M)
    if not m:
        raise GuestDiffError(f'{MODEL}: no `struct Material {{`')
    ob = ms.index('{', m.start())
    cb = CD._match(ms, ob, '{', '}')
    out, pos = {}, ob + 1
    for stmt in _split_top(ms[ob + 1:cb - 1], ';'):
        start = pos
        pos += len(stmt) + 1
        s = stmt.strip()
        if not s:
            continue
        md = re.match(r'((?:(?:const|static|mutable)\s+)*[\w:]+(?:<[^>]*>)?)\s+(.+)$', s, re.S)
        if not md:
            continue
        typ = md.group(1)
        for dcl in _split_top(md.group(2), ','):
            mm = re.match(r'\s*(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*(?:=\s*(.+))?$', dcl, re.S)
            if not mm:
                continue
            init = ' '.join((mm.group(3) or '').split())
            n = int(mm.group(2)) if mm.group(2) else 1
            if init.startswith('{'):
                vals = [_num(x) for x in init.strip('{}').split(',')] if init.strip('{} ') else [0.0] * n
            elif init:
                vals = [_num(init)]
            else:
                vals = None if 'string' in typ else [0.0] * n
            if vals is not None and any(v is None for v in vals):
                vals = None
            off = start + stmt.find(mm.group(1))
            out[mm.group(1)] = {'type': typ, 'n': n, 'default': init or ('""' if 'string' in typ else '{}'), 'vals': vals,
                                'line': line_of(text, off)}
    return out


# ------------------------------------------------------------------ the block walker (both paths)
REF_RE = r'(?:\bdst|\bslot)\s*==\s*&?\s*{r}\.(\w+)'
POS_ARENA = re.compile(r'(?<![!\w.>])(?:o\.(?:arena|prop_slot)|skin_streams)\b')
POS_PLANT = re.compile(r'(?<![!\w:])(?:\w+::)*\w*Plant\w*\(\)')
GUEST_PLANT = (re.compile(r'\bflat_diffuseless\b'), re.compile(r'^\s*(?:\}\s*)?(?:else\s+)?if\s*\(\s*g_draw_plant\s*==\s*\d+\s*\)'))


def jboy_names(ln):
    return re.findall(r'pname\s*==\s*"(\w+)"', ln) + [n + '*' for n in re.findall(r'pname\.rfind\(\s*"(\w+)"\s*,\s*0\s*\)\s*==\s*0', ln)]


def literal_names(ln):
    return re.findall(r'"((?:g_|samp|tex)\w+)"', ln)


def fn_names(ln):
    """the names a line of a followed body selects: its string literals (`is("X")`), a prefix test (`NameStarts(n, "X", ..)`,
    `n.rfind("X", 0) == 0`) as X*."""
    pre = re.findall(r'\bNameStarts\s*\(\s*\w+\s*,\s*"(\w+)"', ln) + re.findall(r'\.rfind\(\s*"(\w+)"\s*,\s*0\s*\)\s*==\s*0', ln)
    return [n for n in literal_names(ln) if n not in pre] + [n + '*' for n in pre]


CALL_SKIP = ('if', 'for', 'while', 'switch', 'return', 'sizeof', 'catch', 'decltype')
MAX_FOLLOW = 4   # call levels followed (BuildModel -> guestfloats::ApplyRows -> matfloat::ApplyFloat is 2)


def _top_level(args):
    """a call's argument text up to its closing parenthesis, every nested (...) blanked: what the call itself is handed."""
    out, depth = [], 0
    for ch in args:
        if ch == '(':
            depth += 1
        elif ch == ')':
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            out.append(ch)
            continue
        out.append(' ')
    return ''.join(out)


def _closing(s, i):
    """the index of the `)` that closes the `(` just before s[i] (len(s) when the line ends first)."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            if depth == 0:
                return j
            depth -= 1
    return len(s)


def _if_conds(ln, a, b):
    """a call line's own conditions: each `if (...)` on the line, as `if (cond)`, with the call's own arguments (ln[a:b]) blanked.
    S111 UNIFY2 review: an argument is not a condition (LoadJboy hands `o.arena` to the sampler table; the table's own
    `!c.arena` tests are what gate its fills)."""
    s = ln[:a] + ' ' * (b - a) + ln[b:]
    out = []
    for m in re.finditer(r'\b(?:else\s+)?if\s*\(', s):
        out.append(m.group(0) + s[m.end():_closing(s, m.end())] + ')')
    return ' '.join(out)


def _context(conds, guest):
    """(arena, plant) of a line under its open blocks."""
    cond = ' '.join(' '.join(conds).split())
    arena = bool(POS_ARENA.search(cond))
    plant = bool(POS_PLANT.search(cond)) or (guest and any(p.search(c) for c in conds for p in GUEST_PLANT))
    return arena, plant


def writes_of(ln, recv):
    """-> [(field, index)] the writes of one masked line: index = int (a literal subscript), 'var' (another subscript), None (whole)."""
    r = re.escape(recv)
    s = re.sub(REF_RE.format(r=r), ' ', ln)
    out = []
    for m in re.finditer(r'\(\s*[^()?;]*\?\s*' + r + r'\.(\w+)\s*:\s*' + r + r'\.(\w+)\s*\)\s*=(?!=)', s):   # (c ? mat.F : mat.G) = v
        out += [(m.group(1), None), (m.group(2), None)]
    s = re.sub(r'\(\s*[^()?;]*\?\s*' + r + r'\.\w+\s*:\s*' + r + r'\.\w+\s*\)\s*=(?!=)', ' ', s)
    for m in re.finditer(r'(?<![=!<>&])&\s*' + r + r'\.(\w+)(?:\[\s*(\w+)\s*\])?', s):                       # slot = &mat.F
        out.append((m.group(1), None if m.group(2) is None else int(m.group(2)) if m.group(2).isdigit() else 'var'))
    for m in re.finditer(r'\?\s*' + r + r'\.(\w+)\s*$', s):                                                   # the vec4 chain's arm
        out.append((m.group(1), None))
    # mat.F (= | [i] =); the subscript may hold one nested [..] (`mat.muscle_uv[pname[13] == 'w' ? 3 : ..] =`: review, missed before)
    for m in re.finditer(r'(?<![\w.>])' + r + r'\.(\w+)\s*(?:\[((?:[^\[\]]|\[[^\]]*\])*)\])?\s*(?:[|&+\-*/]?=)(?!=)', s):
        ix = m.group(2)
        out.append((m.group(1), None if ix is None else int(ix) if ix.strip().isdigit() else 'var'))
    return out


def walk(text, head_re, tail_re, path, recv, names_of, guest):
    """-> ([fill], [call]) over the lines of the block from head_re to tail_re. fill = {'field', 'idx', 'names', 'via', 'line',
    'arena', 'plant', 'cond'}; call = {'fn', 'line', 'path', 'arena', 'plant'} for a call handed the receiver itself (followed by
    the caller). The head line (a followed definition's own signature) is not searched for calls."""
    ms = CD._mask(text)
    a = re.search(head_re, ms, re.M)
    if not a:
        raise GuestDiffError(f'{path}: no line matches {head_re!r}')
    b = re.search(tail_re, ms[a.end():], re.M)
    if not b:
        raise GuestDiffError(f'{path}: no line matches {tail_re!r} after line {line_of(text, a.start())}')
    first = line_of(text, a.start())
    lines = ms[a.start():a.end() + b.end()].split('\n')
    stack, fills, calls = [], [], []
    for k, ln in enumerate(lines):
        no = first + k
        conds = [c for c in stack] + [ln]
        ws = writes_of(ln, recv)
        # a free call (a method call such as push_back is not one). The match ends at its `(`, so a call behind `if (` or inside another
        # call's arguments is still seen (review: the old `\(([^;]*)` swallowed the rest of the line, BuildModel's ApplyRows call with it)
        for mc in (re.finditer(r'(?<![\w.>])([A-Za-z_][\w:]*)\s*\(', ln) if k else ()):
            fn = mc.group(1)
            if fn in CALL_SKIP or fn.startswith('std::'):
                continue
            if re.search(r'(?<![\w.>])' + re.escape(recv) + r'(?![\w.])', _top_level(ln[mc.end():])):
                arena, plant = _context(list(stack) + [_if_conds(ln, mc.end(), _closing(ln, mc.end()))], guest)   # the open blocks + the line's own `if`, never the call's arguments
                calls.append({'fn': fn, 'line': no, 'path': path, 'arena': arena, 'plant': plant})
        if ws:
            own = names_of(ln)
            names = own
            if not names:
                for c in reversed(stack):
                    names = names_of(c)
                    if names:
                        break
            via = []
            if not names:
                for c in [ln] + list(reversed(stack)):
                    via = re.findall(REF_RE.format(r=re.escape(recv)), c)
                    if via:
                        break
            arena, plant = _context(conds, guest)
            for f, ix in ws:
                fills.append({'field': f, 'idx': ix, 'names': list(dict.fromkeys(names)), 'via': via, 'line': no, 'arena': arena,
                              'plant': plant, 'path': path, 'text': ln})
        for ch in ln:
            if ch == '{':
                stack.append(ln)
            elif ch == '}' and stack:
                stack.pop()
    return fills, calls


def resolve_via(fills):
    by = {}
    for f in fills:
        if f['names']:
            by.setdefault(f['field'], []).extend(f['names'])
    for f in fills:
        if not f['names'] and f['via']:
            f['names'] = list(dict.fromkeys(n for g in f['via'] for n in by.get(g, [])))
    return fills


def parse_slot_names(guest_text):
    """SlotOfParam: {TexSlot member: [names]}."""
    ms = CD._mask(guest_text)
    m = re.search(r'TexSlot SlotOfParam\(const char\* name\)\s*\{', ms)
    if not m:
        return {}, None   # S111 UNIFY2: SlotOfParam is deleted; the guest build calls the shared table (assets::matsamp::SlotOf), followed as a call passing mat
    b = CD._match(ms, ms.index('{', m.start()), '{', '}')
    out = {}
    for mm in re.finditer(r'if\s*\(([^;]*?)\)\s*return\s+TexSlot::(\w+)\s*;', ms[m.start():b]):
        out.setdefault(mm.group(2), []).extend(re.findall(r'eq\(\s*"(\w+)"\s*\)', mm.group(1)))
    return out, line_of(guest_text, m.start())


def guest_effects(guest_text):
    ms = CD._mask(guest_text)
    m = re.search(r'static const char\* const kFx\[\]\s*=\s*\{([^}]*)\}', ms)
    if not m:
        raise GuestDiffError(f'{GUEST}: no PipelineEffect `kFx[]` list')
    return re.findall(r'"(\w+)"', m.group(1)), line_of(guest_text, m.start())


def follow_call(fn, read, ref, local=()):
    """the definition of fn taking a writable `Material&` / `assets::Material&` (a const one cannot write), searched first in the
    `local` files (the walked ones, through `read`: a helper beside its caller, as LoadJboy's CPFKeepOld), then in src by git grep at
    ref: -> ('def', path, text, head regex, receiver); ('const', path) when only a const one exists; None when none is found."""
    base = fn.split('::')[-1]
    sig = re.compile(r'\b' + re.escape(base) + r'\s*\([^)]*(?:assets::)?Material\s*&\s*\w+')
    hits = [(p, ln) for p in local for ln in CD._mask(read(p)).split('\n') if sig.search(ln)]
    # `-e`: a bare pattern ends git grep's options, and the working tree's `--untracked` after it was read as a revision (exit 128: review)
    cmd = ['git', '-C', REPO, 'grep', '-n', '-E', '-e', r'\b' + base + r'\s*\([^)]*(assets::)?Material\s*&\s*\w+'] + ([ref] if ref else ['--untracked']) + ['--', 'src']
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
    if r.returncode not in (0, 1):
        raise GuestDiffError(f'git grep for the definition of {fn} failed: {r.stderr.strip()}')
    for ln in r.stdout.splitlines():
        parts = ln.split(':', 3) if ref else ln.split(':', 2)
        if len(parts) >= (4 if ref else 3):
            hits.append((parts[-3], parts[-1]))
    const = None
    for path, txt in hits:
        m = re.search(r'\b' + re.escape(base) + r'\s*\(([^)]*)', txt)
        if not m:
            continue
        for prm in m.group(1).split(','):
            mp = re.match(r'\s*(const\s+)?(?:assets::)?Material\s*&\s*(\w+)\s*$', prm)
            if not mp:
                continue
            if mp.group(1):
                const = const or path
                continue
            head = r'\b' + re.escape(base) + r'\s*\([^)]*?(?:assets::)?Material\s*&\s*' + mp.group(2) + r'\b[^{;]*\{'
            text = read(path)
            if re.search(head, CD._mask(text), re.M):   # a definition, not a declaration
                return ('def', path, text, head, mp.group(2))
    return ('const', const) if const else None


def follow_calls(side, calls, read, ref, problems, cache, depth=1, active=(), local=(JBOY, GUEST)):
    """-> (fills, followed rows): each call handed the receiver, followed into its definition and that body's own such calls (to
    MAX_FOLLOW levels); a followed fill takes the call line's arena / plant condition too (a call behind `if (g_draw_plant == N)` is a plant)."""
    fills, followed = [], []
    for c in calls:
        row = {'side': side, 'fn': c['fn'], 'line': c['line'], 'caller': c['path'], 'depth': depth, 'path': None, 'fills': 0, 'const': None}
        followed.append(row)
        key = c['fn'].split('::')[-1]
        if key not in cache:
            cache[key] = follow_call(c['fn'], read, ref, local)
        d = cache[key]
        if d is None:
            continue
        if d[0] == 'const':
            row['const'] = d[1]
            continue
        _k, path, txt, head, recv = d
        row['path'] = path
        if key in active or depth > MAX_FOLLOW:
            row['stopped'] = True
            continue
        try:
            f2, c2 = walk(txt, head, r'^\}', path, recv, fn_names, side == 'guest')
        except GuestDiffError as e:
            problems.append(str(e))
            continue
        for x in c2:
            x['arena'], x['plant'] = x['arena'] or c['arena'], x['plant'] or c['plant']
        f3, sub = follow_calls(side, c2, read, ref, problems, cache, depth + 1, active + (key,))
        row['fills'] = len(f2)
        fills += [dict(x, arena=x['arena'] or c['arena'], plant=x['plant'] or c['plant']) for x in f2] + f3
        followed += sub
    return fills, followed


# ------------------------------------------------------------------ readers
def parse_resolve_char(char_text):
    """-> {field: [(constant, line)]} from ResolveChar's `if (.. name == "X" ..)` statements."""
    ms = CD._mask(char_text)
    m = re.search(r'^bool ResolveChar\([^)]*\)\s*\{', ms, re.M)
    if not m:
        raise GuestDiffError(f'{CHAR}: no `bool ResolveChar(...) {{`')
    a = ms.index('{', m.start())
    b = CD._match(ms, a, '{', '}')
    mv = re.search(r'const assets::Material&\s*(\w+)\s*=\s*\*c\.mat\s*;', ms[a:b])
    recv = mv.group(1) if mv else 'mt'
    out = {}
    for mi in re.finditer(r'\bif\s*\(', ms[a:b]):
        p0 = a + mi.end() - 1
        p1 = CD._match(ms, p0, '(', ')')
        cond = ms[p0:p1]
        names = re.findall(r'\bname\s*==\s*"(\w+)"', cond)
        if not names:
            continue
        j = p1
        while j < b and ms[j].isspace():
            j += 1
        end = CD._match(ms, j, '{', '}') if ms[j] == '{' else ms.index(';', j) + 1
        fields = set(re.findall(r'(?<![\w.])' + re.escape(recv) + r'\.(\w+)', ms[p0:end]))
        for f in fields:
            for n in names:
                out.setdefault(f, []).append((n, line_of(char_text, a + mi.start())))
    return out, line_of(char_text, m.start())


def constant_effects(fx_dir=None):
    """-> {constant name: {effect}} from the fx registry (field or its name_key)."""
    out = {}
    for (eff, _st, _reg), e in CD.load_fields(fx_dir).items():
        for k in {e['field'], CD.name_key(e['field'])}:
            out.setdefault(k, set()).add(eff)
    return out


def texture_readers(ours, samplers):
    """-> {field: [(slot, role, 'path:line', {effects})]} from the character switch's cases and the role functions."""
    roles = ours['roles']
    slots_of = {}
    for ctx in ('dry', 'wet', 'change'):
        for s, (role, ln) in roles[ctx].items():
            slots_of.setdefault(role, {})[s] = f'{CHAR}:{ln}'
    mu = roles.get('muscle')
    if mu:
        slots_of.setdefault(mu['role'], {})[mu['slot']] = f"{T.MUSCLE}:{mu['line']}"
    ps = {}
    for (eff, st, slot) in samplers:
        if st == 'ps':
            ps.setdefault(slot, set()).add(eff)
    out = {}
    for role, case in ours['switch'].items():
        for f in case['cls']['fields']:
            base = re.sub(r'\[.*', '', f)
            for s, at in sorted(slots_of.get(role, {}).items()):
                out.setdefault(base, []).append((s, role, at, ps.get(s, set())))
    return out


def text_readers(fields, ref, skip):
    """-> {field: ['path:line', ...]}: src lines naming `.F` that do not write it (T.WRITE_RES), outside the `skip` files. [L: text]"""
    out = {f: [] for f in fields}
    if not fields:
        return out
    cmd = ['git', '-C', REPO, 'grep', '-n', '-w', '-F']   # fixed strings: many -w regex patterns make git grep crawl
    for f in fields:
        cmd += ['-e', f]
    cmd += ([ref] if ref else ['--untracked']) + ['--', 'src']
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
    if r.returncode not in (0, 1):
        raise GuestDiffError(f'git grep over src failed: {r.stderr.strip()}')
    for ln in r.stdout.splitlines():
        parts = ln.split(':', 3) if ref else ln.split(':', 2)
        if len(parts) < (4 if ref else 3):
            continue
        path, num, txt = parts[-3], parts[-2], parts[-1].split('//')[0]
        if path in skip:
            continue
        for f in fields:
            if re.search(r'\.\s*' + re.escape(f) + r'\b', txt) and not any(re.search(p.format(f=re.escape(f)), txt) for p in T.WRITE_RES):
                out[f].append(f'{path}:{num}')
    return out


# ------------------------------------------------------------------ CAW records (the asset index)
def asset_db():
    """the file qasset itself opens: qasset.db_path() (WWE_QASSET_DB, else out\\harness\\index\\<qasset.db_name()> of the checkout holding
    the game). Until `q asset` seeds asset.s5.db (S111 MODINDEX), a schema-5 q has only asset.s4.db: that one named file is read instead
    (qasset_mods.seed's own rule: s5 = the s4 tables + the mods rows, which load_caw drops anyway). No other file is ever guessed."""
    import qasset
    p = qasset.db_path()
    if os.path.exists(p):
        return p
    prev = os.path.join(os.path.dirname(p), 'asset.s4.db')
    if not os.environ.get('WWE_QASSET_DB') and qasset.SCHEMA == 5 and os.path.basename(p) == 'asset.s5.db' and os.path.isfile(prev):
        return prev
    return None


def load_caw(path=None):
    """-> {'records', 'pacs', 'effects': {effect: n}, 'names': {name: {effect: {value json: n}}}, 'kind': {name: 'param'|'sampler'}}
    from the edit pacs' material rows; None without the asset index."""
    import qasset_mods
    path = path or asset_db()
    if not path or not os.path.exists(path):
        return None
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    out = {'records': 0, 'pacs': set(), 'effects': {}, 'names': {}, 'kind': {}, 'db': path, 'words': {c: {} for c in RECORD_WORDS.values()}}
    try:
        q = ('SELECT pacs.path, material.effect, material.params, material.samplers, material.technique_word, material.cull_word FROM material '
             'JOIN entry ON material.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id')
        for pac, eff, ps, ss, tw, cw in con.execute(q):
            pp = pac.replace('\\', '/')
            if pp.startswith(qasset_mods.PREFIX) or not CAW_PAC.search(pp):   # the game's own edit pacs only (a schema-5 index lists the mods folder under `mods/` too)
                continue
            out['records'] += 1
            for col, v in (('technique_word', tw), ('cull_word', cw)):
                out['words'][col][v] = out['words'][col].get(v, 0) + 1
            out['pacs'].add(pac)
            out['effects'][eff] = out['effects'].get(eff, 0) + 1
            for col, kind in ((ps, 'param'), (ss, 'sampler')):
                try:
                    d = json.loads(col or '{}')
                except ValueError:
                    continue
                for k, v in (d.items() if isinstance(d, dict) else []):
                    out['kind'][k] = kind
                    e = out['names'].setdefault(k, {}).setdefault(eff, {})
                    js = json.dumps(v)
                    e[js] = e.get(js, 0) + 1
    finally:
        con.close()
    out['pacs'] = sorted(out['pacs'])
    return out


def _differs(v, idx, name, dflt):
    """a record value v (list of floats / texture name) against the field default (floats; None = no numeric default)."""
    if isinstance(v, str):
        return v not in EMPTY_TEX
    if not isinstance(v, list) or dflt is None:
        return True
    eq = lambda x, y: abs(x - y) <= 1e-6 * max(1.0, abs(y))   # noqa: E731
    if isinstance(idx, int):
        return idx < len(dflt) and len(v) >= 1 and not eq(v[0], dflt[idx])
    if idx == 'var' and len(v) == 1:
        if re.search(r'_[xyzw]$', name) and len(dflt) == 4:
            return not eq(v[0], dflt['xyzw'.index(name[-1])])
        if len(set(dflt)) == 1:
            return not eq(v[0], dflt[0])
        return True
    return any(not eq(v[k], dflt[k]) for k in range(min(len(v), len(dflt))))


def caw_of(names, fills, dflt, caw, effects_lc=None):
    """-> {name: {'n', 'differ', 'effects': {effect: (n, differ)}}} for the names (X* = a prefix) over the CAW records."""
    out = {}
    if caw is None:
        return out
    idx_of = {}
    for f in fills:
        for n in f['names']:
            idx_of.setdefault(n, f['idx'])
    for pat in names:
        hits = [k for k in caw['names'] if (fnmatch.fnmatchcase(k, pat) if pat.endswith('*') else k == pat)]
        for k in hits:
            for eff, vals in caw['names'][k].items():
                if effects_lc is not None and eff.lower() not in effects_lc:
                    continue
                n = sum(vals.values())
                d = sum(c for js, c in vals.items() if _differs(json.loads(js), idx_of.get(pat), k, dflt))
                e = out.setdefault(k, {'n': 0, 'differ': 0, 'effects': {}})
                e['n'] += n
                e['differ'] += d
                pn, pd = e['effects'].get(eff, (0, 0))
                e['effects'][eff] = (pn + n, pd + d)
    return out


# ------------------------------------------------------------------ the rows
def load_ours(read, ref):
    texts = {p: read(p) for p in (JBOY, MODEL, GUEST, READER, CHAR)}
    problems = [f'{p}: holds a merge-conflict marker (run with --ref HEAD)' for p, t in texts.items() if re.search(r'^<<<<<<< ', t, re.M)]
    dflt = parse_defaults(texts[MODEL])
    stock, scalls = walk(texts[JBOY], r'^\s*Material mat;', r'^\s*m\.materials\.push_back\(mat\);', JBOY, 'mat', jboy_names, False)
    slots, slot_line = parse_slot_names(texts[GUEST])
    # a line naming TexSlot::X (a `case TexSlot::X:` arm, an `if (slot == TexSlot::X ...)` block: S111 GUESTCUBE) takes SlotOfParam's names for X
    slot_names = lambda ln: [n for s in re.findall(r'\bTexSlot::(\w+)', ln) for n in slots.get(s, [])] or literal_names(ln)   # noqa: E731
    guest, calls = walk(texts[GUEST], r'^\s*assets::Material mat;', r'^\s*out\.materials\.push_back\(mat\);', GUEST, 'mat', slot_names, True)
    cache = {}
    sf, sfol = follow_calls('stock', scalls, read, ref, problems, cache)
    gf, gfol = follow_calls('guest', calls, read, ref, problems, cache)
    stock, guest, followed = stock + sf, guest + gf, sfol + gfol
    eff, eff_line = guest_effects(texts[GUEST])
    rk = re.search(r'if\s*\(\s*kind\s*!=\s*kParamSampler\s*\)\s*continue\s*;', CD._mask(texts[READER]))
    resolve, rc_line = parse_resolve_char(texts[CHAR])
    return {'texts': texts, 'defaults': dflt, 'stock': resolve_via(stock), 'guest': resolve_via(guest), 'slot_names': slots,
            'slot_line': slot_line, 'calls': followed, 'effects': eff, 'effects_line': eff_line,
            'reader_filter': line_of(texts[READER], rk.start()) if rk else None, 'resolve': resolve, 'resolve_line': rc_line}, problems


def build_rows(ours, caw, cons, tex, text_fn, writers=None):
    """text_fn(fields) -> {field: ['path:line']}: called once, for the stock-only fields no resolver / bind case reads (the text search)."""
    writers = writers or {}
    dflt = ours['defaults']
    guest_fx = {e.lower() for e in ours['effects']}
    drawn = guest_fx & {e.lower() for e in caw['effects']} if caw else guest_fx
    fields = sorted({f['field'] for f in ours['stock'] + ours['guest']})

    def reads_of(f):
        reads = []
        for cn, ln in ours['resolve'].get(f, []):
            effs = sorted(e for e in cons.get(cn, set()) if e.lower() in drawn)
            reads.append({'by': f'ResolveChar {cn}', 'at': f'{CHAR}:{ln}', 'effects': effs})
        for s, role, at, effs in tex.get(f, []):
            reads.append({'by': f'slot {s} ({role})', 'at': at, 'effects': sorted(e for e in effs if e.lower() in drawn)})
        return reads
    need = [f for f in fields if any(not x['plant'] and not x['arena'] for x in ours['stock'] if x['field'] == f)
            and not any(not x['plant'] for x in ours['guest'] if x['field'] == f) and not any(r['effects'] for r in reads_of(f))]
    text_rd = text_fn(need)
    rows = []
    for f in fields:
        st = [x for x in ours['stock'] if x['field'] == f and not x['plant']]
        stc = [x for x in st if not x['arena']]
        gu = [x for x in ours['guest'] if x['field'] == f and not x['plant']]
        gplant = [x for x in ours['guest'] if x['field'] == f and x['plant']]
        d = dflt.get(f, {'type': '?', 'default': '?', 'vals': None, 'line': None})
        snames = list(dict.fromkeys(n for x in stc for n in x['names']))
        gnames = list(dict.fromkeys(n for x in gu for n in x['names']))
        reads = reads_of(f)
        read_effects = sorted({e for r in reads for e in r['effects']})
        seen_at = {r['at'] for r in reads}
        elsewhere = [x for x in text_rd.get(f, []) if x not in seen_at] if not read_effects else []
        cw = caw_of(snames, stc, d['vals'], caw, {e.lower() for e in read_effects} if read_effects else drawn)
        cw_all = caw_of(snames, stc, d['vals'], caw, drawn)
        n_named = sum(v['n'] for v in cw_all.values())
        n_differ = sum(v['differ'] for v in cw.values())
        if stc and gu:
            missing = [n for n in snames if n not in gnames and not any(fnmatch.fnmatchcase(n, g) for g in gnames if g.endswith('*'))]
            mcw = caw_of(missing, stc, d['vals'], caw, drawn) if missing else {}
            verdict = 'PARTIAL' if any(v['differ'] for v in mcw.values()) else 'BOTH'
        elif gu:
            verdict = 'GUEST-ONLY'
            missing = []
            others = [x for x in writers.get(f, []) if not x.startswith(GUEST + ':')]
        elif stc:
            missing = []
            derived = not snames
            if derived:
                verdict = 'DERIVED' if (read_effects or elsewhere) else 'UNREAD'
            elif read_effects:
                verdict = 'LOOK' if n_differ else 'SAME' if n_named else 'NOREC'
            elif elsewhere:
                verdict = 'READ' if sum(v['differ'] for v in cw_all.values()) else 'SAME' if n_named else 'NOREC'
            else:
                verdict = 'UNREAD'
            if caw is None and verdict in ('LOOK', 'SAME', 'NOREC', 'READ'):
                verdict = 'LOOK' if read_effects else 'READ'
        else:
            verdict = 'ARENA'
            missing = []
        if verdict != 'GUEST-ONLY':
            others = []
        words = {}
        if caw:
            for x in stc:
                for off in re.findall(r'\bu32\(\s*rec\s*\+\s*(0x[0-9A-Fa-f]+)\s*\)', x['text']):
                    col = RECORD_WORDS.get(int(off, 16))
                    if col and not x['names']:
                        dv = d['vals'][0] if d['vals'] else 0
                        cnt = caw['words'][col]
                        words[col] = {'values': {str(k): v for k, v in sorted(cnt.items(), key=lambda t: -t[1])}, 'differ': sum(v for k, v in cnt.items() if k is not None and k != dv)}
        rows.append({'field': f, 'verdict': verdict, 'type': d['type'], 'default': d['default'], 'default_at': f"{MODEL}:{d['line']}",
                     'other_writers': others, 'record_words': words,
                     'stock': [{'at': f"{x['path']}:{x['line']}", 'names': x['names'], 'arena': x['arena']} for x in st],
                     'stock_names': snames, 'guest': [{'at': f"{x['path']}:{x['line']}", 'names': x['names']} for x in gu],
                     'guest_names': gnames, 'guest_plant': [f"{x['path']}:{x['line']}" for x in gplant], 'missing_names': missing,
                     'reads': reads, 'read_effects': read_effects, 'elsewhere': elsewhere,
                     'caw': {k: {'n': v['n'], 'differ': v['differ'], 'effects': {e: list(t) for e, t in sorted(v['effects'].items())}} for k, v in cw_all.items()},
                     'caw_named': n_named, 'caw_differ': n_differ})
    rank = {v: i for i, v in enumerate(VERDICTS)}
    rows.sort(key=lambda r: (rank[r['verdict']], r['field']))
    return rows


def unclaimed(ours, caw):
    """names the CAW records carry that neither path reads into any field."""
    if caw is None:
        return []
    taken = [n for x in ours['stock'] + ours['guest'] for n in x['names']]
    return sorted(k for k in caw['names'] if not any(k == t or (t.endswith('*') and fnmatch.fnmatchcase(k, t)) for t in taken))


_FX_AT = {}


def fx_dir_at(ref):
    """the fx registry (src/renderer/shaders/fx/*.fx.json) as committed at ref, in a temp folder (removed at exit)."""
    if ref not in _FX_AT:
        r = subprocess.run(['git', '-C', REPO, 'archive', '--format=tar', ref, '--', 'src/renderer/shaders/fx'], capture_output=True, check=True)
        d = tempfile.mkdtemp(prefix='qguestdiff_fx_')
        atexit.register(shutil.rmtree, d, True)
        with tarfile.open(fileobj=io.BytesIO(r.stdout)) as tf:
            for m in tf.getmembers():
                if m.isfile() and m.name.endswith('.fx.json'):
                    with open(os.path.join(d, os.path.basename(m.name)), 'wb') as fh:
                        fh.write(tf.extractfile(m).read())
        _FX_AT[ref] = d
    return _FX_AT[ref]


def compute(read, ref=None, caw=None, fx_dir=None, grep=None, memo=None):
    """-> (rows, ours, caw, problems). caw = a load_caw() result (None: read the asset index; False: none). memo = a dict kept across
    calls (the selftest): the fx registry and the src searches at one ref are read once."""
    memo = {} if memo is None else memo
    ours, problems = load_ours(read, ref)
    base_grep = grep or T.default_grep(ref)

    def grep(fields):
        k = ('grep', ref, tuple(sorted(fields)))
        if k not in memo:
            memo[k] = base_grep(fields)
        return memo[k]
    tours, tprob = T.load_ours(read, grep)
    problems += tprob
    if caw is None:
        caw = load_caw()
    elif caw is False:
        caw = None
    if fx_dir is None and ref:
        fx_dir = fx_dir_at(ref)   # the registry of the same commit (a frozen selftest case must not read today's)
    if ('fx', fx_dir) not in memo:
        memo[('fx', fx_dir)] = (constant_effects(fx_dir), T.load_samplers(fx_dir))
    cons, samplers = memo[('fx', fx_dir)]
    tex = texture_readers(tours, samplers)
    skip = {JBOY, MODEL, GUEST} | {c['path'] for c in ours['calls'] if c['path']}   # the followed fill sources are not readers either

    def text_fn(fields):
        k = ('text', ref, tuple(fields), tuple(sorted(skip)))
        if k not in memo:
            memo[k] = text_readers(fields, ref, skip)
        return memo[k]
    snf = {f['field'] for f in ours['stock'] if not f['plant'] and not f['arena']}
    gonly = sorted({f['field'] for f in ours['guest'] if not f['plant']} - snf)
    rows = build_rows(ours, caw, cons, tex, text_fn, grep(gonly))
    return rows, ours, caw, problems


def fmt_row(r):
    names = '/'.join(r['stock_names']) or ('(no name)' if r['stock'] else 'none in the loader')
    s = f"{r['verdict']:10s} {r['field']:22s} default {r['default']}  stock {names}"
    if r['stock']:
        s += f" ({', '.join(x['at'].split('/')[-1] for x in r['stock'][:3])}{' ...' if len(r['stock']) > 3 else ''})"
    if r['guest']:
        s += f"  guest {'/'.join(r['guest_names']) or '(no name)'} ({', '.join(x['at'].split('/')[-1] for x in r['guest'][:3])})"
    if r['guest_plant']:
        s += f"  guest plant only {', '.join(x.split('/')[-1] for x in r['guest_plant'])}"
    if r['other_writers']:
        s += f"  other src writers {len(r['other_writers'])}: {', '.join(r['other_writers'][:3])}{' ...' if len(r['other_writers']) > 3 else ''}"
    if r['missing_names']:
        s += f"  names only stock reads: {', '.join(r['missing_names'])}"
    if r['caw']:
        s += '  CAW ' + '; '.join(f"{k} {v['n']} ({v['differ']} not default: " + ', '.join(f'{e} {n}/{dd}' for e, (n, dd) in v['effects'].items()) + ')'
                                  for k, v in sorted(r['caw'].items()))
    elif r['stock_names']:
        s += '  CAW none'
    for col, w in r.get('record_words', {}).items():
        s += f"  CAW {col} ({w['differ']} not default): " + ', '.join(f'{k} x{v}' for k, v in list(w['values'].items())[:6])
    if r['reads']:
        s += '  read by ' + '; '.join(f"{x['by']} {x['at'].split('/')[-1]} -> {', '.join(x['effects']) or '(no guest effect)'}" for x in r['reads'])
    elif r['elsewhere']:
        s += f"  read (text, L) {len(r['elsewhere'])}: {', '.join(r['elsewhere'][:3])}{' ...' if len(r['elsewhere']) > 3 else ''}"
    return s


def run(a, read=None, caw=None):
    rows, ours, caw, problems = compute(read or T.default_reader(a.ref), a.ref, caw)
    sel = rows
    if a.field:
        sel = [r for r in sel if fnmatch.fnmatchcase(r['field'], a.field)]
    if a.verdict:
        sel = [r for r in sel if r['verdict'] in a.verdict]
    elif not a.all and not a.field:
        sel = [r for r in sel if r['verdict'] in SHOWN]
    if a.json:
        un = unclaimed(ours, caw)
        bare, why, stale = NR.split(un)   # S111 UNREADNAMES
        print(json.dumps({'rows': sel, 'problems': problems, 'unclaimed': un, 'unreasoned': bare,
                          'noread': [{'pattern': p, 'mark': m, 'reason': r, 'names': ns} for p, m, r, ns in why], 'noread_stale': stale if caw else [],
                          'caw': {'records': caw['records'], 'effects': caw['effects']} if caw else None}, indent=1, default=str))
    else:
        print(f"== q guestdiff (ours {'at ' + a.ref if a.ref else 'in the working tree'}): Material fields the stock loader ({JBOY}) fills against the guest build ({GUEST} BuildModel)")
        for r in sel:
            print(fmt_row(r))
        tot = {v: sum(1 for r in rows if r['verdict'] == v) for v in VERDICTS}
        print(f"totals: {len(rows)} fields filled by either path (of {len(ours['defaults'])} Material fields); " + ', '.join(f'{v} {tot[v]}' for v in VERDICTS)
              + f"; shown {len(sel)}")
        print('LOOK fields: ' + (', '.join(r['field'] for r in rows if r['verdict'] == 'LOOK') or 'none'))
        un = unclaimed(ours, caw)
        bare, why, stale = NR.split(un)   # S111 UNREADNAMES: a covered name is printed with its reason (tools/q/qguestdiff_noread.py)
        print(f"names the CAW records carry that no path reads into a field and no NOREAD reason covers: {', '.join(bare) or 'none'}")
        if why:
            print(f"NOREAD ({sum(len(ns) for _p, _m, _r, ns in why)} names, tools/q/qguestdiff_noread.py" + ('' if a.all else '; --all prints the reasons') + '): '
                  + ', '.join(n for _p, _m, _r, ns in why for n in ns))
            if a.all:
                for p, m, r, ns in why:
                    print(f"  [{m}] {', '.join(ns)}: {r}")
        if stale and caw:
            print(f"NOREAD rows that cover no unread name now (a path reads it, or no record carries it: drop the row): {', '.join(stale)}")
        rf = ours['reader_filter']
        print(f"sources: guest reader {READER}:{rf} `kind != kParamSampler` (its sampler loop keeps kind 15 only; a float field reaches a guest part only through a followed call)" if rf
              else f'sources: guest reader {READER}: no `kind != kParamSampler` filter (reads more than samplers)', end='')
        print((f"; SlotOfParam {GUEST}:{ours['slot_line']}" if ours['slot_line'] else '; SlotOfParam: none (the shared sampler table, followed as a call)') + f"; guest effects {GUEST}:{ours['effects_line']} ({len(ours['effects'])})"
              + (f"; CAW records {caw['records']} in {len(caw['pacs'])} edit pacs ({caw['db']}): " + ', '.join(f'{e} {n}' for e, n in sorted(caw['effects'].items(), key=lambda t: -t[1]))
                 if caw else '; CAW records: NO asset index (q asset builds it): LOOK / SAME / NOREC cannot be told apart')
              + f"; ResolveChar {CHAR}:{ours['resolve_line']}")
        for c in ours['calls']:
            tail = (f"{c['path']} ({c['fills']} fills)" + (' (not walked again: a loop / MAX_FOLLOW)' if c.get('stopped') else '') if c['path']
                    else f"takes a const Material& only ({c['const']}): no writes" if c['const'] else 'no definition taking a Material& found in src (its writes are NOT seen)')
            print(f"{c['side']} call passing mat (level {c['depth']}): {c['fn']} at {c['caller']}:{c['line']} -> {tail}")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def main(argv, con=None):
    ap = argparse.ArgumentParser(prog='q guestdiff', description=__doc__.split('\n\n')[0])
    ap.add_argument('--field', help='a Material field or a glob (hair_*)')
    ap.add_argument('--verdict', nargs='+', choices=VERDICTS, help='only rows of these verdicts')
    ap.add_argument('--all', action='store_true', help='also the BOTH / ARENA rows')
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        return run(a)
    except (GuestDiffError, T.TexDiffError, CD.DiffError) as e:
        print(f'q guestdiff: {e}', file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as e:
        print(f'q guestdiff: git failed: {e.stderr.strip() if e.stderr else e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
