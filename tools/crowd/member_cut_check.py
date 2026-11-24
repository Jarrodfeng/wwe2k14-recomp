#!/usr/bin/env python3
"""Session 102 lane T13 acceptance, offline, 0 game runs, no window: the crowd-member cut ON (--wwe2k14_rdr_member_cut; R13 cut 3, docs\\renderer\\SKIP_GAME_WORK_MAP.md 20).

  py -I tools\\crowd\\member_cut_check.py [--root DIR] [--no-build] [--captures bryan,match131008] [--strip K]

The cut itself and its picture proof are lane 272 / 295's (member_thin_check.py: the cut EMULATED on the captures and replayed byte-equal, holes 0, the class-uniformity census, the stamp
probe, 20 planted faults). The cut can only be shown on a capture made with it on, so, as for the cards (T2), the emulation (member_thin_cut.py: the op-15 of every member but the first of each mesh run removed, the op-21s
and the tally kept) is what is replayed; what the switch itself decides (the per-run mode, the observers, the binder's per-frame read) is proven by the unit test (member_thin_test.cpp: ResolveMemberMode, the run's mode) and by
the wiring lint below. This check adds what T13 needs:
  1. COUNTS on each capture: stream draws per frame (all classes) and member draws per frame before / after the cut (over the frames the cut thinned), and OUR member draws unchanged (thin arm == full arm, holes 0).
  2. PICTURE: the thin arm equals the full arm on every frame (byte-equal presented surface), validation 0.
  3. THE FALLBACK. A frame ours has no scene record for:
       honest   the record of a FULL frame (one the hook recorded in full) is removed: ours falls back, the game's members stay in the stream: holes 0, picture still equal to the full arm.
       PLANT    the record of a THIN frame (members the hook dropped) is removed: nothing replaces the dropped members: the binder must count a hole on EVERY stripped frame and the picture must move. CAUGHT only when both hold.
  4. WIRING (text lint with plants): the switch is a per-frame table entry (rdr_switches.h), read through BoolSeen with the guest frame ONCE per run in OnCrowdDraw; the observers and DropMember use the run's mode (t_dec.mode),
     never a cvar; the binder reads members_thin_now every frame (live.cpp -> CrowdOurs::MembersThin); the old census cvar at 2 still means the cut. Each plant removes one piece and must fail.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import crowd_cards_check as ccc   # noqa: E402
import member_thin_check as mtc   # noqa: E402
import card_cut_check as cck      # noqa: E402
import cards_census as cc         # noqa: E402

SITES = (0x82518574, 0x8276ED60)   # the stands / floor member op-15 call sites (member_ledger.py SITES)


def stream_counts(zip_path):
    """-> {frame number: (all draws the op list replays, member draws)} from the zip alone."""
    cap = cc.Cap(zip_path)
    out = {}
    for fi, num in enumerate(cap.nums):
        f = cap.frame(fi)
        kinds, idx = f.ops['kind'], f.ops['index']
        n_all = int((kinds == 2).sum())
        n_mem = 0
        if f.has_origins:
            cs = f.origins['call_site']
            n_mem = int(sum(1 for k, i in zip(kinds, idx) if k == 2 and int(i) < len(cs) and int(cs[int(i)]) in SITES))
        out[num] = (n_all, n_mem)
    return out, cap


SRC = {
    'sw': ('src', 'renderer_adapter', 'rdr_switches.h'), 'hook': ('src', 'scene_live', 'member_thin_hook.h'), 'core': ('src', 'scene_live', 'member_thin_core.h'),
    'live': ('src', 'renderer_adapter', 'live.cpp'), 'co': ('src', 'renderer_scene', 'crowd_ours.cpp'), 'coh': ('src', 'renderer_scene', 'crowd_ours.h'),
    'hooks': ('src', 'scene_live', 'crowd_hooks.h'), 'sc': ('src', 'scene_crowd.h'), 'cardhook': ('src', 'scene_live', 'card_thin_hook.h'), 'cardcore': ('src', 'scene_live', 'card_thin_core.h'),
}


def fn_body(src, name):
    """The text of `inline <type> name(...) { ... }` (balanced braces), or None."""
    import re as _re
    m = _re.search(r'inline [\w:<>]+ ' + name + r'\([^)]*\) \{', src)
    if not m:
        return None
    k, depth = m.end(), 1
    while depth and k < len(src):
        depth += {'{': 1, '}': -1}.get(src[k], 0)
        k += 1
    return src[m.end():k - 1]


def wiring(s):
    """s = {key: source text} (SRC keys). -> list of failure texts (empty = the wiring holds). Pure: the plants below mutate a COPY and must make it non-empty."""
    bad = []

    def need(key, needle, what):
        if needle not in s[key]:
            bad.append('missing: ' + what)

    def forbid(text, needle, what):
        if needle in text:
            bad.append('forbidden: ' + what)
    need('sw', 'X(MemberCut, "wwe2k14_rdr_member_cut", PerFrame, 0)', 'the switch is a per-frame table entry, default off')
    need('hook', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_member_cut, false', 'the cvar defaults false')
    need('hook', 'rdr_sw::BoolSeen(rdr_sw::kMemberCut, CutRequested(), guest_frame)', 'the switch is read through the table with the guest frame')
    need('hook', 'const Mode m = EffectiveMode(cut);\n  t_dec.mode = m;', "the run's mode is taken once, in OnCrowdDraw")
    need('hook', 'if (BinderFlag(m)) g_counts.MarkSkip(guest_frame);', "the binder's per-frame flag comes from the run's mode (Skip), via the CountRing slot (F1 / F2)")
    need('hook', 'const Mode m = t_dec.mode;   // S102 T13', "DropMember uses the run's mode")
    need('core', 'inline constexpr bool BinderFlag(Mode m) { return m == Mode::Skip; }', 'BinderFlag is the mode, not the switch')
    need('core', 'bool SkipAt(uint64_t frame) const', 'the CountRing slot carries the skip bit')
    need('core', 'const bool cut = cut_switch || raw >= 2;', 'the old census cvar at 2 still means the cut')
    need('live', 'co.members_thin_at = [](uint64_t f) { return wwe2k14::member_thin::g_counts.SkipAt(f); };', "live.cpp hands the binder the hook's per-frame mode (members)")
    need('live', 'co.cards_thin_at = [](uint64_t f) { return wwe2k14::card_thin::g_counts.SkipAt(f); };', "live.cpp hands the binder the hook's per-frame mode (cards)")
    need('coh', 'bool MembersThin() const { return options_.members_thin || mthin_live_; }', 'CrowdOurs::MembersThin is the per-frame OR')
    need('co', 'RefreshMembersThin(in.frame ? in.frame->header.guest_frame : 0);', 'Inject refreshes the flag once per frame with its frame')
    need('co', '(options_.member_probe || MembersThin()) && options_.member_counts && !signal', 'F4a: the stamp probe also runs on the live path')
    need('cardhook', 'if (BinderFlagCards(m)) g_counts.MarkSkip(', "T2: the binder's flag comes from the run's mode")
    need('hooks', 'wwe2k14::member_thin::Observing()', 'crowd_hooks.h observes through the run mode')
    need('sc', 'wwe2k14::member_thin::IsMemberSite(uint32_t(ctx.lr)) && wwe2k14::member_thin::Observing()', 'scene_crowd.h asks DropMember through the run mode')
    for name in ('OnOp14', 'OnMemberOp21', 'DropMember', 'OnPalette'):   # F3: no cvar, no mode resolution inside a run's observers
        b = fn_body(s['hook'], name)
        if b is None:
            bad.append('observer %s not found' % name)
            continue
        for tok in ('RawMode(', 'REXCVAR_GET(', 'EffectiveMode('):
            forbid(b, tok, '%s inside %s' % (tok, name))
    forbid(s['hooks'], 'member_thin::RawMode', 'member_thin::RawMode in crowd_hooks.h')
    forbid(s['sc'], 'member_thin::RawMode', 'member_thin::RawMode in scene_crowd.h')
    for key in ('hook', 'core', 'live', 'cardhook', 'cardcore', 'coh', 'co'):
        forbid(s[key], 'g_cut_seen', 'the sticky g_cut_seen (%s)' % key)
        forbid(s[key], 'members_thin_now', 'members_thin_now (%s)' % key)
    return bad


def wiring_main(root):
    s = {k: open(os.path.join(root, *v), encoding='utf-8').read() for k, v in SRC.items()}
    bad = wiring(s)
    for b in bad:
        print('FAIL  wiring: ' + b)
    if not bad:
        print('ok    wiring: every needle holds and no forbidden text is present')
    plants = [
        ('RawMode( inside OnOp14', 'hook', lambda x: x.replace('if (site < 0 || !Observing()) return;', 'if (site < 0 || RawMode() <= 0) return;', 1)),
        ('REXCVAR_GET inside DropMember', 'hook', lambda x: x.replace('const Mode m = t_dec.mode;   // S102 T13', 'const Mode m = REXCVAR_GET(wwe2k14_rdr_member_cut) ? Mode::Skip : Mode::Off;   // S102 T13', 1)),
        ('member_thin::RawMode in crowd_hooks.h', 'hooks', lambda x: x.replace('wwe2k14::member_thin::Observing()', 'wwe2k14::member_thin::RawMode() > 0', 1)),
        ('the sticky flag back (live.cpp)', 'live', lambda x: x.replace('g_counts.SkipAt(f); };   // S102 T13', 'g_counts.SkipAt(f) || wwe2k14::member_thin::g_cut_seen.load(); };   // S102 T13', 1)),
        ('the binder flag set from the switch (flag_from_cut)', 'hook', lambda x: x.replace('if (BinderFlag(m)) g_counts.MarkSkip(guest_frame);', 'if (cut) g_counts.MarkSkip(guest_frame);', 1)),
        ('F4a reverted (probe without the live flag)', 'co', lambda x: x.replace('(options_.member_probe || MembersThin()) && options_.member_counts && !signal', 'options_.member_probe && options_.member_counts && !signal', 1)),
    ]
    fails = len(bad)
    for name, key, mut in plants:
        c = dict(s)
        c[key] = mut(s[key])
        if c[key] == s[key]:
            print('FAIL  plant "%s" DID NOT APPLY' % name)
            fails += 1
            continue
        caught = bool(wiring(c))
        print(('ok    plant "%s": caught' if caught else 'FAIL  plant "%s": NOT caught') % name)
        fails += 0 if caught else 1
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--captures', default='bryan,match131008')
    ap.add_argument('--strip', type=int, default=6, help='K: thin frames whose record is removed in the fallback arms')
    ap.add_argument('--counts-only', action='store_true', help='only the COUNT lines (no arms)')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    R = ccc.Result()
    for _ in range(wiring_main(root)):
        R.line(False, 'wiring lint')
    main_co = ccc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    zips = {n: os.path.join(main_co, p) for n, p in mtc.CAPTURES}
    names = args.captures.split(',')
    for need in [zips[n] for n in names] + [game]:
        if not os.path.exists(need):
            return ccc.fail_exit('input missing: ' + need)
    if not args.no_build:
        import subprocess
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return ccc.fail_exit('fview build (exit %d)' % p.returncode)
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    exe = _fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 's102_t13')
    os.makedirs(work, exist_ok=True)
    for n in names:
        z = zips[n]
        zt = os.path.join(work, n + '_cut.zip')
        c = mtc.cut(root, z, zt)
        fl = ['--crowd-floor'] if c['floor'] else []
        thin_extra = fl + ['--crowd-members-thin', '--crowd-members-thin-counts', zt[:-4] + '.counts.csv']
        dec = {}
        with open(zt[:-4] + '.dec.csv') as fh:
            for line in fh:
                if line.startswith('#') or not line.strip():
                    continue
                f, kind = line.strip().split(',')[:2]
                dec[int(f)] = kind
        # --- 1. counts
        before, _ = stream_counts(z)
        after, capt = stream_counts(zt)
        thin = [k for k in before if dec.get(k) == 'thin']
        if len(thin) < 5:
            return ccc.fail_exit('%s: fewer than 5 thin frames (%d): the cut did not act' % (n, len(thin)))
        tb = sum(before[k][0] for k in thin) / len(thin)
        ta = sum(after[k][0] for k in thin) / len(thin)
        mb = sum(before[k][1] for k in thin) / len(thin)
        ma = sum(after[k][1] for k in thin) / len(thin)
        if args.counts_only:
            print('COUNT %-11s recorded draws a thinned frame (%d thin of %d frames): all classes %.0f -> %.0f (-%.0f), member draws %.0f -> %.0f; the cut dropped %d member draws, kept %d anchors' % (n, len(thin), len(before), tb, ta, tb - ta, mb, ma, c['dropped'], c['kept']))
            continue
        A = ccc.Arm(exe, z, game, work, n + '_A', fl + ['--crowd-members-probe'])
        B = ccc.Arm(exe, zt, game, work, n + '_B', thin_extra)
        ca, _ = mtc.census(A)
        cb, _ = mtc.census(B)
        print('COUNT %-11s recorded draws a thinned frame (%d frames): all classes %.0f -> %.0f (-%.0f), member draws %.0f -> %.0f (%.0f kept as run anchors); the stream a frame keeps %.1f %%' % (
            n, len(thin), tb, ta, tb - ta, mb, ma, ma, 100.0 * ta / tb))
        if not ca or not cb:
            print(B.text[-2500:])
            R.line(False, '%s: no THIN members census in an arm' % n)
            continue
        R.line(c['thin'] > 0 and cb['thin'] == c['thin'] and cb['holes'] == 0 and cb['refused'] == 0 and cb['dropped'] == c['dropped'] and ta < tb and ma < mb and B.valid == 0 and A.valid == 0,
               '%s: the cut removes %d member draws over %d thin frames, thin frames drawn %d, holes %d, full frames refused %d, validation %d' % (n, c['dropped'], c['thin'], cb['thin'], cb['holes'], cb['refused'], B.valid))
        # --- 2. picture
        diff = B.differing(A)
        R.line(diff == 0 and len(B.hashes) == len(A.hashes), '%s: picture: cut arm == full arm on every frame (%d frames, %d differ)' % (n, len(A.hashes) - 1, diff))
        # --- 2b. the LIVE path (T13 review F4b): members_thin = false, member_probe = false, the binder's per-frame flag members_thin_at true on every frame the hook ran in Skip mode (here: a sidecar row)
        L = ccc.Arm(exe, zt, game, work, n + '_L', fl + ['--crowd-members-thin-live', '--crowd-members-thin-counts', zt[:-4] + '.counts.csv'])
        cl, pl = mtc.census(L)
        dl = L.differing(A)
        R.line(cl is not None and pl is not None and pl[3] > 0 and cl['thin'] == c['thin'] and cl['holes'] == 0 and cl['refused'] == 0 and dl == 0 and L.valid == 0,
               '%s: LIVE path (no start option, only the per-frame flag): thin frames drawn %s of %d, holes %s, picture == full arm (%d differ), stamp probe frames %s (> 0: the probe runs on the live path), validation %d' % (
                   n, cl and cl['thin'], c['thin'], cl and cl['holes'], dl, pl and pl[3], L.valid))
        # --- 3. the fallback
        nums = sorted(dec)
        scene = capt.scene
        K = args.strip
        thin_ok = [j for j in nums if dec[j] == 'thin' and (j - 1) in scene][3::3][:K]
        fulls = [j for j in nums if dec[j] == 'full' and (j - 1) in scene][1:1 + K]
        for nm, js in (('thin frames', thin_ok), ('full frames', fulls)):
            if len(js) < 2:
                return ccc.fail_exit('%s: not enough %s to strip (%d)' % (n, nm, len(js)))

        def arm(label, js, tag, ref=False):
            zm = os.path.join(work, '%s_%s.zip' % (n, tag))
            cck.without_records(zt, zm, [j - 1 for j in js])
            shutil.copy(zt[:-4] + '.counts.csv', zm[:-4] + '.counts.csv')
            M = ccc.Arm(exe, zm, game, work, n + '_' + tag, fl + ['--crowd-members-thin', '--crowd-members-thin-counts', zm[:-4] + '.counts.csv'])
            cm, _ = mtc.census(M)
            os.remove(zm)
            os.remove(zm[:-4] + '.counts.csv')
            d = M.differing(A)
            near = True
            if ref:   # CONTROL: the SAME records stripped from the UNCUT capture (the game recorded every member everywhere): the cut arm must equal it on every frame (a full frame is not touched by the cut)
                zr = os.path.join(work, '%s_%s_ref.zip' % (n, tag))
                cck.without_records(z, zr, [j - 1 for j in js])
                Rf = ccc.Arm(exe, zr, game, work, n + '_' + tag + '_ref', fl + ['--crowd-members-probe'])
                os.remove(zr)
                near = M.differing(Rf) == 0 and len(M.hashes) == len(Rf.hashes)
            print('  %s: stripped %d records; holes %s, full frames refused %s, frames differing from the full arm %d%s' % (label, len(js), cm and cm['holes'], cm and cm['refused'], d, (', from the uncut capture with the same strip: %s' % ('none' if near else 'SOME')) if ref else ''))
            return cm, d, M, near
        cm, d, M, near = arm("honest: FULL frames (the game recorded every member)", fulls, "miss_full", True)
        R.line(cm is not None and cm['holes'] == 0 and near and M.valid == 0,
               '%s: a FULL frame ours has no record for keeps the stream\'s own members: holes %s, %d frames differ from the full arm (the stream is not ours), and the cut arm equals the UNCUT capture with the same strip on every frame: %s, validation %d' % (n, cm and cm['holes'], d, near, M.valid))
        cm, d, M, _ = arm('PLANT: THIN frames (members the hook dropped)', thin_ok, 'miss_thin')
        R.line(cm is not None and cm['holes'] >= len(thin_ok) and d >= len(thin_ok) and M.valid == 0,
               '%s: PLANT caught: a member cut on a frame ours falls back for is a HOLE: holes %s (>= the %d stripped frames) and the picture moves on %d frames (>= %d)' % (n, cm and cm['holes'], len(thin_ok), d, len(thin_ok)))
        for suffix in ('.zip', '.counts.csv', '.dec.csv'):
            try:
                os.remove(zt[:-4] + suffix)
            except OSError:
                pass
    print('\nmember_cut_check:', 'PASS' if not R.fails else '%d FAIL' % R.fails)
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())
