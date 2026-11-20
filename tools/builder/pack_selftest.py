"""Selftest of tools/builder/pack.py (run: py -I tools/builder/pack.py --selftest).

Uses only the invented fixtures under tools/builder/fixtures/ (our own text: a fake note and fake source files). The fixture tree has two
versions: fixtures/new (the target tree) and fixtures/old over it (the tree the fake note was written against: only mover.cpp differs).
fixtures/new/fake_generated stands in for generated\\ and must never be copied into a pack.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, 'fixtures')
SENTINEL = 'PACK_MUST_NOT_COPY_THIS_SENTINEL'

FAILS = []
PASSES = [0]


def check(cond, what):
    if cond:
        PASSES[0] += 1
    else:
        FAILS.append(what)
        print(f'FAIL: {what}')


def run(pack):
    alias = {'fake_generated/': 'generated/'}
    new = pack.DirTree([os.path.join(FIX, 'new')], alias=alias, label='fixtures/new')
    old = pack.DirTree([os.path.join(FIX, 'old'), os.path.join(FIX, 'new')], alias=alias, label='fixtures/old')

    # 0. null check: a note with no citations gives an empty pack, and the CLI exits 0
    nul = pack.Note('null_note.md', open(os.path.join(FIX, 'null_note.md'), encoding='utf-8').read(), None, old)
    p0 = pack.build([nul], new, use_q=False)
    check(len(p0.sites) == 0 and len(p0.blocks) == 0 and len(p0.pointers) == 0, 'null: no sites, blocks or pointers')
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'null_pack.md')
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'pack.py'), os.path.join(FIX, 'null_note.md'), '--out', out, '--no-q'],
                           capture_output=True, text=True, timeout=120)
        check(r.returncode == 0, f'null: CLI exit 0 (got {r.returncode}: {r.stderr.strip()[-200:]})')
        check(os.path.exists(out) and 'sites 0' in open(out, encoding='utf-8').read(), 'null: CLI wrote a pack that says sites 0')

    note = pack.Note('note.md', open(os.path.join(FIX, 'note.md'), encoding='utf-8').read(), None, old)
    p = pack.build([note], new, use_q=False)
    md = p.md

    def sites_of(path):
        return [s for s in p.sites if s.file == path]

    def block_of(path, line):
        return [b for b in p.blocks if b.file == path and b.start <= line <= b.end and getattr(b, 'rule', 'cite') == 'cite']

    W = 'src/alpha/widget.cpp'
    # 1. backslash path, one line, under "Fix sites" -> ranks as fix; block = the whole ApplyWidget (11-18)
    s12 = [s for s in sites_of(W) if s.lines == [(12, 12)]]
    check(len(s12) >= 1, 'backslash: src\\alpha\\widget.cpp:12 parsed')
    check(any(s.fix for s in s12), 'backslash: the site under "Fix sites" ranks as fix')
    b = block_of(W, 12)
    check(len(b) == 1 and (b[0].start, b[0].end) == (11, 18) and 'ApplyWidget' in b[0].names, f'block: ApplyWidget 11-18 (got {[(x.start, x.end, x.names) for x in b]})')
    # 2. forward slashes + a range -> ResetWidget 20-23
    s20 = [s for s in sites_of(W) if s.lines == [(20, 22)]]
    check(len(s20) == 1 and not s20[0].fix, 'forward slashes + range :20-22 parsed, under Answer (not fix)')
    b = block_of(W, 21)
    check(len(b) == 1 and (b[0].start, b[0].end) == (20, 23), 'block: ResetWidget 20-23')
    # 3. bare-name list widget.cpp:12,32,41-42 -> resolved by the unique basename; three sites
    lst = [s for s in sites_of(W) if s.token.replace('\\', '/') == 'widget.cpp']
    got = sorted(l for s in lst for l in s.lines)
    check((32, 32) in got and (41, 42) in got and (12, 12) in got, f'bare list: 12, 32, 41-42 (got {got})')
    check(len(block_of(W, 32)) == 1 and 'CountWidgets' in block_of(W, 32)[0].names, 'bare list: CountWidgets block')
    check(len(block_of(W, 41)) == 1 and 'WidgetTail' in block_of(W, 41)[0].names, 'bare list: WidgetTail block')
    # sites in one function merge into one block (12 from fix 1 and from the list)
    check(len([x for x in p.blocks if x.file == W and x.start <= 12 <= x.end]) == 1, 'merge: one block for ApplyWidget')
    # 4. ambiguous bare name: listed with both candidates, no block
    amb = [s for s in p.sites if s.status == 'ambiguous']
    check(len(amb) == 1 and sorted(amb[0].candidates) == ['src/alpha/core.h', 'src/beta/core.h'], f'ambiguous: core.h with 2 candidates (got {[(s.token, s.candidates) for s in amb]})')
    check('AMBIGUOUS' in md and 'src/beta/core.h' in md, 'ambiguous: printed with candidates')
    # 5. a bare ":8" continues the file cited before it in the same item
    sb = sites_of('src/beta/core.h')
    lines_b = sorted(l for s in sb for l in s.lines)
    check((5, 5) in lines_b and (8, 8) in lines_b, f'continuation: src/beta/core.h :5 and :8 (got {lines_b})')
    check(len(block_of('src/beta/core.h', 5)) == 1 and 'BetaRow' in block_of('src/beta/core.h', 5)[0].names, 'struct block: BetaRow')
    # 6. drift moved: old :10 -> new :13
    M = 'src/alpha/mover.cpp'
    mv = [s for s in sites_of(M) if s.lines == [(10, 10)]]
    check(len(mv) == 1 and mv[0].drift == 'moved' and mv[0].mapped == [(13, 13)], f'drift moved: L10 -> L13 (got {[(s.drift, s.mapped) for s in mv]})')
    check('L10 -> L13' in md.replace('`', ''), 'drift moved: printed "L10 -> L13"')
    # 7. drift gone: old :16-17 rewritten; the enclosing function named in the note (TickMover) is shown instead
    gn = [s for s in sites_of(M) if s.lines == [(16, 17)]]
    check(len(gn) == 1 and gn[0].drift == 'gone', f'drift gone: :16-17 (got {[(s.drift, s.mapped) for s in gn]})')
    check('GONE: the cited lines were rewritten since the note' in md, 'drift gone: printed GONE')
    check(len(block_of(M, 18)) == 1 and 'TickMover' in block_of(M, 18)[0].names, 'drift gone: TickMover shown by name')
    # 8. a file with a function named beside it and no line -> that function's block
    check(any(s.file == W and not s.lines and 'ResetWidget' in s.names for s in p.sites), 'named function: ResetWidget site with no line')
    # 9. a lambda inside a function -> the block is the enclosing function (Helper 25-29)
    b = block_of(W, 27)
    check(len(b) == 1 and (b[0].start, b[0].end) == (25, 29) and 'Helper' in b[0].names, f'lambda: Helper 25-29 (got {[(x.start, x.end, x.names) for x in b]})')
    # 10. bare name found only under tools\ -> python def block
    T = 'tools/fake/fake_tool.py'
    b = block_of(T, 11)
    check(len(b) == 1 and any('bump' in n for n in b[0].names), f'tools bare name: fake_tool.py:11 -> Counter.bump (got {[(x.start, x.end, x.names) for x in b]})')
    # 11. pointers into generated\ and guest addresses: listed with the q command, text never copied
    ptxt = ' '.join(x.text for x in p.pointers)
    check('recomp.12.cpp' in ptxt and '77.cpp' in ptxt, f'pointers: both listing files listed (got {ptxt[:200]})')
    check('q.py decomp sub_82005678' in md and '0x82001234' in md, 'pointers: q commands for sub_X and the address')
    check(SENTINEL not in md, 'pointers: generated text is never copied (sentinel absent)')
    check(not any((x.file or '').startswith('generated/') for x in p.blocks), 'pointers: no block from generated/')
    # 12. the sentence and heading of a site are kept
    check('give the widget a clamp before the add' in md and 'Fix sites' in md, 'sentence + heading kept')
    # 13. fix blocks come before the rest
    first_fix = min((i for i, x in enumerate(p.blocks) if x.fix), default=99)
    first_other = min((i for i, x in enumerate(p.blocks) if not x.fix), default=99)
    check(first_fix < first_other, 'ranking: fix blocks first')
    # 14. size cap: non-fix blocks shrink to 8 lines either side, fix blocks stay whole, and the pack says so
    big = [x for x in p.blocks if x.file == 'src/alpha/big.cpp' and getattr(x, 'rule', 'cite') == 'cite']
    check(len(big) == 1 and (big[0].start, big[0].end) == (4, 69), f'big: whole BigSum 4-69 under the default cap (got {[(x.start, x.end) for x in big]})')
    ps = pack.build([note], new, use_q=False, max_kb=9)
    big = [x for x in ps.blocks if x.file == 'src/alpha/big.cpp' and getattr(x, 'rule', 'cite') == 'cite']
    check(len(big) == 1 and (big[0].start, big[0].end) == (32, 48) and big[0].shrunk, f'size cap: big.cpp shrunk to 32-48 (got {[(x.start, x.end) for x in big]})')
    aw = [x for x in ps.blocks if x.file == W and x.start <= 12 <= x.end and getattr(x, 'rule', 'cite') == 'cite']
    check(len(aw) == 1 and (aw[0].start, aw[0].end) == (11, 18) and not aw[0].shrunk, 'size cap: the fix block keeps its whole function')
    check('shrunk' in ps.md and ps.counts['shrunk'] >= 1, 'size cap: the pack says what was shrunk')
    # 14b. S105 QB: the cap is HARD and shrinks, never skips: under 7 KB the fix blocks shrink too (cited lines kept), no resolved site loses its block
    ph = pack.build([note], new, use_q=False, max_kb=7)
    check(len(ph.md.encode('utf-8')) <= 7 * 1024 and not ph.over_cap, f'hard cap: the pack fits 7 KB (got {len(ph.md.encode("utf-8")) / 1024:.2f})')
    aw = [x for x in ph.blocks if x.file == W and x.start <= 12 <= x.end and getattr(x, 'rule', 'cite') == 'cite']
    check(len(aw) == 1 and aw[0].shrunk and 12 in aw[0].cited and (aw[0].end - aw[0].start) < 8, f'hard cap: the fix block shrank to its cited line (got {[(x.start, x.end) for x in aw]})')
    check(all(x.block is not None for x in ph.sites if x.status == 'ok' and x.drift != 'gone'), 'hard cap: no resolved site is skipped')
    check('HARD CAP' in ph.md, 'hard cap: the pack says it')
    pt = pack.build([note], new, use_q=False, max_kb=1)
    check(pt.over_cap and 'STILL OVER THE CAP' in pt.md and any(x.rule == 'cite' for x in pt.blocks), 'hard cap: a cap below the floor is said loudly and still holds the cited code')
    # 14c. S105 QB review fix: the level-0 clip is said (CLIPPED line, Contents row, no "always kept"), the header does not claim "fix blocks kept whole"
    nclip = pack.Note('clip.md', '## Fix sites\n\n1. `src/alpha/big.cpp:4-60` the whole sum.\n', None, old)
    pc = pack.build([nclip], new, use_q=False, max_kb=1)
    check('CLIPPED: L44-60' in pc.md and 'show src/alpha/big.cpp:44-60' in pc.md and '[CLIPPED: L44-60 not shown]' in pc.md, 'clip: a cut range is named in the block and in Contents with the q show command')
    check('cited lines always kept' not in pc.md and 'fix blocks kept whole' not in pc.md, 'clip: the header does not claim cited lines always kept / fix blocks whole')
    check('fix blocks kept whole' not in ph.md, 'hard cap: no "fix blocks kept whole" beside the hard-cap line')
    # 14d. S105 QB review fix: exit 3 only for sites that could not become code; all-new-files = warning, exit 0; pointers-only = note, exit 0
    with tempfile.TemporaryDirectory() as td:
        def cli(name, text):
            nn = os.path.join(td, name)
            with open(nn, 'w', encoding='utf-8') as f:
                f.write(text)
            return subprocess.run([sys.executable, '-I', os.path.join(HERE, 'pack.py'), nn, '--out', os.path.join(td, name + '.out.md'), '--no-q'], capture_output=True, text=True, timeout=120)
        r = cli('amb.md', '# FAKE\n\n## Sites the build will touch\n\n1. `README.md:1` ambiguous on purpose: the repo has several README.md files.\n')
        check(r.returncode == 3 and 'NO CODE RESOLVED' in r.stderr, f'no code: an ambiguous-only site list exits 3 (got {r.returncode}: {r.stderr.strip()[-160:]})')
        r = cli('new.md', '# FAKE\n\n## Sites the build will touch\n\n1. `src/alpha/does_not_exist.cpp:5` a new file.\n2. `src/alpha/also_new.h:1` another.\n')
        check(r.returncode == 0 and 'new files?' in r.stderr, f'no code: all-new-file sites are a warning, exit 0 (got {r.returncode}: {r.stderr.strip()[-160:]})')
        r = cli('ptr.md', '# FAKE\n\n## Sites the build will touch\n\n1. `generated\\default\\recomp.12.cpp:2` a pointer only.\n')
        check(r.returncode == 0 and 'pointers' in open(os.path.join(td, 'ptr.md.out.md'), encoding='utf-8').read(), f'no code: a pointers-only note exits 0 and carries the note (got {r.returncode})')
        check('game code (pointers) only' in open(os.path.join(td, 'ptr.md.out.md'), encoding='utf-8').read(), 'no code: the pointers-only pack says so')


    # 15. counts in the header
    c = p.counts
    check(c['moved'] == 1 and c['gone'] == 1 and c['ambiguous'] == 1 and c['pointers'] >= 4, f'counts: moved 1 gone 1 ambiguous 1 pointers >= 4 (got {c})')
    # 16. the planted fault: with the bare-name resolver off, the bare sites stay unresolved
    pb = pack.build([note], new, use_q=False, bare=False)
    check(len([s for s in pb.sites if s.status == 'ok']) < len([s for s in p.sites if s.status == 'ok']), 'planted: --no-bare resolves fewer sites')
    check(not [x for x in pb.blocks if x.file == T], 'planted: --no-bare loses the tools bare-name block')


def run2(pack):
    """round 2: the review findings 1-11 (one case each), the hard-rule-1 guard, the companion rules, the drift planted fault"""
    alias = {'fake_generated/': 'generated/', 'fake_thirdparty/': 'thirdparty/', 'fake_out/': 'out/'}
    new = pack.DirTree([os.path.join(FIX, 'new')], alias=alias, label='fixtures/new')
    try:
        old = pack.DirTree([os.path.join(FIX, 'old'), os.path.join(FIX, 'new')], alias=alias, label='fixtures/old', hide=['src/alpha/renamed.cpp'])
    except TypeError:       # (--against an older pack.py: let each case below fail on its own)
        old = pack.DirTree([os.path.join(FIX, 'old'), os.path.join(FIX, 'new')], alias=alias, label='fixtures/old')
    note = pack.Note('note2.md', open(os.path.join(FIX, 'note2.md'), encoding='utf-8').read(), None, old)
    p = pack.build([note], new, use_q=False)
    md = p.md
    Z = 'src/alpha/zip.cpp'
    cite = [b for b in p.blocks if getattr(b, 'rule', 'cite') == 'cite']

    def site(f, lines):
        return [s for s in p.sites if s.file == f and s.lines == lines]
    # R1: a list where L10 was rewritten and L50 moved: each original range is paired with ITS own result
    s = site(Z, [(10, 10), (50, 50)])
    note1 = s[0].drift_note if s else ''
    check('L10 -> L52' not in note1 and 'L50 -> L52' in note1 and 'L10 -> GONE' in note1, f'R1: L10 -> GONE, L50 -> L52 (got "{note1}")')
    # R2: a range with one rewritten line is "partly gone" with its region, the rewritten line is not marked
    s = site(Z, [(20, 22)])
    check(bool(s) and s[0].drift == 'partly gone' and s[0].gone_regions, f'R2: 20-22 partly gone with a region (got {[(x.drift, x.gone_regions) for x in s]})')
    bz = [b for b in cite if b.file == Z and b.start <= 22 <= b.end]
    check(bool(bz) and 22 in bz[0].cited and 24 in bz[0].cited and 23 not in bz[0].cited, 'R2: kept lines 22 / 24 marked, the rewritten 23 not')
    # R3: the file is not in the note's tree: "not mapped", never silently taken as moved / same
    s = site('src/alpha/renamed.cpp', [(3, 3)])
    check(bool(s) and s[0].drift == 'notmapped' and 'not mapped' in s[0].drift_note, f'R3: renamed.cpp not mapped (got {[(x.drift, x.drift_note) for x in s]})')
    # R4: a line past the end of the file is dropped and said so, never clamped to the last line
    s = [x for x in p.sites if x.file == 'src/alpha/widget.cpp' and x.lines == [(400, 400)]]
    check(bool(s) and s[0].status == 'beyond' and 'beyond EOF' in s[0].drift_note, f'R4: widget.cpp:400 dropped (got {[(x.status, x.drift_note) for x in s]})')
    check(not [b for b in cite if b.file == 'src/alpha/widget.cpp' and 45 in b.cited], 'R4: the last line of widget.cpp is not marked cited')
    # R6-R9: the brace scan follows extern "C", #if / #else, a constructor's brace member, macro-made functions
    sy = {x.name: (x.start, x.end) for x in pack.symbols('src/alpha/shapes.cpp', new.read('src/alpha/shapes.cpp'))}
    check(sy.get('CFunc') == (3, 5), f'R6: extern "C" keeps CFunc 3-5 (got {sy.get("CFunc")})')
    check(sy.get('After') == (16, 18) and sy.get('Pick', (0, 0))[1] == 14, f'R7: #if / #else: Pick ends 14, After 16-18 (got {sy.get("Pick")}, {sy.get("After")})')
    check(sy.get('Holder::Holder') == (21, 24), f'R8: constructor with y_{{a}} = 21-24 (got {sy.get("Holder::Holder")})')
    check(sy.get('sub_82000010') == (29, 31) and sy.get('sub_82000020') == (33, 35) and 'REX_HOOK_RAW' not in sy,
          f'R9: hooks named by their first argument (got {[k for k in sy if "8200" in k or "HOOK" in k]})')
    bh = [b for b in cite if b.file == 'src/alpha/shapes.cpp' and b.start <= 34 <= b.end]
    check(bool(bh) and bh[0].names == ['sub_82000020'], f'R9: the :34 block is the second hook (got {[x.names for x in bh]})')
    # R7 (second half): a file whose braces do not balance gets no trusted symbol: a plain window, and the header says so
    check(isinstance(pack.symbols('src/alpha/unbalanced.cpp', new.read('src/alpha/unbalanced.cpp')), list)
          and getattr(pack.symbols('src/alpha/unbalanced.cpp', new.read('src/alpha/unbalanced.cpp')), 'untrusted', False), 'R7: unbalanced file -> untrusted')
    check('brace scan not trusted' in md and 'src/alpha/unbalanced.cpp' in md, 'R7: the pack says the parse was not trusted')
    # R10: a triple-quoted string with column-0 lines does not end the def
    py = {x.name: (x.start, x.end) for x in pack.symbols('tools/fake/doc_tool.py', new.read('tools/fake/doc_tool.py'))}
    check(py.get('has_doc') == (4, 9), f'R10: has_doc 4-9 (got {py.get("has_doc")})')
    # round 2 item 4: a bare name only in an untracked folder, and an out\ path: never expanded
    s = [x for x in p.sites if x.token == 'hidden_only.h']
    check(bool(s) and s[0].status == 'untracked' and not [b for b in p.blocks if b.file.startswith(('thirdparty/', 'out/'))],
          f'G1: hidden_only.h untracked, no block (got {[x.status for x in s]})')
    check('out/scratch/thing.cpp' in md and SENTINEL not in md, 'G2: out\\ path named as scratch, its text never copied')
    try:
        companion_cases(pack, new, old, note)
    except Exception as e:
        FAILS.append(f'companion / drift-plant cases crashed: {e!r}')
        print(f'FAIL: companion / drift-plant cases crashed: {e!r}')


def companion_cases(pack, new, old, note):
    # companions: every rule fires once on the fixtures, labelled
    q = pack.Note('note.md', open(os.path.join(FIX, 'note.md'), encoding='utf-8').read(), None, old)
    import companions as CM
    pc = pack.build([q, note], new, use_q=False, companion_rules=CM.ALL)
    pdef = pack.build([q, note], new, use_q=False)
    check(not any(getattr(b, 'rule', '') == 'lane' for b in pdef.blocks), 'C: the lane rule is off by default (0 DEV hits)')
    rules = {}
    for b in pc.blocks:
        rules.setdefault(getattr(b, 'rule', 'cite'), []).append(b)
    check(any(b.file == 'src/alpha/widget.cpp' and b.start == 1 for b in rules.get('head', [])), 'C-a: head of widget.cpp')
    check(any(b.file == 'tools/fake/widget_unit.cpp' and b.start <= 5 <= b.end for b in rules.get('unit', [])), 'C-b: the unit test naming ApplyWidget (TestApply)')
    check(any(b.file.endswith('fake_widget.json') for b in rules.get('lane', [])), 'C-c: the lane naming the unit')
    check(any(b.file == 'src/beta/reader.cpp' and b.start <= 5 <= b.end for b in rules.get('readers', [])), 'C-d: a reader of kWidgetMax')
    check(any(b.file == 'src/alpha/widget_plants.h' for b in rules.get('plants', [])), 'C-e: the plants table beside widget.cpp')
    check(set(pc.rule_bytes) >= {'cite', 'head', 'unit', 'lane', 'readers', 'plants'}, f'C: bytes per rule (got {sorted(pc.rule_bytes)})')
    check(not any(b.file.startswith(('thirdparty/', 'out/', 'generated/')) for b in pc.blocks) and SENTINEL not in pc.md, 'C: no companion from an untracked folder')
    # planted fault for the drift mapper: with mapping off the moved site keeps its old line, so a drift regression cannot pass
    pd = pack.build([q], new, use_q=False, drift=False)
    mv = [x for x in pd.sites if x.file == 'src/alpha/mover.cpp' and x.lines == [(10, 10)]]
    check(bool(mv) and mv[0].mapped == [(10, 10)] and mv[0].drift == 'same', 'planted drift: mapping off keeps L10 (the check below would fail)')
    pm = pack.build([q], new, use_q=False)
    mv2 = [x for x in pm.sites if x.file == 'src/alpha/mover.cpp' and x.lines == [(10, 10)]]
    check(bool(mv2) and mv2[0].mapped != mv[0].mapped, 'planted drift: the mapped and unmapped answers differ')


def git_cases(pack):
    """against REAL git trees: the tracked-files-only guard (a temp repo), the fixtures exclusion (this repo), a note's own repo (R11)"""
    W_ = os.path.join(tempfile.gettempdir(), f'bpk_selftest_{os.getpid()}')
    import shutil
    shutil.rmtree(W_, ignore_errors=True)
    os.makedirs(os.path.join(W_, 'src', 'a'))
    os.makedirs(os.path.join(W_, 'src', 'b'))
    os.makedirs(os.path.join(W_, 'generated'))
    with open(os.path.join(W_, 'src', 'a', 'dup.cpp'), 'w') as f:
        f.write('// tracked\nint Dup() {\n  return 1;\n}\n')
    with open(os.path.join(W_, 'src', 'b', 'dup.cpp'), 'w') as f:
        f.write('// untracked twin: ' + SENTINEL + '\nint Dup2() { return 2; }\n')
    with open(os.path.join(W_, 'generated', 'only_here.cpp'), 'w') as f:
        f.write('// ignored folder: ' + SENTINEL + '\n')
    with open(os.path.join(W_, '.gitignore'), 'w') as f:
        f.write('generated/\n')
    with open(os.path.join(W_, 'note.md'), 'w') as f:
        f.write('# note\n\n## Fix\n\n1. `dup.cpp:3` the return. `only_here.cpp:1` ignored. `Dup` too.\n')
    env = dict(os.environ, GIT_AUTHOR_NAME='bpk', GIT_AUTHOR_EMAIL='bpk@invalid', GIT_COMMITTER_NAME='bpk', GIT_COMMITTER_EMAIL='bpk@invalid')
    try:
        for cmd in (['init', '-q'], ['add', 'src/a/dup.cpp', '.gitignore', 'note.md'], ['commit', '-q', '-m', 'fixture']):
            subprocess.run(['git', '-C', W_] + cmd, check=True, capture_output=True, env=env, timeout=60)
        t = pack.GitTree('HEAD', root=W_)
        st, f, c = pack.resolve('dup.cpp', t)
        check(st == 'ok' and f == 'src/a/dup.cpp', f'G3: bare dup.cpp -> the tracked one only (got {st} {f} {c})')
        st, f, c = pack.resolve('only_here.cpp', t)
        check(st != 'ok', f'G4: a name only in an ignored folder is not resolved (got {st})')
        hits = t.grep(['Dup2', 'only_here']) if hasattr(t, 'grep') else ['no grep']
        check(not hits, f'G5: grep sees tracked files only (got {len(hits)} hits)')
        n = pack.note_from_file(os.path.join(W_, 'note.md'))
        same = n.tree is not None and os.path.normcase(os.path.abspath(n.tree.root)) == os.path.normcase(os.path.abspath(W_))
        check(n.rev is not None and same, f'R11: a note committed in its own repo gets that repo\'s commit (got {n.rev} {getattr(n.tree, "root", None)})')
        pg = pack.build([n], t, use_q=False)
        check(SENTINEL not in pg.md and any(b.file == 'src/a/dup.cpp' for b in pg.blocks), 'G6: the pack from a real git tree holds the tracked file only')
        loose = os.path.join(tempfile.gettempdir(), f'bpk_loose_{os.getpid()}.md')
        with open(loose, 'w') as f2:
            f2.write('# loose\n\n1. `dup.cpp:2`\n')
        n2 = pack.note_from_file(loose)
        pl = pack.build([n2], t, use_q=False)
        check(n2.rev is None and 'not committed' in pl.md, 'R11: a note with no commit: the target stands in and the header says so')
        os.remove(loose)
    finally:
        shutil.rmtree(W_, ignore_errors=True)
    # R5: this tool's own fixtures are tracked in this repo; a real note's bare names never resolve into them
    t = pack.GitTree('HEAD')
    for nm in ('big.cpp', 'widget.cpp', 'fake_tool.py', 'mover.cpp'):
        st, f, c = pack.resolve(nm, t)
        check('builder/fixtures/' not in (f or '') and not any('builder/fixtures/' in x for x in c),
              f'R5: bare {nm} never resolves into tools/builder/fixtures (got {st} {f} {c[:2]})')


def run3(pack):
    """round 3: the second review's findings (B1-B8) and the 'Sites the build will touch' heading"""
    alias = {'fake_generated/': 'generated/', 'fake_thirdparty/': 'thirdparty/', 'fake_out/': 'out/'}
    new = pack.DirTree([os.path.join(FIX, 'new')], alias=alias, label='fixtures/new')
    old = pack.DirTree([os.path.join(FIX, 'old'), os.path.join(FIX, 'new')], alias=alias, label='fixtures/old')
    note = pack.Note('note3.md', open(os.path.join(FIX, 'note3.md'), encoding='utf-8').read(), None, old)
    p = pack.build([note], new, use_q=False)
    s = [x for x in p.sites if x.file == 'src/alpha/ins.cpp']
    # heading: "Sites the build will touch" ranks like "Fix sites"
    check(bool(s) and s[0].fix, 'H: "Sites the build will touch" ranks as fix')
    # B3: three lines inserted inside a cited range: only the lines that map from cited lines are marked, and it is said
    b = [x for x in p.blocks if x.file == 'src/alpha/ins.cpp' and getattr(x, 'rule', 'cite') == 'cite']
    check(bool(s) and s[0].cited_lines == {2, 6} and 'lines inserted inside' in s[0].drift_note,
          f'B3: cite 2-3 -> marked 2 and 6 only, "lines inserted inside" (got {s[0].cited_lines if s else None}, "{s[0].drift_note if s else ""}")')
    check(bool(b) and 3 not in b[0].cited and 6 in b[0].cited, 'B3: the inserted lines 3-5 are not marked')
    # B6: a macro with no underscore (TEST(Widget, Resets)) is named by its arguments, not TEST
    sy = [x.name for x in pack.symbols('src/alpha/shapes.cpp', new.read('src/alpha/shapes.cpp'))]
    check('TEST' not in sy and 'Widget.Clamps' in sy and 'Widget.Resets' in sy, f'B6: TEST(...) blocks named by their arguments (got {[x for x in sy if "Widget" in x or x == "TEST"]})')
    # B5: the readers rule says what it did not show
    check('more places not shown' in p.md, 'B5: kManyUse has 15 readers: "N more places not shown"')
    import companions as CM
    keep = CM.GENERIC_FILES
    try:
        CM.GENERIC_FILES = 1
        pg = pack.build([note], new, use_q=False)
        check('kManyUse' in pg.md.split('## Contents')[0] and 'skipped' in pg.md.split('## Contents')[0],
              'B5: a name in too many files is listed as skipped in the header')
    finally:
        CM.GENERIC_FILES = keep
    check(hasattr(CM, 'READERS_TOTAL') and sum(1 for x in p.blocks if getattr(x, 'rule', '') == 'readers') <= CM.READERS_TOTAL, 'B5: a total cap across names')
    # B4: caches keyed by id() hold their keys and check identity: a planted stale entry is never returned
    xa, xb = ['a', 'b', 'c'], ['a', 'x', 'b', 'c']
    pack._MAPS[(id(xa), id(xb))] = (['zz'], ['zz'], {1: 999})
    try:
        m = pack.line_map(xa, xb)
        ok = isinstance(m, dict) and m.get(1) == 1
    except Exception:
        ok = False
    check(ok, 'B4: a stale _MAPS entry (other lists, same ids) is not returned')
    ln = ['int F() {', '  return 1;', '}']
    pack._SYMS[('x.cpp', id(ln))] = (['other'], [])
    try:
        sy = pack.symbols('x.cpp', ln)
        ok = [x.name for x in sy] == ['F']
    except Exception:
        ok = False
    check(ok, 'B4: a stale _SYMS entry is not returned')
    # B7: each companion rule, switched off, loses the fixture line built to need it
    q = pack.Note('note.md', open(os.path.join(FIX, 'note.md'), encoding='utf-8').read(), None, old)
    n2 = pack.Note('note2.md', open(os.path.join(FIX, 'note2.md'), encoding='utf-8').read(), None, old)
    need = {'head': ('src/alpha/widget.cpp', 2), 'plants': ('src/alpha/widget_plants.h', 3), 'readers': ('src/beta/reader.cpp', 5),
            'unit': ('tools/fake/widget_unit.cpp', 6)}

    def held(pk, f, ln):
        return any(x.file == f and x.start <= ln <= x.end for x in pk.blocks)
    on = pack.build([q, n2], new, use_q=False, max_kb=100000)
    for rule, (f, ln) in need.items():
        off = pack.build([q, n2], new, use_q=False, max_kb=100000, companion_rules=tuple(r for r in CM.ENABLED if r != rule))
        check(held(on, f, ln) and not held(off, f, ln), f'B7: rule {rule} off loses {f}:{ln}')
    # B8: a fix hunk spanning two adjacent blocks counts as held
    import pack_refind as PR
    b1, b2 = pack.Block('src/x.cpp', 10, 20), pack.Block('src/x.cpp', 21, 30)
    check(PR.holds_any([b1, b2], 'src/x.cpp', 18, 6) and not PR.holds_any([b1], 'src/x.cpp', 18, 6), 'B8: a hunk over two adjacent blocks is held')
    check(PR.holds_any([b1, b2], 'src/x.cpp', 20, 0) and not PR.holds_any([b1, pack.Block('src/x.cpp', 23, 30)], 'src/x.cpp', 21, 2),
          'B8: an insertion between adjacent blocks is held; a gap is not')


def edited_note_cases(pack):
    """B1 / B2 against a real git repo: an edited committed note and an uncommitted note are read as written, and each site says so"""
    import shutil
    W_ = os.path.join(tempfile.gettempdir(), f'bpk_selftest3_{os.getpid()}')
    shutil.rmtree(W_, ignore_errors=True)
    os.makedirs(os.path.join(W_, 'src'))
    env = dict(os.environ, GIT_AUTHOR_NAME='bpk', GIT_AUTHOR_EMAIL='bpk@invalid', GIT_COMMITTER_NAME='bpk', GIT_COMMITTER_EMAIL='bpk@invalid')
    try:
        with open(os.path.join(W_, 'src', 'f.cpp'), 'w') as f:
            f.write('// v1\nint A() {\n  return 1;\n}\n')
        with open(os.path.join(W_, 'note.md'), 'w') as f:
            f.write('# n\n\n## Fix\n\n1. `src/f.cpp:3` the return.\n')
        for cmd in (['init', '-q'], ['add', '.'], ['commit', '-q', '-m', 'v1']):
            subprocess.run(['git', '-C', W_] + cmd, check=True, capture_output=True, env=env, timeout=60)
        with open(os.path.join(W_, 'src', 'f.cpp'), 'w') as f:
            f.write('// v2\n// a line added on top\n// and another\nint A() {\n  return 1;\n}\nint B() {\n  return 2;\n}\n')
        for cmd in (['add', '.'], ['commit', '-q', '-m', 'v2']):
            subprocess.run(['git', '-C', W_] + cmd, check=True, capture_output=True, env=env, timeout=60)
        with open(os.path.join(W_, 'note.md'), 'a') as f:
            f.write('2. `src/f.cpp:8` B\'s return, written against today\'s file.\n')
        n = pack.note_from_file(os.path.join(W_, 'note.md'))
        check(n.rev is None and 'uncommitted edits' in n.tree_note, f'B1: an edited note counts as uncommitted (got {n.rev}, "{n.tree_note}")')
        p = pack.build([n], pack.GitTree('HEAD', root=W_), use_q=False)
        s8 = [x for x in p.sites if x.lines == [(8, 8)]]
        check(bool(s8) and s8[0].drift not in ('gone', 'moved') and 8 in (s8[0].block.cited if s8[0].block else set()),
              f'B1: the new cite :8 is read as written (got {[(x.drift, x.drift_note) for x in s8]})')
        check(all('not mapped: the note has no commit' in x.drift_note for x in p.sites) and 'not mapped: the note has no commit' in p.md,
              'B2: every site of an uncommitted note says it was not mapped')
    finally:
        shutil.rmtree(W_, ignore_errors=True)


def main(pack_dir=None):
    sys.path.insert(0, pack_dir or HERE)
    import pack
    for fn in (run, run2, git_cases, run3, edited_note_cases):
        try:
            fn(pack)
        except Exception as e:   # a crash is a fail, with its line
            import traceback
            traceback.print_exc()
            FAILS.append(f'crash in {fn.__name__}: {e!r}')
    print(f'pack selftest: {PASSES[0]} passed, {len(FAILS)} failed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    # --against DIR: run these cases against another copy of pack.py (to see a new case fail on the old code first)
    sys.exit(main(sys.argv[sys.argv.index('--against') + 1] if '--against' in sys.argv else None))
