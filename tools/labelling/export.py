"""Session 10 Track B, step B2: export the per-function context files the labellers read (SESSION10_PLAN.md).

    py tools\\labelling\\export.py [--batches b01,b10] [--check-only] [--out DIR]                    (pilot, export v2)
    py tools\\labelling\\export.py --selection out\\harness\\labelling\\bulk\\selection.json --wave w01
                                 --out out\\harness\\labelling\\bulk\\w01 [--batches w01b01,...] [--check-only]
                                                                                                  (bulk, export v3)

Pilot mode (no --selection) is unchanged: export v2, read from pilot.json, written to out\\harness\\labelling\\batches\\
(or --out). Bulk mode (--selection, written by pick_bulk.py) exports one wave, one directory per batch (DIR\\w01b01\\
...), export v3 = v2 + section 2b (below); no hidden_labels.json (bulk functions have no db entry). A selected function
that has a db entry by now (labelled since the selection) is skipped and listed in its batch's index.md.

Section 2b (v3 only), "Caller context", context for the PURPOSE, not the function's own code: for up to 4 direct callers
(most-run first: scenarios ran in, then call sites): the caller's db name / system, its RTTI class when it is a vtable
slot, its strings (at most 3, 40 characters), its own direct callers (at most 4, most-run first; db names / systems and
at most 3 strings each). A function with no direct callers gets stand-ins instead, in this order: the functions that
build its address, its RTTI vtable's address (constructors), or the nearest code-built address at / before a data word
pointing at it inside its run of function-pointer words (a vtable without RTTI or a callback table: its constructor /
set-up code); inside a tier the ones that ran first, then tables built by fewer functions (more specific), then
most-run. Then "same block": db-named functions within +-0x400 bytes of the address (at most 6, nearest first, name +
system). Db names follow section 4's rules (never the function's own entry, never one of its batch). Db names are read
live from db\\*.json at export time, so a later wave sees the names written for earlier waves.

Pilot mode reads out\\harness\\labelling\\pilot.json (written by pick.py) and writes, per batch, out\\harness\\labelling\\batches\\bNN\\
(derived from the game: out\\ only, never committed):

  index.md             the batch's addresses in labelling order (leaf first), with instructions / callees
  0x82XXXXXX.md        one per function (export v2), sections in this order:
                         1. q.py fn (one-screen summary; its db / docs lines and host RVA left out, strings in 6)
                         2. callers (q.py callers, at most 15, the ones that ran first; how many were cut)
                         3. callees with [ran: ...] tags (q.py callees has no tags: added from coverage.json)
                         3b. callee digest (v2): per direct callee (at most 12, most call sites first): size, db name +
                            system where allowed, its own callees (at most 6; imports and db-named first; each with
                            its size and the __imp__ kernel / XAM imports IT calls), leaf or not, at most 2 strings
                            (40 characters), and the full listing of callees of at most 12 instructions
                         3c. only for a function with no direct callers (v2): code that builds its address, its
                            .pdata entry (unwind table: not a call path), and per data word (at most 4) the RTTI
                            vtable it sits in + the code that builds the vtable address (constructors), or for a
                            run of function-pointer words the nearest code-built address before it in the run
                         4. known neighbour names: callers / callees with a db entry, address + db name + system
                            only (no summary). Never the function's own entry, never a neighbour in the same batch.
                         5. the PPC listing (same output as tools\\analysis\\list_function.py ADDR 300, read from the
                            generated file the index names instead of searching all 345 files)
                         5b. data constants (v2): per data address the function builds with a certain lis pair (at
                            most 8; float loads first when cut): section (read-only / writable / BSS), the first 4
                            words as hex and float32, 2 doublewords as float64 when read as doubles or plausible,
                            what pointer words point at, and for a lis+addi base the accesses through that register
                            (linear scan, approximate) with the value at each offset. Values are the image at load
                            time (out\\harness\\index\\image.bin, checked against the index first).
                         6. string references (text cut at 60 characters, at most 20)
  db names in 3b / 3c / 5b follow section 4's rules and also never show a name on the batch's hidden list.
  hidden_labels.json   only for a batch with functions that already have db entries (b10): their existing labels
                       (name, summary, system, confidence, labels A / B) for the comparison step. NEVER for labellers.

Then a self-check (also alone with --check-only): every function has its file, every file is under 1500 lines, and
no file of a batch contains the db name or the label A / B name of any function of that batch (b10: its own name,
its siblings' and every name in db\\match_setup.json); the function's own db name / labels are counted separately;
every function file has sections 3b and 5b and the export version, and 3c when it has no direct callers (v3: and
2b). Exit code 1 if any check fails.
"""
import argparse
import glob
import io
import json
import math
import os
import re
import struct
import sys
import time
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
from common import h, q  # noqa: E402

EXPORT_VERSION = 'v2'        # pilot mode
EXPORT_VERSION_BULK = 'v3'   # bulk mode (--selection): v2 + section 2b
BATCH_DIR = os.path.join(common.OUT, 'batches')
BULK_DIR = os.path.join(common.OUT, 'bulk')
CALLER_CAP = 15
CALLEE_CAP = 40
NEIGHBOUR_CAP = 30
LISTING_CAP = 300
STRING_CAP = 20
STRING_CHARS = 60
MAX_LINES = 1500
LABELS_RX = re.compile(r'labels:\s*A\s+([A-Za-z0-9_]+)\s*;\s*B\s+([A-Za-z0-9_]+)(?:\s*\(B confidence (\w+)\))?')

# v2: callee digest (3b), data words / code pointing here (3c), data constants (5b)
DIGEST_CAP = 12            # callees with a digest (most call sites first)
DIGEST_SUB_CAP = 6         # a callee's own callees named
DIGEST_IMPORTS_CAP = 3     # imports named per callee-of-callee
DIGEST_STR_CAP = 2
DIGEST_STR_CHARS = 40
DIGEST_INLINE_MAX = 12     # callees of at most this many instructions get their listing inline
CONST_CAP = 8              # data addresses per function
CONST_OFFSET_CAP = 16      # reads through a lis+addi base register shown per address
OUTSIDE_CAP = 8
DATAWORD_CAP = 4
DATAWORD_REF_CAP = 6       # functions listed per referenced address
RUN_WALK = 16384           # words walked back / forward over a run of function pointers (a CRT table: ~7840)
NEAR_WINDOW = 0x1000       # bytes searched below a data word for the nearest code-built address
# v3: caller context (2b)
CTX_CALLER_CAP = 4         # callers (or stand-ins) with a context entry, most-run first
CTX_UP_CAP = 4             # a caller's own callers named
CTX_STR_CAP = 3            # strings per function
CTX_STR_CHARS = 40
CTX_SIG_CHARS = 100  # function-signature strings ('::') in section 2b: long enough for 'Class::Method
BLOCK_RANGE = 0x400        # "same block": db-named functions within +-this many bytes
BLOCK_CAP = 6
# trim levels, used only when a file would reach MAX_LINES: (digest cap, inline listings, offsets cap, constants cap)
TRIM = ((DIGEST_CAP, True, CONST_OFFSET_CAP, CONST_CAP), (DIGEST_CAP, False, CONST_OFFSET_CAP, CONST_CAP),
        (6, False, 6, CONST_CAP), (3, False, 4, 4))

INSN_RX = re.compile(r'^0x([0-9A-F]{8})  (\S+)(?: (.*))?$')
MEM_RX = re.compile(r'^(r\d+|f\d+|v\d+),(-?(?:0x[0-9a-fA-F]+|\d+))\((r\d+)\)$')
VOLATILE = {'r0'} | {f'r{i}' for i in range(3, 13)}
NO_GPR_DEST = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'icb', 'sync', 'lwsync', 'eieio', 'fcmp', 'vcmp', 'nop',
               'db16cyc', 'cctp')   # = tools/q/build_index.py NO_GPR_DEST_PREFIX
# priority when an address cap cuts: float loads first, then other loads, address bases, stores, ori constants
OP_PRIORITY = {'lfs': 0, 'lfd': 0, 'lwz': 1, 'ld': 1, 'lhz': 1, 'lha': 1, 'lbz': 1, 'addi': 2, 'ori': 4}
# exact values recognised in constants (f64: equal or within 1e-15 relative; f32: the same bits, '~' = 1 ulp off)
KNOWN = (('pi', math.pi), ('pi/2', math.pi / 2), ('pi/4', math.pi / 4), ('3pi/4', 3 * math.pi / 4),
         ('2pi', 2 * math.pi), ('1/pi', 1 / math.pi), ('2/pi', 2 / math.pi), ('1/(2pi)', 1 / (2 * math.pi)),
         ('pi/180 (deg to rad)', math.pi / 180), ('180/pi (rad to deg)', 180 / math.pi), ('e', math.e),
         ('ln 2', math.log(2)), ('log2 e', 1 / math.log(2)), ('ln 10', math.log(10)), ('log10 e', math.log10(math.e)),
         ('sqrt 2', math.sqrt(2)), ('1/sqrt 2', math.sqrt(0.5)), ('sqrt 3', math.sqrt(3)),
         ('sqrt 3 / 2', math.sqrt(3) / 2), ('1/3', 1 / 3))

_TEXT = {}


def file_text(name):
    if name not in _TEXT:
        with open(os.path.join(common.GEN, name), encoding='utf-8') as f:
            _TEXT[name] = f.read()
    return _TEXT[name]


def listing(con, a, limit):
    """tools/analysis/list_function.py's output for (a, limit), as lines, plus the instruction count. Same algorithm;
    the generated file comes from the index (funcs.file) instead of a search over every file."""
    name = f'sub_{a:08X}'
    row = con.execute('SELECT file FROM funcs WHERE addr = ?', (a,)).fetchone()
    paths = [row['file']] if row and row['file'] else []
    paths += [os.path.basename(p) for p in glob.glob(os.path.join(common.GEN, 'wwe2k14_recomp.*.cpp'))
              if os.path.basename(p) not in paths]
    for p in paths:
        text = file_text(p)
        i = text.find(f'DEFINE_REX_FUNC({name})')
        if i < 0:
            continue
        body = text[i:text.find('\n}\n', i)]
        out, address, shown, bad = [], a, 0, 0
        for line in body.splitlines()[1:]:
            s = line.strip()
            label = re.match(r'loc_([0-9A-F]{8}):', s)
            if label:
                if int(label.group(1), 16) != address:
                    bad += 1
                    out.append(f'!! label {label.group(0)} but computed 0x{address:08X}; re-syncing to the label')
                    address = int(label.group(1), 16)
                if shown < limit:
                    out.append(f'           {label.group(0)}')
                continue
            if s.startswith('// '):
                if shown < limit:
                    out.append(f'0x{address:08X}  {s[3:]}')
                    shown += 1
                address += 4
        n = (address - a) // 4
        out.append(f'-- {name}: {n} instructions, ends 0x{address:08X}, label mismatches: {bad}')
        return out, n
    return [f'{name} not found in generated code'], 0


def section_fn(con, a, args):
    r = q.cmd_fn(con, a, args)
    strings = r['strings']
    # The labellers must not see the function's own db entry (b10) or docs that describe it; the host RVA is
    # irrelevant here (and goes stale with every relink); strings get their own section.
    r['db'], r['docs'], r['host_rva'], r['strings'] = [], [], None, []
    buf = io.StringIO()
    with redirect_stdout(buf):
        q.print_fn(r, args)
    lines = buf.getvalue().rstrip('\n').splitlines()
    if strings:
        lines.append(f'  strings: {len(strings)} referenced (section 6)')
    return lines, r, strings


def ran_tag(covf, name, addr):
    if name.startswith('__imp__'):
        return '[import thunk: not instrumented, coverage cannot tell]'
    tags = covf.get(h(addr), [])
    return f'[ran: {",".join(tags) or "never seen"}]'


def section_callers(con, a, args, covf):
    r = q.cmd_callers(con, a, args)
    callers = sorted(r['callers'], key=lambda c: 0 if c.get('ran_in') else 1)   # stable: address order otherwise
    lines = [f'{r["func"]}: {r["n_sites"]} call sites in {r["n_callers"]} functions'
             + (' (callers that ran listed first)' if len(callers) > CALLER_CAP else '')]
    for c in callers[:CALLER_CAP]:
        sites = ', '.join(f'{s["site"]} {s["kind"]}' + (f' (into {s["target"]})' if s['target'] else '')
                          for s in c['sites'][:6]) + (' ...' if len(c['sites']) > 6 else '')
        ran = f'  [ran: {",".join(c.get("ran_in", [])) or "never seen"}]'
        lines.append(f'  {c["func"]:<28} {sites}{ran}')
    if len(callers) > CALLER_CAP:
        cut = callers[CALLER_CAP:]
        lines.append(f'  ... {len(cut)} more callers cut (cap {CALLER_CAP}); '
                     f'{sum(1 for c in cut if c.get("ran_in"))} of them ran')
    if r['data_pointers']:
        dp = r['data_pointers']
        lines.append(f'  data words pointing at it: {len(dp)}: ' + ', '.join(dp[:12]) + (' ...' if len(dp) > 12 else ''))
    for s in r['vtable_slots']:
        lines.append(f'  vtable slot: {s["class"]} vtable {s["vtable"]} slot {s["slot"]}')
    return lines, [int(c['addr'], 16) for c in r['callers']]


def section_callees(con, a, covf):
    f = con.execute('SELECT n_bctrl, n_bctr FROM funcs WHERE addr = ?', (a,)).fetchone()
    ce = common.callees(con, a)
    lines = [f'sub_{a:08X}: {len(ce)} direct callees (save/restore helpers hidden); indirect calls (bctrl) '
             f'{f["n_bctrl"]}, indirect jumps (bctr) {f["n_bctr"]}']
    for dst, sites in list(ce.items())[:CALLEE_CAP]:
        n = q.fname(con, dst)
        kinds = '/'.join(sorted({s['kind'] for s in sites}))
        into = [s['target'] for s in sites if s['target']]
        into = f' into {",".join(into)}' if into else ''
        lines.append(f'  {n:<28} x{len(sites)} {kinds}{into}  {ran_tag(covf, n, dst)}')
    if len(ce) > CALLEE_CAP:
        lines.append(f'  ... {len(ce) - CALLEE_CAP} more callees cut (cap {CALLEE_CAP})')
    return lines, list(ce)


def section_neighbours(callers, callees, a, batch_addrs, db_by_addr):
    role = {}
    for x in callers:
        role.setdefault(x, set()).add('caller')
    for x in callees:
        role.setdefault(x, set()).add('callee')
    rows = []
    for x in sorted(role, key=lambda x: (0 if 'callee' in role[x] else 1, x)):
        if x == a or x in batch_addrs:   # never the function's own entry, never a neighbour of the same batch
            continue
        for fn, e in db_by_addr.get(x, []):
            rows.append(f'  {h(x)}  {e.get("name")}  [{e.get("system")}]  ({"+".join(sorted(role[x]))})')
    if not rows:
        return ['  (none of the callers / callees has a db entry)']
    out = rows[:NEIGHBOUR_CAP]
    if len(rows) > NEIGHBOUR_CAP:
        out.append(f'  ... {len(rows) - NEIGHBOUR_CAP} more cut (cap {NEIGHBOUR_CAP})')
    return out


def section_strings(strings):
    if not strings:
        return ['  (no string references)']
    out = []
    for s in strings[:STRING_CAP]:
        t = s['text']
        t = json.dumps(t[:STRING_CHARS] + ('...' if len(t) > STRING_CHARS else ''))
        out.append(f'  @{s["site"]} -> {s["addr"]}  {t}' + ('' if s['how'] == 'direct' else f'  ({s["how"]})'))
    if len(strings) > STRING_CAP:
        out.append(f'  ... {len(strings) - STRING_CAP} more cut (cap {STRING_CAP})')
    return out


# ---------------------------------------------------------------- v2 sections: 3b, 3c, 5b

class Names:
    """Which db names a file may show. Never the function's own entry, never a function of the same batch (the
    rules of section 4), and never a name on the batch's hidden list (hidden_names: the batch's db names and labels
    A / B; for b10 every name of db\\match_setup.json): such an address is shown as its sub_ name only."""

    def __init__(self, con, a, batch_addrs, forbidden, db_by_addr):
        self.con, self.a, self.batch_addrs, self.db_by_addr = con, a, batch_addrs, db_by_addr
        self.forbidden = {n.lower() for n in forbidden if n}

    def db(self, x):
        if x is None or x == self.a or x in self.batch_addrs:
            return []
        return [(e.get('name'), e.get('system')) for _, e in self.db_by_addr.get(x, [])
                if e.get('name') and e['name'].lower() not in self.forbidden]

    def label(self, x):
        n = q.fname(self.con, x)
        d = self.db(x)
        return n + ('=' + '/'.join(name for name, _ in d) if d else '')


def func_row(con, x):
    return con.execute('SELECT * FROM funcs WHERE addr = ?', (x,)).fetchone() if x is not None else None


def strings_of(con, x, cap, chars, sig_chars=None):
    """The first `cap` distinct strings function x references (certain lis pairs), cut at `chars`; + how many more.
    sig_chars: a longer cut for function-signature strings (containing '::'), so the method name survives the cut
    (bulk 2b only; the strings stay verbatim)."""
    texts = []
    for r in con.execute("""SELECT s.text FROM strrefs r JOIN strings s ON s.addr = r.addr
                            WHERE r.func = ? AND r.op NOT LIKE '%?' ORDER BY r.site""", (x,)):
        if r['text'] not in texts:
            texts.append(r['text'])
    def cut(t):
        n = sig_chars if sig_chars and '::' in t else chars
        return t[:n] + ('...' if len(t) > n else '')
    shown = [json.dumps(cut(t)) for t in texts[:cap]]
    return shown, len(texts) - len(shown)


def ref_lines(con, rows, names, cap=DATAWORD_REF_CAP, indent='    '):
    """Functions that build an address (imms rows func/site/op), one line per function."""
    by = {}
    for r in rows:
        by.setdefault(r['func'], []).append(f'{r["op"]} @{h(r["site"])}')
    out = []
    for f, sites in list(by.items())[:cap]:
        fr = func_row(con, f)
        info = []
        if fr and fr['has_body']:
            info.append(f'{fr["insns"]} insns')
        callers = [r[0] for r in con.execute('SELECT DISTINCT src FROM edges WHERE dst = ? ORDER BY src LIMIT 3', (f,))]
        if len(callers) <= 2:
            info.append(f'{len(callers)} caller{"s" if len(callers) != 1 else ""}'
                        + (': ' + ', '.join(names.label(c) for c in callers) if callers else ''))
        else:
            info.append(f'{common.n_callers(con, f)} callers')
        out.append(f'{indent}{names.label(f)} [{"; ".join(info)}]  ' + ', '.join(sites[:3])
                   + (' ...' if len(sites) > 3 else ''))
    if len(by) > cap:
        out.append(f'{indent}... {len(by) - cap} more functions (cap {cap})')
    return out


def lis_refs(con, lo, hi=None):
    """Certain lis-pair uses of the addresses lo..hi (inclusive), highest address first."""
    return con.execute("""SELECT value, func, site, op FROM imms WHERE value BETWEEN ? AND ? AND op LIKE 'lis+%'
                          AND op NOT LIKE '%?' ORDER BY value DESC, func, site""",
                       (lo, lo if hi is None else hi)).fetchall()


def digest_sub_label(con, x, names, callee, a):
    n = names.label(x)
    if x == callee:
        return n + ' (itself)'
    if x == a:
        return n + ' (the function being labelled)'
    fr = func_row(con, x)
    if fr is None or not fr['has_body']:
        return n
    imps = [q.fname(con, y) for y in common.callees(con, x) if q.fname(con, y).startswith('__imp__')]
    extra = f'{fr["insns"]} insn{"s" if fr["insns"] != 1 else ""}'
    if imps:
        extra += '; -> ' + ', '.join(imps[:DIGEST_IMPORTS_CAP]) + (' ...' if len(imps) > DIGEST_IMPORTS_CAP else '')
    return f'{n} [{extra}]'


def digest_one(con, a, dst, sites, names, inline):
    kinds = '/'.join(sorted({s['kind'] for s in sites}))
    if dst is None:
        return [f'? x{len(sites)} {kinds}: branch target in no known function']
    head = f'{names.label(dst)}  x{len(sites)} {kinds}'
    if dst == a:
        return [f'{head}: the function itself (recursion)']
    f = func_row(con, dst)
    if f is None or not f['has_body']:
        n = q.fname(con, dst)
        what = ('kernel / XAM import thunk (no guest body; the name is the imported function)' if n.startswith('__imp__')
                else 'mapped name without a guest body')
        return [f'{head}: {what}']
    bits = [f'{f["insns"]} insn{"s" if f["insns"] != 1 else ""}']
    d = names.db(dst)
    if d:
        bits.append('db: ' + ', '.join(f'{n} [{s}]' for n, s in d))
    sub = common.callees(con, dst)
    nb = f['n_bctrl']
    if not sub and not nb:
        bits.append('leaf (no calls)')
    lines = [f'{head}: ' + '; '.join(bits)]
    if sub:
        # imports and db-named callees first (the most informative), then the rest in call-site order
        order = sorted(sub, key=lambda x: 0 if q.fname(con, x).startswith('__imp__') else 1 if names.db(x) else 2)
        shown = [digest_sub_label(con, x, names, dst, a) for x in order[:DIGEST_SUB_CAP]]
        more = len(sub) - len(shown)
        lines.append(f'    calls {len(sub)}' + (f' (+{nb} indirect, bctrl)' if nb else '') + ': ' + ', '.join(shown)
                     + (f', +{more} more' if more else ''))
    elif nb:
        lines.append(f'    no direct calls; {nb} indirect call(s) (bctrl)')
    shown, more = strings_of(con, dst, DIGEST_STR_CAP, DIGEST_STR_CHARS)
    if shown:
        lines.append('    strings: ' + '; '.join(shown) + (f' (+{more} more)' if more else ''))
    if inline and f['insns'] <= DIGEST_INLINE_MAX:
        lst, _ = listing(con, dst, DIGEST_INLINE_MAX)
        lines += ['    ' + x for x in lst if not x.startswith('-- ')]
    return lines


def section_digest(con, a, names, cap, inline):
    ce = common.callees(con, a)
    if not ce:
        return ['  (no direct callees)']
    order = sorted(ce.items(), key=lambda kv: (-len(kv[1]), int(kv[1][0]['site'], 16)))
    out = [f'{len(ce)} direct callees, most call sites first'
           + (f'; the first {cap} have a digest' if len(ce) > cap else '')
           + (f'; listing inline for callees of at most {DIGEST_INLINE_MAX} instructions' if inline else '')
           + '. "=name" = db name; [N insns; -> imports] = a callee-of-callee and the kernel imports it calls']
    for dst, sites in order[:cap]:
        out += digest_one(con, a, dst, sites, names, inline)
    if len(ce) > cap:
        rest = [names.label(d) for d, _ in order[cap:]]
        out.append(f'... {len(rest)} more callees without a digest (cap {cap}): ' + ', '.join(rest[:20])
                   + (' ...' if len(rest) > 20 else ''))
    return out


_FUNC_PTR_WORDS = {}


def is_func_ptr(con, at):
    """Is the data word at `at` a function pointer (ptrs kind 'func')? The word addresses are loaded once per connection
    (~150k): the run walks below ask this up to 2 x RUN_WALK times per data word (one query each was ~85% of the
    export time of a wave of functions without direct callers)."""
    words = _FUNC_PTR_WORDS.get(id(con))
    if words is None:
        words = _FUNC_PTR_WORDS[id(con)] = {r[0] for r in con.execute("SELECT at FROM ptrs WHERE kind = 'func'")}
    return at in words


def vtable_at(con, w):
    vt = con.execute('''SELECT v.vt, v.nslots, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td
                        WHERE v.vt <= ? ORDER BY v.vt DESC LIMIT 1''', (w,)).fetchone()
    return vt if vt and w < vt['vt'] + 4 * max(1, vt['nslots']) else None


def dataword_lines(con, w, img, names):
    out = []
    own = lis_refs(con, w)
    vt = vtable_at(con, w)
    if vt:
        out.append(f'{h(w)} [{img.describe(w)}]: slot {(w - vt["vt"]) // 4} of the vtable at {h(vt["vt"])} '
                   f'(RTTI class {vt["name"]}, {vt["nslots"]} slots)')
        refs = lis_refs(con, vt['vt']) if vt['vt'] != w else own
        if refs:
            out.append('  code that builds the vtable address (likely constructors / destructors of the class):')
            out += ref_lines(con, refs, names)
        else:
            out.append('  no code builds the vtable address with a lis pair')
        if own and vt['vt'] != w:
            out.append('  code that builds this slot\'s address:')
            out += ref_lines(con, own, names)
        return out
    s = w
    while w - s < 4 * RUN_WALK and is_func_ptr(con, s - 4):
        s -= 4
    e = w
    while e - w < 4 * RUN_WALK and is_func_ptr(con, e + 4):
        e += 4
    n_run = (e - s) // 4 + 1
    capped = is_func_ptr(con, s - 4) or is_func_ptr(con, e + 4)
    out.append(f'{h(w)} [{img.describe(w)}]: ' +
               (f'entry {(w - s) // 4} of {n_run} consecutive function-pointer words from {h(s)} (a table, or a '
                f'vtable without RTTI)' if n_run > 1 else 'a function pointer with no function pointer next to it '
                                                          '(a field of a struct / table entry?)')
               + (f'; the run goes on past the {RUN_WALK}-word walk limit' if capped else ''))
    if own:
        out.append('  code that builds this word\'s address:')
        out += ref_lines(con, own, names)
    found = bool(own)
    if n_run > 1:
        refs = lis_refs(con, s + 4, w - 4) if w - 4 >= s + 4 else []   # inside the run, before this word
        if refs:
            v = refs[0]['value']
            out.append(f'  nearest address before it in the run that code builds: {h(v)} (entry {(v - s) // 4}; this '
                       f'word is slot {(w - v) // 4} from it; likely a table / vtable start, so these set it up):')
            out += ref_lines(con, [r for r in refs if r['value'] == v], names)
            found = True
        ends = {}
        for label, x in (('the run start', s), ('the word just before the run start', s - 4),
                         ('the word just after the run end', e + 4)):
            if x == w:
                continue
            rr = lis_refs(con, x)
            if rr:
                out.append(f'  code that builds {label} ({h(x)}):')
                out += ref_lines(con, rr, names)
                ends[label] = {r['func'] for r in rr}
                found = True
        lo_end = ends.get('the run start', set()) | ends.get('the word just before the run start', set())
        both = lo_end & ends.get('the word just after the run end', set())
        if both:
            out.append(f'  {", ".join(names.label(f) for f in sorted(both))} builds both ends of the run (a loop over '
                       f'the whole table, likely: check its listing)')
    if not found:
        sec = img.section(w)[0]
        lo = max(img.base, w - NEAR_WINDOW)
        near = [r for r in lis_refs(con, lo, w - 1) if img.section(r['value'])[0] == sec]
        if near:
            v = near[0]['value']
            out.append(f'  no code builds an address in its run; nearest code-built address below it: {h(v)} '
                       f'({w - v} bytes before; maybe the start of the struct / table holding it, heuristic):')
            out += ref_lines(con, [r for r in near if r['value'] == v], names)
        else:
            out.append(f'  no code builds an address within 0x{NEAR_WINDOW:X} bytes below it (same section)')
    return out


def section_pointing(con, a, img, names):
    """3c, for a function with no direct callers: code that builds its address, and the data words holding it."""
    code = con.execute("""SELECT func, site, op FROM imms WHERE value = ? AND op LIKE 'lis+%' AND op NOT LIKE '%?'
                          ORDER BY func, site""", (a,)).fetchall()
    data = [r['at'] for r in con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func' ORDER BY at", (a,))]
    out = []
    if code:
        out.append(f'code that builds this function\'s address ({len(code)} site{"s" if len(code) != 1 else ""}: a '
                   f'callback / function pointer being set up):')
        out += ref_lines(con, code, names)
    pdata = [w for w in data if img.section(w)[0] == '.pdata']
    other = [w for w in data if w not in pdata]
    if pdata:
        out.append(f'{len(pdata)} data word(s) in .pdata ({", ".join(h(w) for w in pdata[:3])}): the exception / '
                   f'unwind table entry for this function, not a way to reach it')
    for w in other[:DATAWORD_CAP]:
        out += dataword_lines(con, w, img, names)
    if len(other) > DATAWORD_CAP:
        out.append(f'... {len(other) - DATAWORD_CAP} more data words (cap {DATAWORD_CAP}): '
                   + ', '.join(h(w) for w in other[DATAWORD_CAP:DATAWORD_CAP + 12])
                   + (' ...' if len(other) > DATAWORD_CAP + 12 else ''))
    if not other:
        out.append('no data word outside .pdata points here' + ('' if code else
                   ', and no code builds its address: reached in a way the index does not show'))
    return out


# ---------------------------------------------------------------- v3 section: 2b caller context

def ctx_name(con, names, x):
    """sub_X=db_name [system] (db names under section 4's rules), or the plain name."""
    d = names.db(x)
    return q.fname(con, x) + ('=' + ' / '.join(f'{n} [{s}]' for n, s in d) if d else '')


def ctx_strings(con, x):
    shown, more = strings_of(con, x, CTX_STR_CAP, CTX_STR_CHARS, sig_chars=CTX_SIG_CHARS)
    return '; '.join(shown) + (f' (+{more} more)' if more else '') if shown else 'none'


def ctx_class(con, x):
    """' vtable slot of RTTI class C' when x sits in a vtable (the first class, + how many more)."""
    slots = q.rtti_slots_of(con, x)
    if not slots:
        return ''
    classes = sorted({s['class'] for s in slots})
    return f'; vtable slot of RTTI class {classes[0]}' + (f' (+{len(classes) - 1} more classes)' if len(classes) > 1
                                                          else '')


def ran_n(covf, x):
    return len(covf.get(h(x), []))


def direct_callers(con, x, covf):
    """[(src, call sites into x)] without x itself, most-run first (scenarios ran in, then call sites, address)."""
    rows = con.execute('SELECT src, COUNT(*) AS n FROM edges WHERE dst = ? GROUP BY src', (x,)).fetchall()
    return sorted(((r['src'], r['n']) for r in rows if r['src'] != x),
                  key=lambda t: (-ran_n(covf, t[0]), -t[1], t[0]))


def ctx_entry(con, a, c, head, names, covf, img):
    fr = func_row(con, c)
    size = f'{fr["insns"]} insns' if fr is not None and fr['has_body'] else 'no body'
    k = ran_n(covf, c)
    out = [f'{head}: {ctx_name(con, names, c)}  [{size}; ran in {k} scenario{"s" if k != 1 else ""}'
           f'{ctx_class(con, c)}]',
           f'    its strings: {ctx_strings(con, c)}']
    up = direct_callers(con, c, covf)
    if not up:
        dp = [r[0] for r in con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func'", (c,))]
        n_pd = sum(1 for w in dp if img.section(w)[0] == '.pdata')
        out.append(f'    its callers: none direct (data words pointing at it: {len(dp) - n_pd}'
                   + (f', + {n_pd} .pdata unwind entry' if n_pd else '') + ')')
        return out
    out.append(f'    its callers ({len(up)}' + (f', first {CTX_UP_CAP} most-run' if len(up) > CTX_UP_CAP else '') + '):')
    for cc, _ in up[:CTX_UP_CAP]:
        if cc == a:
            out.append(f'      {q.fname(con, cc)} (the function being labelled)')
            continue
        out.append(f'      {ctx_name(con, names, cc)}  strings: {ctx_strings(con, cc)}')
    return out


def lis_funcs(con, v):
    """Functions that build address v with a certain lis pair."""
    return [r['func'] for r in con.execute("""SELECT DISTINCT func FROM imms WHERE value = ? AND op LIKE 'lis+%'
                                              AND op NOT LIKE '%?' ORDER BY func""", (v,))]


class BlockIndex:
    """Sorted addresses of db entries that are function starts in the index (for "same block")."""

    def __init__(self, con, db_by_addr):
        self.addrs = sorted(x for x in db_by_addr if func_row(con, x) is not None)

    def near(self, a, rng):
        import bisect
        lo, hi = bisect.bisect_left(self.addrs, a - rng), bisect.bisect_right(self.addrs, a + rng)
        return self.addrs[lo:hi]


def section_caller_context(con, a, names, covf, block, img):
    out = ['CONTEXT FOR PURPOSE ONLY, NOT THIS FUNCTION\'S CODE: who calls it, who calls them, their strings, and the',
           'db-named functions next to it in the image. Db names are other functions\' labels (leads, not facts);',
           f'strings: at most {CTX_STR_CAP} per function, cut at {CTX_STR_CHARS} characters. Use it for the purpose; '
           'the mechanism comes from section 5.']
    callers = direct_callers(con, a, covf)
    if callers:
        out.append(f'direct callers: {len(callers)}' + (f', the {CTX_CALLER_CAP} most-run below'
                                                      if len(callers) > CTX_CALLER_CAP else '')
                   + ' (scenarios ran in, then call sites)')
        for i, (c, n) in enumerate(callers[:CTX_CALLER_CAP], 1):
            out += ctx_entry(con, a, c, f'caller {i} ({n} call site{"s" if n != 1 else ""} into this function)',
                             names, covf, img)
    else:
        # stand-ins, in tiers: code that builds this function's address (a callback being set up), its RTTI vtable's
        # address (constructors / destructors), or the start of a function-pointer table holding it (a vtable
        # without RTTI or a callback table: its constructor / set-up code)
        builders = {}

        def add(func, tier, why, group=0):
            if func != a and func not in builders:
                builders[func] = (tier, why, group)

        for r in lis_funcs(con, a):
            add(r, 0, 'builds this function\'s address')
        for s in q.rtti_slots_of(con, a):
            for r in lis_funcs(con, int(s['vtable'], 16)):
                add(r, 1, f'builds the vtable address of RTTI class {s["class"]}: a constructor / destructor, likely')
        words = [r['at'] for r in con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func' ORDER BY at", (a,))]
        for w in [w for w in words if img.section(w)[0] != '.pdata'][:DATAWORD_CAP]:
            if vtable_at(con, w):
                continue   # an RTTI vtable slot: tier 1
            st = w
            while w - st < 4 * RUN_WALK and is_func_ptr(con, st - 4):
                st -= 4
            refs = lis_refs(con, st - 4, w)   # highest address first: the nearest code-built address at / before w
            if refs:
                v = refs[0]['value']
                why = (f'builds the table word {h(w)} that points here: reads or patches this slot' if v == w else
                       f'builds the table start {h(v)}, {(w - v) // 4} words before {h(w)} which points here: '
                       f'constructor / set-up, likely')
                fs = sorted({x['func'] for x in refs if x['value'] == v})
                for r in fs:
                    add(r, 2, why, len(fs))   # a table built by fewer functions is the more specific one
        # tier, then stand-ins that ran, then tables built by fewer functions (more specific), then most-run
        order = sorted(builders, key=lambda f: (builders[f][0], ran_n(covf, f) == 0, builders[f][2],
                                                -ran_n(covf, f), f))
        if not order:
            out.append('direct callers: none, and no code builds its address, its vtable\'s address or the start of a '
                       'table holding it (see 3c)')
        else:
            out.append(f'direct callers: none; stand-ins: {len(order)} function(s) that build its address, its '
                       f'vtable\'s address or the start of a table holding it'
                       + (f'; the first {CTX_CALLER_CAP} below (in that order; ones that ran, then tables built by '
                          f'fewer functions, then most-run, first)' if len(order) > CTX_CALLER_CAP else ''))
            prev = None
            for i, c in enumerate(order[:CTX_CALLER_CAP], 1):
                why = builders[c][1]
                out += ctx_entry(con, a, c, f'stand-in {i} ({why if why != prev else "same role as above"})',
                                 names, covf, img)
                prev = why
    near = [x for x in block.near(a, BLOCK_RANGE) if x != a and names.db(x)]
    near.sort(key=lambda x: (abs(x - a), x))
    if near:
        out.append(f'same block: db-named functions within +-0x{BLOCK_RANGE:X} bytes of {h(a)}: {len(near)}'
                   + (f', the {BLOCK_CAP} nearest' if len(near) > BLOCK_CAP else '') + ' (offset, name, system)')
        for x in sorted(near[:BLOCK_CAP]):
            d = x - a
            out.append(f'  {"+" if d > 0 else "-"}0x{abs(d):03X}  {ctx_name(con, names, x)}')
    else:
        out.append(f'same block: no db-named function within +-0x{BLOCK_RANGE:X} bytes of {h(a)}')
    return out


def f32s(w):
    b = struct.pack('>I', w)
    x = struct.unpack('>f', b)[0]
    if math.isnan(x):
        return 'nan'
    if math.isinf(x):
        return 'inf' if x > 0 else '-inf'
    for p in range(1, 10):
        s = f'{x:.{p}g}'
        try:
            if struct.pack('>f', float(s)) == b:
                break
        except OverflowError:   # near FLT_MAX a short decimal rounds past it (e.g. 3.4e38 -> '4e+38'): more digits
            continue
    if abs(x) >= 1e7 or abs(x) < 1e-4:
        return s
    s = repr(float(s))   # the shortest decimal that gives these bits, without forced exponents (-120.0, not -1.2e+02)
    return s[:-2] if s.endswith('.0') else s


def word32(con, img, w, names):
    """One word for an f32 row: the float, or what it obviously is when a float reading is meaningless."""
    if 0 < w < 0x00800000:
        return f'{w}(int)'          # a denormal as a float: a small integer
    if w > 0xFF800000:
        return f'{w - (1 << 32)}(int)'   # NaN as a float: a small negative integer
    if ptr_note(con, img, w, names):
        return '(ptr)'
    k = known32(w)
    return f32s(w) + (f' (= {k})' if k else '')


def f64s(x):
    return 'nan' if math.isnan(x) else ('inf' if x > 0 else '-inf') if math.isinf(x) else repr(x)


def known32(w):
    for name, v in KNOWN:
        for sign, nm in ((1, name), (-1, '-' + name)):
            t = struct.unpack('>I', struct.pack('>f', sign * v))[0]
            if w == t:
                return nm
            if abs(w - t) == 1:
                return '~' + nm
    return None


def known64(x):
    for name, v in KNOWN:
        for sign, nm in ((1, name), (-1, '-' + name)):
            if x == sign * v or abs(x - sign * v) <= 1e-15 * abs(v):
                return nm
    return None


def plausible64(x):
    return x == 0.0 or (math.isfinite(x) and 1e-10 <= abs(x) <= 1e12)


def ptr_note(con, img, w, names):
    """What a 32-bit word points at, when it is an address inside the image (else None)."""
    if w is None or not img.inside(w) or w < img.base + 0x1000:
        return None
    fr = func_row(con, w)
    if fr is not None:
        return '-> ' + names.label(w)
    st = con.execute('SELECT text FROM strings WHERE addr = ?', (w,)).fetchone()
    if st:
        t = st['text']
        return '-> ' + json.dumps(t[:DIGEST_STR_CHARS] + ('...' if len(t) > DIGEST_STR_CHARS else ''))
    vt = con.execute('''SELECT c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td WHERE v.vt = ?''',
                     (w,)).fetchone()
    if vt:
        return f'-> vtable of {vt["name"]}'
    if w % 4 == 0:
        return f'-> {h(w)} ({img.describe(w)})'
    return None


def value_at(con, img, v, op, names):
    """The value an access `op` reads / writes at address v, as text (image value at load time)."""
    base_op = op[:-1] if op.endswith('x') else op
    if base_op.startswith(('lfs', 'stfs')):
        w = img.u32(v)
        if w is None:
            return 'outside the image'
        k = known32(w)
        return f'f32 {f32s(w)}' + (f' (= {k})' if k else '') + f'  [{w:08X}]'
    if base_op.startswith(('lfd', 'stfd')):
        raw = img.raw(v, 8)
        if raw is None:
            return 'outside the image'
        x = struct.unpack('>d', raw)[0]
        k = known64(x)
        return f'f64 {f64s(x)}' + (f' (= {k})' if k else '') + f'  [{raw.hex().upper()}]'
    if base_op.startswith(('lhz', 'lha', 'sth')):
        raw = img.raw(v, 2)
        return 'outside the image' if raw is None else f'u16 0x{int.from_bytes(raw, "big"):04X} ({int.from_bytes(raw, "big")})'
    if base_op.startswith(('lbz', 'stb')):
        raw = img.raw(v, 1)
        return 'outside the image' if raw is None else f'u8 0x{raw[0]:02X} ({raw[0]})'
    if base_op.startswith(('ld', 'std')):
        raw = img.raw(v, 8)
        return 'outside the image' if raw is None else f'u64 0x{raw.hex().upper()}'
    if base_op.startswith(('lv', 'stv')):
        ws = [img.u32(v + 4 * i) for i in range(4)]
        return ('vector ' + ' '.join('--------' if x is None else f'{x:08X}' for x in ws) + '  f32 '
                + ' '.join('-' if x is None else f32s(x) for x in ws))
    w = img.u32(v)
    if w is None:
        return 'outside the image'
    p = ptr_note(con, img, w, names)
    return f'u32 0x{w:08X}' + (f' {p}' if p else f' ({w if w < 0x80000000 else w - (1 << 32)})')


def writes_reg(mn, parts, reg):
    if not parts:
        return False
    if mn.startswith(NO_GPR_DEST):
        return False
    return parts[0] == reg or (mn.endswith('ux') and len(parts) == 3 and parts[1] == reg)


def is_call(mn):
    m = mn.rstrip('+-')
    return m[0] == 'b' and (m.endswith('l') or m.endswith('ctrl') or m.endswith('lrl'))


def base_reads(insns, i0, reg):
    """Accesses through register `reg` after instruction i0 (a lis+addi that put a data address in it): a linear
    scan of the listing that stops where `reg` is overwritten, at a call when `reg` is volatile, or at the end of
    the function. Linear = not flow-checked: an approximation (branches are not followed)."""
    out = []
    for site, mn, ops in insns[i0 + 1:]:
        parts = ops.split(',') if ops else []
        m = MEM_RX.match(ops)
        if m and m.group(3) == reg:
            out.append((int(m.group(2), 0), mn, site))
            if mn.endswith('u'):
                break
        elif mn == 'addi' and len(parts) == 3 and parts[1] == reg:
            try:
                out.append((int(parts[2], 0), 'addi', site))
            except ValueError:
                pass
        elif mn.endswith('x') and len(parts) == 3 and reg in parts[1:] and mn[0] in 'ls':
            out.append((None, mn, site))
        if writes_reg(mn, parts, reg) or (m and m.group(3) == reg and mn.endswith('u')):
            break
        if is_call(mn) and reg in VOLATILE:
            break
    return out


def section_constants(con, a, img, names, insns, offset_cap, const_cap):
    """5b: the data at every address the function builds with a certain lis pair (image values at load time)."""
    rows = con.execute("SELECT value, op, site FROM imms WHERE func = ? AND op LIKE 'lis+%' ORDER BY site",
                       (a,)).fetchall()
    n_unsure = sum(1 for r in rows if r['op'].endswith('?'))
    groups = {}
    for r in rows:
        if not r['op'].endswith('?'):
            groups.setdefault(r['value'], []).append((r['op'][4:], r['site']))
    idx = {s: i for i, (s, _, _) in enumerate(insns)}
    entries, outside, strs = [], [], []
    for v, uses in groups.items():
        ops = {o for o, _ in uses}
        name, kind = img.section(v)
        if ops == {'ori'} and kind not in ('data', 'rdata', 'bss'):
            continue   # a 32-bit constant built with lis+ori (section 1 lists it)
        if not img.inside(v):
            outside.append(v)
        elif con.execute('SELECT 1 FROM strings WHERE addr = ?', (v,)).fetchone():
            strs.append(v)
        else:
            entries.append((min(OP_PRIORITY.get(o, 3) for o in ops), min(s for _, s in uses), v, uses, kind))
    out = ['(image words at load time, from out\\harness\\index\\image.bin; the game may change them at run time, '
           'except in read-only sections)']
    if not entries and not outside and not strs:
        return out + ['  (the function builds no data address with a lis pair)'] + \
            ([f'  ({n_unsure} uncertain lis pairs left out)'] if n_unsure else [])
    keep = sorted(entries)[:const_cap]
    keep.sort(key=lambda e: e[1])
    for _, _, v, uses, kind in keep:
        by_op = {}
        for o, s in uses:
            by_op.setdefault(o, []).append(h(s))
        opstr = '; '.join(f'lis+{o} @' + ', @'.join(ss[:3]) + (' ...' if len(ss) > 3 else '') for o, ss in by_op.items())
        if kind == 'code':
            c = q.containing(con, v)
            where = f'{names.label(c["addr"])}+0x{v - c["addr"]:X}' if c and v != c['addr'] else names.label(v)
            out.append(f'{h(v)}  [code]  {opstr}  -> code address {where} (a pointer to code is built)')
            continue
        head = f'{h(v)}  [{img.describe(v)}]  {opstr}'
        mem = [o for o in by_op if o != 'addi' and o != 'ori']
        if mem and kind != 'bss':
            head += ('  value at load ' if mem[0].startswith('st') else '  reads ') + value_at(con, img, v, mem[0], names)
        reads = []
        if 'addi' in by_op:
            for s in [int(x, 16) for x in by_op['addi']]:
                i0 = idx.get(s)
                if i0 is not None:
                    reads += base_reads(insns, i0, insns[i0][2].split(',')[0])
        vt = con.execute('SELECT v.nslots, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td WHERE v.vt = ?',
                         (v,)).fetchone()
        out.append(head)
        if vt:
            out.append(f'  vtable of RTTI class {vt["name"]} ({vt["nslots"]} slots)')
        if kind == 'bss':
            out.append('  (BSS: zero at load; the value is written at run time)' +
                       (f'; accessed through the register at offsets '
                        # {d:+d}: '+8' as before, and '-4' for a negative offset ('+-4' broke the sort)
                        + ', '.join(f'{d:+d}' for d in sorted({d for d, _, _ in reads if d is not None}))[:200]
                        if reads else ''))
            continue
        dbl_table = any(mn.startswith(('lfd', 'stfd')) for _, mn, _ in reads)   # a table read as doubles
        ws = [img.u32(v + 4 * i) for i in range(4)]
        if all(w == 0 for w in ws):
            out.append('  hex  00000000 x4 (all zero at load)')
        else:
            out.append('  hex  ' + ' '.join('--------' if w is None else f'{w:08X}' for w in ws))
            out.append('  f32  ' + '  '.join(word32(con, img, w, names) for w in ws if w is not None))
            dbl_used = any(o.startswith(('lfd', 'stfd', 'ld', 'std')) for o in by_op) or dbl_table
            sgl_used = any(o.startswith(('lfs', 'stfs')) for o in by_op)
            dw = [img.raw(v + 8 * i, 8) for i in range(2)]
            dv = [struct.unpack('>d', x)[0] for x in dw if x is not None]
            if dv and (dbl_used or (not sgl_used and all(plausible64(x) for x in dv) and any(x != 0 for x in dv))):
                out.append('  f64  ' + '  '.join(f64s(x) + (f' (= {known64(x)})' if known64(x) else '') for x in dv)
                           + ('' if dbl_used else '  (only meaningful if these are doubles)'))
            pn = [(i, ptr_note(con, img, w, names)) for i, w in enumerate(ws)]
            pn = [f'w{i} {p}' for i, p in pn if p]
            if pn:
                out.append('  ptrs ' + '; '.join(pn))
        if reads:
            reg = insns[idx[int(by_op['addi'][0], 16)]][2].split(',')[0]
            uniq = {}
            for d, mn, site in reads:
                uniq.setdefault((d, mn), []).append(site)
            items = sorted(uniq.items(), key=lambda kv: (kv[0][0] is None, kv[0][0] or 0, kv[0][1]))
            out.append(f'  accesses through {reg} after the lis+addi (linear scan of the listing, approximate: '
                       f'{len(items)} distinct):')
            for (d, mn), sites in items[:offset_cap]:
                at = f' @{h(sites[0])}' + (f' (+{len(sites) - 1} more)' if len(sites) > 1 else '')
                if d is None:
                    out.append(f'    indexed {mn}{at}: offset computed at run time')
                elif mn == 'addi':
                    t = (v + d) & 0xFFFFFFFF
                    ws2 = [img.u32(t + 4 * i) for i in range(4)]
                    dv2 = [struct.unpack('>d', x)[0] for x in (img.raw(t, 8), img.raw(t + 8, 8)) if x is not None]
                    out.append(f'    +{d:<5} addi{at}: address {h(t)}, hex '
                               + ' '.join('--------' if w is None else f'{w:08X}' for w in ws2)
                               + ('  f64 ' + '  '.join(f64s(x) + (f' (= {known64(x)})' if known64(x) else '')
                                                       for x in dv2) if dbl_table and dv2 else ''))
                else:
                    out.append(f'    +{d:<5} {mn:<5}{at}: {value_at(con, img, (v + d) & 0xFFFFFFFF, mn, names)}')
            if len(items) > offset_cap:
                out.append(f'    ... {len(items) - offset_cap} more accesses (cap {offset_cap})')
    if len(entries) > const_cap:
        cut = sorted(entries)[const_cap:]
        out.append(f'... {len(cut)} more data addresses (cap {const_cap}; float loads kept first): '
                   + ', '.join(h(e[2]) for e in sorted(cut, key=lambda e: e[1])[:12]) + (' ...' if len(cut) > 12 else ''))
    if strs:
        out.append(f'{len(strs)} string address(es) built: see section 6')
    if outside:
        out.append('not in the image (a 32-bit constant, or a run-time address: heap, GPU, hardware): '
                   + ', '.join(h(v) for v in outside[:OUTSIDE_CAP]) + (' ...' if len(outside) > OUTSIDE_CAP else ''))
    if n_unsure:
        out.append(f'({n_unsure} uncertain lis pairs left out: the lis value may not reach them on every path)')
    return out


def parse_insns(lines):
    out = []
    for x in lines:
        m = INSN_RX.match(x)
        if m:
            out.append((int(m.group(1), 16), m.group(2), (m.group(3) or '').strip()))
    return out


def render(con, fn, batch, batch_addrs, db_by_addr, covf, args, img, forbidden, level=0, version=EXPORT_VERSION,
           block=None):
    """block (a BlockIndex) switches on section 2b (export v3)."""
    a = int(fn['address'], 16)
    digest_cap, inline, offset_cap, const_cap = TRIM[level]
    names = Names(con, a, batch_addrs, forbidden, db_by_addr)
    fn_lines, r, strings = section_fn(con, a, args)
    caller_lines, caller_addrs = section_callers(con, a, args, covf)
    callee_lines, callee_addrs = section_callees(con, a, covf)
    nb = section_neighbours(caller_addrs, callee_addrs, a, batch_addrs, db_by_addr)
    full, n = listing(con, a, 1 << 20)
    lst, _ = listing(con, a, LISTING_CAP)
    if n > LISTING_CAP:
        lst.append(f'TRUNCATED at {LISTING_CAP} of {n} instructions')
    insns = parse_insns(full)

    def fenced(lines):
        return ['```text'] + lines + ['```', '']

    out = [f'# {fn["address"]} (sub_{a:08X}), batch {batch}', '',
           f'Context for labelling (export {version}). Derived from the game: stays in out\\ (never copy '
           'listings or strings into db\\ or docs\\).' + (f' Trimmed to fit {MAX_LINES} lines (level {level}).'
                                                          if level else ''), '',
           '## 1. Summary (q.py fn)', ''] + fenced(fn_lines)
    out += ['## 2. Callers (q.py callers)', ''] + fenced(caller_lines)
    if block is not None:
        out += ['## 2b. Caller context (for the purpose only: NOT this function\'s code)', ''] + \
            fenced(section_caller_context(con, a, names, covf, block, img))
    out += ['## 3. Callees', ''] + fenced(callee_lines)
    out += ['## 3b. Callee digest', ''] + fenced(section_digest(con, a, names, digest_cap, inline))
    if fn['n_callers'] == 0:
        out += ['## 3c. Data words / code pointing here (no direct callers)', ''] + \
            fenced(section_pointing(con, a, img, names))
    out += ['## 4. Known neighbour names (db name + system only)', ''] + fenced(nb)
    out += [f'## 5. Listing (list_function.py, cap {LISTING_CAP} instructions)', ''] + fenced(lst)
    out += ['## 5b. Data constants', ''] + fenced(section_constants(con, a, img, names, insns, offset_cap, const_cap))
    out += ['## 6. String references', ''] + fenced(section_strings(strings))
    return out


def render_fit(con, fn, batch, batch_addrs, db_by_addr, covf, args, img, forbidden, version=EXPORT_VERSION,
               block=None):
    """render(), trimmed (digests first, then constants) when the file would reach MAX_LINES."""
    for level in range(len(TRIM)):
        text = render(con, fn, batch, batch_addrs, db_by_addr, covf, args, img, forbidden, level, version, block)
        if len(text) < MAX_LINES - 5:
            break
    return text, level


def hidden_names(pilot, b, db_by_addr):
    """{name: owner address} that no file of batch b may contain: the db names and labels A / B of the batch's own
    functions (every db file); for the comparison batch (b10) also every name and label of db\\match_setup.json."""
    fns = [f for f in pilot['functions'] if f['batch'] == b]
    forbidden = {}

    def add(e, owner):
        forbidden[e.get('name')] = owner
        for x in e.get('evidence', []):
            m = LABELS_RX.search(x)
            if m:
                forbidden.setdefault(m.group(1), owner)
                forbidden.setdefault(m.group(2), owner)

    compare = False
    for fn in fns:
        for file, e in db_by_addr.get(int(fn['address'], 16), []):
            add(e, fn['address'])
            compare |= file == common.COMPARE_DB
    if compare:
        for x, entries in db_by_addr.items():
            for file, e in entries:
                if file == common.COMPARE_DB and e.get('name') not in forbidden:
                    add(e, h(x))
    forbidden.pop(None, None)
    return forbidden


def hidden_labels(fns, db_by_addr):
    out = []
    for fn in fns:
        if not fn['in_db']:
            continue
        for file, e in db_by_addr.get(int(fn['address'], 16), []):
            m = next((LABELS_RX.search(x) for x in e.get('evidence', []) if LABELS_RX.search(x)), None)
            out.append({'address': fn['address'], 'file': f'db/{file}', 'name': e.get('name'),
                        'summary': e.get('summary'), 'system': e.get('system'), 'confidence': e.get('confidence'),
                        'label_A': m.group(1) if m else None, 'label_B': m.group(2) if m else None,
                        'label_B_confidence': m.group(3) if m else None, 'source': e.get('source')})
    return out


def check(pilot, batches, db_by_addr, root=BATCH_DIR, version=EXPORT_VERSION):
    """The self-check. Returns a list of problems (empty = pass) and prints what it checked. pilot: a document with
    'functions' (pilot.json, or in bulk mode the selection's functions without the skipped ones)."""
    problems = []
    n_files = 0
    rx = {}

    def found(name, text):
        if name not in rx:
            rx[name] = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(name) + r'(?![A-Za-z0-9_])', re.I)
        return rx[name].search(text) is not None

    for b in batches:
        d = os.path.join(root, b)
        fns = [f for f in pilot['functions'] if f['batch'] == b]
        by_file = {f'{fn["address"]}.md': fn for fn in fns}
        forbidden = hidden_names(pilot, b, db_by_addr)
        is_compare = any(f == common.COMPARE_DB for fn in fns for f, _ in db_by_addr.get(int(fn['address'], 16), []))
        compare = sorted({e.get('name') for es in db_by_addr.values() for f, e in es
                          if f == common.COMPARE_DB and e.get('name')}) if is_compare else []
        missing_compare = [n for n in compare if n not in forbidden]
        if missing_compare:
            problems.append(f'{b}: db\\{common.COMPARE_DB} names not on the hidden list: {missing_compare[:5]}')
        files = sorted(glob.glob(os.path.join(d, '*.md')))
        want = set(by_file) | {'index.md'}
        have = {os.path.basename(p) for p in files}
        if want - have:
            problems.append(f'{b}: missing {sorted(want - have)}')
        if have - want:
            problems.append(f'{b}: unexpected {sorted(have - want)}')
        hits = own_hits = 0
        for p in files:
            base = os.path.basename(p)
            with open(p, encoding='utf-8') as f:
                text = f.read()
            nl = text.count('\n')
            if nl >= MAX_LINES:
                problems.append(f'{b}/{base}: {nl} lines (limit {MAX_LINES})')
            for name, owner in forbidden.items():
                if name and found(name, text):
                    hits += 1
                    problems.append(f'{b}/{base}: contains the label "{name}" of {owner}')
            fn = by_file.get(base)
            if fn is None:
                continue
            # the function's own db names / labels (any db file), checked on their own as well
            for _, e in db_by_addr.get(int(fn['address'], 16), []):
                own = [e.get('name')]
                for x in e.get('evidence', []):
                    m = LABELS_RX.search(x)
                    if m:
                        own += [m.group(1), m.group(2)]
                for name in own:
                    if name and found(name, text):
                        own_hits += 1
                        problems.append(f'{b}/{base}: contains its own db name / label "{name}"')
            # v2 structure: every function file has the digest and the constants; no-caller functions have 3c
            need = ['## 3b. Callee digest', '## 5b. Data constants', f'(export {version})']
            if fn['n_callers'] == 0:
                need.append('## 3c. ')
            if version != EXPORT_VERSION:
                need.append('## 2b. Caller context')
            for x in need:
                if x not in text:
                    problems.append(f'{b}/{base}: no "{x}"')
        n_files += len(have & set(by_file))
        print(f'check {b}: {len(have & want)} of {len(want)} files, {len(forbidden)} hidden label names searched'
              + (f' (incl. all {len(compare)} db\\{common.COMPARE_DB} names)' if compare else '')
              + f', {hits} hits, own-name hits {own_hits}')
    print(f'check: {n_files} function files in {len(batches)} batches')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--batches', help='comma-separated (default: all in pilot.json; bulk mode: all of the wave)')
    ap.add_argument('--check-only', action='store_true', help='only run the self-check on existing files')
    ap.add_argument('--selection', help='bulk mode (export v3): a selection.json written by pick_bulk.py')
    ap.add_argument('--wave', help='bulk mode: the wave to export (w01, w02, ...); required with --selection')
    ap.add_argument('--out', help='output directory (default: pilot out\\harness\\labelling\\batches; bulk '
                                  'out\\harness\\labelling\\bulk\\<wave>)')
    args = ap.parse_args()
    t0 = time.perf_counter()
    db_by_addr = {}
    for a, file, e in common.db_entries():
        db_by_addr.setdefault(a, []).append((file, e))
    if args.selection:
        return main_bulk(args, t0, db_by_addr)
    if args.wave:
        ap.error('--wave needs --selection')
    base = args.out or BATCH_DIR
    with open(common.PILOT, encoding='utf-8') as f:
        pilot = json.load(f)
    batches = args.batches.split(',') if args.batches else list(pilot['batches'])

    if not args.check_only:
        con, img, covf, qargs = open_inputs()
        stats = []
        for b in batches:
            fns = [f for f in pilot['functions'] if f['batch'] == b]
            d = os.path.join(base, b)
            prepare_dir(d, fns)
            batch_addrs = {int(fn['address'], 16) for fn in fns}
            forbidden = hidden_names(pilot, b, db_by_addr)
            idx = [f'# {b}: {len(fns)} functions, label in this order (leaf first)', '',
                   'address      instructions  callees']
            for fn in fns:
                ind = f' (+{fn["n_indirect_calls"]} indirect)' if fn['n_indirect_calls'] else ''
                idx.append(f'{fn["address"]}  {fn["n_instructions"]:>12}  {fn["n_callees"]}{ind}')
                text, level = render_fit(con, fn, b, batch_addrs, db_by_addr, covf, qargs, img, forbidden)
                stats.append((len(text), f'{b}/{fn["address"]}', level))
                write_lines(os.path.join(d, f'{fn["address"]}.md'), text)
            write_lines(os.path.join(d, 'index.md'), idx)
            hl = os.path.join(d, 'hidden_labels.json')
            labels = hidden_labels(fns, db_by_addr)
            if labels:
                with open(hl, 'w', encoding='utf-8', newline='\n') as f:
                    json.dump({'note': 'Existing db labels of this batch, for the comparison step only. NEVER give '
                                       'this file to a labeller.', 'labels': labels}, f, indent=1)
                    f.write('\n')
            elif os.path.exists(hl):
                os.remove(hl)
            print(f'export {b}: {len(fns)} functions' + (f', hidden_labels.json ({len(labels)})' if labels else ''))
        print_stats(stats)
        print(f'export: wrote {base} in {time.perf_counter() - t0:.1f} s')

    problems = check(pilot, batches, db_by_addr, base)
    for p in problems:
        print(f'check FAILED: {p}')
    print(f'check: {"PASS" if not problems else f"{len(problems)} problems"}; total {time.perf_counter() - t0:.1f} s')
    return 1 if problems else 0


def open_inputs():
    con = common.connect()
    for w in common.index_warnings(con):
        print(f'export: WARNING {w}', file=sys.stderr)
    img = common.GuestImage()
    for line in img.verify(con):
        print(f'export: image check: {line}')
    covf = (q.load_coverage() or {}).get('functions', {})
    qargs = argparse.Namespace(limit=25, maybe=False, json=False)
    return con, img, covf, qargs


def prepare_dir(d, fns):
    os.makedirs(d, exist_ok=True)
    keep = {f'{fn["address"]}.md' for fn in fns} | {'index.md'}
    for p in glob.glob(os.path.join(d, '*.md')):   # files of an earlier selection
        if os.path.basename(p) not in keep:
            os.remove(p)


def write_lines(path, lines):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def print_stats(stats):
    stats.sort()
    n = [x[0] for x in stats]
    print(f'export: lines per file min {n[0]}, median {n[len(n) // 2]}, max {n[-1]} ({stats[-1][1]}); '
          f'trimmed {sum(1 for x in stats if x[2])}')


def main_bulk(args, t0, db_by_addr):
    """Bulk mode: one wave of a pick_bulk.py selection, export v3 (v2 + section 2b), one directory per batch."""
    if not args.wave:
        raise SystemExit('export: --selection needs --wave (w01, w02, ...)')
    with open(args.selection, encoding='utf-8') as f:
        sel = json.load(f)
    wave_fns = [fn for fn in sel['functions'] if fn.get('wave') == args.wave]
    if not wave_fns:
        raise SystemExit(f'export: no functions of wave {args.wave} in {args.selection}')
    batches = list(dict.fromkeys(fn['batch'] for fn in wave_fns))
    if args.batches:
        want = args.batches.split(',')
        bad = [b for b in want if b not in batches]
        if bad:
            raise SystemExit(f'export: {bad} not in wave {args.wave}')
        batches = [b for b in batches if b in want]
    base = args.out or os.path.join(BULK_DIR, args.wave)
    # a selected function that has a db entry by now was labelled since the selection: skipped, never exported
    skipped = {fn['address']: sorted({file for file, _ in db_by_addr.get(int(fn['address'], 16), [])})
               for fn in wave_fns if int(fn['address'], 16) in db_by_addr}
    for a, files in skipped.items():
        print(f'export: WARNING {a} has a db entry now ({", ".join(files)}): skipped (selection older than the db)',
              file=sys.stderr)
    doc = {'functions': [fn for fn in wave_fns if fn['address'] not in skipped]}

    if not args.check_only:
        con, img, covf, qargs = open_inputs()
        block = BlockIndex(con, db_by_addr)
        stats = []
        wave_idx = [f'# Wave {args.wave}: {len(wave_fns)} functions in {len(batches)} batches (export '
                    f'{EXPORT_VERSION_BULK})', '', f'Selection: {args.selection}. Order: leaf first, then most-called '
                    'first (pick_bulk.py). Batches can run in parallel.', '', 'batch    functions  orders']
        for b in batches:
            all_b = [fn for fn in wave_fns if fn['batch'] == b]
            fns = [fn for fn in doc['functions'] if fn['batch'] == b]
            d = os.path.join(base, b)
            prepare_dir(d, fns)
            batch_addrs = {int(fn['address'], 16) for fn in fns}
            forbidden = hidden_names(doc, b, db_by_addr)
            idx = [f'# {b}: {len(fns)} functions (wave {args.wave}), label in this order (leaf first, then '
                   f'most-called)', '', 'order  address      instructions  callees  callers']
            for fn in fns:
                ind = f' (+{fn["n_indirect_calls"]} indirect)' if fn['n_indirect_calls'] else ''
                idx.append(f'{fn["order"]:>5}  {fn["address"]}  {fn["n_instructions"]:>12}  '
                           f'{str(fn["n_callees"]) + ind:<7}  {fn["n_callers"]}')
                text, level = render_fit(con, fn, b, batch_addrs, db_by_addr, covf, qargs, img, forbidden,
                                         EXPORT_VERSION_BULK, block)
                stats.append((len(text), f'{b}/{fn["address"]}', level))
                write_lines(os.path.join(d, f'{fn["address"]}.md'), text)
            sk = [fn['address'] for fn in all_b if fn['address'] in skipped]
            if sk:
                idx += ['', 'skipped (a db entry exists now: labelled since the selection): '
                        + ', '.join(f'{x} ({", ".join(skipped[x])})' for x in sk)]
            write_lines(os.path.join(d, 'index.md'), idx)
            wave_idx.append(f'{b}  {len(fns):>9}  {all_b[0]["order"]}-{all_b[-1]["order"]}'
                            + (f'  ({len(sk)} skipped: in db now)' if sk else ''))
            print(f'export {b}: {len(fns)} functions' + (f' ({len(sk)} skipped: in db now)' if sk else ''))
        write_lines(os.path.join(base, 'index.md'), wave_idx)
        print_stats(stats)
        print(f'export: wrote {base} in {time.perf_counter() - t0:.1f} s')

    problems = check(doc, batches, db_by_addr, base, EXPORT_VERSION_BULK)
    for p in problems:
        print(f'check FAILED: {p}')
    print(f'check: {"PASS" if not problems else f"{len(problems)} problems"}; total {time.perf_counter() - t0:.1f} s')
    return 1 if problems else 0


if __name__ == '__main__':
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
