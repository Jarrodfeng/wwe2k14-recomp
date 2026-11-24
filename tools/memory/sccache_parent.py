"""sccache_parent.py: with the compile cache ON, which process starts the clang++ of a cached tool compile, and by which path (CQM 4a, 2026-11-20).

    py -I tools\\memory\\sccache_parent.py [--src src\\renderer_scene\\record_io.cpp] [--timeout 180]

Starts ONE compile through tools\\memory\\rig_unit_line.ps1 (build_rig's line, cache ON, a unique -D so it is a cache MISS and the real compile runs)
under proc.run (hard timeout, output pipes drained by its threads, the tree killed at the deadline), and a watcher thread that looks every 0.5 s for
clang / clang++ processes whose command line names the unique define, printing each one's parent chain, image path and argv[0]. The object goes
under out\\cqm\\cache. Note: the clang++ runs in the sccache SERVER's tree (the finding), so a timeout kills the pwsh side only; the server's compile
ends by itself.
"""
import argparse
import os
import shutil
import sys
import threading
import uuid

import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'compile'))
import proc  # noqa: E402


def chain(p):
    out = []
    while p is not None and len(out) < 8:
        try:
            out.append(f'{p.pid}:{p.name()}')
            p = p.parent()
        except Exception:
            break
    return ' <- '.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=r'src\renderer_scene\record_io.cpp')
    ap.add_argument('--timeout', type=float, default=180)
    a = ap.parse_args()
    tag = 'CQM_PROBE_' + uuid.uuid4().hex[:10]
    env = dict(os.environ)
    env.pop('WWE_NO_CCACHE', None)
    out = os.path.join('out', 'cqm', 'cache', tag + '.o')
    seen = {}
    stop = threading.Event()

    def watch():
        while not stop.is_set():
            for q in psutil.process_iter(['pid', 'name']):
                nm = (q.info['name'] or '').lower()
                if q.info['pid'] in seen or not nm.startswith('clang'):
                    continue
                try:
                    cl = q.cmdline()
                except Exception:
                    continue
                if any(tag in x for x in cl):
                    try:
                        exe = q.exe()
                    except Exception:
                        exe = '?'
                    seen[q.info['pid']] = (exe, cl[0] if cl else '', chain(q), '-E' in cl)
            stop.wait(0.5)

    th = threading.Thread(target=watch, daemon=True)

    def on_start(job, popen):
        proc.close_handle(job)                     # the job handle is not needed here
        th.start()

    cmd = [shutil.which('pwsh'), '-NoProfile', '-File', os.path.join(REPO, 'tools', 'memory', 'rig_unit_line.ps1'), '-Src', a.src, '-Out', out,
           '-Extra', '-D' + tag + '=1']
    try:
        r = proc.run(cmd, timeout=a.timeout, cwd=REPO, env=env, on_start=on_start)
    finally:
        stop.set()
        if th.is_alive():
            th.join(5)
    print(f'compile rc {r.returncode}{" (TIMED OUT: the pwsh tree was killed)" if r.timed_out else ""}; probe define {tag}')
    for pid, (exe, a0, ch, pre) in seen.items():
        print(f'  clang pid {pid} ({"preprocess -E" if pre else "compile"}): image {exe}; argv[0] {a0!r}')
        print(f'    parent chain: {ch}')
    if not seen:
        print('  no clang process with the probe define was seen (lived < 0.5 s?)')
    if r.returncode:
        print(((r.stdout or '') + (r.stderr or ''))[-1500:])
    return 0 if r.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
