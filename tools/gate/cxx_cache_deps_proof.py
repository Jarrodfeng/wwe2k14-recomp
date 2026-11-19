r"""Proof that tools\cxx_cache.ps1 writes honest dependency manifests (S103 lane DIB). 0 game runs, no game build: a tiny two-file "repo" in a temp folder.
  py -I tools\gate\cxx_cache_deps_proof.py
Arms (each a build script in <tmp>\tools\scene\ that dot-sources the REAL cxx_cache.ps1, as every manifest-writing build does):
  A  plain compile + link: out\tools\t.exe.deps.txt and out\deps\tools_scene_build_t.ps1.deps.txt list the .cpp, the header it includes, the build script and the helper;
     a header the .cpp does NOT include is not listed
  B  the cached arm (private sccache dir): a second compile of the same unit is a cache HIT and still leaves a valid manifest (-MMD must not break sccache's hit path,
     and the .d must come back with the object)
  C  an object with no .d (compiled before manifests existed): the manifest is INVALID, with the reason; the tool-cache key then falls back to the whole-tree key
  D  a source linked in the same call as its compile (`clang++ a.cpp -o x.exe`): INVALID (nothing named its objects)
  E  an input generated under out\: INVALID (its content before the build says nothing)
  F  WWE_NO_DEPS=1 writes nothing
"""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REAL = HERE.parents[1]
sys.path.insert(0, str(HERE))
import deps as gdeps

FAILS = []


def ok(c, t):
    print(('ok    ' if c else 'FAIL  ') + t)
    if not c:
        FAILS.append(t)


def run_ps(script, env_extra=None, timeout=240):
    env = dict(os.environ)
    env.update(env_extra or {})
    p = subprocess.run(['pwsh', '-NoProfile', '-File', str(script)], capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


def mk_repo(base, name):
    r = Path(base) / name
    for rel, txt in {'src/a.cpp': '#include "a.h"\nint main() { return A; }\n', 'src/a.h': '#define A 0\n', 'src/unused.h': '#define U 1\n',
                     'src/gen_user.cpp': '#include "../out/gen/g.h"\nint main() { return G; }\n'}.items():
        p = r / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(txt)
    (r / 'out' / 'gen').mkdir(parents=True)
    (r / 'out' / 'gen' / 'g.h').write_text('#define G 0\n')
    (r / 'tools' / 'scene').mkdir(parents=True)
    return r


def build_script(r, body):
    real = str(REAL).replace("'", "''")
    s = r / 'tools' / 'scene' / 'build_t.ps1'
    s.write_text("$ErrorActionPreference = 'Stop'\n$repo = Resolve-Path (Join-Path $PSScriptRoot '..\\..')\n$env:PATH = 'C:\\Program Files\\LLVM\\bin;' + $env:PATH\n"
                 f". '{real}\\tools\\cxx_cache.ps1'\n" + body + "\n")
    return s


def main():
    if not shutil.which('pwsh'):
        print('FAIL: pwsh not found')
        return 1
    base = Path(tempfile.mkdtemp(prefix='cxx_deps_proof_'))
    try:
        plain = {'WWE_NO_CCACHE': '1'}
        # ---- A
        r = mk_repo(base, 'a')
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\n& $cxx0 @cxxRest -c src\\a.cpp -o out\\tools\\a.o\nif ($LASTEXITCODE -ne 0) { throw 'compile' }\n"
                            "& clang++ -fuse-ld=lld out\\tools\\a.o -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\n")
        rc, out = run_ps(s, plain)
        ok(rc == 0 and (r / 'out/tools/t.exe').is_file(), 'A: the build script ran and linked (rc %d)' % rc)
        m = gdeps.read_manifest(r / 'out/tools/t.exe.deps.txt')
        ok(m is not None and 'src/a.cpp' in m and 'src/a.h' in m, 'A: <exe>.deps.txt lists the .cpp and the header it includes (%s)' % (m,))
        ok(m is not None and 'src/unused.h' not in m, 'A: a header the unit does not include is NOT listed')
        ok(m is not None and 'tools/scene/build_t.ps1' in m, 'A: the build script itself is an input')
        ms = gdeps.read_manifest(r / 'out/deps/tools_scene_build_t.ps1.deps.txt')
        ok(ms is not None and set(m or []) <= set(ms), 'A: out\\deps\\<script>.deps.txt holds the exe manifest')
        # ---- B
        r = mk_repo(base, 'b')
        cdir = base / 'sccache'
        envb = {'WWE_CCACHE_DIR': str(cdir), 'WWE_CCACHE_SIZE': '1G', 'WWE_NO_CCACHE': '0'}
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\n& $cxx0 @cxxRest -c src\\a.cpp -o out\\tools\\a.o\nif ($LASTEXITCODE -ne 0) { throw 'compile' }\n"
                            "& clang++ -fuse-ld=lld out\\tools\\a.o -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\nShow-CxxCache\n")
        rc1, out1 = run_ps(s, envb)
        for f in ('out/tools/a.o', 'out/tools/a.d', 'out/tools/t.exe', 'out/tools/t.exe.deps.txt'):
            (r / f).unlink(missing_ok=True)
        rc2, out2 = run_ps(s, envb)
        line = [l for l in out2.splitlines() if 'cxx-cache' in l]
        if line and ' off' in line[0].split('[cxx-cache]')[1][:6]:
            print('SKIP  B: sccache is not available here: ' + line[0])
        else:
            ok(rc1 == 0 and rc2 == 0 and line and ' 1 hits' in line[0].replace(', ', ' ').replace('1 compiles ', '') or (line and '1 hits' in line[0]), 'B: the second compile is a cache HIT with -MMD (%s)' % (line[:1],))
            ok(gdeps.read_manifest(r / 'out/tools/t.exe.deps.txt') is not None and (r / 'out/tools/a.d').is_file(), 'B: a cache hit restores the .d and the manifest is valid')
        # ---- C: an object with no .d
        r = mk_repo(base, 'c')
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\nif (-not (Test-Path out\\tools\\a.o)) { & $cxx0 @cxxRest -c src\\a.cpp -o out\\tools\\a.o }\n"
                            "& clang++ -fuse-ld=lld out\\tools\\a.o -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\n")
        run_ps(s, plain)
        (r / 'out/tools/t.exe.deps.txt').unlink(missing_ok=True)
        (r / 'out/tools/a.d').unlink()
        rc, out = run_ps(s, plain)   # the object exists, its .d does not: it is not recompiled
        why = gdeps.invalid_reason(r / 'out/deps/tools_scene_build_t.ps1.deps.txt')
        ok(rc == 0 and why and 'no .d file' in why and not (r / 'out/tools/t.exe.deps.txt').exists(), 'C: an object without a .d: the script manifest is INVALID (%s) and no exe manifest is left' % (why,))
        ok(gdeps.read_manifest(r / 'out/deps/tools_scene_build_t.ps1.deps.txt') is None, 'C: ...and an INVALID manifest reads as none (the whole-tree key is used)')
        # ---- D: compile + link in one call
        r = mk_repo(base, 'd')
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\n& clang++ -fuse-ld=lld src\\a.cpp -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\n")
        rc, out = run_ps(s, plain)
        why = gdeps.invalid_reason(r / 'out/deps/tools_scene_build_t.ps1.deps.txt')
        ok(rc == 0 and why and 'linked directly' in why, 'D: a source compiled and linked in one call: INVALID (%s)' % (why,))
        # ---- E: a generated input under out\
        r = mk_repo(base, 'e')
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\n& $cxx0 @cxxRest -c src\\gen_user.cpp -o out\\tools\\a.o\nif ($LASTEXITCODE -ne 0) { throw 'compile' }\n"
                            "& clang++ -fuse-ld=lld out\\tools\\a.o -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\n")
        rc, out = run_ps(s, plain)
        why = gdeps.invalid_reason(r / 'out/deps/tools_scene_build_t.ps1.deps.txt')
        ok(rc == 0 and why and 'generated file under out' in why, 'E: a header generated under out\\ makes the manifest INVALID (%s)' % (why,))
        # ---- F: switched off
        r = mk_repo(base, 'f')
        s = build_script(r, "New-Item -ItemType Directory -Force out\\tools | Out-Null\n& $cxx0 @cxxRest -c src\\a.cpp -o out\\tools\\a.o\nif ($LASTEXITCODE -ne 0) { throw 'compile' }\n"
                            "& clang++ -fuse-ld=lld out\\tools\\a.o -o out\\tools\\t.exe\nif ($LASTEXITCODE -ne 0) { throw 'link' }\n")
        rc, out = run_ps(s, dict(plain, WWE_NO_DEPS='1'))
        ok(rc == 0 and not (r / 'out/tools/t.exe.deps.txt').exists() and not (r / 'out/deps').exists(), 'F: WWE_NO_DEPS=1 writes no manifest')
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('PASS: all claims hold' if not FAILS else 'FAIL: %d claim(s) failed' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
