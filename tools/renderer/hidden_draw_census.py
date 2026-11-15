r"""S102 lane R24-D part 2: the census of the frame class's HIDDEN draws (draws that write 0 pixels), offline, 0 game runs, 0 Surface runs.
  py -I tools\renderer\hidden_draw_census.py [--root DIR] [--only NAME] [--trace-only]
For each capture (the gate's Bryan, lane86 Bryan / Brock, lane86 wm30 = a match) rplay replays with `--frame-trace` (the tool-only per-draw precise occlusion query, FRAME_CLASS.md): every draw of every
frame-class pass has the pixels it wrote. Draws are grouped by a stable identity (draw class, technique, pass site, pass ordinal, program, vertices, blend, depth / stencil flags, viewport, scissor,
bound textures). Per group: occurrences, occurrences that wrote 0 px, and a CPU-SIDE PROOF CLASS from the draw's own state (never from "it was 0 in the capture"):
  P1  no_target_write  every colour write mask is 0 AND no depth write AND no stencil test: the draw writes no attachment whatever the shader does (the proof used by `drop_hidden`).
  P2  empty_area       the scissor or the viewport has no area (area 0): no fragment can exist.
  P3  alpha_zero       (reported only) alpha blend whose output alpha could be 0: needs the constants product; NOT a state fact, never a drop.
  none                nothing in the state says it must write 0 px: the 0 px is content (depth fail / off-screen by the vertex data / alpha) and a drop would be a guess.
Output: a table per capture and a summary with the groups that were 0 px on EVERY occurrence and which proof (if any) covers each. Counts and names only (hard rule 1)."""
import argparse, collections, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import frame_class_census as fcc  # read_trace, CLASS_NAMES
import pcap  # noqa: E402

CAPTURES = {
    'bryan92': ('out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237),
    'bryan86': ('out/lane86/bryan/capture', 89),
    'brock86': ('out/lane86/brock/capture', 341),
    'wm30': ('out/lane86/wm30/capture', 423),
}


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def proof_class(d, pass_w, pass_h):
    """The proof the draw's own state gives that it writes nothing (see the header). d = a trace 'd' record."""
    wm = d['blend'][7]
    if wm == 0 and d['dw'] == 0 and d['st'] == 0:
        return 'P1'
    sc = d['sc']
    vp = d['vp']
    if d['area'] == 0 or sc[2] == 0 or sc[3] == 0 or vp[2] == 0 or vp[3] == 0:
        return 'P2'
    return 'none'


def census(capture_dir, trace, frames):
    tr = fcc.read_trace(trace)
    fs = sorted(tr)
    cap = pcap.Capture(capture_dir)
    nf = min(len(fs), len(cap.frames), frames)
    groups = collections.OrderedDict()
    for k in range(nf):
        f = fs[k]
        t = tr[f]
        cf = cap.frame(k)
        secs = cf.secs
        strings = b''
        if 67 in secs:
            es, cnt, off = secs[67]
            strings = bytes(cf.buf[off:off + cnt * es])

        def string(o):
            if not o:
                return ''
            j = strings.find(b'\0', o - 1)
            return strings[o - 1:j if j >= 0 else len(strings)].decode('utf-8', 'replace')
        ops = cf.ops
        pmeta = {pr['p']: pr for pr in t['p']}
        order = {p: i for i, p in enumerate(sorted(pmeta))}
        for (p, op), d in sorted(t['d'].items()):
            if op >= len(ops) or int(ops['kind'][op]) != 2:
                continue
            oi = int(ops['index'][op])
            if cf.has_origins and oi < len(cf.origins):
                o = cf.origins[oi]
                kl, tech, site = fcc.CLASS_NAMES[int(o['klass'])], string(int(o['technique'])) or '-', int(o['pass_site'])
            else:
                kl, tech, site = 'no_origin', '-', 0
            key = (kl, tech, site, order.get(p, -1), d['prog'], d['verts'], tuple(d['blend']), d['dt'], d['dw'], d['st'], tuple(d['vp']), tuple(d['sc']), d['tex'])
            g = groups.setdefault(key, dict(n=0, zero=0, frames=set(), proof=proof_class(d, pmeta[p]['w'], pmeta[p]['h']), area=d['area']))
            g['n'] += 1
            px = t['o'].get((p, op), 0)
            g['zero'] += px == 0
            g['frames'].add(k)
    return nf, groups


def selftest():
    """The proof classes recognise what they claim, and refuse what they must (planted twins)."""
    base = dict(blend=[1, 0, 0, 0, 0, 0, 0, 15], dw=1, st=0, area=921600, sc=[0, 0, 1280, 720], vp=[0.0, 0.0, 1280.0, 720.0])
    cases = [('plain draw', dict(base), 'none'),
             ('mask 0, no depth write, no stencil', dict(base, blend=[1, 0, 0, 0, 0, 0, 0, 0], dw=0), 'P1'),
             ('mask 0 but depth write (writes depth)', dict(base, blend=[1, 0, 0, 0, 0, 0, 0, 0], dw=1), 'none'),
             ('mask 0, no depth write, stencil on (may write stencil)', dict(base, blend=[1, 0, 0, 0, 0, 0, 0, 0], dw=0, st=1), 'none'),
             ('empty scissor', dict(base, sc=[0, 0, 0, 720], area=0), 'P2'),
             ('depth tested, 0 px in the capture only', dict(base, dw=0), 'none')]
    bad = [n for n, d, want in cases if proof_class(d, 1280, 720) != want]
    print(('FAIL selftest: %s' % bad) if bad else 'PASS selftest: %d proof-class cases (a draw is proven only by its state, never by a 0 px capture)' % len(cases))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--only')
    ap.add_argument('--json')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    main_co = main_checkout(a.root)
    outdir = os.path.join(a.root, 'out', 's102', 'r24d_census')
    os.makedirs(outdir, exist_ok=True)
    rplay = os.path.join(a.root, 'out', 'renderer', 'dev', 'rplay.exe')
    summary = {}
    for name, (sub, frames) in CAPTURES.items():
        if a.only and a.only != name:
            continue
        capture = os.path.join(main_co, *sub.split('/'))
        if not os.path.isfile(os.path.join(capture, 'images.bin')):
            print('FAIL cannot read the capture', capture)
            return 1
        trace = os.path.join(outdir, name + '.jsonl')
        if not os.path.exists(trace):
            p = subprocess.run([rplay, '--capture', capture, '--frames', str(frames), '--frame-trace', trace, '--game', os.path.join(main_co, 'game_data', 'extracted')], capture_output=True, text=True, timeout=1800)
            if p.returncode != 0 or not os.path.exists(trace):
                print('FAIL rplay --frame-trace %s: exit %d\n%s' % (name, p.returncode, (p.stdout + p.stderr)[-600:]))
                return 1
        nf, groups = census(capture, trace, frames)
        always = {k: g for k, g in groups.items() if g['zero'] == g['n']}
        print('== %s: %d frames, %d draw groups, %d groups 0 px on EVERY occurrence (%d draws a frame)' % (name, nf, len(groups), len(always), sum(g['n'] for g in always.values()) / max(1, nf)))
        by = collections.Counter()
        for k, g in sorted(always.items(), key=lambda kv: (kv[0][0], kv[0][1])):
            kl, tech, site, po, prog, verts, blend, dt, dw, st, vp, sc, tex = k
            print('   %-10s %-24s site 0x%08X pass#%d prog %-3d verts %-5d blend %s dt%d dw%d st%d sc %s: %d draws, %d frames, proof %s' % (kl, tech[:24], site, po, prog, verts, ''.join(map(str, blend[:1])) + '/' + str(blend[7]), dt, dw, st, list(sc), g['n'], len(g['frames']), g['proof']))
            by[g['proof']] += g['n']
        print('   0 px on every occurrence, by proof class (draw occurrences): %s' % dict(by))
        sometimes = sum(1 for g in groups.values() if 0 < g['zero'] < g['n'])
        print('   groups 0 px on SOME occurrences only: %d' % sometimes)
        summary[name] = dict(frames=nf, groups=len(groups), always_zero_groups=len(always), always_zero_per_frame=sum(g['n'] for g in always.values()) / max(1, nf), by_proof=dict(by),
                             any_proven=any(g['proof'] != 'none' for g in always.values()))
    proven = [n for n, v in summary.items() if v['any_proven']]
    print('CENSUS: %d capture(s); captures with an always-0-px draw group that has a CPU-side proof: %s (none = no drop is built)' % (len(summary), proven or 'none'))
    if a.json:
        json.dump(summary, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
