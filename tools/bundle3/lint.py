"""S102 bundle 3 lint: the player default bundle (src/bundle3.h) and the Surface bench's base (tools/surface/base_flags.txt) cannot drift.

  py -I tools/bundle3/lint.py [--root DIR]            the real check (exit 0 = agree)
  py -I tools/bundle3/lint.py --selftest [--root DIR]  the same check on planted copies: every plant must FAIL, the clean copy must pass

Rule: bundle levers == base_flags.txt entries minus EXCLUDED (each with a reason). Fixes in WWE2K14_BUNDLE3_FIXES are bundle-only and must be in FIXES_OK
(a reason each); an entry of base_flags.txt that is also a fix is fine (the bench then measures the player config). Values must be equal.
Every cvar must be defined in src (REXCVAR_DEFINE_*), so a typo cannot silently set nothing, and each lever must be a PerFrame table switch or in START_TIME_OK.
Prints both lists (the player bundle and the bench @BASE) so the equality is read, not assumed.
"""
import re
import sys
from pathlib import Path

# Bench-only or diagnostic lines of base_flags.txt that a player does NOT get, with the reason.
EXCLUDED = {
    'wwe2k14_rdr_gpu_stamps': 'diagnostic: GPU role stamps, a measuring aid (cost, no gain)',
    'wwe2k14_rdr_provenance': 'bench fidelity: =false keeps the +dev kit from installing the provenance writer; players are already off (default false)',
    'wwe2k14_case': 'bench fidelity: =false keeps the +dev kit from running the bug button ring; players are already off (default false)',
    'wwe2k14_display_720p': 'the graphics settings decide the output size (launcher / in-game menu), not a bundle',
    'wwe2k14_display_720p_mode': 'the graphics settings decide the output size, not a bundle',
}
# Correctness fixes shipped with the bundle that are not measured speed levers.
FIXES_OK = {
    'wwe2k14_rdr_run_end_keep': 'K1 / K2 round 2: a copy run ending on a skipped frame no longer leaves an image one frame stale (a shipped picture bug)',
}
# Levers that are not rdr_switches.h PerFrame table entries but are read after OnPreSetup (so applying them there is early enough).
START_TIME_OK = {
    'wwe2k14_nf_replay_loop': 'read once per replay through rdr_sw::BoolSeen (replay_watch.h)',
    'wwe2k14_nf_render_dispatch': 'read once per replay through rdr_sw::BoolSeen (replay_watch.h)',
    # Plain cvar reads in reader.h that run after OnPreSetup: StartLive (reader.h 2490, called from the frame hook at 7017) copies the low_* levers into the
    # renderer options at renderer start (the tier is resolved by then: ApplyLauncherSettings runs in OnPreSetup, after the bundle), and the three below are
    # stored into atomics each frame (7194 / 7195) or once at start (2279).
    'wwe2k14_rdr_low_shadow_small': 'reader.h StartLive (2754), only under the LOW tier (low_levers.h)',
    'wwe2k14_rdr_low_present_noclear': 'reader.h StartLive (2762), only under the LOW tier',
    'wwe2k14_rdr_low_no_fullsize_copies': 'reader.h StartLive (2763), only under the LOW tier',
    'wwe2k14_rdr_fb_fast': 'reader.h 7194, stored every frame',
    'wwe2k14_rdr_pack_once': 'reader.h 7195, stored every frame',
    'wwe2k14_rdr_skip_dead_prelight': 'reader.h 2279, stored at StartLive (census flag; the drop itself is dead_work.h)',
}


def parse_base(text):
    out = {}
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        if '=' not in ln:
            raise SystemExit('base_flags.txt: line without = : %r' % ln)
        k, v = ln.split('=', 1)
        if k in out:
            raise SystemExit('base_flags.txt: %s twice' % k)
        out[k] = v
    return out


def parse_header(text):
    def block(macro):
        m = re.search(r'#define\s+%s\(X\)\s*\\\n((?:.*\\\n)*.*)' % macro, text)
        if not m:
            raise SystemExit('bundle3.h: macro %s not found' % macro)
        return dict(re.findall(r'X\("([^"]+)",\s*"([^"]*)"\)', m.group(1).split('\n\n')[0].split('\n//')[0]))
    levers = block('WWE2K14_BUNDLE3_LEVERS')
    fixes = block('WWE2K14_BUNDLE3_FIXES')
    return levers, fixes


def check(base_text, header_text, src_text, table_text):
    problems = []
    base = parse_base(base_text)
    levers, fixes = parse_header(header_text)
    want = {k: v for k, v in base.items() if k not in EXCLUDED and k not in FIXES_OK}
    for k in sorted(set(want) - set(levers)):
        problems.append('in base_flags.txt but not in the bundle (add it to WWE2K14_BUNDLE3_LEVERS, or exclude it with a reason in lint.py): %s=%s' % (k, want[k]))
    for k in sorted(set(levers) - set(want)):
        problems.append('in the bundle but not in base_flags.txt (the bench would not measure the player config; add it there, or move it to the fixes with a reason): %s=%s' % (k, levers[k]))
    for k in sorted(set(levers) & set(want)):
        if levers[k] != want[k]:
            problems.append('value differs: %s bundle=%s base=%s' % (k, levers[k], want[k]))
    for k in sorted(set(fixes) - set(FIXES_OK)):
        problems.append('a fix without a reason in lint.py FIXES_OK: %s' % k)
    for k in sorted(set(FIXES_OK) - set(fixes)):
        problems.append('FIXES_OK names %s but the bundle has no such fix' % k)
    for k in sorted(set(fixes) & set(base)):
        if fixes[k] != base[k]:
            problems.append('fix value differs from base_flags.txt: %s' % k)
    for k in sorted(set(EXCLUDED) - set(base)):
        problems.append('EXCLUDED names %s but base_flags.txt no longer has it (remove the exclusion)' % k)
    for k in sorted(set(levers) & set(EXCLUDED)):
        problems.append('excluded lever %s is in the bundle' % k)
    for k in sorted(set(levers) | set(fixes)):
        if not re.search(r'REXCVAR_DEFINE_[A-Z0-9_]+\(\s*%s\s*,' % re.escape(k), src_text):
            problems.append('cvar %s is not defined anywhere in src (typo? it would set nothing)' % k)
        elif k not in START_TIME_OK and not re.search(r'X\(\w+,\s*"%s"\s*,\s*PerFrame' % re.escape(k), table_text):
            problems.append('%s is not a PerFrame switch of rdr_switches.h and not in START_TIME_OK (a start-only read before OnPreSetup would miss it)' % k)
    return problems, base, levers, fixes


def load(root):
    r = Path(root)
    src = []
    for p in (r / 'src').rglob('*'):
        if p.suffix in ('.h', '.cpp'):
            src.append(p.read_text(encoding='utf-8', errors='replace'))
    return ((r / 'tools/surface/base_flags.txt').read_text(encoding='utf-8'), (r / 'src/bundle3.h').read_text(encoding='utf-8'),
            '\n'.join(src), (r / 'src/renderer_adapter/rdr_switches.h').read_text(encoding='utf-8'))


def main():
    root = '.'
    if '--root' in sys.argv:
        root = sys.argv[sys.argv.index('--root') + 1]
    base_t, head_t, src_t, tab_t = load(root)
    problems, base, levers, fixes = check(base_t, head_t, src_t, tab_t)
    print('player bundle 3 (src/bundle3.h): %d levers, %d fixes' % (len(levers), len(fixes)))
    for k, v in levers.items():
        print('  bundle %s=%s' % (k, v))
    for k, v in fixes.items():
        print('  fix    %s=%s' % (k, v))
    print('bench @BASE (base_flags.txt): %d lines, %d excluded with a reason' % (len(base), len([k for k in base if k in EXCLUDED])))
    for k, v in base.items():
        print('  base   %s=%s%s' % (k, v, '   [excluded: %s]' % EXCLUDED[k] if k in EXCLUDED else ('   [fix]' if k in FIXES_OK else '')))
    if '--selftest' in sys.argv:
        if problems:
            print('SELFTEST: the clean copy must pass, it has %d problem(s):' % len(problems))
            for p in problems:
                print('  ' + p)
            return 1
        any_lever = next(iter(levers))
        plants = {
            'extra lever planted in base_flags.txt': (base_t + '\nwwe2k14_rdr_planted_lever=true\n', head_t),
            'extra lever planted in the bundle': (base_t, head_t.replace('#define WWE2K14_BUNDLE3_LEVERS(X) \\\n', '#define WWE2K14_BUNDLE3_LEVERS(X) \\\n  X("wwe2k14_rdr_lasting_draw_pct", "50") \\\n', 1)),
            'lever removed from the bundle': (base_t, re.sub(r'  X\("%s", "[^"]*"\)( \\)?\n' % re.escape(any_lever), '', head_t, count=1)),
            'value changed in the bundle': (base_t, head_t.replace('X("wwe2k14_rdr_swap_once", "7")', 'X("wwe2k14_rdr_swap_once", "3")', 1)),
            'typo cvar in the bundle': (base_t + '\nwwe2k14_rdr_no_such_lever=true\n', head_t.replace('X("wwe2k14_rdr_lazy_banks", "true")', 'X("wwe2k14_rdr_no_such_lever", "true")\\\n  X("wwe2k14_rdr_lazy_banks", "true")', 1)),
        }
        bad = 0
        for name, (b, h) in plants.items():
            if b == base_t and h == head_t:
                print('PLANT DID NOT APPLY: %s' % name)
                bad += 1
                continue
            try:
                pr, *_ = check(b, h, src_t, tab_t)
            except SystemExit as e:
                pr = [str(e)]
            print('plant %-42s -> %s' % (name, 'caught (%d)' % len(pr) if pr else 'NOT CAUGHT'))
            if not pr:
                bad += 1
        print('SELFTEST %s' % ('FAIL' if bad else 'ok'))
        return 1 if bad else 0
    for p in problems:
        print('PROBLEM: ' + p)
    print('bundle3 lint: %d problem(s)' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
