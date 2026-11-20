"""S102 bundle 3 lint: the player default bundle (src/bundle3.h) and the Surface bench's base (tools/surface/base_flags.txt) cannot drift.

  py -I tools/bundle3/lint.py [--root DIR]            the real check (exit 0 = agree)
  py -I tools/bundle3/lint.py --selftest [--root DIR]  the same check on planted copies: every plant must FAIL, the clean copy must pass

Rule: bundle levers == base_flags.txt entries minus EXCLUDED (each with a reason). Fixes in WWE2K14_BUNDLE3_FIXES are bundle-only and must be in FIXES_OK
(a reason each); an entry of base_flags.txt that is also a fix is fine (the bench then measures the player config). Values must be equal.
Every cvar must be defined in src (REXCVAR_DEFINE_*), so a typo cannot silently set nothing, and each lever must be a PerFrame table switch or in START_TIME_OK.
Prints both lists (the player bundle and the bench @BASE) so the equality is read, not assumed.

S103 SLV2: WWE2K14_BUNDLE3_OFF_LEVERS (src/bundle3.h) names the levers the STREAM-FREE build reads. Each must be a bundle lever AND its cvar must be defined in a unit the stream-free exe compiles
(not a unit of tools/scene/keep_stream_sources.txt, not a header of keep_stream_headers.txt, and outside any `#if WWE2K14_STREAM()` region): the SLV review found two levers in the bundle for weeks with
no such cvar in the stream-free build, so the bundle set nothing there. Apply() also reports such a name at run time (bundle3::g_missing, an ERROR line), so it cannot be silent either way.
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
    'wwe2k14_rdr_device_lost_message': 'bench fidelity: =false keeps an unattended test run from opening the S105 A9 device-lost box (players get the box: default true); not a speed lever',
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


def off_visible_defines(files):
    """Names of the cvars defined (REXCVAR_DEFINE_*) where the stream-free build sees them: `files` = [(text, stream_only_file)]. A stream-only file contributes nothing; in the others a define inside
    `#if WWE2K14_STREAM()` (before its #else) or `#if !WWE2K14_STREAM()`'s #else branch is ON-only and skipped. Any other #if / #ifdef is transparent."""
    found = set()
    for text, stream_only in files:
        if stream_only:
            continue
        stack = []   # per open #if: 'on' = the ON-only branch, 'off' = the OFF-only branch, 'any' = not a stream test
        for ln in text.splitlines():
            t = ln.strip()
            m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', t)
            if m:
                d, rest = m.group(1), m.group(2).strip()
                if d in ('if', 'ifdef', 'ifndef'):
                    stack.append('on' if (d == 'if' and rest == 'WWE2K14_STREAM()') else 'off' if (d == 'if' and rest == '!WWE2K14_STREAM()') else 'any')
                elif d == 'else' and stack:
                    stack[-1] = {'on': 'off', 'off': 'on'}.get(stack[-1], 'any')
                elif d == 'elif' and stack:
                    stack[-1] = 'any'
                elif d == 'endif' and stack:
                    stack.pop()
                continue
            if 'on' in stack:
                continue
            for name in re.findall(r'REXCVAR_DEFINE_[A-Z0-9_]+\(\s*(\w+)\s*,', t):
                found.add(name)
    return found


def off_stored(start_text):
    """S103 CHS (review fix 4): the cvars start_settings.cpp READS (REXCVAR_GET) inside its stream-free START-ONLY block(s) (`// START-ONLY-BEGIN(OFF)` ... `// START-ONLY-END`): the ones whose value reaches the
    renderer in the stream-free build. A cvar that is only DEFINED there (or defined and never stored) sets nothing: the bundle's value is read by nobody (C5: stencil_dead was a bundle lever, defined in reader.h only)."""
    got = set()
    for blk in re.findall(r'//\s*START-ONLY-BEGIN\(OFF\).*?//\s*START-ONLY-END', start_text, flags=re.S):
        got |= set(re.findall(r'REXCVAR_GET\(\s*(\w+)\s*\)', blk))
    return got


def check(base_text, header_text, src_text, table_text, off_defined=None, start_text=None):
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
    if off_defined is not None:
        m = re.search(r'#define\s+WWE2K14_BUNDLE3_OFF_LEVERS\(X\)\s*\\\n((?:.*\\\n)*.*)', header_text)
        off_levers = re.findall(r'X\("([^"]+)"\)', m.group(1).split('\n\n')[0].split('\n//')[0]) if m else []
        if not m:
            problems.append('bundle3.h: the macro WWE2K14_BUNDLE3_OFF_LEVERS is missing')
        start_defs = off_visible_defines([(start_text, False)]) - set(re.findall(r'REXCVAR_GET\(\s*(\w+)\s*\)', start_text)) if start_text is not None else set()   # defined in start_settings.cpp's stream-free copies and never stored by its start
        for k in sorted((set(levers) | set(fixes)) & start_defs - set(off_levers)):   # S103 SMF (review A1): such a bundle lever is turned ON in the stream-free build by bundle3::Apply, which never ran there
            problems.append('%s is a bundle lever defined in a unit the stream-free build compiles but is not in WWE2K14_BUNDLE3_OFF_LEVERS: Apply() would switch it on there (never run stream-free). Remove the define (a reader that wants it must tolerate the flag being absent), or list it as an OFF lever after proving it stream-free' % k)
        for k in off_levers:
            if k not in levers:
                problems.append('WWE2K14_BUNDLE3_OFF_LEVERS names %s which is not a bundle lever' % k)
            if k not in off_defined:
                problems.append('%s is listed as read by the stream-free build but no cvar of that name is defined in a unit the stream-free build compiles (define it in start_settings.cpp under #if !WWE2K14_STREAM(), as combine_lite / skip_blank_plane are)' % k)
            if start_text is not None and k not in off_stored(start_text):
                problems.append('%s is a WWE2K14_BUNDLE3_OFF_LEVERS name but start_settings.cpp never REXCVAR_GET-stores it inside its START-ONLY-BEGIN(OFF) block: the bundle sets a cvar nothing reads (S103 CHS C5 / SLV2 class)' % k)
    return problems, base, levers, fixes


def load(root):
    r = Path(root)
    src = []
    off_files = []
    def listed(name):
        f = r / 'tools/scene' / name
        return {ln.strip().replace('\\', '/') for ln in f.read_text(encoding='utf-8').splitlines() if ln.strip() and not ln.strip().startswith('#')} if f.exists() else set()
    only_units, only_headers = listed('keep_stream_sources.txt'), listed('keep_stream_headers.txt')
    for p in (r / 'src').rglob('*'):
        if p.suffix in ('.h', '.cpp', '.inc'):
            text = p.read_text(encoding='utf-8', errors='replace')
            rel = p.relative_to(r).as_posix()
            if p.suffix in ('.h', '.cpp'):
                src.append(text)
            off_files.append((text, rel in only_units or rel[len('src/'):] in only_headers))
    load.off_defined = off_visible_defines(off_files)
    return ((r / 'tools/surface/base_flags.txt').read_text(encoding='utf-8'), (r / 'src/bundle3.h').read_text(encoding='utf-8'),
            '\n'.join(src), (r / 'src/renderer_adapter/rdr_switches.h').read_text(encoding='utf-8'))


def start_text_of(root):
    return (Path(root) / 'src/start_settings.cpp').read_text(encoding='utf-8')


def main():
    root = '.'
    if '--root' in sys.argv:
        root = sys.argv[sys.argv.index('--root') + 1]
    base_t, head_t, src_t, tab_t = load(root)
    start_t = start_text_of(root)
    problems, base, levers, fixes = check(base_t, head_t, src_t, tab_t, load.off_defined, start_t)
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
        plants['OFF lever with no cvar in the stream-free build (reader.h only)'] = (base_t, head_t.replace('X("wwe2k14_rdr_skip_blank_plane")', 'X("wwe2k14_rdr_skip_blank_plane") \\\n  X("wwe2k14_rdr_lazy_banks")', 1))
        plants['OFF lever that is not a bundle lever'] = (base_t, head_t.replace('X("wwe2k14_rdr_skip_blank_plane")', 'X("wwe2k14_rdr_skip_blank_plane") \\\n  X("wwe2k14_rdr_not_a_lever")', 1))
        # S103 CHS: the lever is DEFINED in the stream-free build but never stored by its start (the shape C5's bug had once the define existed), and the exact pre-CHS state: listed as an OFF lever with no define and no store
        stored_gone = re.sub(r'.*REXCVAR_GET\(wwe2k14_rdr_stencil_dead\).*\n', '', start_t)
        plants_start = {'OFF lever defined but never stored by the start (store line removed)': stored_gone,
                        'OFF lever neither defined nor stored (start_settings.cpp as it was before CHS C5)': start_t.replace('REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead_x').replace('REXCVAR_GET(wwe2k14_rdr_stencil_dead)', 'REXCVAR_GET(wwe2k14_rdr_stencil_dead_x)')}
        for name, st in plants_start.items():
            if st == start_t:
                print('PLANT DID NOT APPLY: %s' % name)
                bad_start = True
            else:
                pr, *_ = check(base_t, head_t, src_t, tab_t, load.off_defined, st)
                print('plant %-42s -> %s' % (name, 'caught (%d)' % len(pr) if pr else 'NOT CAUGHT'))
                bad_start = not pr
            if bad_start:
                return 1
        bad = 0
        # S103 SMF (review A1) twin: a bundle lever (swap_once) defined where the stream-free build sees it, and not an OFF lever: the shape of the SMF define that would have switched it on stream-free
        pr, *_ = check(base_t, head_t, src_t, tab_t, load.off_defined, start_t + '\n#if !WWE2K14_STREAM()\nREXCVAR_DEFINE_INT32(wwe2k14_rdr_swap_once, 0, "wwe2k14", "x");\n#endif\n')
        hit = any('swap_once is a bundle lever defined in a unit the stream-free' in x or 'wwe2k14_rdr_swap_once is a bundle lever defined' in x for x in pr)
        print('plant %-42s -> %s' % ('bundle lever defined stream-free, not OFF', 'caught' if hit else 'NOT CAUGHT'))
        if not hit:
            bad += 1
        for name, (b, h) in plants.items():
            if b == base_t and h == head_t:
                print('PLANT DID NOT APPLY: %s' % name)
                bad += 1
                continue
            try:
                pr, *_ = check(b, h, src_t, tab_t, load.off_defined, start_t)
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
