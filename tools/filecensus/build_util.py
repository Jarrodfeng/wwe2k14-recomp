"""Lane 21: the census viewer is this checkout's own build, rebuilt when anything it is made from is newer (never another checkout's exe).

  ensure_census_exe(root, main) -> path of root\\out\\tools\\wwe2k14_assets_rdr_census.exe, after make_census_build.py + build_viewer.ps1 -Backend rdr when the
  exe is missing or older than the newest file under src\\renderer, src\\renderer_scene, src\\assets (the viewer, its loaders, the fx headers),
  tools\\assets\\build_viewer.ps1, tools\\filecensus\\make_census_build.py.   stale_reason(root) says why a rebuild is due (None = current)."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIRS = [('src', 'renderer'), ('src', 'renderer_scene'), ('src', 'assets'), ('src', 'launcher')]
SOURCE_FILES = [('tools', 'assets', 'build_viewer.ps1'), ('tools', 'filecensus', 'make_census_build.py')]


def exe_path(root):
    return os.path.join(root, 'out', 'tools', 'wwe2k14_assets_rdr_census.exe')


def newest_source(root):
    best, which = 0.0, ''
    for parts in SOURCE_DIRS:
        for dp, _, fns in os.walk(os.path.join(root, *parts)):
            for fn in fns:
                if fn.endswith(('.cpp', '.h', '.hpp', '.c', '.inc', '.spv', '.ps1', '.py')):
                    p = os.path.join(dp, fn)
                    t = os.path.getmtime(p)
                    if t > best:
                        best, which = t, p
    for parts in SOURCE_FILES:
        p = os.path.join(root, *parts)
        if os.path.exists(p) and os.path.getmtime(p) > best:
            best, which = os.path.getmtime(p), p
    return best, which


def stale_reason(root):
    exe = exe_path(root)
    if not os.path.exists(exe):
        return 'no census viewer yet'
    t, which = newest_source(root)
    if os.path.getmtime(exe) < t:
        return 'the census viewer is older than %s' % which
    return None


def ensure_census_exe(root, main=None, log=print):
    why = stale_reason(root)
    if why:
        log('census viewer: %s; rebuilding' % why)
        p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'filecensus', 'make_census_build.py'), '--root', root], capture_output=True, text=True, timeout=300)
        if p.returncode:
            raise RuntimeError('make_census_build.py failed: %s' % (p.stdout + p.stderr)[-600:])
        script = os.path.join(root, 'out', 'filecensus', 'build', 'build_census_viewer.ps1')
        p = subprocess.run(['pwsh', '-NoProfile', '-File', script, '-Backend', 'rdr'], capture_output=True, text=True, timeout=1700)
        if p.returncode or not os.path.exists(exe_path(root)):
            raise RuntimeError('the census viewer did not build: %s' % (p.stdout + p.stderr)[-800:])
        # (the exe is newer than every source now; touch it in case the linker kept an older stamp)
        os.utime(exe_path(root), None)
    return exe_path(root)
