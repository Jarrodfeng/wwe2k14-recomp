"""Session 102 W1/W2 (review A, G, opt-in rule): make_release.ps1 -PlanOnly, no build, no lock, no game.
    py -I tools\\release\\check_plan_only.py [--plant regex|stale|optin]
Checks (exit 0 = all hold):
  1. -PlanOnly -GuestProfdata <profile> -V3Exe prints both configure lines; the profile path has forward slashes only (a '-replace "\\\\"' written as
     '\\' is an invalid regex and kills any release with a profile); every switch is explicit on both lines; the build is forced clean.
  2. -PlanOnly without -V3Exe prints no wide configure (the AVX2 exe is opt-in), and -V3Exe with -NoV3Exe is refused (exit 1).
  3. Get-ProfileStaleCount counts the out-of-date lines of a sample log (3).
--plant X runs the same checks against a mutated COPY of the scripts and must exit 1: regex = the invalid '\\' regex is put back; stale = the stale
count looks for the wrong text; optin = the AVX2 exe is off by default again (the pre-2026-11-18 opt-in); streamon = the configure args pass KEEP_STREAM=ON.
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
PROFILE = os.path.join(MAIN, 'out', 'pgo', 'guest_match.profdata')
plant = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == '--plant' else ''
src = os.path.join(ROOT, 'tools', 'release')
work = src
if plant:
    work = tempfile.mkdtemp(prefix='plan_only_')
    for f in ('make_release.ps1', 'release_configure_args.ps1'):
        shutil.copy(os.path.join(src, f), os.path.join(work, f))
    def mutate(f, old, new):
        p = os.path.join(work, f)
        t = open(p, encoding='utf-8').read()
        if t.count(old) < 1:
            print('check_plan_only: FAIL: planted edit does not apply:', old)
            sys.exit(2)
        open(p, 'w', encoding='utf-8').write(t.replace(old, new))
    if plant == 'regex':
        mutate('release_configure_args.ps1', "-replace '\\\\', '/'", "-replace '\\', '/'")
    elif plant == 'stale':
        mutate('release_configure_args.ps1', 'profile-instr-out-of-date|profile data may be out of date', 'nothing-matches-this')
    elif plant == 'optin':
        mutate('make_release.ps1', 'if (-not $NoV3Exe) { $V3Exe = [switch]$true }', '')
    elif plant == 'streamon':
        mutate('release_configure_args.ps1', "$a += '-DWWE2K14_KEEP_STREAM=OFF'; $a += '-DWWE2K14_KEEP_STREAM_RUN=ON'", "$a += '-DWWE2K14_KEEP_STREAM=ON'; $a += '-DWWE2K14_KEEP_STREAM_RUN=OFF'")
    else:
        print('unknown plant', plant)
        sys.exit(2)


def pw(*args):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(work, 'make_release.ps1'), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


bad = 0
def check(ok, what):
    global bad
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        bad += 1


rc, out = pw('-PlanOnly', '-GuestProfdata', PROFILE)   # the AVX2 exe is in every release by default (2026-11-18)
lines = [l for l in out.splitlines() if l.startswith('plan:')]
check(rc == 0 and len(lines) == 3, f'plan with a profile exits 0 and prints 3 plan lines (rc {rc}, {len(lines)} lines){"" if rc == 0 else ": " + out[-200:]}')
want = 'guest_match.profdata'
for l in lines[1:]:
    m = re.search(r'-DWWE2K14_GUEST_PGO_USE=(\S+)', l)
    check(bool(m) and '\\' not in m.group(1) and m.group(1).endswith(want) and '/' in m.group(1), f'profile path has forward slashes only: {m.group(1) if m else "(missing)"}')
    for sw in ('-DWWE2K14_KEEP_STREAM=OFF', '-DWWE2K14_KEEP_STREAM_RUN=ON', '-DWWE2K14_GUEST_ISA=', '-DWWE2K14_OURS_ISA=', '-DWWE2K14_PGO_OURS=ON', '-DWWE2K14_GUEST_PGO_GEN=OFF'):
        check(sw in l, f'configure line has {sw}')
check(bool(lines) and 'clean build: yes' in lines[0], 'a profile forces a clean build')
check(len(lines) == 3 and '-DWWE2K14_GUEST_ISA=x86-64-v3' in lines[2] and '-DWWE2K14_GUEST_ISA= ' in lines[1], 'base ISA empty, wide ISA v3')

rc, out = pw('-PlanOnly')
check(rc == 0 and 'wide configure: ' in out and 'KEEP_STREAM=OFF' in out, 'no switches: the wide (AVX2) configure is printed by default, stream-free')
rc, out = pw('-PlanOnly', '-NoV3Exe')
check(rc == 0 and 'wide configure: ' not in out and 'no wide configure' in out and '-DWWE2K14_GUEST_PGO_USE= ' in out and '-DWWE2K14_PGO_OURS=OFF' in out,
      '-NoV3Exe: no wide configure (the explicit opt-out) and the profile values are explicitly empty/OFF')
rc, out = pw('-PlanOnly', '-V3Exe', '-NoV3Exe')
check(rc != 0, f'-V3Exe with -NoV3Exe is refused (rc {rc})')

log = os.path.join(tempfile.mkdtemp(prefix='stale_'), 'b.log')
open(log, 'w').write('a.cpp:1:1: warning: x: profile data may be out of date: of 10 functions, 2 have mismatched data [-Wprofile-instr-out-of-date]\n'
                     + 'noise\n' * 2 + 'b.cpp: warning: [-Wprofile-instr-out-of-date]\nc.cpp: warning: [-Wprofile-instr-out-of-date]\n')
r = subprocess.run(['pwsh', '-NoProfile', '-Command', f'. "{os.path.join(work, "release_configure_args.ps1")}"; Get-ProfileStaleCount "{log}"'], capture_output=True, text=True)
check(r.stdout.strip() == '3', f'stale warning count is 3 (got "{r.stdout.strip()}")')
print('check_plan_only:', 'PASS' if not bad else f'FAIL ({bad})')
sys.exit(1 if bad else 0)
