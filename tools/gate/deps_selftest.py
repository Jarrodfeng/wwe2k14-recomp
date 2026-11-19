r"""Self test for tools\gate\deps.py and the manifest-keyed tool cache (Session 103, lane DIB). 0 game runs, no compiler: it plants trees in a temp folder.
  py -I tools\gate\deps_selftest.py
Part 1 (manifests): the key a manifest gives must change when a file the build compiled changes and must NOT change when an unrelated file does; a manifest
that is missing / INVALID falls back to the whole-tree key; a new file in a directory the build lives in, or a new directory, changes the key.
Later parts (selection, lint) are added by the later pieces of the lane.
"""
import json, os, re, shutil, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import deps as gdeps
import toolcache as gtc

FAILS = []


def ok(c, t):
    print(('ok    ' if c else 'FAIL  ') + t)
    if not c:
        FAILS.append(t)


def mk_tree(base, name, files):
    r = Path(base) / name
    for rel, txt in files.items():
        p = r / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(txt, encoding='utf-8')
    (r / 'tools').mkdir(exist_ok=True)
    (r / 'tools' / 'sdk_pins.json').write_text('{}')
    return r


def part1(base):
    quiet = lambda *_a, **_k: None
    files = {'src/a/main.cpp': '#include "x.h"\n', 'src/a/x.h': 'int x;\n', 'src/b/other.cpp': 'int o;\n', 'src/b/unrelated.h': 'int u;\n',
             'tools/scene/build_t.ps1': '# build\n'}
    t = mk_tree(base, 't', files)
    for d in (gdeps.forget,):
        d()
    man_exe = ['src/a/main.cpp', 'src/a/x.h', 'tools/scene/build_t.ps1']
    # the exe manifest + the script manifest as the build writes them
    gdeps.write_manifest(t / 'out/tools/t.exe.deps.txt', man_exe)
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_t.ps1.deps.txt', man_exe)
    (t / 'out/tools/t.exe').write_bytes(b'EXE')
    cache = Path(base) / 'cache'
    c = gtc.ToolCache(t, cache, log=quiet)
    ident = 'tools/scene/build_t.ps1|'
    k_tree = c.key(ident)
    ok(c.how[ident] == 'tree', 'no stored manifest: the whole-tree key')
    ok(c.note_manifest(ident, ['out/tools/t.exe', 'out/tools/t.exe.deps.txt', 'out/deps/tools_scene_build_t.ps1.deps.txt']), 'the build\'s manifest is stored')
    k1 = c.key(ident)
    ok(c.how[ident] == 'manifest' and k1 != k_tree, 'a stored manifest: the manifest key')
    ok(c.publish('t', k1, ['out/tools/t.exe', 'out/tools/t.exe.deps.txt']), 'publish under the manifest key')
    # an edit to a file that is NOT in the manifest does not change the key (the old whole-tree key changed on any edit)
    gdeps.forget()
    (t / 'src/b/unrelated.h').write_text('int u2; // edited\n')
    (t / 'src/b/other.cpp').write_text('int o2;\n')
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) == k1, 'edit of files outside the manifest: same key (hit)')
    ok(gtc.tree_digest(t) != gtc.tree_digest(mk_tree(base, 'fresh', files)), '(the old whole-tree digest DID change on that edit)')
    # PLANT: an edit to a header the build compiled changes the key
    (t / 'src/a/x.h').write_text('int x; // edited\n')
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) != k1, 'PLANT: edit of a header in the manifest: new key (miss)')
    (t / 'src/a/x.h').write_text('int x;\n')
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) == k1, 'edit reverted: the same key again')
    # PLANT: a changed build script (a manifest entry) changes the key
    (t / 'tools/scene/build_t.ps1').write_text('# build -DNEW\n')
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) != k1, 'PLANT: edit of the build script: new key')
    (t / 'tools/scene/build_t.ps1').write_text('# build\n')
    # PLANT: a new file in a directory the build lives in (a shadowing header, a source the script's directory walk would pick up) changes the key
    gdeps.forget()
    (t / 'src/a/new_shadow.h').write_text('x')
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) != k1, 'PLANT: a new file next to the compiled files: new key')
    (t / 'src/a/new_shadow.h').unlink()
    gdeps.forget()
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) == k1, 'file removed again: the same key')
    (t / 'src/c').mkdir()
    gdeps.forget()
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) != k1, 'PLANT: a new directory under src: new key (a -Recurse source list may pick it up)')
    (t / 'src/c').rmdir()
    gdeps.forget()
    # a new file in a directory the build does NOT touch: same key
    (t / 'src/b/more.h').write_text('x')
    gdeps.forget()
    ok(gtc.ToolCache(t, cache, log=quiet).key(ident) == k1, 'a new file in a directory the build does not use: same key')
    (t / 'src/b/more.h').unlink()
    # the entry restores in a twin checkout whose manifest files have the same content; a twin with another header does not hit
    twin = mk_tree(base, 'twin', files)
    gdeps.forget()
    ct = gtc.ToolCache(twin, cache, log=quiet)
    ok(ct.key(ident) == k1 and ct.restore('t', k1) is not None and (twin / 'out/tools/t.exe').read_bytes() == b'EXE', 'a twin checkout (same content, no out\\) hits and gets the exe')
    twin2 = mk_tree(base, 'twin2', dict(files, **{'src/a/x.h': 'int different;\n'}))
    gdeps.forget()
    ok(gtc.ToolCache(twin2, cache, log=quiet).key(ident) != k1, 'a twin with a different header: miss')
    # INVALID / unreadable manifests are no manifest
    inv = Path(base) / 'inv.deps.txt'
    inv.write_text('# wwe2k14-deps INVALID: no .d file for x.o\n')
    ok(gdeps.read_manifest(inv) is None and gdeps.invalid_reason(inv) is not None, 'an INVALID manifest reads as none (with its reason)')
    ok(gdeps.read_manifest(Path(base) / 'nope.txt') is None, 'a missing manifest reads as none')
    c2 = gtc.ToolCache(t, Path(base) / 'cache2', log=quiet)
    gdeps.write_manifest(c2._store_path(ident), man_exe)
    gdeps.forget()
    ok(c2.key(ident) == k1 and c2.how[ident] == 'manifest', 'the key depends on the manifest content, not on which cache dir holds it')
    inv2 = c2._store_path(ident)
    inv2.write_text('# wwe2k14-deps INVALID: x\n')
    c2.key(ident)
    ok(c2.how[ident] == 'tree', 'an INVALID stored manifest falls back to the whole-tree key')
    # a manifest that names an out\ generated file is written INVALID by the build; a missing manifest file = 'gone' (a different key, never a crash)
    gdeps.write_manifest(c.dir / '_deps' / 'gone.deps.txt', ['src/a/main.cpp', 'src/a/deleted.h'])
    d_gone = gdeps.digest(gdeps.read_manifest(c.dir / '_deps' / 'gone.deps.txt'), t)
    ok(isinstance(d_gone, str) and d_gone != gdeps.digest(man_exe, t), 'a manifest file that no longer exists is a defined state (gone), not an error')
    # thirdparty entries resolve under the checkout, else the main checkout
    main = mk_tree(base, 'main', {'thirdparty/sdk/h.h': 'T1\n'})
    ents = ['src/a/main.cpp', 'thirdparty/sdk/h.h']
    d1 = gdeps.digest(ents, t, main)
    (main / 'thirdparty/sdk/h.h').write_text('T2\n')
    os.utime(main / 'thirdparty/sdk/h.h', ns=(1_000_000_000, 2_000_000_000))   # GKL fix: same size in one NTFS tick; the hash cache keys on (path, mtime_ns, size)
    ok(gdeps.digest(ents, t, main) != d1, 'a thirdparty header is hashed (from the main checkout for a worktree)')
    # script_manifest lookup order: this checkout, the main checkout, the cache's _scripts store
    ok(gdeps.script_manifest(t, main, 'tools/scene/build_t.ps1') == sorted(man_exe), 'script_manifest reads this checkout\'s out\\deps')
    ok(gdeps.script_manifest(twin, main, 'tools/scene/build_t.ps1') is None, 'script_manifest: a checkout without out\\deps and no store: none')
    ok(gdeps.script_manifest(twin, main, 'tools/scene/build_t.ps1', store=cache / '_scripts') == sorted(man_exe), 'script_manifest falls through to the tool cache store')


def part1b(base):
    """The proof key of a rig-style check (a python script that runs a manifest-writing .ps1 build): cacheable on the build's manifest, honest otherwise."""
    import fastgate as fg
    import proofkey as pk
    files = {'tools/x_check.py': "import subprocess\nsubprocess.run(['pwsh', '-File', 'tools/scene/build_t.ps1'])\n",
             'tools/scene/build_t.ps1': ". (Join-Path $repo 'tools\\env.ps1')\n. (Join-Path $repo 'tools\\cxx_cache.ps1')\n",
             'tools/cxx_cache.ps1': "# names tools\\bench\\run_list.ps1 in a comment only\n", 'tools/env.ps1': '# env\n',
             'tools/bench/run_list.ps1': '# runs the game\n', 'src/a/main.cpp': '#include "x.h"\n', 'src/a/x.h': 'int x;\n', 'src/b/free.h': 'int f;\n'}
    t = mk_tree(base, 'k', files)
    gdeps.forget()
    chk = {'name': 'x.check', 'cmd': ['py', '-I', 'tools/x_check.py'], 'affects': ['tools/x_check.py']}
    key = lambda: fg.cache_key_ex(t, chk, b'{}', [], None, t, 'tc')
    def clear():
        pk._CLOSURE.clear(); pk._FILE_INFO.clear(); gdeps.forget(); fg._MEMO.clear(); pk._DIR_STATE.clear()
    k, why = key()
    ok(k is None and 'no dependency manifest' in why, 'a rig-style check with no build manifest yet is refused, with the reason (' + why[:50] + '...)')
    man = ['src/a/main.cpp', 'src/a/x.h', 'tools/scene/build_t.ps1', 'tools/cxx_cache.ps1', 'tools/env.ps1']
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_t.ps1.deps.txt', man)
    clear()
    k1, why = key()
    ok(k1 is not None and not why, 'with the build manifest: cacheable (the closure no longer drags in run_list.ps1 from a helper\'s comment)')
    (t / 'src/b/free.h').write_text('int f2;\n')
    clear()
    ok(key()[0] == k1, 'an edit outside the build\'s manifest: same key (a hit)')
    (t / 'src/a/x.h').write_text('int x; // edited\n')
    clear()
    k2 = key()[0]
    ok(k2 is not None and k2 != k1, 'PLANT: an edit to a header the build compiled: new key (the check re-runs)')
    (t / 'src/a/x.h').write_text('int x;\n')
    clear()
    ok(key()[0] == k1, 'reverted: same key')
    # PLANT: a stale manifest from before a header was added to an includer: the includer's edit is in the key, so the key still moves
    (t / 'src/a/main.cpp').write_text('#include "x.h"\n#include "y.h"\n')
    (t / 'src/a/y.h').write_text('int y;\n')
    clear()
    ok(key()[0] != k1, 'PLANT: a new #include added to a compiled file (and a new header next to it): new key even though the manifest is older')
    (t / 'src/a/main.cpp').write_text('#include "x.h"\n')
    (t / 'src/a/y.h').unlink()
    clear()
    # INVALID manifest and a non-manifest .ps1 in the closure are refused
    (t / 'out/deps/tools_scene_build_t.ps1.deps.txt').write_text('# wwe2k14-deps INVALID: no .d file for x.o\n')
    clear()
    k3, why3 = key()
    ok(k3 is None and 'no dependency manifest' in why3, 'an INVALID build manifest refuses the check')
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_t.ps1.deps.txt', man)
    (t / 'tools/x_check.py').write_text("import subprocess\nsubprocess.run(['pwsh', '-File', 'tools/scene/build_t.ps1'])\nsubprocess.run(['pwsh', '-File', 'tools/bench/run_list.ps1'])\n")
    clear()
    k4, why4 = key()
    ok(k4 is None and 'does not write dependency manifests' in why4, 'PLANT: the script also runs a .ps1 that writes no manifest (run_list.ps1): refused')


def part2():
    """The shared game_syntax run: parsing, union, and a verdict per check that can never pass on a unit that did not print ok."""
    import gs_share as gs
    base = ['py', '-I', 'tools/gate/game_syntax.py', '--root', '.']
    a, b, c = base, base + ['--files', 'src/x.cpp,src/y.cpp'], base + ['--no-twin', '--files', 'src/z.cpp']
    ok(gs.parse(a) == ([], True) and gs.parse(b) == (['src/x.cpp', 'src/y.cpp'], True) and gs.parse(c) == (['src/z.cpp'], False), 'parse: the three command forms')
    ok(gs.parse(base + ['--weird']) is None and gs.parse(['py', '-I', 'tools/other.py']) is None, 'parse: an unknown option / another script runs alone')
    files, twin = gs.union([gs.parse(a), gs.parse(b), gs.parse(c)])
    ok(files == ['src/x.cpp', 'src/y.cpp', 'src/z.cpp'] and twin, 'union: every file once, twin when any wants it')
    ok(gs.union([gs.parse(c)])[1] is False and '--no-twin' in gs.shared_cmd(*gs.union([gs.parse(c)])), 'union: all --no-twin = no twin')
    good = ('ok   src/main.cpp\nok   src/renderer_adapter/live.cpp\nok   src/x.cpp\nok   src/y.cpp\nok   src/z.cpp\n'
            'ok   planted twin (a member named small after main.cpp): failed to compile, as planted (error: x)\npp-cache: 3 of 6 units served\nPASS: 5 game translation units compile\n')
    ok(gs.verdict(a, 0, good)[0] == 'PASS' and gs.verdict(b, 0, good)[0] == 'PASS' and gs.verdict(c, 0, good)[0] == 'PASS', 'all units ok: every check PASSes')
    # PLANT: unit y.cpp fails -> b FAILs (it asked for y), a and c still PASS (they did not)
    bad = good.replace('ok   src/y.cpp', 'FAIL src/y.cpp\nerror: boom')
    ok(gs.verdict(b, 1, bad)[0] == 'FAIL' and gs.verdict(a, 1, bad)[0] == 'PASS' and gs.verdict(c, 1, bad)[0] == 'PASS', 'PLANT: a failing unit fails exactly the checks that asked for it')
    # PLANT: main.cpp fails -> every check FAILs (all check it)
    bad2 = good.replace('ok   src/main.cpp', 'FAIL src/main.cpp\nerror: small')
    ok(all(gs.verdict(x, 1, bad2)[0] == 'FAIL' for x in (a, b, c)), 'PLANT: a failing default unit fails every shared check')
    # PLANT: the planted twin COMPILED (a blind check) -> twin checks FAIL, a --no-twin check does not care
    blind = good.replace('ok   planted twin (a member named small after main.cpp): failed to compile, as planted (error: x)', 'FAIL: the planted twin compiled: `small` is not a live macro')
    ok(gs.verdict(a, 1, blind)[0] == 'FAIL' and gs.verdict(c, 1, blind)[0] == 'PASS', 'PLANT: a blind twin fails the twin checks only')
    # a run that died before any unit (ninja missing): nobody passes
    ok(all(gs.verdict(x, 1, 'FAIL: ninja not found')[0] == 'FAIL' for x in (a, b, c)), 'a run with no unit lines fails every check (never a silent pass)')
    ok(gs.verdict(a, 0, '')[0] == 'FAIL', 'an empty output fails')
    # exes.infer no longer builds fview for a script that names it only in its docstring
    import exes
    ok('fview' not in exes._without_docstrings('"""runs nothing: names fview and rplay here"""\n# fview in a comment\nx = 1\n') and
       'fview' in exes._without_docstrings('"""doc"""\nsubprocess.run(["out/renderer/dev/fview.exe"])\n'), 'exes.infer: a docstring / comment mention is not a use; a code mention is')


def fake_index(root, tus, incs, shas=None):
    """A tiny ours.db: tus = [(id, file, errors)], incs = {id: [files]}, shas = {path: sha1} (default: the tree's current content)."""
    import hashlib, sqlite3
    d = Path(root) / 'out' / 'harness' / 'index'
    d.mkdir(parents=True, exist_ok=True)
    p = d / 'ours.db'
    if p.exists():
        p.unlink()
    db = sqlite3.connect(p)
    db.execute('create table tus(id integer primary key, file text, build integer, errors integer, first_err text, secs real, cursors integer)')
    db.execute('create table incs(tu integer, file text)')
    db.execute('create table files(path text primary key, sha text, size integer, mtime_ns integer)')
    for tid, f, err in tus:
        db.execute('insert into tus values(?,?,?,?,?,?,?)', (tid, f, 1, err, '', 0.0, 0))
    for tid, fl in incs.items():
        db.executemany('insert into incs values(?,?)', [(tid, x) for x in fl])
    seen = {t[1] for t in tus} | {x for fl in incs.values() for x in fl}
    for f in seen:
        pth = Path(root) / f
        sha = (shas or {}).get(f) or (hashlib.sha1(pth.read_bytes()).hexdigest() if pth.is_file() else 'x')
        db.execute('insert into files values(?,?,?,?)', (f, sha, 0, 0))
    db.commit()
    db.close()


def part3(base):
    """Selection by derived deps: planted dependencies, the S102 08:56 stale-fview re-find, the partial-TU rule, the stale-index fallback."""
    import fastgate as fg
    import proofkey as pk
    import sched as gs

    def reset():
        pk._CLOSURE.clear(); pk._FILE_INFO.clear(); gdeps.forget(); fg._MEMO.clear()

    def sel(root, chk, changed, lane='tools/gate/lanes/x.json', cache=None):
        reset()
        tb = gdeps.Table(root, root, cache_path=Path(root) / 'out/gate/dt.json', log=lambda *_a: None)
        d = tb.get(chk)
        res = fg.selected(chk, changed, lane, d, tb.idx, root)
        tb.close()
        return res, d

    files = {'src/main.cpp': '#include "a.h"\n', 'src/a.h': 'int a;\n', 'src/b.h': 'int b;\n', 'src/c.h': 'int c;\n', 'src/renderer_adapter/reader.h': 'int r;\n',
             'tools/gate/game_syntax.py': '# stub\n', 'tools/gate/gs_share.py': '# stub\n',
             'tools/x_pure.py': "import sys\nprint(open('src/b.h').read())\n",
             'tools/x_scan.py': "import os\nfor dp, dn, fn in os.walk('src'):\n    pass\n",
             'tools/x_fview.py': "import subprocess\nsubprocess.run(['out/renderer/dev/fview.exe', '--x'])\n",
             'tools/x_writer.py': "open('out/x.txt', 'w').write(open('src/b.h').read())\n",
             'tools/fview/build.ps1': ". (Join-Path $repo 'tools\\cxx_cache.ps1')\n", 'tools/cxx_cache.ps1': '# helper\n', 'tools/env.ps1': '# env\n'}
    t = mk_tree(base, 'sel', files)
    gsyn = {'name': 'x.game_syntax', 'cmd': ['py', '-I', 'tools/gate/game_syntax.py', '--root', '.']}
    # ---- game_syntax from the index: units + include closure ----
    fake_index(t, [(1, 'src/main.cpp', 0), (2, 'src/renderer_adapter/live.cpp', 0)], {1: ['src/a.h', 'src/b.h'], 2: ['src/renderer_adapter/reader.h']})
    (t / 'src/renderer_adapter/live.cpp').write_text('#include "reader.h"\n')
    fake_index(t, [(1, 'src/main.cpp', 0), (2, 'src/renderer_adapter/live.cpp', 0)], {1: ['src/a.h', 'src/b.h'], 2: ['src/renderer_adapter/reader.h']})
    (r, why), d = sel(t, gsyn, ['src/b.h'])
    ok(r and d['tier'] == 'A' and d['exact'], 'game_syntax: derived tier A from the index; a header in the closure selects it (' + why[:60] + ')')
    (r, why), d = sel(t, gsyn, ['src/unrelated.h'])
    ok(not r, 'game_syntax with no affects: an unrelated change skips it (derived deps exact)')
    gsyn_aff = dict(gsyn, affects=['src/a.h'])
    (r, why), d = sel(t, gsyn_aff, ['src/b.h'])
    ok(r and 'derived' in why, 'PLANT (the 101-lane hole): affects names only a.h; a change to b.h, which main.cpp includes, now selects it')
    # ---- partial TU: parsed with errors -> not exact, a no-affects check always runs, a dir dep is added ----
    fake_index(t, [(1, 'src/main.cpp', 1), (2, 'src/renderer_adapter/live.cpp', 0)], {1: ['src/a.h'], 2: ['src/renderer_adapter/reader.h']})
    (r, why), d = sel(t, gsyn, ['src/unrelated.h'])
    ok(r and not d['exact'] and d['tier'] == 'D' and 'src' in d['dirs'], 'PLANT: a TU the index parsed with errors is not exact: the no-affects check still runs, and any src change selects it')
    # ---- stale index: a closure file differs from the index and is not in the diff ----
    fake_index(t, [(1, 'src/main.cpp', 0), (2, 'src/renderer_adapter/live.cpp', 0)], {1: ['src/a.h', 'src/b.h'], 2: ['src/renderer_adapter/reader.h']}, shas={'src/c.h': 'x', 'src/b.h': 'deadbeef' * 5})
    (r, why), d = sel(t, gsyn, ['src/unrelated.h'])
    ok(r and 'stale' in why, 'PLANT: the index is stale for b.h (its sha differs and b.h is not in the diff): the check runs (' + why[:50] + ')')
    (r, why), d = sel(t, gsyn, ['src/b.h'])
    ok(r, 'a file that differs from the index AND is in the diff is just a change (selected by derived deps)')
    # ---- missing index: falls back (never a silent skip) ----
    (t / 'out/harness/index/ours.db').unlink()
    (r, why), d = sel(t, gsyn, ['src/unrelated.h'])
    ok(r and d['tier'] == 'D', 'no index: tier D, the no-affects check runs; the gate says so')
    (r, why), d = sel(t, gsyn_aff, ['src/unrelated.h'])
    ok(not r, 'no index, with affects: affects alone decides (as before)')
    # ---- script-derived (tier B) ----
    pure = {'name': 'x.pure', 'cmd': ['py', '-I', 'tools/x_pure.py']}
    (r, why), d = sel(t, pure, ['src/b.h'])
    ok(r and d['tier'] == 'B' and d['exact'], 'tier B: a file the script names (src/b.h) selects it')
    (r, why), d = sel(t, pure, ['src/c.h'])
    ok(not r, 'tier B: a file it does not name does not (it was always-run before)')
    (r, why), d = sel(t, pure, ['tools/x_pure.py'])
    ok(r, 'tier B: the script itself')
    scan = {'name': 'x.scan', 'cmd': ['py', '-I', 'tools/x_scan.py']}
    (r, why), d = sel(t, scan, ['src/c.h'])
    ok(r and d['tier'] == 'C' and not d['exact'], 'PLANT: a script that os.walk()s src is tier C: never skipped, selected by any src change')
    writer = {'name': 'x.writer', 'cmd': ['py', '-I', 'tools/x_writer.py']}
    (r, why), d = sel(t, writer, ['src/c.h'])
    ok(r and d['exact'] and d['writes'] and 'writes' in why, 'a check that writes files is never skipped on derived deps alone')
    inline = {'name': 'x.inline', 'cmd': ['py', '-I', '-c', 'print(1)']}
    (r, why), d = sel(t, inline, ['src/c.h'])
    ok(r and d['tier'] == 'D', 'an inline -c body names no script: opaque, always runs')
    # ---- the S102 08:56 re-find: fview built from a list that missed src/renderer_adapter; the build manifest names reader.h ----
    gdeps.write_manifest(t / 'out/deps/tools_fview_build.ps1.deps.txt', ['src/main.cpp', 'src/renderer_adapter/reader.h', 'tools/fview/build.ps1', 'tools/cxx_cache.ps1'])
    gdeps.write_manifest(t / 'out/renderer/dev/fview.exe.deps.txt', ['src/main.cpp', 'src/renderer_adapter/reader.h', 'tools/fview/build.ps1'])
    fv = {'name': 'x.fview', 'cmd': ['py', '-I', 'tools/x_fview.py']}
    (r, why), d = sel(t, fv, ['src/renderer_adapter/reader.h'])
    ok(r and 'src/renderer_adapter/reader.h' in d['files'], 'S102 08:56 RE-FOUND: touching src/renderer_adapter/reader.h selects the check that runs fview (the old hand-kept source list missed that folder)')
    (r, why), d = sel(t, fv, ['src/c.h'])
    ok(not r, '...and an unrelated header does not')
    (t / 'out/renderer/dev/fview.exe.deps.txt').unlink()
    (r, why), d = sel(t, fv, ['src/c.h'])
    ok(r and not d['exact'], 'no manifest for the exe a script runs: not exact, the check runs')
    # ---- the table cache is invalidated by an edit to a closure file ----
    reset()
    tb = gdeps.Table(t, t, cache_path=t / 'out/gate/dt2.json', log=lambda *_a: None)
    tb.get(pure)
    tb.save()
    tb2 = gdeps.Table(t, t, cache_path=t / 'out/gate/dt2.json', log=lambda *_a: None)
    tb2.get(pure)
    ok(tb2.hits == 1 and tb2.misses == 0, 'the derived table is served from out\\gate\\deps.json')
    (t / 'tools/x_pure.py').write_text("print(open('src/c.h').read())\n")
    reset()
    tb3 = gdeps.Table(t, t, cache_path=t / 'out/gate/dt2.json', log=lambda *_a: None)
    d3 = tb3.get(pure)
    ok(tb3.misses == 1 and 'src/c.h' in d3['files'] and 'src/b.h' not in d3['files'], 'PLANT: the script now reads c.h instead of b.h: the cached entry is not used, the table is recomputed')
    # ---- keys: a no-affects exact write-free check is cacheable and its key follows its derived set; a writer / a scanner stay uncached ----
    (t / 'tools/x_pure.py').write_text("print(open('src/b.h').read())\n")
    reset()
    tb4 = gdeps.Table(t, t, cache_path=t / 'out/gate/dt3.json', log=lambda *_a: None)
    kp = lambda chk, pure_=True: fg.cache_key_ex(t, chk, b'{}', [], None, t, 'tc', None, derived=tb4.get(chk), pure=pure_)
    k1, why1 = kp(pure)
    ok(k1 and not why1, 'a no-affects, exact, write-free check is now cacheable (was: "no affects: inputs cannot be enumerated")')
    (t / 'src/c.h').write_text('int c2;\n')
    reset(); tb4 = gdeps.Table(t, t, cache_path=t / 'out/gate/dt3.json', log=lambda *_a: None)
    ok(kp(pure)[0] == k1, 'its key ignores an edit outside its derived set')
    (t / 'src/b.h').write_text('int b2;\n')
    reset(); tb4 = gdeps.Table(t, t, cache_path=t / 'out/gate/dt3.json', log=lambda *_a: None)
    ok(kp(pure)[0] != k1, 'PLANT: an edit to a file it names changes the key (the PASS is not served)')
    ok(kp(writer)[0] is None and kp(scan)[0] is None and kp(inline)[0] is None, 'a writer, a scanner and an opaque check stay uncached')
    ok(kp(pure, False)[0] is None, 'a check whose write footprint is not empty (not pure) is not cached on derived deps')
    # narrowing is opt-in
    aff = dict(pure, affects=['src/**'])
    ka = fg.cache_key_ex(t, aff, b'{}', [], None, t, 'tc', None, derived=tb4.get(aff), pure=True)[0]
    (t / 'src/c.h').write_text('int c3;\n')
    ok(fg.cache_key_ex(t, aff, b'{}', [], None, t, 'tc', None, derived=tb4.get(aff), pure=True)[0] != ka, 'with affects the default key still follows the whole glob (narrowing is opt-in)')
    nar = dict(aff, narrow=True)
    kn = fg.cache_key_ex(t, nar, b'{}', [], None, t, 'tc', None, derived=tb4.get(nar), pure=True)[0]
    (t / 'src/c.h').write_text('int c4;\n')
    ok(fg.cache_key_ex(t, nar, b'{}', [], None, t, 'tc', None, derived=tb4.get(nar), pure=True)[0] == kn, '"narrow": true: the key follows only the derived set')


def part5(base):
    """S103 DIB2: one planted twin per honesty bug of docs/research/s103/DIB_review.md (bug numbers in the names)."""
    import fastgate as fg
    import proofkey as pk
    import sched as gs
    import exes
    import subprocess

    def reset():
        pk._CLOSURE.clear(); pk._FILE_INFO.clear(); gdeps.forget(); fg._MEMO.clear(); pk._DIR_STATE.clear()

    # ---- bug 1: exes always by bytes; a .ps1 named only in a message string is not in the closure ----
    files = {'tools/x_check.py': "import subprocess\nEXE = 't.exe'\nsubprocess.run(['pwsh', '-File', 'tools/scene/build_t.ps1'])\nsubprocess.run(['out/tools/' + EXE])\n"
                                 "import msgmod\n",
             'tools/msgmod.py': "MSG = 'rebuild with tools\\\\bench\\\\run_list.ps1 first'\n",
             'tools/scene/build_t.ps1': ". (Join-Path $repo 'tools\\env.ps1')\n. (Join-Path $repo 'tools\\cxx_cache.ps1')\n",
             'tools/cxx_cache.ps1': '# helper\n', 'tools/env.ps1': '# env\n', 'tools/bench/run_list.ps1': '# runs the game\n',
             'src/a/main.cpp': '#include "x.h"\n', 'src/a/x.h': 'int x;\n'}
    t = mk_tree(base, 'p5', files)
    man = ['src/a/main.cpp', 'src/a/x.h', 'tools/scene/build_t.ps1', 'tools/cxx_cache.ps1', 'tools/env.ps1']
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_t.ps1.deps.txt', man, t)
    gdeps.write_manifest(t / 'out/tools/t.exe.deps.txt', man, t)
    (t / 'out/tools').mkdir(parents=True, exist_ok=True)
    (t / 'out/tools/t.exe').write_bytes(b'EXE-1')
    chk = {'name': 'x.check', 'cmd': ['py', '-I', 'tools/x_check.py'], 'affects': ['tools/x_check.py']}
    reset()
    key = lambda c=chk: fg.cache_key_ex(t, c, b'{}', [], None, t, 'tc')
    k1, why = key()
    ok(k1 is not None, 'bug 1 setup: the check is keyed (' + why[:60] + ')')
    ok(not any('run_list' in f for f, *_ in gs.closure_files(t, 'tools/x_check.py')), 'bug 1: a .ps1 named only inside a message string is not in the closure')
    (t / 'out/tools/t.exe').write_bytes(b'EXE-2')
    reset()
    ok(key()[0] != k1, 'PLANT bug 1: the exe\'s bytes changed while its manifests exist: the key moves (an exe is never skipped from the key)')
    (t / 'out/tools/t.exe').write_bytes(b'EXE-1')

    # ---- bug 2: exes.infer reads the import closure ----
    (t / 'tools/helper_mod.py').write_text("import subprocess\ndef go():\n    subprocess.run(['out/renderer/dev/fview.exe', '--x'])\n")
    (t / 'tools/uses_helper.py').write_text("import helper_mod\nhelper_mod.go()\n")
    (t / 'tools/docs_only_mod.py').write_text('"""names fview.exe in its docstring only"""\nX = 1\n')
    (t / 'tools/uses_docs.py').write_text("import docs_only_mod\n")
    reset()
    reg = exes.registry()
    ok(exes.infer({'cmd': ['py', '-I', 'tools/uses_helper.py']}, t, reg) == ['out/renderer/dev/fview.exe'], 'PLANT bug 2: an imported module that runs fview.exe makes infer find it')
    ok(exes.infer({'cmd': ['py', '-I', 'tools/uses_docs.py']}, t, reg) == [], 'bug 2: an imported module that only names fview in a docstring does not')

    # ---- bug 3 / 5: the manifest of a generator build holds the shader sources; a stale / other-flavour manifest is not used ----
    (t / 'src/shaders').mkdir(parents=True, exist_ok=True)
    (t / 'src/shaders/a.frag').write_text('void main(){}\n')
    mf = man + ['src/shaders/a.frag']
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_g.ps1.deps.txt', mf, t)
    reset()
    d1 = gdeps.digest(gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1'), t)
    (t / 'src/shaders/a.frag').write_text('void main(){ /* edited */ }\n')
    reset()
    ok(gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1') is None, 'PLANT bug 5: a manifest whose recorded hashes differ from this tree (a build of another state) is stale: no manifest')
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_g.ps1.deps.txt', mf, t)   # (a build of the edited tree)
    reset()
    ok(gdeps.digest(gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1'), t) != d1, 'PLANT bug 3: a shader source edit changes the digest of a manifest that holds it')
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_g.ps1@release.deps.txt', mf + ['src/a/extra.h'], t)
    (t / 'src/a/extra.h').write_text('int e;\n')
    gdeps.write_manifest(t / 'out/deps/tools_scene_build_g.ps1@release.deps.txt', mf + ['src/a/extra.h'], t)
    reset()
    ok(gdeps.script_id('tools/a/b.ps1', ['-Release']) == 'tools_a_b.ps1@release' and gdeps.script_id('tools/a/b.ps1', ['-Surface', '-Release']) == 'tools_a_b.ps1@release+surface'
       and gdeps.script_id('tools/a/b.ps1') == 'tools_a_b.ps1', 'bug 5: the build arguments are part of the script id')
    dev = gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1', flags=[])
    rel = gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1', flags=['-Release'])
    both = gdeps.script_manifest(t, t, 'tools/scene/build_g.ps1')
    ok('src/a/extra.h' not in dev and 'src/a/extra.h' in rel and 'src/a/extra.h' in both, 'PLANT bug 5: the dev and the -Release manifests are separate; no flags = their union')

    # ---- bug 4: a derived file change moves the key of a check that has `affects` ----
    (t / 'tools/x_pure.py').write_text("print(open('src/a/x.h').read())\n")
    aff = {'name': 'x.aff', 'cmd': ['py', '-I', 'tools/x_pure.py'], 'affects': ['tools/x_pure.py']}
    reset()
    tb = gdeps.Table(t, t, cache_path=t / 'out/gate/p5.json', log=lambda *_a: None)
    ka = fg.cache_key_ex(t, aff, b'{}', [], None, t, 'tc', None, derived=tb.get(aff), pure=True)[0]
    (t / 'src/a/x.h').write_text('int x; // derived edit\n')
    reset()
    tb = gdeps.Table(t, t, cache_path=t / 'out/gate/p5b.json', log=lambda *_a: None)
    ok(ka is not None and fg.cache_key_ex(t, aff, b'{}', [], None, t, 'tc', None, derived=tb.get(aff), pure=True)[0] != ka,
       'PLANT bug 4: a file only the derived set names (affects does not) changes the key')
    (t / 'src/a/x.h').write_text('int x;\n')

    # ---- bug 6: `scan:` selects by its directories, `uncacheable` returns no key ----
    (t / 'tools/x_scan2.py').write_text("print(1)\n")
    sc = {'name': 'x.scan2', 'cmd': ['py', '-I', 'tools/x_scan2.py'], 'deps': 'scan:src/a'}
    reset()
    tb = gdeps.Table(t, t, cache_path=t / 'out/gate/p5c.json', log=lambda *_a: None)
    dsc = tb.get(sc)
    ok(dsc['tier'] == 'C' and gdeps.deps_hit(dsc, ['src/a/zzz.h']) == 'src/a/zzz.h' and gdeps.deps_hit(dsc, ['src/b/zzz.h']) is None, 'PLANT bug 6: "deps": "scan:src/a" is tier C and selects on a change under src/a only')
    unc = {'name': 'x.unc', 'cmd': ['py', '-I', 'tools/x_scan2.py'], 'affects': ['tools/x_scan2.py'], 'uncacheable': 'reads the clock'}
    kk, why = fg.cache_key_ex(t, unc, b'{}', [], None, t, 'tc')
    ok(kk is None and 'uncacheable' in why, 'PLANT bug 6: "uncacheable" returns no key')

    # ---- S1: tier C adds derived FILES only, never blanket directories ----
    (t / 'tools/x_walk.py').write_text("import os\nfor r, d, f in os.walk('src'):\n    pass\nprint(open('src/a/x.h').read())\n")
    wk = {'name': 'x.walk', 'cmd': ['py', '-I', 'tools/x_walk.py'], 'affects': ['tools/x_walk.py']}
    reset()
    tb = gdeps.Table(t, t, cache_path=t / 'out/gate/p5d.json', log=lambda *_a: None)
    dw = tb.get(wk)
    ok(dw['tier'] == 'C' and gdeps.deps_hit(dw, ['src/a/main.cpp']) is None and gdeps.deps_hit(dw, ['src/a/x.h']) == 'src/a/x.h',
       'S1: a scanning (tier C) check selects on the files it names, not on every file under src')

    # ---- bug 7: an unresolved literal / a new enumerator makes a check inexact ----
    (t / 'tools/x_here.py').write_text("import os\nHERE = os.path.dirname(__file__)\nprint(open(os.path.join(HERE, 'data.json')).read())\nprint(open('src/a/x.h').read())\n")
    (t / 'tools/x_iglob.py').write_text("import glob\nfor f in glob.iglob('src/a/*.h'):\n    print(f)\n")
    (t / 'tools/x_fs.py').write_text("name = 'x'\nprint(open(f'src/a/{name}.h').read())\n")
    for nm, scr, want_c in (('here', 'tools/x_here.py', False), ('iglob', 'tools/x_iglob.py', True), ('fs', 'tools/x_fs.py', False)):
        reset()
        tb = gdeps.Table(t, t, cache_path=t / f'out/gate/p5e{nm}.json', log=lambda *_a: None)
        dd = tb.get({'name': 'x.' + nm, 'cmd': ['py', '-I', scr]})
        ok(not dd['exact'] and (dd['tier'] == 'C') == want_c, f'PLANT bug 7: {nm} is not exact (tier {dd["tier"]})')

    # ---- bug 8: toolcache.rekey keys from the entries in hand, not from a re-read of the shared store ----
    quiet = lambda *_a, **_k: None
    cache = Path(base) / 'cache_p5'
    c = gtc.ToolCache(t, cache, log=quiet)
    ident = 'tools/scene/build_t.ps1|'
    gdeps.write_manifest(t / 'out/tools/t.exe.deps.txt', man, t)
    k_ok = c.rekey(ident, ['out/tools/t.exe', 'out/tools/t.exe.deps.txt'])
    c2 = gtc.ToolCache(t, cache, log=quiet)
    c2.note_manifest(ident, ['out/tools/t.exe', 'out/tools/t.exe.deps.txt'])
    in_hand = c2._last_ents
    gdeps.write_manifest(c2._store_path(ident), ['src/a/main.cpp'], t)   # the race: another gate rewrites the shared store between the write and the key
    ok(c2.key(ident, in_hand) != c2.key(ident) and c2.key(ident, in_hand) == k_ok, 'PLANT bug 8: the key from the entries in hand is not the key of whatever the shared store holds now')

    # ---- bug 9: a link with no .o inputs writes an INVALID manifest and removes the exe's old one (real pwsh; skipped without clang++ / pwsh) ----
    pw = shutil.which('pwsh')
    cl = shutil.which('clang++') or (r'C:/Program Files/LLVM/bin/clang++.exe' if os.path.isfile(r'C:/Program Files/LLVM/bin/clang++.exe') else None)
    if pw and cl:
        r9 = mk_tree(base, 'p9', {'tools/env.ps1': '# env\n', 'tools/scene/build_n.ps1':
                                   "param([switch]$Release)\n$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)\n. (Join-Path $repo 'tools\\env.ps1')\n. (Join-Path $repo 'tools\\cxx_cache.ps1')\n"
                                   "New-Item -ItemType Directory -Force (Join-Path $repo 'out\\tools') | Out-Null\nclang++ --version -o (Join-Path $repo 'out\\tools\\n.exe') | Out-Null\n"})
        shutil.copy(HERE.parents[1] / 'tools' / 'cxx_cache.ps1', r9 / 'tools' / 'cxx_cache.ps1')
        (r9 / 'out' / 'tools').mkdir(parents=True, exist_ok=True)
        (r9 / 'out/tools/n.exe.deps.txt').write_text('# wwe2k14-deps v1\nsrc/old.h\n')   # an old manifest of the exe this link replaces
        env = dict(os.environ, WWE_NO_CCACHE='1', PATH=os.path.dirname(cl) + os.pathsep + os.environ.get('PATH', ''))
        subprocess.run([pw, '-NoProfile', '-File', str(r9 / 'tools/scene/build_n.ps1'), '-Release'], env=env, capture_output=True, text=True, timeout=120)
        sm = r9 / 'out/deps/tools_scene_build_n.ps1@release.deps.txt'
        ok(sm.is_file() and sm.read_text().startswith('# wwe2k14-deps INVALID') and not (r9 / 'out/tools/n.exe.deps.txt').exists(),
           'PLANT bug 9: a link with no .o inputs: the script manifest (with the @release flavour in its name) is INVALID and the exe\'s old manifest is gone')
    else:
        print('SKIP  bug 9 real-pwsh twin: pwsh or clang++ not found')

    # ---- bug 10: the derived table is per checkout, and a literal that appears changes the signature ----
    (t / 'tools/x_lit.py').write_text("print(open('src/a/late.h').read())\n")
    reset()
    lit = {'name': 'x.lit', 'cmd': ['py', '-I', 'tools/x_lit.py']}
    tbl = gdeps.Table(t, t, cache_path=t / 'out/gate/p5f.json', log=lambda *_a: None)
    d_before = tbl.get(lit)
    tbl.save()
    (t / 'src/a/late.h').write_text('int late;\n')
    reset()
    tbl2 = gdeps.Table(t, t, cache_path=t / 'out/gate/p5f.json', log=lambda *_a: None)
    d_after = tbl2.get(lit)
    ok('src/a/late.h' not in d_before['files'] and 'src/a/late.h' in d_after['files'] and tbl2.misses == 1, 'PLANT bug 10: a file a literal names appears: the cached entry is recomputed')
    other = mk_tree(base, 'p5other', files)
    tbl3 = gdeps.Table(other, t, cache_path=t / 'out/gate/p5f.json', log=lambda *_a: None)
    tbl3.get(lit)
    ok(tbl3.hits == 0, 'bug 10: another checkout never reads this checkout\'s entry of the same check name')

    # ---- S2: a manifest is not rewritten when its content is the same; the stamp follows content ----
    mp = t / 'out/deps/s2.deps.txt'
    gdeps.write_manifest(mp, ['src/a/x.h'], t)
    m1 = os.stat(mp).st_mtime_ns
    time.sleep(0.05)
    gdeps.write_manifest(mp, ['src/a/x.h'], t)
    ok(os.stat(mp).st_mtime_ns == m1, 'S2: same content: the manifest file is not rewritten')

    # ---- item 2 (HEAD guard): a gate whose HEAD moves mid-run marks the later checks "tree moved" ----
    git = shutil.which('git')
    if git:
        g = Path(base) / 'headguard'
        (g / 'tools/gate/lanes').mkdir(parents=True)
        (g / 'tools/gate/lanes/x.json').write_text(json.dumps({'checks': [
            {'name': 'hg.move', 'cmd': ['py', '-I', '-c', "import subprocess;subprocess.run(['git','-c','user.name=t','-c','user.email=t@t','commit','--allow-empty','-m','moved'],capture_output=True)"], 'timeout_s': 60},
            {'name': 'hg.after', 'cmd': ['py', '-I', '-c', 'print(1)'], 'timeout_s': 60}]}))
        run = lambda *a: subprocess.run([git, '-C', str(g)] + list(a), capture_output=True, text=True)
        run('init', '-q'); run('add', '-A'); run('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'one')
        genv = dict(os.environ, GATE_NO_RESTORE='1')
        pr = subprocess.run([sys.executable, '-I', str(HERE / 'gate.py'), '--root', str(g), '--out', str(g / 'o'), '--no-toolcache', '--no-cache', '--jobs', '1'],
                            capture_output=True, text=True, timeout=120, env=genv)
        o = pr.stdout
        ok('TREE MOVED' in o and 'tree moved' in o and pr.returncode == 1 and not re.search(r'^PASS\s+\S+\s+hg\.after', o, re.M),
           'PLANT item 2: HEAD moved while the gate ran: the later check is FAIL "tree moved", never a PASS, and the gate fails')
    else:
        print('SKIP  HEAD guard twin: git not found')

    # ---- DPC (QX4 stall): a cache-key computation that never returns is shown as 'computing cache key since HH:MM' and gives up: the check runs uncached ----
    kg = Path(base) / 'keyhang'
    (kg / 'tools/gate/lanes').mkdir(parents=True)
    (kg / 'src').mkdir()
    (kg / 'src/a.h').write_text('int a;\n')
    (kg / 'tools/gate/lanes/x.json').write_text(json.dumps({'checks': [
        {'name': 'kh.a', 'cmd': ['py', '-I', '-c', 'print(1)'], 'timeout_s': 60, 'affects': ['src/**']}]}))
    kenv = dict(os.environ, GATE_NO_RESTORE='1', GATE_TEST_KEY_HANG='kh.a', GATE_TEST_KEY_HANG_S='25', GATE_KEY_TIMEOUT='3', GATE_STALL_S='1')
    kp_ = subprocess.run([sys.executable, '-I', str(HERE / 'gate.py'), '--root', str(kg), '--out', str(kg / 'o'), '--no-toolcache', '--jobs', '1'],
                         capture_output=True, text=True, timeout=110, env=kenv)
    ko = kp_.stdout
    ok('computing cache key since' in ko and 'kh.a' in ko, 'DPC PLANT: a hung compute_key is reported as state "key" ("computing cache key since HH:MM"), not "blocked by ?"')
    ok(re.search(r'^PASS\s+\S+\s+kh\.a', ko, re.M) is not None and 'did not finish in 3 s' in ko, 'DPC PLANT: ...and it times out (GATE_KEY_TIMEOUT) to uncacheable: the check runs and passes')

    # ---- DPC: the table survives an index rebuild with unchanged files; a killed save leaves no tmp; shards per root; a failing save does not crash ----
    import fastgate as fg
    files = {'src/main.cpp': '#include "a.h"\n', 'src/a.h': 'int a;\n', 'tools/gate/game_syntax.py': '# stub\n', 'tools/gate/gs_share.py': '# stub\n'}
    t = mk_tree(base, 'dpc', files)
    gsyn = {'name': 'x.game_syntax', 'cmd': ['py', '-I', 'tools/gate/game_syntax.py', '--root', '.']}
    (t / 'tools').mkdir(exist_ok=True)
    (t / 'tools/x_pure.py').write_text("print(open('src/a.h').read())\n")
    pure = {'name': 'x.pure', 'cmd': ['py', '-I', 'tools/x_pure.py']}
    tus, incs = [(1, 'src/main.cpp', 0), (2, 'src/renderer_adapter/live.cpp', 0)], {1: ['src/a.h'], 2: []}
    (t / 'src/renderer_adapter').mkdir(parents=True, exist_ok=True)
    (t / 'src/renderer_adapter/live.cpp').write_text('int l;\n')
    fake_index(t, tus, incs)
    mk = lambda: gdeps.Table(t, t, cache_path=t / 'out/gate/dpc.json', log=lambda *_a: None)
    tb = mk(); tb.get(pure); tb.get(gsyn); tb.save(); tb.close()
    time.sleep(0.05)
    fake_index(t, tus, incs)   # a rebuilt ours.db: new mtime, same file shas
    tb = mk(); tb.get(pure)
    ok(tb.hits == 1 and tb.misses == 0, 'DPC PLANT: an ours.db rebuild with unchanged files keeps a script entry warm (the old mtime key made it cold)')
    # F2: a game_syntax derivation is never cached: it reads the index fresh every time (a changed index edge is seen at once)
    tb.get(gsyn)
    ok('x.game_syntax' not in tb.cache and tb.hits == 1, 'DPC F2: a game_syntax entry is never stored in the table')
    tb.close()
    fake_index(t, tus, {1: ['src/a.h', 'src/main.cpp'], 2: []})
    tb = mk()
    ok(set(tb.get(gsyn)['idx_files']) >= {'src/a.h', 'src/main.cpp'}, 'DPC F2 PLANT: a game_syntax result follows the live index (no stale cached closure)')
    tb.close()
    # F1: a failed derivation is returned, never stored
    real_derive = gdeps.derive
    def bad_derive(*a, **k):
        raise RuntimeError('planted transient')
    gdeps.derive = bad_derive
    tb = mk()
    try:
        d = tb.get({'name': 'x.bad', 'cmd': ['py', '-I', 'tools/x_pure.py']})
    finally:
        gdeps.derive = real_derive
    ok(d['tier'] == 'D' and 'x.bad' not in tb.cache and 'x.bad' not in tb.new, 'DPC F1 PLANT: a failed derivation is returned as tier D and not stored (nor marked dirty)')
    tb.close()
    # F3: a path the derivation looked up and did not find is part of the signature: it appearing recomputes
    (t / 'tools/x_arg.py').write_text("import sys\nprint(sys.argv)\n")
    argchk = {'name': 'x.arg', 'cmd': ['py', '-I', 'tools/x_arg.py', 'src/later.h']}
    tb = mk(); tb.get(argchk); tb.save(); tb.close()
    (t / 'src/later.h').write_text('int later;\n')
    tb = mk(); d = tb.get(argchk)
    ok(tb.misses == 1 and 'src/later.h' in d['files'], 'DPC F3 PLANT: a command-line path that did not exist and now does recomputes the entry')
    tb.close()
    dchk = {'name': 'x.dir', 'cmd': ['py', '-I', 'tools/x_arg.py', 'src/dirlater2']}
    tb = mk(); tb.get(dchk); tb.save(); tb.close()
    (t / 'src/dirlater2').mkdir()
    tb = mk(); tb.get(dchk)
    ok(tb.misses == 1, 'DPC F3 PLANT: a folder that appears is seen (exists(), not is_file())')
    tb.close()
    # a killed save (the write raises) leaves no temp file, and never raises
    tb = mk(); tb.dirty = True; tb.new = {'x': {'sig': 's', 'mstamp': 'm', 'd': {}}}
    real = Path.write_text
    def boom(self, *a, **k):
        if self.name.endswith('.tmp') and 'dpc' in self.name:
            real(self, 'partial')
            raise MemoryError('planted')
        return real(self, *a, **k)
    Path.write_text = boom
    try:
        tb.save()
        raised = False
    except BaseException:
        raised = True
    finally:
        Path.write_text = real
    ok(not raised and not list((t / 'out/gate').glob('dpc*.tmp')), 'DPC PLANT: a save that fails mid-write (MemoryError) is logged and skipped, and leaves no .tmp behind')
    tb.close()
    # an old orphan tmp is swept at load, a fresh one (another live save) is not
    old, fresh = t / 'out/gate/dpc.json.1.tmp', t / 'out/gate/dpc.json.2.tmp'
    old.write_text('x'); fresh.write_text('x')
    os.utime(old, (time.time() - 3600, time.time() - 3600))
    mk().close()
    ok(not old.exists() and fresh.exists(), 'DPC: Table() sweeps *.tmp older than 15 min and leaves a fresh one')
    # per-root shards: a root that no longer exists is pruned on the next save
    gone = Path(base) / 'dpc_gone'
    gone.mkdir(exist_ok=True)
    tg = gdeps.Table(gone, t, cache_path=t / 'out/gate/dpc.json', log=lambda *_a: None)
    tg.dirty = True; tg.new = {'y': {'sig': 's', 'mstamp': 'm', 'd': {}}}; tg.cache.update(tg.new); tg.save(); tg.close()
    ok(tg.path.is_file(), 'DPC: each checkout root has its own shard file')
    gone.rmdir()
    tb = mk(); tb.get(pure); tb.dirty = True; tb.save(); tb.close()
    ok(not tg.path.exists() and tb.path.is_file(), 'DPC: a save drops the shard of a root that no longer exists and keeps this root\'s')
    # F4: a shard whose own recorded root is gone is dropped even when roots.json is lost
    gone2 = Path(base) / 'dpc_gone2'
    gone2.mkdir(exist_ok=True)
    tg2 = gdeps.Table(gone2, t, cache_path=t / 'out/gate/dpc.json', log=lambda *_a: None)
    tg2.dirty = True; tg2.new = {'y': {'sig': 's', 'mstamp': 'm', 'd': {}}}; tg2.cache.update(tg2.new); tg2.save(); tg2.close()
    gone2.rmdir()
    (t / 'out/gate/dpc.roots.json').unlink()
    tb = mk(); tb.get(pure); tb.dirty = True; tb.save(); tb.close()
    ok(tg2.path.is_file() is False and tb.path.is_file(), 'DPC F4 PLANT: a shard whose own root folder is gone is deleted although roots.json was lost')
    # a gate's default --root is its own checkout, not the main one
    pr = subprocess.run([sys.executable, '-I', str(HERE / 'gate.py'), '--help'], capture_output=True, text=True, timeout=60)
    ok('own head' in ' '.join(pr.stdout.split()).lower(), 'DPC: gate.py --root defaults to its own checkout (a worktree gate compares its own HEAD, not the main HEAD)')


def part4_real():
    """Against the real repo and index (skipped, loudly, when the index is absent): every check has a tier, and the S102 08:56 class is caught by the real fview manifest."""
    root = HERE.parents[1]
    main = None
    try:
        import subprocess
        c = subprocess.run(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        main = Path(c).parent if c else root
    except Exception:
        main = root
    idx = gdeps.open_index(root, main)
    if idx is None:
        print('SKIP  real-repo checks: no ours.db index (py tools\\q\\q.py ours update)')
        return
    lanes = sorted((root / 'tools/gate/lanes').glob('*.json'))
    tb = gdeps.Table(root, main, cache_path=root / 'out' / 'gate' / 'deps_selftest.json', log=lambda *_a: None)
    n = 0
    tiers = {}
    for f in lanes:
        for c in json.loads(f.read_text(encoding='utf-8')).get('checks', []):
            d = tb.get(c)
            tiers[d['tier']] = tiers.get(d['tier'], 0) + 1
            n += 1
    ok(n > 100 and set(tiers) <= {'A', 'B', 'C', 'D'}, f'real repo: {n} checks all derive a tier {tiers}')
    tb.save()


def main():
    base = tempfile.mkdtemp(prefix='deps_selftest_')
    try:
        part1(base)
        part1b(base)
        part2()
        part3(base)
        part5(base)
        part4_real()
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('PASS deps selftest' if not FAILS else f'FAIL deps selftest ({len(FAILS)})')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
