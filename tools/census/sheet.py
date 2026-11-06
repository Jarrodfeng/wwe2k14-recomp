"""Session 92 census: the morning sheet - what is on the new renderer, what is not, what looks broken.

What it answers: "after the sweep, which items are 100% TRUE NATIVE and glitch free, which hand back to the old path
(and with what signature), what crashed or hung, what did the detectors flag?" Reads out\\census\\<pass>\\results\\*.json.

  py tools\\census\\sheet.py --pass pass1
Writes out\\census\\<pass>\\sheet.md (plain top block for a phone, then one line per item), results.json (everything
merged, for diffing passes) and sheets\\<family>_NN.jpg (contact sheets, <= 24 labelled shots each).
Covered = verdict owned + status ok + no detector flag (the shot-checker's read is added later by the main session).
"""
import argparse
import glob
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import detect  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CEN = os.path.join(REPO, 'out', 'census')
FAMILIES = ('roster', 'arena', 'type', 'wm30', 'mode')
FAMNAME = {'roster': 'Roster six-mans', 'arena': 'Arenas', 'type': 'Match types', 'wm30': '30 Years',
           'mode': 'Modes and screens'}
RING = ('entrance', 'match')


def pct(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def font(size):
    for f in ('C:\\Windows\\Fonts\\arial.ttf', 'C:\\Windows\\Fonts\\segoeui.ttf'):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pass', dest='pass_name', required=True)
    a = ap.parse_args()
    pdir = os.path.join(CEN, a.pass_name)
    res = []
    for p in sorted(glob.glob(os.path.join(pdir, 'results', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                res.append(json.load(f))
        except (OSError, ValueError):
            pass
    try:
        with open(os.path.join(CEN, 'items.json'), encoding='utf-8') as f:
            all_items = json.load(f)
    except (OSError, ValueError):
        all_items = []
    ids_done = {r['id'] for r in res}
    # family statistics for the relative detectors
    fam_hf, fam_fl = {}, {}
    for r in res:
        for s in r.get('shots', []):
            m = s.get('metrics')
            if m and s.get('phase') in RING:
                fam_hf.setdefault(r['family'], []).append(m['hf'])
        for fl in r.get('flicker', []):
            if fl.get('diff', -1) >= 0:
                fam_fl.setdefault(r['family'], []).append(fl['diff'])
    stats = {f: {'hf_median': statistics.median(v)} for f, v in fam_hf.items() if v}
    flicker_thr = {}
    for f, v in fam_fl.items():
        med = statistics.median(v)
        flicker_thr[f] = max(pct(v, 95), detect.THRESH['flicker_ratio'] * med) if len(v) >= 10 else max(15.0, 4 * med)
    for r in res:
        r['flags'] = []
        for s in r.get('shots', []):
            if s.get('metrics'):
                s['flags'] = detect.flags(s['metrics'], s.get('phase'), stats.get(r['family']))
                for fl in s['flags']:
                    r['flags'].append({'flag': fl[0], 'value': fl[1], 'shot': s.get('thumb') or s['path'],
                                       'phase': s.get('phase')})
            else:
                s['flags'] = []
        for fl in r.get('flicker', []):
            if fl.get('diff', -1) > flicker_thr.get(r['family'], 1e9):
                r['flags'].append({'flag': 'flicker', 'value': fl['diff'], 'shot': fl['a'], 'phase': fl.get('phase')})
        r['covered'] = r.get('verdict') == 'owned' and r.get('status') == 'ok' and not r['flags']
    # ---- aggregates
    fam = {}
    for f in FAMILIES:
        rs = [r for r in res if r['family'] == f]
        planned = len([i for i in all_items if i['family'] == f]) + len([r for r in rs if r['id'].startswith('fu_')])
        fam[f] = {'planned': max(planned, len(rs)), 'ran': len(rs),
                  'covered': sum(r['covered'] for r in rs),
                  'owned': sum(r.get('verdict') == 'owned' for r in rs),
                  'handback': sum(r.get('verdict') == 'handback' for r in rs),
                  'nodata': sum(r.get('verdict') == 'nodata' for r in rs),
                  'flagged': sum(1 for r in rs if r['flags']),
                  'crash': sum(r.get('status') == 'crash' for r in rs),
                  'stuck': sum(r.get('status') == 'stuck' for r in rs),
                  'unbuildable': sum(r.get('status') == 'unbuildable' for r in rs),
                  'error': sum(r.get('status') == 'error' for r in rs)}
    sigs = {}
    for r in res:
        for s in r.get('signatures', []):
            d = sigs.setdefault(s['key'], {'key': s['key'], 'items': [], 'count': 0, 'phases': {}})
            d['items'].append(r['id'])
            d['count'] += s['count']
            for ph, n in s['phases'].items():
                d['phases'][ph] = d['phases'].get(ph, 0) + n
    siglist = sorted(sigs.values(), key=lambda d: (-len(d['items']), -d['count']))
    total = sum(v['planned'] for v in fam.values())
    covered = sum(v['covered'] for v in fam.values())
    ran = len(res)
    L = []
    w = L.append
    w('# TRUE NATIVE census - %s' % a.pass_name)
    w('Written %s. Items run: %d of %d planned.' % (time.strftime('%Y-%m-%d %H:%M'), ran, total))
    w('')
    w('## The short answer')
    w('**%d of %d items are fully on the new renderer and look clean** (covered: nothing handed back to the old path, '
      'no lost clears, no crash, no flagged picture).' % (covered, total))
    bad = [r for r in res if r.get('status') in ('crash', 'stuck', 'error')]
    w('%d crashed or hung or failed to run, %d could not be built, %d handed passes back to the old path, '
      '%d have picture flags to look at, %d have no usable data.' % (
          len(bad), sum(v['unbuildable'] for v in fam.values()), sum(v['handback'] for v in fam.values()),
          sum(v['flagged'] for v in fam.values()), sum(v['nodata'] for v in fam.values())))
    w('')
    w('## Per family (covered / planned)')
    for f in FAMILIES:
        v = fam[f]
        w('- **%s**: %d / %d covered. Ran %d; %d owned, %d handed back, %d crash, %d hung, %d unbuildable, %d error, '
          '%d flagged pictures, %d no data.' % (FAMNAME[f], v['covered'], v['planned'], v['ran'], v['owned'],
                                                v['handback'], v['crash'], v['stuck'], v['unbuildable'], v['error'],
                                                v['flagged'], v['nodata']))
    w('')
    w('## What still goes to the old path (by how many items show it)')
    if not siglist:
        w('Nothing handed back in the items that ran.')
    for d in siglist[:25]:
        ph = ', '.join('%s %d' % kv for kv in sorted(d['phases'].items()))
        its = ', '.join(d['items'][:6]) + (' +%d more' % (len(d['items']) - 6) if len(d['items']) > 6 else '')
        w('- %d items, %d passes (%s): %s  [%s]' % (len(d['items']), d['count'], ph, d['key'], its))
    if len(siglist) > 25:
        w('- ... %d more signatures in results.json' % (len(siglist) - 25))
    w('(The game logs only the top few signatures per 600-frame window, so small ones can be missing.)')
    w('')
    w('## Lost clears')
    lost = [r for r in res if r.get('totals', {}).get('lost_clears')]
    if not lost:
        w('None in the counted windows.')
    for r in lost:
        w('- %s: %d (frames %s)' % (r['id'], r['totals']['lost_clears'], r.get('lost_clear_frames', [])[:5]))
    w('')
    # Session 92 M0: what of the 360 machinery still runs (SDK free_counters.h; every window of every item, not only
    # the counted phases). Each "free from the 360" milestone drives one of these to 0.
    fr = [r for r in res if r.get('free')]
    w('## The 360 machinery still running (M0 counters, all windows)')
    if not fr:
        w('No item has the counters (an SDK before M0).')
    else:
        keys = ('truth_entries', 'translator_runs', 'packets', 'register_writes', 'wait_reg_mem', 'answers_walks',
                'clears_library_way', 'direct_waits', 'resolves_regs', 'cache_resolves', 'presents_packet',
                'swap_fetch_regs', 'fetch0_stale')
        tot = {k: sum(r['free'].get(k, 0) for r in fr) for k in keys}
        wins = sum(r['free'].get('windows', 0) for r in fr) or 1
        w('%d items, %d windows. Per 600-frame window on average: %s.' % (
            len(fr), wins, ', '.join('%s %d' % (k.replace('_', ' '), tot[k] // wins) for k in keys)))
        shaders = []
        for r in fr:
            for s in r['free'].get('new_shaders', []):
                if s not in shaders:
                    shaders.append(s)
        w('Guest shaders the 360 translator still translated (first seen per run, union over items): %d - %s' % (
            len(shaders), ', '.join(shaders[:60]) + (' ...' if len(shaders) > 60 else '')))
    w('')
    # Session 92 M4f: the typed front end's share of the token draws per item, and why the rest went to the old path.
    ty = [r for r in fr if r['free'].get('typed_offered')]
    w('## Typed front end (M4f: token draws drawn from the typed state)')
    if not ty:
        w('No item printed the M4f-A line (latch without bit 32, or an SDK before M4f-A).')
    else:
        reasons = {}
        for r in ty:
            off, drawn = r['free']['typed_offered'], r['free']['typed_drawn']
            w('- %s: %d of %d typed (%.1f%%)' % (r['id'], drawn, off, 100.0 * drawn / max(off, 1)))
            for k, v in r['free'].get('typed_reasons', {}).items():
                reasons[k] = reasons.get(k, 0) + v
        w('Handed to the old path, all items: %s' % (
            ', '.join('%s %d' % (k, v) for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])) or 'none'))
        bkeys = ('crowd_members', 'crowd_declined', 'inline_typed', 'inline_old', 'cover_rects', 'bounds_rects', 'memexport_typed')
        if any(r['free'].get(k) for r in ty for k in bkeys):
            w('M4f-B, all items: %s' % ', '.join('%s %d' % (k.replace('_', ' '), sum(r['free'].get(k, 0) for r in ty)) for k in bkeys))
    pe = [r for r in fr if r['free'].get('pend_windows')]
    if pe:
        pkeys = ('pend_made', 'pend_consumed_start', 'pend_depth_given', 'pend_copy_filled', 'pend_superseded', 'pend_unconsumed',
                 'pend_unservable', 'pend_retried', 'pend_refusals', 'pend_unpendable')
        w('Pending clears (M4f-B), all items: %s' % ', '.join('%s %d' % (k[5:].replace('_', ' '), sum(r['free'].get(k, 0) for r in pe))
                                                             for k in pkeys))
    w('')
    # Session 92 M4f-C: the cut's proof per item (every window after the latch frame's; the latch frame's window = boot, listed apart).
    cu = [r for r in fr if r['free'].get('cut_windows')]
    w('## The cut (M4f-C: the decoder executes; nothing reaches the old path)')
    if not cu:
        w('No item printed the [d3d][cut] line (the cut not latched, or an SDK before M4f-C).')
    else:
        def nz(d):
            return {k: v for k, v in (d or {}).items() if v}
        bad = []
        for r in cu:
            f = r['free']
            probs = []
            for k in ('cut_records_as_packets', 'cut_segments_after', 'cut_poison_rewritten', 'cut_entry_failed'):
                if f.get(k):
                    probs.append('%s %d' % (k[4:].replace('_', ' '), f[k]))
            for k in ('cut_drops', 'cut_refused', 'cut_readers'):
                if nz(f.get(k)):
                    probs.append('%s: %s' % (k[4:], ', '.join('%s %d' % kv for kv in nz(f[k]).items())[:200]))
            if f.get('cut_armed') != 'yes':
                probs.append('detector 2 armed %s' % f.get('cut_armed'))
            if probs:
                bad.append((r['id'], probs))
        w('%d items; after the latch frame: %d with a finding.' % (len(cu), len(bad)))
        for i, probs in bad:
            w('- %s: %s' % (i, '; '.join(probs)))
        boot = {}
        for r in cu:
            for k, v in (r['free'].get('cut_boot') or {}).items():
                if isinstance(v, int) and v:
                    boot[k] = boot.get(k, 0) + v
        w('Boot windows (the latch frame), all items: %s' % (', '.join('%s %d' % (k[4:].replace('_', ' '), v) for k, v in sorted(boot.items()))
                                                           or 'nothing counted'))
        skips, mmio, bases = {}, {}, {}
        for r in cu:
            for src, dst in (('cut_skips', skips), ('mmio', mmio), ('cut_type0_bases', bases)):
                for k, v in (r['free'].get(src) or {}).items():
                    dst[k] = dst.get(k, 0) + v
        w('Skips by a setting (not drops), all items: %s' % (', '.join('%s %d' % kv for kv in sorted(skips.items(), key=lambda kv: -kv[1]))
                                                            or 'none'))
        w('Guest MMIO reads by register, all items: %s' % (', '.join('%s %d' % kv for kv in sorted(mmio.items(), key=lambda kv: -kv[1]))
                                                           or 'none'))
        if bases:
            w('Segment type-0 base registers after the latch, all items: %s' % ', '.join('%s %d' % kv for kv in sorted(bases.items())))
    w('')
    # Session 92: the command thread's cost per scene (FAST-FORWARDED: relative weight between scenes only, not fps).
    w('## CPU per scene (fast-forwarded: relative only)')
    cs = [r for r in fr if r['free'].get('cut_us') or r['free'].get('typed_us')]
    if not cs:
        w('No item printed the executor / typed-draw cost.')
    else:
        rows = []
        for r in cs:
            f = r['free']
            ex = sum(f.get('cut_us') or [0]) / max(1, len(f.get('cut_us') or []))
            td = sum(f.get('typed_us') or [0]) / max(1, len(f.get('typed_us') or []))
            rows.append((ex + td, r['id'], ex, td))
        rows.sort(reverse=True)
        w('Heaviest first (us a frame, window average): ' + '; '.join('%s executor %.0f + typed draw %.0f' % (i, ex, td)
                                                                      for _, i, ex, td in rows[:25]))
    w('')
    # Session 92: code the checks rarely reach - which items exercised it.
    w('## Rare code coverage')
    def items_with(pred):
        return [r['id'] for r in fr if pred(r['free'])]
    w('- memexport draws typed: %s' % (', '.join(items_with(lambda f: f.get('memexport_typed'))) or 'none'))
    w('- inline 3-vertex rectangle lists drawn as draws: %s' % (', '.join(items_with(
        lambda f: any('3-vertex' in k for k in (f.get('cut_skips') or {})))) or 'none'))
    w('- crowd batch refusals by reason: %s' % (', '.join('%s %d' % kv for kv in sorted(
        {k: sum((r['free'].get('crowd_refused') or {}).get(k, 0) for r in fr) for r in fr
         for k in (r['free'].get('crowd_refused') or {})}.items())) or 'none'))
    w('- draws skipped while their pipeline compiled (owned-only frames): %s' % (', '.join('%s %d' % (r['id'], r['free']['nopipe_owned'])
                                                                                for r in fr if r['free'].get('nopipe_owned')) or 'none'))
    w('- pending clears consumed by a copy fill: %s' % (', '.join(items_with(lambda f: f.get('pend_copy_filled'))) or 'none'))
    m6 = [r for r in fr if r['free'].get('m6_variants')]
    if m6:
        w('M6: translation variant lines in %d items (%d lines; the union: py -I tools\\shaders\\m6_variants.py <the runs\' logs>)' % (
            len(m6), sum(r['free']['m6_variants'] for r in m6)))
    w('')
    # Session 92: feature_proof.py per item (tools/harness/feature_proofs.toml): a switch the game was started with whose
    # proof line never printed did not engage - that item's verdict is not about the feature it was meant to test.
    w('## Features not engaged')
    fne = [r for r in res if (r.get('features') or {}).get('not_engaged')]
    ferr = [r for r in res if (r.get('features') or {}).get('error')]
    nof = [r for r in res if not r.get('features')]
    if not fne:
        w('None.' + (' (%d items have no proof data: an older result)' % len(nof) if nof else ''))
    for r in fne:
        ft = r['features']
        w('- %s (run %s): %s' % (r['id'], r.get('run'), '; '.join(
            '%s - %s' % (n, ft.get('details', {}).get(n, '')) for n in ft['not_engaged'])[:400]))
    for r in ferr:
        w('- %s: the proof check itself failed: %s' % (r['id'], r['features']['error']))
    w('')
    w('## Crashes, hangs, failures to run')
    if not bad and not any(r.get('status') == 'unbuildable' for r in res):
        w('None.')
    for r in res:
        if r.get('status') in ('crash', 'stuck', 'error', 'unbuildable'):
            w('- %s: %s - %s (run %s)' % (r['id'], r['status'].upper(), (r.get('note') or '')[:200].replace('\n', ' '),
                                         r.get('run')))
    w('')
    w('## Picture flags (detectors; thresholds are first guesses)')
    fl_items = [r for r in res if r['flags']]
    if not fl_items:
        w('None.')
    for r in fl_items:
        kinds = {}
        for fl in r['flags']:
            kinds[fl['flag']] = kinds.get(fl['flag'], 0) + 1
        w('- %s: %s (e.g. %s)' % (r['id'], ', '.join('%s x%d' % kv for kv in kinds.items()), r['flags'][0]['shot']))
    w('')
    w('## Entrances (roster six-mans)')
    ne = [(r['id'], r.get('no_entrance')) for r in res if r.get('no_entrance')]
    if not ne:
        w('Every wrestler that ran got an entrance event, or no six-man ran.')
    for i, wr in ne:
        w('- %s: no entrance event for wrestler id(s) %s (a follow-up 1v1 fu_<id> is queued/run)' % (i, wr))
    w('')
    w('## Detector distributions (to tune the thresholds)')
    for f in FAMILIES:
        v = fam_hf.get(f)
        if v:
            w('- %s ring shots, high-frequency energy: n %d, median %.2f, p95 %.2f, max %.2f (noise flag > %.1f x median)'
              % (f, len(v), statistics.median(v), pct(v, 95), max(v), detect.THRESH['noise_ratio']))
        v = fam_fl.get(f)
        if v:
            w('- %s flicker pair differences: n %d, median %.2f, p95 %.2f, max %.2f, flag above %.2f' % (
                f, len(v), statistics.median(v), pct(v, 95), max(v), flicker_thr[f]))
    w('')
    w('## Every item')
    for f in FAMILIES:
        rs = [r for r in res if r['family'] == f]
        if not rs:
            continue
        w('')
        w('### %s' % FAMNAME[f])
        done_ids = {r['id'] for r in rs}
        for r in rs:
            t = r.get('totals', {})
            tag = 'COVERED' if r['covered'] else r.get('verdict', '?').upper()
            nm = ', '.join(r['names']) if r.get('names') else (r.get('spec') or '')
            fl = (' FLAGS ' + ','.join(sorted({x['flag'] for x in r['flags']}))) if r['flags'] else ''
            w('- `%s` **%s** %s | windows %s passes %s fallback %s lost %s nopipe %s | %d shots | run %s | %ss%s%s' % (
                r['id'], tag, nm[:110], t.get('windows'), t.get('passes'), t.get('fallback'), t.get('lost_clears'),
                t.get('no_pipeline'), len(r.get('shots', [])), r.get('run'), r.get('elapsed_s'), fl,
                (' | ' + r['note'][:100].replace('\n', ' ')) if r.get('note') else ''))
        nr = [i['id'] for i in all_items if i['family'] == f and i['id'] not in done_ids]
        if nr:
            w('- not run (%d): %s' % (len(nr), ', '.join(nr)))
    # ---- contact sheets
    sdir = os.path.join(pdir, 'sheets')
    os.makedirs(sdir, exist_ok=True)
    for old in glob.glob(os.path.join(sdir, '*.jpg')):
        os.remove(old)
    made = []
    F = font(13)
    for f in FAMILIES:
        rs = [r for r in res if r['family'] == f]
        chunks, cur = [], []
        for r in rs:
            sh = [s for s in r.get('shots', []) if s.get('thumb') and os.path.exists(os.path.join(CEN, s['thumb']))]
            if not sh:
                continue
            bad_sh = [s for s in sh if s.get('flags')]
            if f == 'mode':
                pick = sh[:24]
            else:
                n = 6 if f == 'type' else 4
                idx = sorted({round(k * (len(sh) - 1) / max(n - 1, 1)) for k in range(n)})
                pick = [sh[i] for i in idx]
                for s in bad_sh[:2]:
                    if s not in pick:
                        pick.append(s)
            if len(cur) + len(pick) > 24 and cur:
                chunks.append(cur)
                cur = []
            cur += [(r, s) for s in pick]
        if cur:
            chunks.append(cur)
        for ci, ch in enumerate(chunks):
            cols, tw, th = 6, 320, 180
            rows = (len(ch) + cols - 1) // cols
            sheet = Image.new('RGB', (cols * tw, rows * th), (20, 20, 20))
            d = ImageDraw.Draw(sheet)
            for k, (r, s) in enumerate(ch):
                try:
                    im = Image.open(os.path.join(CEN, s['thumb'])).convert('RGB').resize((tw, th), Image.LANCZOS)
                except OSError:
                    continue
                x, y = (k % cols) * tw, (k // cols) * th
                sheet.paste(im, (x, y))
                cap = '%s %s%s' % (r['id'], str(s.get('label', ''))[:22], (' !' + ','.join(x[0] for x in s['flags'])) if s.get('flags') else '')
                d.rectangle([x, y, x + tw, y + 16], fill=(0, 0, 0))
                d.text((x + 3, y + 1), cap, fill=(255, 235, 80) if not s.get('flags') else (255, 90, 90), font=F)
            out = os.path.join(sdir, '%s_%02d.jpg' % (f, ci))
            sheet.save(out, 'JPEG', quality=82)
            made.append(out)
    w('')
    w('## Contact sheets')
    for p in made:
        w('- %s' % os.path.relpath(p, REPO))
    with open(os.path.join(pdir, 'sheet.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    merged = {'pass': a.pass_name, 'written': time.strftime('%Y-%m-%d %H:%M:%S'), 'families': fam,
              'signatures': siglist, 'thresholds': detect.THRESH, 'flicker_thresholds': flicker_thr,
              'hf_medians': stats, 'items': res}
    tmp = os.path.join(pdir, 'results.json.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(merged, f, indent=1)
    os.replace(tmp, os.path.join(pdir, 'results.json'))
    print('\n'.join(L[:40]))
    print('... sheet: %s, %d contact sheets' % (os.path.join(pdir, 'sheet.md'), len(made)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
