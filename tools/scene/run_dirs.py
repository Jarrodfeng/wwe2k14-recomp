"""Per-run output folders for check scripts (S103 PSO).

The class this closes: a check that defaults its output / reference folder to a FIXED path under the MAIN checkout's out\\ (every worktree and every parallel
check shares it) races with itself: two runs rewrite each other's reference and served pictures (false FAIL, half-written PNG errors, a possible false PASS).

  run_out(name, explicit=None)  ->  RunOut (a str subclass: use it as the path)
      1. the script's own --out argument (explicit): kept, never removed
      2. else a per-run folder <main>\\out\\<name>_<pid> (made when asked for), removed by finish() on PASS, kept and printed on FAIL
  finish(path, rc) -> rc   removes the per-run folder when rc == 0; on a failure prints where the pictures are
  Use at the end of main():   return run_dirs.finish(out, rc)

A script that defaults an output to a fixed path rooted at the main checkout shares it with every worktree; this module gives each run its own folder.
"""
import os
import shutil
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', _ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.normpath(os.path.dirname(common)) if common else _ROOT


class RunOut(str):
    """the output folder path; .owned = this run made it (so finish() may remove it)"""
    owned = False


def run_out(name, explicit=None, env=None, pid=None):
    env = os.environ if env is None else env
    if explicit:
        return RunOut(explicit)
    r = RunOut(os.path.join(main_checkout(), 'out', '%s_%d' % (name, os.getpid() if pid is None else pid)))
    r.owned = True
    return r


def finish(path, rc=0, quiet=False):
    """PASS: remove a per-run folder this run made. FAIL: keep it and say where it is. Returns rc."""
    if not isinstance(path, RunOut) or not path.owned:
        return rc
    if rc == 0:
        shutil.rmtree(path, ignore_errors=True)
    elif not quiet:
        print('INFO  the pictures of this failed run are kept in ' + str(path))
    return rc
