"""record_layout_dump: proves a header move kept the scene-record layout (S106 G4-1A; docs\\research\\s106\\G4.md "The round-trip proof").

  py -I tools\\scene\\record_layout_dump.py --base s106 [--moved-lines] [--layout]   (both when neither flag is given)
  py -I tools\\scene\\record_layout_dump.py --selftest

--layout: a unit is generated from the BASE headers' own names (every namespace-level struct, every `inline constexpr`, every enumerator of
  scene_record.h / record_io.h) and compiled against the base tree and against this tree (clang -fdump-record-layouts: sizeof / alignof / every
  field's offset of every rdr_scene record; plus every constant and enumerator value and SceneTables' size and traits, printed by running it).
  The two reports must be identical (this tree may add nothing and lose nothing).
--moved-lines: every non-blank line of the base's scene_record.h, record_io.h, front_end.h, input_changes.h, detached_piece.h must still exist,
  byte for byte (CR / trailing space aside), somewhere in this tree's same files + the new files of src\\renderer_scene (as a multiset: a line
  that exists twice must still exist twice); the lines that are new must be includes, namespace lines, `#pragma once` or `// S106 G4` markers
  or the two named split declarations. Nothing else may appear or disappear.
Exit: 0 = identical; 1 = a difference (printed); 2 = could not run. No game, no run lock, no SDK; compile only.
"""
# entry: rdr_scene record headers (scene_record.h, record_io.h and the rows_*.h they split into)
import argparse, os, re, shutil, subprocess, sys, tarfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RS = 'src/renderer_scene'
SPLIT_BASE = '72b1f135d'   # S106 G4-1AF: the commit before the record header split; the selftest's base never moves
OLD_FILES = [RS + '/scene_record.h', RS + '/record_io.h', RS + '/front_end.h', 'src/scene_live/input_changes.h', RS + '/detached_piece.h']
NAME_FILES = ['scene_record.h', 'record_io.h']
DEFS = ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV']
# the declared exceptions (G4_1A contract site 10: line 492 becomes two declarations, same values)
LOST_OK = {'inline constexpr uint32_t kPlaneEyesLayout = 1, kPartClockLayout = 1;'}
ADDED_OK_EXACT = {'#pragma once', 'namespace rdr_scene {', '}  // namespace rdr_scene',
                  'inline constexpr uint32_t kPlaneEyesLayout = 1;', 'inline constexpr uint32_t kPartClockLayout = 1;'}
ADDED_OK_RE = [re.compile(r'^#include\b'), re.compile(r'^// S106 G4')]


def norm_lines(text: str):
    return [ln.rstrip() for ln in text.replace('\r', '').split('\n') if ln.strip()]


def git_show(ref, path):
    r = subprocess.run(['git', 'show', f'{ref}:{path}'], cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else None


def base_listing(ref):
    r = subprocess.run(['git', 'ls-tree', '--name-only', ref, RS + '/'], cwd=REPO, capture_output=True, text=True, check=True)
    return {Path(x).name for x in r.stdout.split()}


def moved_check(old_texts: dict, new_texts: dict):
    """old_texts / new_texts: {filename: text}. Returns (lost, added) as lists of lines (empty both = the move is a pure text move)."""
    old = Counter(l for t in old_texts.values() for l in norm_lines(t))
    new = Counter(l for t in new_texts.values() for l in norm_lines(t))
    lost = list((old - new).elements())
    added = list((new - old).elements())
    lost_bad = [l for l in lost if l not in LOST_OK]
    # an allowed lost line must actually be gone and replaced, never silently doubled: the replacement lines must be present
    added_bad = [l for l in added if l not in ADDED_OK_EXACT and not any(r.match(l) for r in ADDED_OK_RE)]
    return lost_bad, added_bad, lost, added


def new_set_texts(root: Path, base_names):
    names = set(OLD_FILES)
    for p in (root / RS).iterdir():
        if p.is_file() and p.suffix == '.h' and p.name not in base_names:
            names.add(RS + '/' + p.name)
    return {n: (root / n).read_text(encoding='utf-8', errors='replace') for n in names if (root / n).exists()}


def old_set_texts(ref):
    out = {}
    for n in OLD_FILES:
        t = git_show(ref, n)
        if t is not None:
            out[n] = t
    return out


# ---- layout ------------------------------------------------------------------------------------------------------------------------------
def strip_comments(s: str) -> str:
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def split_top(s: str):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '({<[':
            depth += 1
        elif ch in ')}>]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def names_from(text: str):
    """namespace-level struct names, `inline constexpr` names, enumerators (name, scoped_enum_or_None) of one header's text (comments stripped)."""
    t = strip_comments(text.replace('\r', ''))
    structs = re.findall(r'(?m)^(?:struct|class)\s+(\w+)\s*(?:final\s*)?\{', t)
    consts = []
    for m in re.finditer(r'(?m)^inline constexpr\s+([\w:<> ]+?)\s+(\w[^;]*);', t):
        for d in split_top(m.group(2)):
            nm = re.match(r'\s*(\w+)\s*(\(?)', d)
            if nm and not nm.group(2) and not d.strip().startswith('operator'):   # `name(` is a constexpr function, not a constant
                consts.append(nm.group(1))
    enums = []
    for m in re.finditer(r'(?m)^enum\s+(class\s+)?(\w+)\s*(?::\s*[\w:]+\s*)?\{([^}]*)\}', t):
        scoped, ename, body = bool(m.group(1)), m.group(2), m.group(3)
        for d in split_top(body):
            nm = re.match(r'\s*(\w+)', d)
            if nm:
                enums.append((nm.group(1), ename if scoped else None))
    return structs, consts, enums


UNIT_HEAD = r'''// generated by record_layout_dump.py from the base headers' names (never committed)
#if __has_include("record_all.h")
#include "record_all.h"
#else
#include "record_io.h"
#endif
#include <cstdio>
#include <cstddef>
#include <type_traits>
using namespace rdr_scene;
template <class T> static void Show(const char* n, const T& v) {
  if constexpr (std::is_enum_v<T>) printf("%s=%lld\n", n, (long long)v);
  else if constexpr (std::is_arithmetic_v<T>) printf("%s=%.17g\n", n, (double)v);
  else printf("%s=<%zu bytes>\n", n, sizeof(T));
}
'''


def make_unit(names):
    structs, consts, enums = names
    L = [UNIT_HEAD]
    for s in sorted(set(structs)):
        L.append(f'static_assert(sizeof({s}) > 0 && alignof({s}) > 0, "{s}");')
    L.append('int main() {')
    for s in sorted(set(structs)):
        L.append(f'  printf("sizeof {s}=%zu alignof=%zu\\n", sizeof({s}), alignof({s}));')
    for c in sorted(set(consts)):
        L.append(f'  Show("{c}", {c});')
    for e, sc in sorted(set(enums), key=lambda x: (x[1] or '', x[0])):
        q = f'{sc}::{e}' if sc else e
        L.append(f'  Show("{q}", {q});')
    L += ['  printf("SceneTables sizeof=%zu alignof=%zu\\n", sizeof(SceneTables), alignof(SceneTables));',
          '  printf("SceneTables nothrow_move_ctor=%d nothrow_move_assign=%d copy_ctor=%d move_ctor=%d default_ctor=%d nothrow_default=%d trivially_copyable=%d standard_layout=%d\\n",',
          '         int(std::is_nothrow_move_constructible_v<SceneTables>), int(std::is_nothrow_move_assignable_v<SceneTables>), int(std::is_copy_constructible_v<SceneTables>),',
          '         int(std::is_move_constructible_v<SceneTables>), int(std::is_default_constructible_v<SceneTables>), int(std::is_nothrow_default_constructible_v<SceneTables>),',
          '         int(std::is_trivially_copyable_v<SceneTables>), int(std::is_standard_layout_v<SceneTables>));',
          '  return 0;', '}']
    return '\n'.join(L) + '\n'


def clang():
    c = shutil.which('clang++')
    if c:
        return c
    p = Path(r'C:\Program Files\LLVM\bin\clang++.exe')
    if p.exists():
        return str(p)
    r = subprocess.run(['pwsh', '-NoProfile', '-Command', '. tools\\env.ps1 | Out-Null; (Get-Command clang++).Source'], cwd=REPO, capture_output=True, text=True)
    return r.stdout.strip().splitlines()[-1]


def layout_report(tree: Path, unit_text: str, work: Path, tag: str, cxx: str):
    work.mkdir(parents=True, exist_ok=True)
    cpp = work / f'unit_{tag}.cpp'
    cpp.write_text(unit_text, encoding='utf-8')
    exe = work / f'unit_{tag}.exe'
    cmd = [cxx, '-std=c++20', '-O0', '-fuse-ld=lld', *DEFS, '-Xclang', '-fdump-record-layouts', '-I', str(tree / RS), str(cpp), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'layout unit ({tag}) did not compile:\n' + (r.stderr or '')[-2500:])
    blocks, cur = {}, None
    for ln in r.stdout.splitlines():
        if ln.startswith('*** Dumping AST Record Layout'):
            cur = []
            continue
        if cur is not None:
            if not ln.strip():
                if cur:
                    m = re.match(r'\s*0 \| (?:struct|class|union) (\S+)', cur[0])
                    if m and m.group(1).startswith('rdr_scene::'):
                        blocks[m.group(1)] = '\n'.join(cur)
                cur = None
            else:
                cur.append(ln)
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    if run.returncode:
        raise RuntimeError(f'layout unit ({tag}) exit {run.returncode}')
    return blocks, run.stdout.splitlines()


def layout_compare(old_rep, new_rep):
    (ob, ov), (nb, nv) = old_rep, new_rep
    diffs = []
    for k in sorted(set(ob) | set(nb)):
        if k not in nb:
            diffs.append(f'layout of {k}: gone in the new tree')
        elif k not in ob:
            diffs.append(f'layout of {k}: only in the new tree')
        elif ob[k] != nb[k]:
            diffs.append(f'layout of {k} differs:\n--- old\n{ob[k]}\n--- new\n{nb[k]}')
    for a, b in zip(ov, nv):
        if a != b:
            diffs.append(f'value: old `{a}` new `{b}`')
    if len(ov) != len(nv):
        diffs.append(f'value lines: old {len(ov)} new {len(nv)}')
    return diffs, (len(ob), len(ov))


def archive_base(ref, dest: Path):
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    tar = dest.parent / 'base_layout.tar'
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(tar), ref, 'src'], cwd=REPO, check=True)
    with tarfile.open(tar) as t:
        t.extractall(dest, filter='data')
    tar.unlink()


def do_layout(ref, out: Path):
    cxx = clang()
    old = out / 'old'
    archive_base(ref, old)
    nm = ([], [], [])
    for n in NAME_FILES:
        t = git_show(ref, f'{RS}/{n}')
        s, c, e = names_from(t)
        nm = (nm[0] + s, nm[1] + c, nm[2] + e)
    unit = make_unit(nm)
    # S106: one compile after the other (wiring_lint: a Python compile pool must hold the compile queue; two compiles do not need a pool).
    orp = layout_report(old, unit, out, 'old', cxx)
    nrp = layout_report(REPO, unit, out, 'new', cxx)
    diffs, (nblocks, nvals) = layout_compare(orp, nrp)
    print(f'layout: {len(nm[0])} structs, {len(nm[1])} constants, {len(nm[2])} enumerators named by the base headers; {nblocks} record layouts and {nvals} value lines compared')
    for d in diffs[:15]:
        print('  LAYOUT DIFF', d[:900])
    print('layout: ' + ('IDENTICAL' if not diffs else f'FAIL ({len(diffs)} differences)'))
    return not diffs


def do_moved(ref):
    old = old_set_texts(ref)
    new = new_set_texts(REPO, base_listing(ref))
    lost_bad, added_bad, lost, added = moved_check(old, new)
    n_old = sum(len(norm_lines(t)) for t in old.values())
    n_new = sum(len(norm_lines(t)) for t in new.values())
    print(f'moved-lines: {n_old} code lines in the base files {sorted(old)}; {n_new} in the new set ({len(new)} files); '
          f'{len(lost)} base lines not found ({len(lost) - len(lost_bad)} declared: the split declaration), {len(added)} new lines ({len(added) - len(added_bad)} allowed: includes, namespace, pragma, markers, the split declarations)')
    for l in lost_bad[:20]:
        print('  LOST (in the base, not found byte-identical in the new set):', l[:200])
    for l in added_bad[:20]:
        print('  ADDED (a new line that is not an include / namespace / marker):', l[:200])
    ok = not lost_bad and not added_bad
    print('moved-lines: ' + ('every moved line is byte-identical' if ok else 'FAIL'))
    return ok


# ---- selftest -----------------------------------------------------------------------------------------------------------------------------
def selftest(out: Path, ref: str = SPLIT_BASE):
    old = old_set_texts(ref)
    new = new_set_texts(REPO, base_listing(ref))
    results = []

    def arm(name, ok_expected, fn):
        got = fn()
        results.append((name, got == ok_expected, got))

    # unplanted: this tree against the base passes only when the real move is a text move; the selftest uses the base against ITSELF so it is
    # independent of the move under test
    arm('moved-lines: base against itself passes', True, lambda: not moved_check(old, dict(old))[0] and not moved_check(old, dict(old))[1])
    # planted 1 (unsafe direction: a lost line): drop one code line of a family
    def lost():
        n = dict(new)
        k = next(k for k in n if k.endswith('rows_stage_fx.h'))
        n[k] = n[k].replace('  uint32_t asset = 0;    // index into kScAssets (Model): the model the row is for\n', '', 1).replace(
            '  uint32_t asset = 0;    // index into kScAssets (Model): the model the row is for\r\n', '', 1)
        r = moved_check(old, n)
        return not r[0] and not r[1]
    arm('moved-lines: a dropped field line FAILS', False, lost)
    # planted 2: a field added to a moved block (a new line that is not an include / marker)
    def added():
        n = dict(new)
        k = next(k for k in n if k.endswith('rows_stage_fx.h'))
        n[k] = n[k].replace('struct StageFxRow {', 'struct StageFxRow {\n  uint32_t planted_extra = 0;', 1)
        r = moved_check(old, n)
        return not r[0] and not r[1]
    arm('moved-lines: an added field line FAILS', False, added)
    # planted 3: a changed constant value inside a moved line
    def changed():
        n = dict(new)
        k = next(k for k in n if k.endswith('rows_stage_fx.h'))
        n[k] = n[k].replace('kStageFxLayout = 1;', 'kStageFxLayout = 2;', 1)
        r = moved_check(old, n)
        return not r[0] and not r[1]
    arm('moved-lines: a changed constant FAILS', False, changed)
    # layout arms: a header closure overlay of this tree, once unplanted and once with Object growing one uint32
    cxx = clang()
    unit_names = ([], [], [])
    for n in NAME_FILES:
        s, c, e = names_from(git_show(ref, f'{RS}/{n}'))
        unit_names = (unit_names[0] + s, unit_names[1] + c, unit_names[2] + e)
    unit = make_unit(unit_names)
    work = out / 'selftest'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    cpp0 = work / 'mm.cpp'
    cpp0.write_text(unit, encoding='utf-8')
    r = subprocess.run([cxx, '-std=c++20', '-MM', *DEFS, '-I', str(REPO / RS), str(cpp0)], capture_output=True, text=True)
    if r.returncode:
        print('selftest: -MM failed', r.stderr[-400:])
        return 2
    deps = [Path(x) for x in r.stdout.replace('\\\r\n', ' ').replace('\\\n', ' ').split()[1:] if x.strip() and x != '\\']
    ovs = {}
    for tag in ('same', 'planted'):
        ov = work / tag
        for d in deps:
            d = d if d.is_absolute() else REPO / d
            if d == cpp0 or work in d.parents:
                continue
            rel = d.resolve().relative_to(REPO)
            (ov / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(d, ov / rel)
        ovs[tag] = ov
    sr = ovs['planted'] / RS / 'scene_record.h'
    marker = '  float tint[4] = {1, 1, 1, 1};      // fades (object alpha), 1 = none'
    s = sr.read_bytes().decode('utf-8')
    if marker not in s:
        print('selftest: Object marker not found (planted layout twin cannot be made): BLOCKED')
        return 2
    sr.write_bytes(s.replace(marker, marker + '\r\n  uint32_t planted_extra = 0;', 1).encode('utf-8'))
    rep_ref = layout_report(ovs['same'], unit, work, 'same', cxx)
    rep_pl = layout_report(ovs['planted'], unit, work, 'planted', cxx)
    arm('layout: this tree against an identical copy passes', True, lambda: not layout_compare(rep_ref, rep_ref)[0])
    arm('layout: Object grown by one uint32 FAILS', False, lambda: not layout_compare(rep_ref, rep_pl)[0])
    bad = 0
    for name, good, got in results:
        print(('ok    ' if good else 'WRONG ') + name + f' (verdict: {"PASS" if got else "FAIL"})')
        bad += not good
    print('selftest: ' + ('OK' if not bad else f'NOT OK ({bad} arm(s) wrong)'))
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', default='s106')
    ap.add_argument('--moved-lines', action='store_true')
    ap.add_argument('--layout', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--selftest-ref', default=SPLIT_BASE, help='twin only: the selftest base (default: the pre-split commit)')
    ap.add_argument('--out', default=r'out\g4\layout')
    a = ap.parse_args()
    out = Path(a.out) if os.path.isabs(a.out) else REPO / a.out
    try:
        if a.selftest:
            return selftest(out, a.selftest_ref)
        both = not a.moved_lines and not a.layout
        ok = True
        if a.moved_lines or both:
            ok &= do_moved(a.base)
        if a.layout or both:
            ok &= do_layout(a.base, out)
        return 0 if ok else 1
    except Exception as e:   # S106 G4-1AF: any unexpected exception is "could not run" (2), never a traceback exit 1
        print('record_layout_dump: could not run:', type(e).__name__, e)
        return 2


if __name__ == '__main__':
    sys.exit(main())
