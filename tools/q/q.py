"""Query the code index (out/harness/index/index.db, built by tools/q/build_index.py). Session 8, task 5.

    py tools/q/q.py [--json] COMMAND ...

  callers FUNC                 who calls / tail-calls / branches into FUNC; data words pointing at it; vtable slots
  callees FUNC [--depth N]     direct callees (tree for N > 1); --all keeps __savegprlr_/__restgprlr_ helpers
  str REGEX [-i]               strings matching REGEX and the functions referencing them (+ data pointers to them)
  imm VALUE                    functions using an immediate (lis pairs give full 32-bit values)
  rtti NAME [--slots]          RTTI classes whose name contains NAME -> vtables -> slot functions, base classes
  addr ADDR | --host X         guest address -> function + offset (+ string/data info); --host wwe2k14.exe+0x..
                               or a host RVA -> host symbol -> guest function (needs the map of THAT build)
  db FUNC                      db/*.json entry + system + the probe notes' lodged claims (db/claims/*.json) + docs/systems|modules
                               mentions (read live; FUNC may also be a data address or name that has a db entry or a claim)
  fn FUNC                      one-screen summary: size, callers/callees, strings, constants, RTTI, db, coverage
  vcall OFF [--class VT] [--all]  every virtual call site with that vtable slot byte offset (0x34 = slot 13), receiver / class where the code shows it (qx7.py)
                               vcall OFF --reg rN [--value V] = the value of that argument register at every site, folded (histogram)
  args F [--reg rN] [--value V] [--in CALLER]  one line per call site of F: the argument registers F reads, folded (constant / image address + string / in_rN / expression) (qargs.py)
  nodes [CATEGORY]             the draw-node registry: sub_82C30A10 registration sites -> category -> node vtable -> slot 1 (qx7.py)
                                 nodes CAT --state = per node, the recorder-wrapper calls of its draw method in listing order, values named, and the state at its exits per path (qenum.py)
{{usage:u20}}
  asm FUNC|ADDR [FROM TO] [--into FROM TO]  per-address instruction window of the generated listing, branch targets marked; --into = which branches enter a range from outside (qx6.py)
                               asm FUNC --at OFF[..OFF2] [--ctx N] (or FUNC+OFF) = a window of a long function by offset from its start
                               asm FUNC [FROM TO] --writes|--reads rN|sp+D = every instruction that writes / reads the register or stack slot (qregs.py)
  decomp FUNC [--fields] [--callers N]   a compact mechanical view of one function: blocks with loads / stores / calls /
                               branches, provenance folded (tools/q/qdecomp.py; cache out/harness/index/decomp/;
                               docs/agents/q_decomp.md)
  switch FUNC                  every jump-table switch of FUNC: case -> target, the li constants / stores of constants / calls in each case (`decomp FUNC --cases` = the same; qgx.py)
  text ID [--lang N] [--all]   text id -> string from the game's string tables (string.pac; qgx.py)
  csdt [ERA [KEY]] [--find K]  evtdata.pac /CSDT caption tables: key, start / end frame, text (qgx.py)
{{usage:u31}}
  field OFF --stores           `field OFF --writes` + indexed stores (stbx / sthx / stwx ...) + memcpy writers (qstores.py)
  ours includes F --included-by | --transitive | --closure   the include graph of our src (qgx.py)
  info                         what the index was built from, counts, staleness
{{usage:u35}}
  callers FUNC --in CALLER     only the call sites inside one function (--in)
  show FILE:N-M --ref REF      the lines as they were at a git ref (a branch, tag or commit)
{{usage:u103}}
  ours body FUNC | outline FILE | enclosing FILE:LINE   a function's lines | a file's functions / types (no bodies) | the function holding a line (tools/q/qsym.py, text)
  py body NAME                 the text of a python / ps1 tool function (def span)
  py def|refs|callers|callees|writes|reads|fn NAME   our python / ps1 tools first: `ours ... --tools` (tools/q/qtools.py: ast-based, this checkout's own files)
{{usage:u114}}
{{usage:new}}

FUNC: sub_82CB1F50 | 0x82CB1F50 | 82cb1f50 | a mapped name (__imp__XamInputGetState, xstart) | a db name
(gpu_wait_poll_step). An address inside a function resolves to that function (noted).
VALUE/ADDR: 0x... and anything with a hex letter are hex; a bare 8-digit token is hex (an address); for `addr` every
bare token is hex; for `imm` a bare 1-7 digit token is decimal (100, -1) and 010 is refused as ambiguous. The
output says how it was read.
imm/addr/str/fn hide uncertain lis pairs (the lis value may not reach the use on every path; op ends in '?') and
say how many; --maybe shows them.
Every command takes --json (machine-readable, for agents) and --limit N. Errors: {"error": ...} on stdout with
--json (else `q: ...` on stderr), exit 2; a bug in q itself: the same with "internal error: ...", exit 3.
"""
import argparse
import bisect
import glob
import json
import os
import re
import sqlite3
import sys

ROUTING = """WHICH q COMMAND ANSWERS YOUR QUESTION (q -h = every command)
- not sure which? q find X [X2 ...]: tries every index (C++ / python name, file, path, cvar, env, log line, game address, sdk, code text, docs, fact)
  and says what matched, each list with its total; --in PATH narrows, --all shows everything. (q X with an unknown first word = q find X)
- several questions in one call: q multi "ours callers F" "show FILE:10-40" "decomp sub_X --fields" (each answered whole, with its exit code)
- who calls / refs / writes / reads a C++ or python name: ours refs|callers|writes|reads NAME (python: py refs NAME; importers: py refs MODULE)
- where defined / one screen: ours def|fn NAME; a file's functions: ours outline FILE; a function's lines: ours body NAME; lines: show FILE:N-M
- who names a script / exe: ours runs FILE; a path / string / number in src+tools: ours lit VALUE; any of several words: ours text W1 W2 [--and] [-E regex] [--in FILE]
  (a word in docs / briefs / CMake: ours text WORD --docs; at a branch: ours text WORD --branch REF)
- who prints / parses a log line: ours line "LINE"; a census field: ours census FIELD; env var: ours env NAME; cvar: ours cvar NAME
- includes: ours includes|includers FILE; SDK / SDL source: sdk def|refs NAME; lodged: fact WORDS; a note's section: note NAME --section H
- game code: decomp sub_X [--fields|--callers N]; field OFF [--writes|--stores]; callers|fn|addr|str|imm X; asm FUNC
{{route:r147}}
- a window inside one long game function: asm FUNC --at OFF (or FUNC+OFF); the call sites of X inside one function: callers X --in FUNC; a file at a git ref: show FILE:N-M --ref REF
{{route:new}}
(bare outline / body / refs / def / lit / text WORD mean `ours` ...)
Not answered by any of these? `q missed "<the question>"` logs the gap (voluntary; the list of what to build next), then use grep.
"""


# S111 QVERBS: the verb registry. tools/q/verbs/NAME.toml (one file per verb) holds a verb's usage lines, routing line, module, error classes; this file keeps only the
# special verbs (find multi switch text csdt decomp menu py missed asm args vcall nodes + the argparse ones). {{usage:SLOT}} / {{route:SLOT}} marker lines of the two
# texts are replaced by the registered lines (qverbs.expand); a new verb is a NEW file and never edits this one (tools/q/README.md "Adding a verb").
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qverbs  # noqa: E402
_REG_LIST, REG_WARNINGS = qverbs.load()
REG = {e['name']: e for e in _REG_LIST}
ROUTING = qverbs.expand(ROUTING, 'route', _REG_LIST)
__doc__ = qverbs.expand(__doc__, 'usage', _REG_LIST)


REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
DB = os.path.join(REPO, 'out', 'harness', 'index', 'index.db')
COVERAGE = os.path.join(REPO, 'out', 'harness', 'coverage', 'coverage.json')
HELPER_RE = re.compile(r'^__(save|rest)(gpr|fpr|vmx)')
IMAGE_BASE = 0x82000000


class QError(Exception):
    pass


def h(a):
    return f'0x{a:08X}' if a is not None else '?'


def connect(path):
    if os.path.abspath(path) == os.path.abspath(DB):
        # S110 QREBUILD: the newest COMPLETE index: a rebuild that found index.db held open by a reader left its file beside it (tools/q/qpublish.py)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qpublish
        p = qpublish.newest(path)
        path = p if os.path.exists(p) else path      # another reader put that copy in place a moment ago: it is index.db now
    if not os.path.exists(path):
        raise QError(f'no index at {path}: build it with  py tools\\q\\build_index.py')
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    con.row_factory = sqlite3.Row
    return con


def meta(con):
    return {r['key']: json.loads(r['value']) for r in con.execute('SELECT key, value FROM meta')}


def staleness(m, keys=('init_cpp', 'image')):
    """Warnings when the sources changed after the index was built (cheap: size/mtime + codegen fingerprint). S110 QREBUILD: the linker map is not
    one of them (it changes with every relink; host symbols: tools/q/qhostmap.py); an old index's recorded map is ignored."""
    warn = []
    gen = m.get('gen_dir') or os.path.join(REPO, 'generated', 'default')
    try:
        with open(os.path.join(gen, 'codegen.stamp'), encoding='utf-8') as f:
            fp = json.load(f).get('fingerprint', '')
        if fp != m.get('codegen_fingerprint'):
            warn.append(f'codegen changed since the index was built (fingerprint {m.get("codegen_fingerprint")} -> {fp})')
    except (OSError, ValueError):
        warn.append('cannot read codegen.stamp')
    for key in keys:
        sig = m.get(key)
        if not sig:
            continue
        try:
            st = os.stat(sig['path'])
            if st.st_size != sig['size'] or int(st.st_mtime) != sig['mtime']:
                warn.append(f'{key} changed since the index was built: {sig["path"]}')
        except OSError:
            warn.append(f'{key} missing: {sig["path"]}')
    if warn:
        warn.append('rebuild: py tools\\q\\build_index.py (~10 s)')
    return warn


_DB_CACHE = []


def load_db_entries():
    """[(addr, file, entry)] from db/*.json, read live (cached per process). A file that cannot be parsed (e.g. being
    written by a labeller right now) is reported on stderr, never skipped silently: 'no db entry' must mean none."""
    if _DB_CACHE:
        return _DB_CACHE[0]
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, 'db', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            print(f'q: WARNING cannot parse {os.path.relpath(p, REPO)} ({e}): its entries are missing from this answer',
                  file=sys.stderr)
            continue
        if not isinstance(d, (list, dict)):
            continue
        for e in d if isinstance(d, list) else d.get('functions', []):
            if isinstance(e, dict) and 'address' in e:
                try:
                    a = int(str(e['address']), 16)
                except ValueError:
                    continue
                out.append((a, os.path.basename(p), e))
    _DB_CACHE.append(out)
    return out


_CLAIMS_CACHE = []


def load_claims():
    """S111 LODGE: [claim row + {'file'}] from db/claims/*.json (one file per lodged probe note, tools/handback/lodge_note.py), read live and
    cached per process; an unparsable file is reported on stderr like a db file."""
    if _CLAIMS_CACHE:
        return _CLAIMS_CACHE[0]
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, 'db', 'claims', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            print(f'q: WARNING cannot parse {os.path.relpath(p, REPO)} ({e}): its claims are missing from this answer', file=sys.stderr)
            continue
        for r in d if isinstance(d, list) else []:
            if isinstance(r, dict):
                out.append({'file': 'claims/' + os.path.basename(p), **r})
    _CLAIMS_CACHE.append(out)
    return out


def _claim_addr(r, key):
    try:
        return int(str(r.get(key)), 16)
    except ValueError:
        return None


def claims_for(a):
    """The lodged claims on `a`: rows whose address is `a` or whose function is `a` (a claim inside the function)."""
    return [r for r in load_claims() if a in (_claim_addr(r, 'address'), _claim_addr(r, 'function'))]


def claim_line(r):
    where = f'+{r.get("offset")} ' if r.get('function') and r.get('offset') not in (None, '0x0') else ''
    return f'[{r.get("mark")}] {where}{r.get("claim")} (evidence: {r.get("evidence")}; note {r.get("note")})'


def resolve(con, s, allow_inside=True, allow_data=False):
    """FUNC argument -> (addr, note). allow_data: also accept an address that only a db entry names (a global or
    table, not a function: `q db` of a data entry)."""
    s = s.strip()
    row = con.execute('SELECT addr FROM funcs WHERE name = ?', (s,)).fetchone()
    if row:
        return row['addr'], None
    m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', s)
    if m:
        a = int(m.group(1), 16)
        if con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone():
            return a, None
        if allow_data and any(x == a for x, _, _ in load_db_entries()):
            return a, f'{h(a)} is a db data address, not a function'
        if allow_data and any(_claim_addr(r, 'address') == a and r.get('function') is None for r in load_claims()):
            return a, f'{h(a)} is a data address a lodged claim names (db/claims), not a function'
        if allow_inside:
            r = containing(con, a)
            if r:
                return r['addr'], f'{h(a)} is inside {r["name"]} (+0x{a - r["addr"]:X})'
        raise QError(f'{s}: not a function start and not inside any function (for data: q addr {h(a)})')
    for a, fname, e in load_db_entries():
        if e.get('name') == s:
            if allow_data or con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone():
                return a, f'db name {s} ({fname})'
            raise QError(f'{s} is a db entry for a data address ({h(a)}, {fname}), not a function: '
                         f'use  q db {s}  or  q addr {h(a)}')
    raise QError(f'{s}: unknown function (use sub_XXXXXXXX, an address, a mapped name or a db name)')


def containing(con, a):
    r = con.execute('SELECT * FROM funcs WHERE addr <= ? AND has_body = 1 ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
    if r and a < r['addr'] + r['size']:
        return r
    return None


def fname(con, a, cache={}):
    if a is None:
        return '?'
    if a not in cache:
        r = con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
        cache[a] = r['name'] if r else h(a)
    return cache[a]


# ---------------------------------------------------------------- commands

def cmd_callers(con, a, args):
    rows = con.execute('SELECT src, target, site, kind FROM edges WHERE dst = ? ORDER BY src, site', (a,)).fetchall()
    if getattr(args, 'in_func', None):      # S111 QGAPS: `callers X --in FUNC` = the call sites of X inside one function
        inside, _note = resolve(con, args.in_func)
        rows = [r for r in rows if r['src'] == inside]
    by = {}
    for r in rows:
        by.setdefault(r['src'], []).append({'site': h(r['site']), 'kind': r['kind'],
                                            'target': h(r['target']) if r['target'] != a else None})
    data = con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func' ORDER BY at", (a,)).fetchall()
    slots = rtti_slots_of(con, a)
    cov = load_coverage()
    covf = cov.get('functions', {}) if isinstance(cov, dict) and 'functions' in cov else None
    return {'func': fname(con, a), 'addr': h(a), 'n_sites': len(rows), 'n_callers': len(by),
            'callers': [{'func': fname(con, s), 'addr': h(s), 'sites': v,
                         **({'ran_in': covf.get(h(s), [])} if covf is not None else {})} for s, v in by.items()],
            'data_pointers': [h(r['at']) for r in data], 'vtable_slots': slots, 'indirect': indirect_of(con, a)}


def indirect_of(con, a):
    """S110 ICALL: the callers that reach `a` through an indirect call (qx7 icall tables) and the count of sites this list cannot see; {'error': text} when the tables are not there"""
    import qx7
    try:
        r = qx7.icall_callers(con, a)
    except (qx7.XError, sqlite3.Error) as e:
        return {'error': str(e)}
    r['sites'] = [{**s, 'site': h(s['site']), 'func': fname(con, s['func'])} for s in r['sites']]
    return r


def print_indirect(r):
    ind = r.get('indirect')
    if ind is None:
        return
    if 'error' in ind:
        print(f'  indirect callers: NOT KNOWN ({ind["error"]})')
        return
    # S110 CLASSFLOW: how = the rule that resolved the site (class, this-class, ... or the class-flow letters a b c d e h); an L = the result rests on memory the scan cannot
    # prove it sees every write to (a global: f; a pointer field: b's field read; a stack slot: h): a candidate
    def kind_of(s):
        return 'sure' if s['status'] != 'cand' else 'candf' if str(s.get('how') or '').endswith('L') else 'cand'
    for kind, head in (('sure', 'indirect callers (a virtual / pointer call that reaches it'),
                       ('cand', 'candidate indirect callers (table matched by field offset only; class not checked'),
                       ('candf', 'candidate indirect callers (class flow through a global, a pointer field or a stack slot (rules f / b / h): writes the scan does not see are not excluded')):
        ss_all = [s for s in ind['sites'] if kind_of(s) == kind]
        by = {}
        for s in ss_all:
            by.setdefault(s['func'], []).append(s)
        if by:
            print(f'  {head}; {len(ss_all)} sites in {len(by)} functions):')
            for f, ss in by.items():
                print(f'    {f:<28} ' + ', '.join(f'{s["site"]} slot +{s["off"]}' + ('' if s['status'] in ('exact', 'cand') else f' (1 of {s["n"]} targets)') + (f' [{s["how"]}]' if s.get('how') else '') for s in ss))
    if ind['n_incomplete'] or ind['n_open']:
        offs = ', '.join(f'+{o}' for o in ind['offsets'])
        print(f'  incomplete: {ind["n_incomplete"]} sites call slot {offs or "(none)"} (class not shown)'
              + (f' ({", ".join(f"+{o}: {n}" for o, n in ind["offsets"].items())})' if len(ind['offsets']) > 1 else '')
              + f'; {ind["n_open"]} with CTR unknown')


def qfactcard_caller_fact(c, args, shown):
    """S111 FACTCARD: one settled fact under a caller (the claim that cites its call site, else its heaviest); at most MAX_CALLER_FACTS in all; returns the running count"""
    import qfactcard
    fm = qfactcard.mode_of(args)
    if fm == 'none' or (fm == 'card' and shown >= qfactcard.MAX_CALLER_FACTS):
        return shown
    ca = int(c['addr'], 16)
    facts = qfactcard.collect(ca, None, None, 'callers', fm)
    line = None
    for s in c['sites']:     # the fact is the CALLER's: at its call site, else the caller's own line
        so = int(s['site'], 16) - ca
        line = line or qfactcard.site_fact(facts, so, fm, f'caller +0x{so:X}: ')
    line = line or qfactcard.fn_fact(facts, fm, 'caller: ')
    if line:
        print('      ' + line)
        shown += 1
    return shown


def print_callers(r, args):
    shown = 0
    print(f'{r["func"]}: {r["n_sites"]} call sites in {r["n_callers"]} functions')
    for c in r['callers'][:args.limit]:
        sites = ', '.join(f'{s["site"]} {s["kind"]}' + (f' (into {s["target"]})' if s['target'] else '') for s in c['sites'])
        ran = f'  [ran: {",".join(c["ran_in"]) or "never seen"}]' if 'ran_in' in c else ''
        print(f'  {c["func"]:<28} {sites}{ran}')
        shown = qfactcard_caller_fact(c, args, shown)
    if len(r['callers']) > args.limit:
        print(f'  ... {len(r["callers"]) - args.limit} more (--limit)')
    if r['data_pointers']:
        dp = r['data_pointers']
        print(f'  data words pointing at it: {len(dp)}: ' + ', '.join(dp[:12]) + (' ...' if len(dp) > 12 else ''))
    for s in r['vtable_slots']:
        print(f'  vtable slot: {s["class"]} vtable {s["vtable"]} slot {s["slot"]}')
    print_indirect(r)
    import qseen; qseen.print_observed(r)   # S110 SEEN


def rtti_slots_of(con, a):
    rows = con.execute('''SELECT s.vt, s.idx, c.name FROM rtti_slot s JOIN rtti_vtable v ON v.vt = s.vt
                          JOIN rtti_class c ON c.td = v.td WHERE s.func = ? ORDER BY c.name, s.vt''', (a,)).fetchall()
    return [{'class': r['name'], 'vtable': h(r['vt']), 'slot': r['idx']} for r in rows]


def callees_of(con, a, include_helpers):
    rows = con.execute('SELECT dst, target, site, kind FROM edges WHERE src = ? ORDER BY site', (a,)).fetchall()
    out = {}
    for r in rows:
        n = fname(con, r['dst'])
        if not include_helpers and HELPER_RE.match(n):
            continue
        out.setdefault(r['dst'], []).append({'site': h(r['site']), 'kind': r['kind'],
                                             'target': h(r['target']) if r['target'] != r['dst'] else None})
    return out


def cmd_callees(con, a, args):
    depth = max(1, args.depth)
    seen = {a}

    def walk(f, d):
        kids = []
        for dst, sites in callees_of(con, f, args.all).items():
            node = {'func': fname(con, dst), 'addr': h(dst), 'sites': sites}
            if d < depth and dst is not None and dst not in seen:
                seen.add(dst)
                node['callees'] = walk(dst, d + 1)
            elif d < depth and dst in seen:
                node['seen'] = True
            kids.append(node)
        return kids

    r = con.execute('SELECT n_bctrl, n_bctr FROM funcs WHERE addr = ?', (a,)).fetchone()
    return {'func': fname(con, a), 'addr': h(a), 'depth': depth, 'indirect_calls': r['n_bctrl'] if r else 0,
            'indirect_jumps': r['n_bctr'] if r else 0, 'callees': walk(a, 1)}


def print_callees(r, args):
    print(f'{r["func"]}: indirect calls (bctrl) {r["indirect_calls"]}, indirect jumps (bctr) {r["indirect_jumps"]}')
    count = [0]

    def show(nodes, ind):
        for n in nodes:
            count[0] += 1
            if count[0] > args.limit * max(1, r['depth']):
                return
            kinds = sorted({s['kind'] for s in n['sites']})
            extra = ' (seen above)' if n.get('seen') else ''
            into = [s['target'] for s in n['sites'] if s['target']]
            into = f' into {",".join(into)}' if into else ''
            print(f'{"  " * ind}{n["func"]}  x{len(n["sites"])} {"/".join(kinds)}{into}{extra}')
            if n.get('callees'):
                show(n['callees'], ind + 1)

    show(r['callees'], 1)
    if count[0] > args.limit * max(1, r['depth']):
        print('  ... (--limit)')


def cmd_str(con, pattern, args):
    flags = re.I if args.ignore_case else 0
    try:
        rx = re.compile(pattern, flags)
    except re.error as e:
        raise QError(f'bad regex: {e}')
    con.create_function('rx', 1, lambda t: 1 if t is not None and rx.search(t) else 0, deterministic=True)
    srows = con.execute('SELECT addr, enc, text FROM strings WHERE rx(text) ORDER BY addr').fetchall()
    out = []
    hidden = 0
    for s in srows:
        refs = con.execute(f'SELECT func, site, how, op FROM strrefs WHERE addr = ? AND {lis_op_filter(args)} '
                           'ORDER BY func, site', (s['addr'],)).fetchall()
        hidden += n_hidden_unsure(con, 'strrefs', 'addr = ?', (s['addr'],), args)
        dps = con.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'str' ORDER BY at", (s['addr'],)).fetchall()
        out.append({'addr': h(s['addr']), 'enc': s['enc'], 'text': s['text'],
                    'refs': [{'func': fname(con, r['func']), 'addr': h(r['func']), 'site': h(r['site']),
                              'how': r['how'], 'op': r['op']} for r in refs],
                    'data_pointers': [h(d['at']) for d in dps]})
    return {'pattern': pattern, 'n_strings': len(out), 'strings': out,
            **({'uncertain_hidden': hidden} if hidden else {})}


def print_str(r, args):
    shown = r['strings'] if args.unref else [s for s in r['strings'] if s['refs'] or s['data_pointers']]
    hidden = len(r['strings']) - len(shown)
    print(f'{r["n_strings"]} strings match /{r["pattern"]}/' +
          (f' ({hidden} with no code or data reference hidden: --unref)' if hidden else ''))
    for s in shown[:args.limit]:
        enc = '' if s['enc'] == 'ascii' else ' [utf16be]'
        print(f'  {s["addr"]}{enc} {json.dumps(s["text"])[:110]}')
        funcs = {}
        for ref in s['refs']:
            funcs.setdefault(ref['func'], []).append(ref['site'] + ('' if ref['how'] == 'direct' else f' {ref["how"]}')
                                                     + (' ?' if ref['op'].endswith('?') else ''))
        for f, sites in funcs.items():
            print(f'      {f:<28} {", ".join(sites[:6])}{" ..." if len(sites) > 6 else ""}')
        if s['data_pointers']:
            dp = s['data_pointers']
            print(f'      data words pointing at it: {", ".join(dp[:6])}{" ..." if len(dp) > 6 else ""}')
    if len(shown) > args.limit:
        print(f'  ... {len(shown) - args.limit} more (--limit)')
    print_hidden(r)


def parse_value(s, kind='imm'):
    """VALUE/ADDR text -> u32. 0x... = hex; anything with a hex letter = hex; a bare 8-digit token = hex (an
    address, as FUNC reads it). For `addr` every bare token is hex. For `imm` a bare 1-7 digit token is decimal
    (-1, 100, 4096) and one with a leading zero (010) is refused as ambiguous. Returns (value, how it was read)."""
    t = s.strip()
    neg = t.startswith('-')
    body = t[1:] if neg else t
    if re.fullmatch(r'0[xX][0-9A-Fa-f]+', body):
        v, how = int(body, 16), 'hex'
    elif re.fullmatch(r'[0-9]+', body):
        if kind == 'addr' or len(body) == 8:
            v, how = int(body, 16), 'hex (bare 8-digit/address token)'
        elif len(body) > 1 and body[0] == '0':
            raise QError(f'{s}: ambiguous (leading zero): write 0x{body} for hex or {body.lstrip("0") or "0"} for decimal')
        else:
            v, how = int(body, 10), 'decimal'
    elif re.fullmatch(r'[0-9A-Fa-f]+', body):
        v, how = int(body, 16), 'hex'
    else:
        raise QError(f'{s}: not a number (hex 0x82130000 / 82130000, decimal 100, negative -1)')
    if neg:
        v = -v
    if v < -0x80000000 or v > 0xFFFFFFFF:
        raise QError(f'{s}: out of 32-bit range')
    return v & 0xFFFFFFFF, how


def lis_op_filter(args):
    """SQL condition on an `op` column: uncertain lis pairs (op ending in '?') only with --maybe."""
    return '1' if getattr(args, 'maybe', False) else "op NOT LIKE '%?'"


def n_hidden_unsure(con, table, where, params, args):
    if getattr(args, 'maybe', False):
        return 0
    return con.execute(f"SELECT COUNT(*) FROM {table} WHERE {where} AND op LIKE '%?'", params).fetchone()[0]


def cmd_imm(con, s, args):
    import qdatatag
    tag = qdatatag.parse_tag(s)       # S111 QCSTR: POF0 / 'BEEF' = the 4 bytes' word
    v, how = (tag, 'tag "%s"' % s.strip("'\"")) if tag is not None else parse_value(s, 'imm')
    rows = con.execute(f'SELECT func, site, op FROM imms WHERE value = ? AND {lis_op_filter(args)} ORDER BY func, site',
                       (v,)).fetchall()
    hidden = n_hidden_unsure(con, 'imms', 'value = ?', (v,), args)
    by = {}
    for r in rows:
        by.setdefault(r['func'], []).append({'site': h(r['site']), 'op': r['op']})
    extra = {'read_as': how}
    if hidden:
        extra['uncertain_hidden'] = hidden
    if IMAGE_BASE <= v < IMAGE_BASE + 0x1A60000:
        st = con.execute('SELECT enc, text FROM strings WHERE addr = ?', (v,)).fetchone()
        if st:
            extra['string'] = st['text']
        c = containing(con, v)
        if c:
            extra['inside_function'] = f'{c["name"]}+0x{v - c["addr"]:X}'
    if v > 0xFFFF:          # S111 QCSTR: the same 4 bytes as DATA in the image (a tag table, a constant), not only as a code immediate
        import qwords
        ip = qwords.image_path()
        if ip:
            with open(ip, 'rb') as fh:
                img = fh.read()

            def in_func(a):
                return containing(con, a)

            def code_uses(a):
                return [f'{fname(con, r["func"])} {h(r["site"])}' for r in
                        con.execute('SELECT func, site FROM imms WHERE value = ? ORDER BY func, site LIMIT 6', (a,))]
            extra['data'] = qdatatag.data_hits(img, v, in_func, code_uses)
    return {'value': h(v), 'signed': v - (1 << 32) if v & 0x80000000 else v, 'n_sites': len(rows),
            'n_funcs': len(by), 'funcs': [{'func': fname(con, f), 'addr': h(f), 'sites': s} for f, s in by.items()],
            **extra}


def print_imm(r, args):
    print(f'{r["value"]} ({r["signed"]}, read as {r["read_as"]}): {r["n_sites"]} uses in {r["n_funcs"]} functions'
          + (f'; is string {json.dumps(r["string"])[:80]}' if 'string' in r else '')
          + (f'; is code inside {r["inside_function"]}' if 'inside_function' in r else ''))
    for f in r['funcs'][:args.limit]:
        ops = ', '.join(f'{s["site"]} {s["op"]}' for s in f['sites'][:5])
        print(f'  {f["func"]:<28} {ops}{" ..." if len(f["sites"]) > 5 else ""}')
    if r['n_funcs'] > args.limit:
        print(f'  ... {r["n_funcs"] - args.limit} more (--limit)')
    if 'data' in r:
        import qdatatag
        qdatatag.print_hits(r['data'])
    print_hidden(r)


def print_hidden(r):
    if r.get('uncertain_hidden'):
        print(f'  ({r["uncertain_hidden"]} uncertain lis-pair uses hidden: the lis value may not reach them on every '
              f'path; --maybe shows them marked "?")')


def cmd_rtti(con, name, args):
    rows = con.execute('SELECT td, mangled, name FROM rtti_class WHERE name LIKE ? OR mangled LIKE ? ORDER BY name',
                       (f'%{name}%', f'%{name}%')).fetchall()
    out = []
    show_slots = args.slots or len(rows) <= 3
    for c in rows:
        vts = con.execute('SELECT vt, col, nslots FROM rtti_vtable WHERE td = ? ORDER BY vt', (c['td'],)).fetchall()
        vlist = []
        for v in vts:
            off = con.execute('SELECT offset FROM rtti_col WHERE col = ?', (v['col'],)).fetchone()
            item = {'vtable': h(v['vt']), 'col': h(v['col']), 'this_offset': off['offset'] if off else None,
                    'nslots': v['nslots']}
            if show_slots:
                item['slots'] = [{'idx': s['idx'], 'func': fname(con, s['func']), 'addr': h(s['func'])} for s in
                                 con.execute('SELECT idx, func FROM rtti_slot WHERE vt = ? ORDER BY idx', (v['vt'],))]
            vlist.append(item)
        bases = [b['base'] for b in con.execute('SELECT base FROM rtti_base WHERE cls = ? AND idx > 0 ORDER BY idx',
                                                (c['name'],))]
        out.append({'class': c['name'], 'mangled': c['mangled'], 'type_descriptor': h(c['td']),
                    'bases': bases, 'vtables': vlist})
    return {'query': name, 'n_classes': len(out), 'classes': out}


def print_rtti(r, args):
    print(f'{r["n_classes"]} RTTI classes match "{r["query"]}"')
    for c in r['classes'][:args.limit]:
        b = f'  bases: {", ".join(c["bases"])}' if c['bases'] else ''
        print(f'  {c["class"]}  (TD {c["type_descriptor"]}, {len(c["vtables"])} vtables){b}')
        for v in c['vtables']:
            print(f'    vtable {v["vtable"]} (this offset {v["this_offset"]}): {v["nslots"]} slots')
            for s in v.get('slots', [])[:args.limit]:
                print(f'      [{s["idx"]:>2}] {s["func"]}')
    if r['n_classes'] > args.limit:
        print(f'  ... {r["n_classes"] - args.limit} more (--limit)')


def parse_host(s, sections):
    """wwe2k14.exe+0xOFFSET, an RVA, or an absolute address at the preferred base 0x140000000. An address from an
    ASLR-relocated process (0x7FF6...) is refused: subtract the module base and pass wwe2k14.exe+0x...
    `sections`: [[rva_start, rva_end, name]] of the map (empty for an old index: then no range check)."""
    m = re.fullmatch(r'(wwe2k14(?:_v3)?\.exe\+)?(0x[0-9A-Fa-f]+|[0-9A-Fa-f]+)', s.strip())
    if not m:
        raise QError(f'{s}: expected wwe2k14.exe+0xOFFSET or an RVA')
    v = int(m.group(2), 16)
    image_end = max((x[1] for x in sections), default=None)
    if not m.group(1) and v >= 0x140000000:
        if image_end is not None and v - 0x140000000 >= image_end:
            raise QError(f'{s}: not inside the image at its preferred base 0x140000000 (image size 0x{image_end:X}). '
                         'An address from a running process is relocated (ASLR): pass wwe2k14.exe+0xOFFSET, i.e. the '
                         'address minus the module base shown in the crash log / debugger')
        v -= 0x140000000
    if sections and not any(lo <= v < hi for lo, hi, _ in sections):
        raise QError(f'{s}: RVA 0x{v:X} is not inside any section of the map (image size 0x{image_end:X}); '
                     'for an absolute address subtract the module base: wwe2k14.exe+0xOFFSET')
    return v


def host_map(strict):
    """S110 QREBUILD: the linker map's symbols, from their own file per map version (tools/q/qhostmap.py; never an input of index.db). -> (con or None, info)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import qhostmap
    con, info = qhostmap.open_map(os.path.dirname(DB))
    if con is None and strict:
        raise QError(info['error'])
    return con, info


def host_rva(name):
    """the host RVA of a guest function's symbol in the current map, or None (no map, or the map is being written)"""
    if not name:
        return None
    con, _ = host_map(False)
    if con is None:
        return None
    try:
        r = con.execute('SELECT rva FROM syms WHERE name = ?', (name,)).fetchone()
    finally:
        con.close()
    return r[0] if r else None


def stale_line(m, st):
    """S110 QREBUILD: a lookup never rebuilds the index and never waits for a rebuild. It answers from the newest complete index and says so in ONE line,
    and starts at most ONE background build (tools/q/qpublish.kick: the build lock, and an attempt record so a failed build is not repeated by every call)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_index
    import qpublish
    gen = m.get('gen_dir') or build_index.GEN
    image = os.path.join(os.path.dirname(DB), 'image.bin')
    # S110 review: 6 workers, not build_index's default of up to 16: this build starts by itself on a PC shared by several agents, builds and game runs (it
    # also runs below normal priority). A build by hand keeps the default.
    cmd = [sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build_index.py'), '--gen', gen, '--image', image, '--out', DB,
           '-j', '6']
    what = qpublish.kick(DB, build_index.index_key(gen, image), cmd, os.path.join(os.path.dirname(DB), 'build_index.log'),
                         spawn=os.environ.get('WWE_Q_NO_AUTOUPDATE') != '1')
    why = '; '.join(w for w in st if not w.startswith('rebuild:'))
    return f'stale: this answer is from the index built {m.get("built_at")} ({why}); {what}'


def cmd_addr(con, s, args, m):
    if args.host:
        if args.map:
            # another build's map (e.g. the one next to a crashed exe): parsed live, ~2 s
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from build_index import parse_map, map_timestamp
            sections = []
            try:
                syms = sorted(parse_map(args.map, sections))
                map_ts = map_timestamp(args.map)
            except OSError as e:
                raise QError(f'cannot read map {args.map}: {e.strerror or e}')
            if not syms:
                raise QError(f'no symbols parsed from {args.map}')
            rva = parse_host(s, sections)
            rvas = [x[0] for x in syms]
            k = bisect.bisect_right(rvas, rva) - 1
            if k < 0:
                raise QError('no host symbol at or below that RVA')
            r = {'rva': syms[k][0], 'name': syms[k][1], 'obj': syms[k][2]}
            nxt = next(({'rva': x[0]} for x in syms[k + 1:] if x[0] > r['rva']), None)
            map_path = args.map
        else:
            if s.strip().lower().startswith('wwe2k14_v3.exe'):
                raise QError(f'{s}: q reads wwe2k14.exe\'s map; pass --map with wwe2k14_v3.map')
            hm, hinfo = host_map(True)
            try:
                sections = hinfo['sections']
                rva = parse_host(s, sections)
                r = hm.execute('SELECT rva, name, obj FROM syms WHERE rva <= ? ORDER BY rva DESC LIMIT 1', (rva,)).fetchone()
                if not r:
                    raise QError('no host symbol at or below that RVA')
                nxt = hm.execute('SELECT rva FROM syms WHERE rva > ? ORDER BY rva LIMIT 1', (r['rva'],)).fetchone()
            finally:
                hm.close()
            map_path, map_ts = hinfo['path'], hinfo['timestamp']
        sec = next((x for x in sections if x[0] <= rva < x[1]), None)
        if nxt is None and sec is None:
            raise QError(f'RVA 0x{rva:X} is past the last map symbol ({r["name"]} at 0x{r["rva"]:X}) and the map gives '
                         'no section extents to check it against: not resolvable')
        # the symbol's extent ends at the next symbol or at the end of its section, whichever comes first
        end = min([x for x in (nxt['rva'] if nxt else None, sec[1] if sec else None) if x is not None])
        if r['rva'] < (sec[0] if sec else r['rva']):
            raise QError(f'RVA 0x{rva:X} is in section {sec[2]} before its first symbol: no symbol to attribute it to')
        out = {'host_rva': f'0x{rva:X}', 'symbol': r['name'], 'symbol_rva': f'0x{r["rva"]:X}',
               'offset': f'0x{rva - r["rva"]:X}', 'obj': r['obj'],
               'symbol_size': f'0x{end - r["rva"]:X}', 'section': sec[2] if sec else None,
               'map': map_path, 'map_timestamp': map_ts,
               'caveat': 'host offsets are only valid against the map of the exact build that produced them'}
        mm = re.fullmatch(r'(?:__imp__)?(sub_[0-9A-F]{8})', r['name'])
        if mm:
            out['guest_function'] = mm.group(1)
        else:
            row = con.execute('SELECT addr FROM funcs WHERE name = ?', (r['name'].replace('__imp__', '', 1),)).fetchone()
            if row:
                out['guest_function'] = fname(con, row['addr'])
        return out
    a, how = parse_value(s, 'addr')
    out = {'addr': h(a)}
    c = containing(con, a)
    if c:
        out['function'] = c['name']
        out['offset'] = f'0x{a - c["addr"]:X}'
        out['func_size'] = c['size']
        out['where'] = f'{c["file"]}:{c["line"]}'
    else:
        f = con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
        if f:
            out['function'] = f['name']
            out['offset'] = '0x0'
    st = con.execute('SELECT addr, enc, text FROM strings WHERE addr <= ? ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
    if st and a < st['addr'] + len(st['text']) * (2 if st['enc'] == 'utf16be' else 1) + 1:
        out['string'] = {'addr': h(st['addr']), 'text': st['text'], 'offset': a - st['addr']}
    vt = con.execute('''SELECT v.vt, v.nslots, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td
                        WHERE v.vt <= ? ORDER BY v.vt DESC LIMIT 1''', (a,)).fetchone()
    if vt and a < vt['vt'] + 4 * max(1, vt['nslots']):
        out['vtable'] = {'class': vt['name'], 'vtable': h(vt['vt']), 'slot': (a - vt['vt']) // 4}
    p = con.execute('SELECT value, kind FROM ptrs WHERE at = ?', (a,)).fetchone()
    if p:
        out['word_points_to'] = fname(con, p['value']) if p['kind'] == 'func' else h(p['value'])
    refs = con.execute(f'SELECT func, site, op FROM imms WHERE value = ? AND op LIKE ? AND {lis_op_filter(args)} '
                       'ORDER BY func LIMIT 200', (a, 'lis+%')).fetchall()
    if refs:
        out['code_refs'] = [{'func': fname(con, r['func']), 'site': h(r['site']), 'op': r['op']} for r in refs]
    hidden = n_hidden_unsure(con, 'imms', "value = ? AND op LIKE 'lis+%'", (a,), args)
    if hidden:
        out['uncertain_hidden'] = hidden
    host = host_rva(out.get('function', ''))
    if host is not None:
        out['host_rva_of_function'] = f'0x{host:X}'
    if len(out) == 1:
        out['note'] = 'not in any function, string, vtable or indexed data word'
    if how != 'hex':
        out['read_as'] = how
    return out


def print_kv(r, args):
    for k, v in r.items():
        if isinstance(v, list):
            print(f'{k}: {len(v)}')
            for x in v[:args.limit]:
                print(f'  {x}')
        else:
            print(f'{k}: {v}')


def docs_mentions(a):
    pats = [f'sub_{a:08X}', f'0x{a:08X}']
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, 'docs', 'systems', '*.md')) +
                    glob.glob(os.path.join(REPO, 'docs', 'modules', '*.md'))):
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = [i + 1 for i, line in enumerate(f) if any(x.lower() in line.lower() for x in pats)]
        except OSError:
            continue
        if lines:
            out.append({'doc': os.path.relpath(p, REPO), 'lines': lines})
    return out


def documented_in(a):
    """Session 98: the format / system / module page sections that name `a` (the library index, tools/q/qfact.py)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import qfact
    return qfact.documented_in(a)


def cmd_db(con, a, args):
    entries = [{'file': fn, **e} for x, fn, e in load_db_entries() if x == a]
    return {'func': fname(con, a), 'addr': h(a), 'entries': entries, 'claims': claims_for(a), 'docs': docs_mentions(a),
            'documented_in': documented_in(a)}


def print_documented(r, indent='  '):
    for d in r.get('documented_in', [])[:8]:
        print(f'{indent}documented in: {d["file"]}:{d["line"]}  {d["heading"]}')


def print_db(r, args):
    if not r['entries']:
        print(f'{r["func"]}: no db entry')
    if r.get('claims'):   # S111 LODGE: the probe notes' lodged claims (db/claims/*.json)
        print(f'{r["func"]}: {len(r["claims"])} lodged claim(s):')
        for c in r['claims'][:args.limit]:
            print(f'  claim {claim_line(c)}  ({c["file"]})')
    for e in r['entries']:
        print(f'{r["func"]} = {e.get("name")}  [{e.get("system")}, confidence {e.get("confidence")}]  ({e["file"]})')
        print(f'  {e.get("summary")}')
        for ev in e.get('evidence', [])[:args.limit]:
            print(f'  - {ev}')
        if e.get('source'):
            print(f'  source: {e["source"]}')
    print_documented(r)
    for d in r['docs']:
        print(f'  mentioned in {d["doc"]} lines {", ".join(map(str, d["lines"][:10]))}')


_COV = []


def load_coverage():
    """out/harness/coverage/coverage.json (task 6, tools/coverage/cov.py) or None; cached per process."""
    if not _COV:
        if not os.path.exists(COVERAGE):
            _COV.append(None)
        else:
            try:
                with open(COVERAGE, encoding='utf-8') as f:
                    _COV.append(json.load(f))
            except (OSError, ValueError) as e:
                _COV.append({'error': str(e)})
    return _COV[0]


def coverage_for(a):
    """Scenarios in which this function ran (coverage build). Format wwe2k14-coverage/1: functions{"0xADDR": [scenario]};
    anything else: generic search for the address anywhere in the JSON."""
    cov = load_coverage()
    if cov is None or 'error' in cov:
        return cov
    if str(cov.get('format', '')).startswith('wwe2k14-coverage/'):
        scen = cov.get('functions', {}).get(f'0x{a:08X}', [])
        return {'file': COVERAGE, 'format': cov['format'], 'built': cov.get('built'),
                'scenarios': scen, 'all_scenarios': list(cov.get('scenarios', {})),
                'hits': [{'path': s} for s in scen]}
    keys = {f'sub_{a:08X}', f'0x{a:08X}', f'{a:08X}', f'0x{a:08x}', f'{a:08x}', f'sub_{a:08x}'}
    hits = []

    def match(x):
        return (isinstance(x, str) and x in keys) or (isinstance(x, int) and not isinstance(x, bool) and x == a)

    def walk(node, path):
        if len(hits) >= 50:
            return
        if isinstance(node, dict):
            for k, v in node.items():
                if match(k):
                    hits.append({'path': '/'.join(path + [str(k)]), 'value': v if not isinstance(v, (dict, list)) else None})
                else:
                    walk(v, path + [str(k)])
        elif isinstance(node, list):
            for i, v in enumerate(node):
                if match(v):
                    hits.append({'path': '/'.join(path) or '/', 'value': None})
                else:
                    walk(v, path + [str(i)] if isinstance(v, (dict, list)) else path)

    walk(cov, [])
    return {'file': COVERAGE, 'hits': hits}


def cmd_fn(con, a, args):
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
    if f is None:
        raise QError(f'{h(a)} is not a function start (for data: q addr {h(a)} / q db {h(a)})')
    callers = cmd_callers(con, a, args)
    callees = callees_of(con, a, False)
    flt = lis_op_filter(args)
    strs = con.execute(f'''SELECT r.site, r.how, r.op, s.addr, s.text FROM strrefs r JOIN strings s ON s.addr = r.addr
                           WHERE r.func = ? AND {flt.replace('op', 'r.op')} ORDER BY r.site''', (a,)).fetchall()
    consts = con.execute(f'SELECT value, op, site FROM imms WHERE func = ? AND {flt} ORDER BY site', (a,)).fetchall()
    hidden = n_hidden_unsure(con, 'imms', "func = ? AND op LIKE 'lis+%'", (a,), args)
    cvals = {}
    for c in consts:
        cvals.setdefault((c['value'], c['op'].startswith('lis+')), []).append(c['op'])
    host = host_rva(f['name'])
    dbe = cmd_db(con, a, args)
    try:   # S103 QD: fields read / written + calls, from the decomp cache (never builds, never fails fn)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qdecomp
        decomp_lines = qdecomp.fn_summary(con, a)
    except Exception:
        decomp_lines = []
    return {'decomp': decomp_lines,
        'func': f['name'], 'addr': h(a), 'size': f['size'], 'insns': f['insns'],
        'where': f'generated/default/{f["file"]}:{f["line"]}' if f['file'] else None,
        'host_rva': f'0x{host:X}' if host is not None else None,
        'callers': {'n_sites': callers['n_sites'], 'n_funcs': callers['n_callers'],
                    'list': [c['func'] for c in callers['callers']]},
        'data_pointers': callers['data_pointers'], 'vtable_slots': callers['vtable_slots'],
        'callees': [{'func': fname(con, d), 'n': len(s), 'kinds': sorted({x['kind'] for x in s})} for d, s in callees.items()],
        'indirect_calls': f['n_bctrl'], 'indirect_jumps': f['n_bctr'],
        'strings': [{'site': h(s['site']), 'addr': h(s['addr']), 'text': s['text'], 'how': s['how'],
                     **({'uncertain': True} if s['op'].endswith('?') else {})} for s in strs],
        'addresses': [{'value': h(v), 'ops': sorted(set(o))} for (v, isaddr), o in cvals.items() if isaddr],
        'constants': [{'value': h(v), 'ops': sorted(set(o))} for (v, isaddr), o in cvals.items() if not isaddr],
        'db': dbe['entries'], 'claims': dbe['claims'], 'docs': dbe['docs'], 'documented_in': dbe['documented_in'], 'coverage': coverage_for(a),
        **({'uncertain_hidden': hidden} if hidden else {}),
    }


def print_fn(r, args):
    L = args.limit
    print(f'{r["func"]}  {r["addr"]}  size {r["size"]} ({r["insns"]} insns)  {r["where"] or "(no body: import/helper)"}'
          + (f'  host RVA {r["host_rva"]}' if r['host_rva'] else ''))
    for e in r['db']:
        print(f'  db: {e.get("name")} [{e.get("system")}, {e.get("confidence")}] ({e["file"]}): {e.get("summary", "")[:160]}')
    for c in r.get('claims', [])[:L]:
        print(f'  claim: {claim_line(c)[:200]}  ({c["file"]})')
    print_documented(r)
    for d in r['docs']:
        print(f'  doc: {d["doc"]} lines {", ".join(map(str, d["lines"][:8]))}')
    c = r['callers']
    print(f'  callers: {c["n_sites"]} sites in {c["n_funcs"]} funcs: ' + ', '.join(c['list'][:L]) + (' ...' if len(c['list']) > L else ''))
    if r['data_pointers']:
        print(f'  data words pointing at it: {len(r["data_pointers"])}: ' + ', '.join(r['data_pointers'][:8]))
    for s in r['vtable_slots'][:L]:
        print(f'  vtable slot: {s["class"]} {s["vtable"]} [{s["slot"]}]')
    ce = r['callees']
    print(f'  callees ({len(ce)}; indirect calls {r["indirect_calls"]}, indirect jumps {r["indirect_jumps"]}): '
          + ', '.join(x['func'] + ('' if x['kinds'] == ['call'] else f'({"/".join(x["kinds"])})') for x in ce[:L])
          + (' ...' if len(ce) > L else ''))
    for s in r['strings'][:L]:
        print(f'  string @{s["site"]}: {json.dumps(s["text"])[:100]}' + ('' if s['how'] == 'direct' else f'  ({s["how"]})')
              + ('  ? uncertain' if s.get('uncertain') else ''))
    if len(r['strings']) > L:
        print(f'  ... {len(r["strings"]) - L} more strings')
    if r['addresses']:
        print('  lis pairs (addresses or 32-bit constants): ' + ', '.join(f'{x["value"]} {"/".join(x["ops"])}' for x in r['addresses'][:L])
              + (' ...' if len(r['addresses']) > L else ''))
    if r['constants']:
        print('  constants: ' + ', '.join(f'{x["value"]}' for x in r['constants'][:L * 2])
              + (' ...' if len(r['constants']) > L * 2 else ''))
    for line in r.get('decomp', []):
        print(line)
    cov = r['coverage']
    if cov is None:
        print('  coverage: (no out/harness/coverage/coverage.json yet)')
    elif cov.get('error'):
        print(f'  coverage: unreadable ({cov["error"]})')
    elif 'scenarios' in cov:
        n = len(cov['all_scenarios'])
        print(f'  coverage: ran in {len(cov["scenarios"])} of {n} scenarios' +
              (': ' + ', '.join(cov['scenarios']) if cov['scenarios'] else ' (never seen running)') +
              f'  (coverage.json built {cov.get("built")})')
    else:
        print(f'  coverage: {len(cov["hits"])} hits: ' + ', '.join(x['path'] for x in cov['hits'][:L]))
    print_hidden(r)


def cmd_info(con, args, m):
    return {**m, 'db_path': DB, 'stale': staleness(m)}


# ---------------------------------------------------------------- main

def bare_ours(argv):
    """S108: the bare verbs of `ours` (bare `q outline` failed 11 of 12 times). `text` stays the game's string-table lookup (qgx.text_main) whenever the line
    fits ITS grammar: every positional a number (S111 QFIX4: several ids allowed), with --lang N / --all / --json / -h around it (`q text 12`, `q text 12 40356`, `q text --lang 1 12`, `q text -h`)."""
    if argv[0] in ('outline', 'body', 'refs', 'def', 'lit'):
        return True
    if argv[0] != 'text':
        return False
    pos, i = [], 1
    while i < len(argv):
        x = argv[i]
        i += 2 if x == '--lang' else 1
        if x not in ('--lang', '--all', '--json', '-h', '--help') and not x.startswith('--lang='):
            pos.append(x)
    return not all(re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', x) for x in pos)      # S111 QFIX4: every positional numeric (one or several ids) = the string-table lookup; a word = the code search


VERBS = ('find', 'multi', 'switch', 'text', 'csdt', 'decomp', 'menu', 'py', 'missed', 'asm', 'args', 'vcall', 'nodes', 'callers', 'callees',
         'str', 'imm', 'rtti', 'addr', 'db', 'fn', 'info', 'outline', 'body', 'refs', 'def', 'lit') + tuple(REG)    # + every verb of tools/q/verbs/ (qverbs.py)


def _edits(a, b):
    """Levenshtein distance (small words only)"""
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def near_verb(w):
    """S108 FD: an unknown first word that looks like a mistyped q verb (`decmp`, `ours-callers`) -> the verb line it probably meant, else None.
    Only lowercase words (letters and -) are candidates: a name (CamelCase, snake_case, digits, a path) is a find question, not a typo."""
    if not re.fullmatch(r'[a-z][a-z-]*', w):
        return None
    parts = [p for p in w.split('-') if p]
    if len(parts) > 1 and parts[0] in VERBS:
        return ' '.join(parts)
    best = None
    for v in VERBS:
        d = _edits(w, v)
        if d and d <= (1 if len(v) <= 4 else 2) and (best is None or d < best[0]):
            best = (d, v)
    return best[1] if best else None


def main(argv=None):
    # Session 89: `q.py map ...` = the harness knowledge map (tools\q\qmap.py; no index needed).
    argv = sys.argv[1:] if argv is None else argv
    for w in REG_WARNINGS:       # S111 QVERBS: a bad verb file is skipped, said once per call, everything else works
        print(w, file=sys.stderr)
    if not argv:    # S108: the first screen says which command answers which question
        print(ROUTING)
        return 0
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if any(a == '--where' or a.startswith('--where=') for a in argv) and argv[0] != 'multi':      # S111 QWHERE: --where REGEX [--context N] on ANY verb (tools/q/qwhere.py); the one seam every verb and every multi question passes
        import qwhere
        rc = qwhere.run(list(argv), main)
        if rc is not None:
            return rc
    if argv[0] == 'find':      # S108 FD: the front door (tools/q/qfind.py)
        import qfind
        return qfind.main(list(argv[1:]), sys.modules[__name__])
    if argv[0] == 'multi':     # S108 FD: several q questions in one call (tools/q/qmulti.py)
        import qmulti
        return qmulti.main(list(argv[1:]), main)
    if not argv[0].startswith('-') and argv[0] not in VERBS:
        # S108 FD: an unknown first word is a find question; a mistyped verb is said FIRST, on stdout, and exits 2 (never a quiet "no hit")
        import qfind
        meant = near_verb(argv[0])
        if not meant:
            print(f'q: {argv[0]!r} is not a q verb: answering `q find {" ".join(argv)}`', file=sys.stderr)
            return qfind.main(list(argv), sys.modules[__name__])
        # S108 FD review (coordinator's rule): near a verb AND no hit as a name (file / stem, C++ or python symbol, cvar, env var, game) = a typo:
        # the did-you-mean line FIRST, then find's answer, exit 2. A name hit = a question (`q qfact`, `q test`): find's exit, one closing note.
        import contextlib
        import io
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = qfind.main(list(argv), sys.modules[__name__])
        except SystemExit as e:        # find's own usage error (an option of the meant verb): still a typo
            rc = e.code if isinstance(e.code, int) else 2
            qfind.LAST.clear()
        note_to = sys.stderr if '--json' in argv else sys.stdout      # --json: stdout stays one JSON document
        if qfind.structural_hit(qfind.LAST, argv[0]):
            sys.stdout.write(buf.getvalue())
            print(f"(also a near-miss of the verb '{meant.split()[0]}': q {meant} ...)", file=note_to)
            return rc
        print(f'q: unknown verb {argv[0]!r}: did you mean `q {meant} ...`? (q -h lists the verbs; answered below as q find {" ".join(argv)}; exit 2)',
              file=note_to, flush=True)
        sys.stdout.write(buf.getvalue())
        return 2
    if bare_ours(argv):
        argv = ['ours'] + list(argv)
    # S105 QG: `switch` / `text` / `csdt` / `decomp --cases` / `field OFF --stores` / `ours includes F --included-by|--transitive` = tools/q/qgx.py + qstores.py
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    # lazy: a broken qgx.py must not break every other q command (L4)
    if argv and (argv[0] in ('switch', 'text', 'csdt') or (argv[0] in ('decomp', 'field', 'ours') and any(a in argv for a in ('--cases', '--stores', '--included-by', '--transitive', '--closure')))):
        import qgx
        if qgx.wants(argv):
            return qgx.dispatch(argv, connect, DB, resolve)
    if argv and argv[0] in REG:       # S111 QVERBS: a verb registered in tools/q/verbs/NAME.toml (save, map, fact, ours, show, note, sdk, asset, field, feed, the diff tools ...)
        return qverbs.dispatch(REG[argv[0]], argv, sys.modules[__name__])
    # S105 QM: `q.py menu ...` = menu pages / rows / state words for route lanes (tools/q/qmenu.py; out/harness/index/menu.json)
    if argv and argv[0] == 'menu':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qmenu
        try:
            sys.stdout.reconfigure(errors='replace')
        except Exception:
            pass
        return qmenu.main(argv[1:])
    if argv and argv[0] == 'py':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qours
        import contextlib
        import io
        eb, ob = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(eb), contextlib.redirect_stdout(ob):
            rc = qours.main(argv[1:] + ['--tools'])
        err, outt = eb.getvalue(), ob.getvalue()
        sys.stderr.write(err)
        sys.stdout.write(outt)
        if rc == 0 and not re.search(r'\.(py|ps1):\d', outt) and len(argv) > 2 and argv[1] in ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn'):
            print('q py: (C++ index: no python / ps1 symbol of that name)', file=sys.stderr)
        return rc
    # S103 QD: `q.py decomp FUNC [--callers N] [--fields]` = a compact mechanical view of one function (tools/q/qdecomp.py;
    # cache out/harness/index/decomp/, keyed by the generated source's hash).
    # S103: `q.py missed "QUESTION"` = log a question q could not answer (out/harness/q_missed.log): voluntary (S108: the q-first hook that
    # refused raw searches is gone); the log is the list of q entries to build next.
    if argv and argv[0] == 'missed':
        import time
        q = ' '.join(argv[1:]).strip()
        if not q:
            print('usage: q.py missed "the question q could not answer"', file=sys.stderr)
            return 2
        p = os.path.join(REPO, 'out', 'harness', 'q_missed.log')
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'a', encoding='utf-8') as f:
            f.write(f'{time.strftime("%Y-%m-%d %H:%M")}\t{os.path.basename(os.getcwd())}\t{q}\n')
        print('q: logged as a q gap (out\\harness\\q_missed.log)')
        return 0
    # S103 QX3: `q.py asm FUNC|ADDR [FROM TO] [--into FROM TO]` = a per-address window of the generated listing, branch targets marked (tools/q/qx6.py).
    if argv and argv[0] == 'asm':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qx6
        import qdecomp
        try:
            return qx6.asm_main(argv[1:], connect(DB), resolve, REPO)
        except (QError, qx6.XError, qdecomp.DecompError) as e:
            print(f'q: {e}', file=sys.stderr)
            return 2
        except BrokenPipeError:
            return 0
    if argv and argv[0] == 'args':      # S111 QARGS: `q args F [--reg rN] [--value V] [--in CALLER]` = call-site argument values (tools/q/qargs.py)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qargs
        import qdecomp
        try:
            return qargs.args_main(argv[1:], connect(DB), resolve)
        except (QError, qargs.ArgsError, qdecomp.DecompError) as e:
            print(f'q: {e}', file=sys.stderr)
            return 2
        except BrokenPipeError:
            return 0
    # S103 QX4: `q.py vcall OFF [--class VT]` (virtual call sites by slot offset), `q.py nodes [CAT]` (the draw-node registry) = tools/q/qx7.py;
    # Both build their own table in out/harness/index on first use.
    if argv and argv[0] in ('vcall', 'nodes'):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qx7
        try:
            return qx7.main(argv, connect(DB))
        except (QError, qx7.XError) as e:
            print(f'q: {e}', file=sys.stderr)
            return 2
        except BrokenPipeError:
            return 0
    if argv and argv[0] == 'decomp':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qdecomp
        try:
            con = connect(DB)
            return qdecomp.main(argv[1:], con, resolve)
        except (QError, qdecomp.DecompError) as e:
            print(f'q: {e}', file=sys.stderr)
            return 2
        except BrokenPipeError:
            return 0
    ap = argparse.ArgumentParser(prog='q', description=ROUTING + '\n' + __doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true', help='machine-readable output')
    common.add_argument('--limit', type=int, default=25)
    common.add_argument('--index', default=DB)
    # the same options before the command (q.py --json fn X) as after it (q.py fn X --json)
    ap.add_argument('--json', dest='top_json', action='store_true', help='machine-readable output')
    ap.add_argument('--limit', dest='top_limit', type=int, default=None)
    ap.add_argument('--index', dest='top_index', default=None)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('callers', parents=[common]); p.add_argument('func')
    p.add_argument('--in', dest='in_func', default=None, metavar='CALLER', help='only the call sites inside this function')
    import qfactcard
    qfactcard.add_arg(p)
    p = sub.add_parser('callees', parents=[common]); p.add_argument('func')
    p.add_argument('--depth', type=int, default=1); p.add_argument('--all', action='store_true')
    maybe = argparse.ArgumentParser(add_help=False)
    maybe.add_argument('--maybe', action='store_true',
                       help='also show uncertain lis pairs (op ending in "?": the lis value may not reach the use on '
                            'every path)')
    p = sub.add_parser('str', parents=[common, maybe]); p.add_argument('regex')
    p.add_argument('-i', '--ignore-case', action='store_true')
    p.add_argument('--unref', action='store_true', help='also list strings with no code/data reference')
    p = sub.add_parser('imm', parents=[common, maybe]); p.add_argument('value')
    p = sub.add_parser('rtti', parents=[common]); p.add_argument('name'); p.add_argument('--slots', action='store_true')
    p = sub.add_parser('addr', parents=[common, maybe]); p.add_argument('addr'); p.add_argument('--host', action='store_true')
    p.add_argument('--map', help='with --host: resolve against this linker map instead of the indexed one')
    p = sub.add_parser('db', parents=[common]); p.add_argument('func')
    p = sub.add_parser('fn', parents=[common, maybe]); p.add_argument('func')
    sub.add_parser('info', parents=[common])
    args = ap.parse_args(argv)
    args.json = args.json or args.top_json
    if args.top_limit is not None:
        args.limit = args.top_limit
    if args.top_index is not None:
        args.index = args.top_index
    try:
        con = connect(args.index)
        m = meta(con)
        # S110 QREBUILD: the linker map is no input of index.db any more (it changed with every relink and made every addr / fn call rebuild); host symbols:
        # host_map(). S103's rule kept (never stale silently), without the cost: a stale index answers AT ONCE with one `stale:` line and at most one
        # background build (stale_line); the lookup never rebuilds and never waits.
        st = staleness(m, ('init_cpp', 'image'))
        if st and args.cmd != 'info' and os.path.abspath(args.index) == os.path.abspath(DB):
            st = [stale_line(m, st)]
        for w in st:
            print(f'q: WARNING {w}', file=sys.stderr)
        if m.get('default_gen') is False:
            print(f'q: NOTE this index was built from {m.get("gen_dir")} (not generated/default)', file=sys.stderr)
        if args.cmd == 'callers':
            a, note = resolve(con, args.func); r = cmd_callers(con, a, args); pr = print_callers
        elif args.cmd == 'callees':
            a, note = resolve(con, args.func); r = cmd_callees(con, a, args); pr = print_callees
        elif args.cmd == 'str':
            note = None; r = cmd_str(con, args.regex, args); pr = print_str
        elif args.cmd == 'imm':
            note = None; r = cmd_imm(con, args.value, args); pr = print_imm
        elif args.cmd == 'rtti':
            note = None; r = cmd_rtti(con, args.name, args); pr = print_rtti
        elif args.cmd == 'addr':
            note = None; r = cmd_addr(con, args.addr, args, m); pr = print_kv
        elif args.cmd == 'db':
            a, note = resolve(con, args.func, allow_data=True); r = cmd_db(con, a, args); pr = print_db
        elif args.cmd == 'fn':
            a, note = resolve(con, args.func); r = cmd_fn(con, a, args); pr = print_fn
        else:
            note = None; r = cmd_info(con, args, m); pr = print_kv
        if note:
            r['note'] = note
        if args.json:
            print(json.dumps(r, indent=1))
        else:
            if note:
                print(f'({note})')
            pr(r, args)
        return 0
    except QError as e:
        alt = None
        if 'unknown function' in str(e) and getattr(args, 'func', None):    # S108 FD: no dead end: what the other indexes hold for the name
            import qfind
            alt = qfind.elsewhere(qfind.ROOT, args.func)
        if getattr(args, 'json', False):
            print(json.dumps({'error': str(e), **({'elsewhere': alt} if alt else {})}))
        else:
            print(f'q: {e}', file=sys.stderr)
            for ln in alt or []:
                print(ln)
        return 2
    except BrokenPipeError:   # `q ... | head`: the reader went away, not an error
        try:
            sys.stdout = open(os.devnull, 'w')
        except OSError:
            pass
        return 0
    except Exception as e:   # a bug in q itself: still one JSON object for agents (exit 3), traceback on stderr
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qm
        if isinstance(e, OSError) and qm.closed_pipe(e):   # S106 QM: Windows' closed pipe is OSError [Errno 22] (or EPIPE), not BrokenPipeError
            qm.silence_stdout()
            return 0
        import traceback
        traceback.print_exc()
        if getattr(args, 'json', False):
            print(json.dumps({'error': f'internal error: {e!r}'}))
        else:
            print(f'q: internal error: {e!r}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(errors='replace')   # a db summary with a non-cp1252 character must not crash text output
        except (AttributeError, ValueError):
            pass
    # QTF (S103 WTA fix 5): a hard timeout for every q call (WWE_Q_TIMEOUT s, default 120, 0 = off; `sdk|asset update` get 10x).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import qtimeout
    _secs = qtimeout.timeout_secs()
    if sys.argv[1:3] in (['sdk', 'update'], ['asset', 'update']):
        _secs *= 10
    elif sys.argv[1:2] == ['multi']:      # S108 FD: each question of a batch gets the usual time (up to 5 of them)
        _secs *= min(max(1, len([a for a in sys.argv[2:] if a != '--json'])), 5)
    qtimeout.mark_owner()
    qtimeout.start_watchdog(_secs)
    import qm   # S106 QM: every dispatch (ours / py / show / fact / map ...) ends quietly on a closed stdout pipe, also at the final flush
    sys.exit(qm.guarded(main))
