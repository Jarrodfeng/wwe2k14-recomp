#!/usr/bin/env python3
r"""S102 lane U4: the benchmark button's offline proof. 0 game runs.

  py -I tools\launcher\s102_tests\bench_button_check.py [--root DIR]

1 build   clang++ compiles tools\launcher\s102_tests\bench_button_test.cpp against src\launcher\bench_button.h (header only, std only)
2 unit    the test passes: scene list (@quick, never --wwe2k14_launch=lesnar_bryan; arena id from db\ids\arenas.json), bench\ file set (a bench\ without states.toml hides
          the button), numbers on synthetic runs, the route's marks as the window, NO RESULT for: no 'bench: done', an old log, a scene past its time limit, bad CSV
3 json    results.json parses and carries hardware, three sides, shown fps for every scene (the file the launcher puts in the report zip)
4 real    on a real game's run (the newest frames_*.csv + log in the MAIN checkout's out\build\...\logs whose log has 'bench: done' and two marks) the C++ numbers equal an
          independent python recomputation of the same rule; FAILs if no such run exists (never a silent skip)
5 plant   a scenes folder whose route lost 'log bench: done' makes the unit test FAIL (the route list is really checked)
6 bench_dir  tools\launcher\make_bench_dir.py output equals the Surface routes apart from `screenshot` lines (M3), has nav\boot_to_menu.txt and states.toml, and the C++ file
          list is satisfied by exactly that output (RequiredFiles == what it writes)
7 wired   launcher_main.cpp carries the S102 U4 lines (button + STOP, tick, poll hook, guards, report zip, @quick, save copy, settings copy)
"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
results = []


def report(name, ok, ev):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {ev}', flush=True)


def clang():
    for c in ('clang++', r'C:\Program Files\LLVM\bin\clang++.exe'):
        if shutil.which(c) or Path(c).exists():
            return c
    return None


def py_measure(csv_text, log_text):
    """The same rule in python: rows with frame_ms > 0; window = frame of `mark 1` .. the largest mark frame; avg = n / sum; low = 1000 / sorted[int(.99 n)]."""
    marks = [(int(m.group(1)), int(m.group(2))) for m in re.finditer(r'\[wwe2k14\]\[input\] mark (\d+)[^\n]*?\(frame (\d+)\)', log_text)]
    first = next(f for i, f in marks if i == 1)
    last = max(f for _, f in marks)
    lines = [l for l in csv_text.splitlines() if l]
    head = lines[0].split(',')
    ix = {n: i for i, n in enumerate(head)}
    rows = []
    for l in lines[1:]:
        f = l.split(',')
        if len(f) != len(head):
            continue
        ms = float(f[ix['frame_ms']]) if f[ix['frame_ms']] else -1
        fr = int(f[ix['frame']])
        if ms > 0 and first <= fr <= last:
            rows.append((ms, int(f[ix['presented']] or 0), float(f[ix['gpu_ms']]) if f[ix['gpu_ms']] else -1))
    ms = sorted(r[0] for r in rows)
    gpu = [r[2] for r in rows if r[2] >= 0]
    return {'frames': len(rows), 'swap_fps': 1000 * len(rows) / sum(r[0] for r in rows), 'low1': 1000 / ms[min(len(ms) - 1, int(0.99 * len(ms)))],
            'gpu_avg': sum(gpu) / len(gpu) if gpu else None, 'shown_fps': 1000 * sum(r[1] for r in rows) / sum(r[0] for r in rows)}


def find_real():
    logs = MAIN / 'out' / 'build' / 'win-amd64-release' / 'logs'
    for log in sorted(logs.glob('wwe2k14_*.log'), key=lambda f: f.stat().st_mtime, reverse=True):
        n = log.stem.split('_')[1]
        csv = logs / f'frames_{n}.csv'
        if not csv.exists():
            continue
        t = log.read_text(errors='replace')
        if 'bench: done' in t and len(re.findall(r'\[wwe2k14\]\[input\] mark \d+', t)) >= 2 and 'mark 1 ' in t:
            return csv, log
    return None, None


def main():
    root = ROOT
    if len(sys.argv) > 2 and sys.argv[1] == '--root':
        root = Path(sys.argv[2]).resolve()
    cc = clang()
    if not cc:
        print('FAIL  clang++ not found (tools\\env.ps1 puts the standalone LLVM first)')
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='u4_bench_'))
    exe = tmp / 'bench_button_test.exe'
    p = subprocess.run([cc, '-std=c++20', '-Wall', '-Wextra', '-I', str(root / 'src' / 'launcher'), str(root / 'tools' / 'launcher' / 's102_tests' / 'bench_button_test.cpp'), '-o', str(exe)],
                       capture_output=True, text=True)
    report('build', p.returncode == 0 and exe.exists() and 'warning' not in p.stderr, (p.stderr or 'clean')[:600])
    if not exe.exists():
        return 1
    scenes = root / 'tools' / 'input_scripts' / 'scenes'
    arenas = root / 'db' / 'ids' / 'arenas.json'
    csv, log = find_real()
    out = tmp / 'out'
    out.mkdir()
    cmd = [str(exe), str(scenes), str(out), str(arenas)] + ([str(csv), str(log)] if csv else [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    report('unit', p.returncode == 0, (p.stdout + p.stderr).strip()[-500:])
    try:
        j = json.loads((out / 'results.json').read_text())
        ok = len(j['scenes']) == 3 and all(k in j['hardware'] for k in ('gpu', 'driver_api', 'cpu', 'ram', 'resolution', 'tier'))
        ok = ok and j['scenes'][0]['game']['avg_ms'] == 4.0 and j['scenes'][0]['gpu']['avg_ms'] == 12.0 and j['scenes'][0]['shown_fps'] is not None
        ok = ok and j['scenes'][2]['error'] != '' and j['scenes'][0]['error'] == ''   # a failed later scene keeps the earlier one's numbers
        report('json', ok, f'{len(j["scenes"])} scenes; first: shown {j["scenes"][0]["shown_fps"]} fps, sides {j["scenes"][0]["game"]["avg_ms"]}/{j["scenes"][0]["render_thread"]["avg_ms"]}/{j["scenes"][0]["gpu"]["avg_ms"]} ms; third: {j["scenes"][2]["error"][:60]}')
    except Exception as e:  # noqa: BLE001
        report('json', False, f'results.json unreadable: {e}')
    if csv is None:
        report('real', False, f'no run with frames CSV + a log holding marks and "bench: done" under {MAIN / "out" / "build" / "win-amd64-release" / "logs"}: nothing to cross-check (never skipped)')
    else:
        try:
            c = json.loads((out / 'real.json').read_text())['scenes'][0]
            e = py_measure(csv.read_text(errors='replace'), log.read_text(errors='replace'))
            ok = (c['frames'] == e['frames'] and abs(c['swap_fps'] - e['swap_fps']) < 0.01 and abs(c['swap_low1_fps'] - e['low1']) < 0.01
                  and abs(c['shown_fps'] - e['shown_fps']) < 0.01 and (e['gpu_avg'] is None or abs(c['gpu']['avg_ms'] - e['gpu_avg']) < 0.01))
            report('real', ok, f'{csv.name} + {log.name}: C++ {c["frames"]} frames {c["swap_fps"]:.2f} fps low {c["swap_low1_fps"]:.2f}; python {e["frames"]} {e["swap_fps"]:.2f} low {e["low1"]:.2f}')
        except Exception as ex:  # noqa: BLE001
            report('real', False, f'real.json: {ex}')
    bad = tmp / 'scenes_planted'
    shutil.copytree(scenes, bad)
    f = bad / 'bryan_entrance.txt'
    f.write_text(f.read_text().replace('log bench: done', 'log bench: finished'))
    (tmp / 'out2').mkdir(exist_ok=True)
    q = subprocess.run([str(exe), str(bad), str(tmp / 'out2'), str(arenas)], capture_output=True, text=True)
    report('plant', q.returncode != 0 and 'bryan_entrance' in q.stdout and 'bench: done' in q.stdout, f'exit {q.returncode}; {q.stdout.strip()[:200]}')
    # 6: the bench folder
    sys.path.insert(0, str(root / 'tools' / 'launcher'))
    import make_bench_dir as mb
    stage = tmp / 'stage'
    made = mb.build(stage, root)
    ok = True
    notes = []
    for s in mb.SCENES:
        src = (scenes / f'{s}.txt').read_text(encoding='utf-8').splitlines(keepends=True)
        got = (stage / 'bench' / f'{s}.txt').read_text(encoding='utf-8').splitlines(keepends=True)
        shots = [l for l in src if re.match(r'\s*screenshot\b', l)]
        rest = [l for l in src if not re.match(r'\s*screenshot\b', l)]
        good = got == rest and len(shots) > 0 and not any('screenshot' in l.split('#')[0] for l in got)
        ok &= good
        notes.append(f'{s}: {len(shots)} screenshot lines removed, rest equal {got == rest}')
    ok &= (stage / 'bench' / 'nav' / 'boot_to_menu.txt').read_bytes() == (root / 'tools' / 'input_scripts' / 'nav' / 'boot_to_menu.txt').read_bytes()
    ok &= (stage / 'bench' / 'states.toml').exists()
    # the C++ required list == what the tool wrote
    hdr = (root / 'src' / 'launcher' / 'bench_button.h').read_text()
    need = set(re.findall(r'"(bench/[^"]+)"', hdr.split('inline std::vector<std::string> RequiredFiles()')[1].split('}')[0]))
    need |= {f'bench/{s}.txt' for s in mb.SCENES}
    ok &= need == set(made)
    # PLANT: a planted screenshot kept in a generated route must be noticed by the same comparison
    planted = (stage / 'bench' / 'match.txt').read_text(encoding='utf-8') + 'screenshot sc_planted\n'
    planted_rest = [l for l in planted.splitlines(keepends=True) if not re.match(r'\s*screenshot\b', l)]
    ok &= planted_rest != planted.splitlines(keepends=True)
    report('bench_dir', ok, '; '.join(notes) + f'; files {sorted(made)}')
    lm = (root / 'src' / 'launcher' / 'launcher_main.cpp').read_text(errors='replace')
    need_marks = ['#include "bench_button.h"', 'bench_step_', 'BenchTick();', 'StopBench', '"@quick"', 'wwe2k14_settings_file', 'root / "save"', 'ReportsDir()', 'quick_arena = bn::kArenaOverTheLimit']
    missing = [n for n in need_marks if n not in lm]
    report('wired', not missing and len(re.findall(r'S102 U4', lm)) >= 6 and '--wwe2k14_launch=lesnar_bryan' not in hdr.split('inline std::string SceneFlagsError')[0].split('Scenes()')[1],
           f'S102 U4 marks: {len(re.findall("S102 U4", lm))}; missing: {missing}')
    print(f'scratch {tmp}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
