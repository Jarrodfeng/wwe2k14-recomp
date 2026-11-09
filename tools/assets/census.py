"""Session 96: the asset census - every arena and every wrestler attire through wwe2k14_assets.exe (our own viewer,
src\\assets\\view\\), shots from set camera angles, loader warnings and time per item, against the game census.

Answers: does every arena / wrestler load and draw from the player's own files, what does the loader warn about, and
how long does an item take (the game census, out\\census\\final 2026-11-05: 146 s/item mean, arenas 133 s, six-man
roster items 259 s = ~43 s a wrestler).

  py -I tools\\assets\\census.py --pass P [--only arena|wrestler] [--limit N] [--ids 5,107] [--exe PATH] [--game PATH]
  py -I tools\\assets\\census.py --pass P --report          (sheets + summary only, from results.json)

Outputs (gitignored, game pictures stay local): out\\assets\\census\\P\\ results.json, shots\\*.png, sheet_*.png, report.md.
Resumable: items already in results.json are skipped. The viewer is started once (hidden) and driven over its pipe;
if it dies or stops answering (60 s: a watchdog kills it, viewer_link.send), it is restarted and the item is recorded as failed.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from viewer_link import EXE, GAME, Viewer  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE = {'all': 146.0, 'arena': 133.0, 'wrestler': 259.0 / 6.0}  # s per item, out\census\final (2026-11-05)
MOTION = 'idle'
ANGLES = {'arena': ['ring', 'ring_side', 'ring_back'], 'wrestler': ['front', 'three_quarter', 'close']}


def run_item(v, kind, ident, attire, shots_dir):
    key = '%s_%d_%d' % (kind, ident, attire) if kind == 'wrestler' else '%s_%d' % (kind, ident)
    t0 = time.time()
    r = v.send({'cmd': 'load', 'kind': kind, 'id': ident, 'attire': attire})
    item = {'key': key, 'kind': kind, 'id': ident, 'attire': attire, 'load': r}
    if not r.get('ok', True) or r.get('error'):
        item['ok'] = False
        item['seconds'] = time.time() - t0
        return item
    item['shots'] = []
    if kind == 'wrestler' and MOTION:  # Session 97: wrestlers in the default idle (frame 30) unless --no-motion
        m = v.send({'cmd': 'motion', 'name': MOTION})
        v.send({'cmd': 'frame', 'f': 30})
        item['motion'] = m.get('ok', True) and not m.get('error')
    for a in ANGLES[kind]:
        v.send({'cmd': 'camera', 'preset': a})
        path = os.path.join(shots_dir, '%s_%s.png' % (key, a))
        s = v.send({'cmd': 'shot', 'path': path, 'w': 1280, 'h': 720})
        item['shots'].append({'angle': a, 'path': path, 'ok': s.get('ok', True) and os.path.exists(path)})
    item['ok'] = all(s['ok'] for s in item['shots'])
    item['seconds'] = time.time() - t0
    return item


def sheets(out_dir, results):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return []
    made = []
    for kind in ('arena', 'wrestler'):
        items = [r for r in results if r['kind'] == kind and r.get('shots')]
        per = 24
        for page in range(0, len(items), per):
            chunk = items[page:page + per]
            cols, tw, th = 6, 320, 180
            rows = (len(chunk) + cols - 1) // cols
            sheet = Image.new('RGB', (cols * tw, rows * (th + 16)), (24, 24, 24))
            d = ImageDraw.Draw(sheet)
            for i, it in enumerate(chunk):
                x, y = (i % cols) * tw, (i // cols) * (th + 16)
                shot = next((s['path'] for s in it['shots'] if s['ok']), None)
                if shot:
                    sheet.paste(Image.open(shot).convert('RGB').resize((tw, th)), (x, y + 16))
                warn = len(it['load'].get('warnings', []) or [])
                d.text((x + 4, y + 2), '%s  w%d  %.1fs' % (it['key'], warn, it['seconds']), fill=(230, 230, 230))
            p = os.path.join(out_dir, 'sheet_%s_%02d.png' % (kind, page // per))
            sheet.save(p)
            made.append(p)
    return made


def report(out_dir, results):
    lines = ['# Asset census %s' % os.path.basename(out_dir), '']
    for kind in ('arena', 'wrestler'):
        rs = [r for r in results if r['kind'] == kind]
        if not rs:
            continue
        ok = [r for r in rs if r.get('ok')]
        mean = sum(r['seconds'] for r in rs) / len(rs)
        lines.append('- **%s:** %d items, %d ok, %d failed; %.2f s/item (game census: %.0f s) = %.0fx faster; total %.0f s'
                     % (kind, len(rs), len(ok), len(rs) - len(ok), mean, BASELINE[kind], BASELINE[kind] / max(mean, 1e-6),
                        sum(r['seconds'] for r in rs)))
    if results:
        mean = sum(r['seconds'] for r in results) / len(results)
        lines.append('- **all:** %d items, %.2f s/item; total %.0f s (the game census: 143 items in 5.8 lane-hours)'
                     % (len(results), mean, sum(r['seconds'] for r in results)))
    warns = {}
    for r in results:
        for w in (r['load'].get('warnings') or []):
            k = w.split(':')[0][:80]
            warns[k] = warns.get(k, 0) + 1
    lines += ['', '## Most common loader warnings', '']
    lines += ['- %d x %s' % (n, k) for k, n in sorted(warns.items(), key=lambda kv: -kv[1])[:20]]
    lines += ['', '## Failed items', '']
    lines += ['- %s: %s' % (r['key'], r['load'].get('error') or [s for s in r.get('shots', []) if not s['ok']])
              for r in results if not r.get('ok')] or ['- none']
    with open(os.path.join(out_dir, 'report.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pass', dest='pas', required=True)
    ap.add_argument('--only', choices=['arena', 'wrestler'])
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--ids', default='')
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--max-attires', type=int, default=8)
    ap.add_argument('--no-motion', action='store_true', help='wrestlers in the bind (T) pose instead of the idle')
    a = ap.parse_args()
    global MOTION
    if a.no_motion:
        MOTION = ''
    out_dir = os.path.join(REPO, 'out', 'assets', 'census', a.pas)
    shots_dir = os.path.join(out_dir, 'shots')
    os.makedirs(shots_dir, exist_ok=True)
    res_path = os.path.join(out_dir, 'results.json')
    results = json.load(open(res_path)) if os.path.exists(res_path) else []
    done = {r['key'] for r in results}
    if not a.report:
        v = Viewer(a.exe, a.game, 'cen_' + a.pas)
        v.start()
        t_all = time.time()
        try:
            kinds = [a.only] if a.only else ['arena', 'wrestler']
            want = {int(x) for x in a.ids.split(',') if x.strip()}
            for kind in kinds:
                ids = v.send({'cmd': 'list', 'kind': kind}).get('ids', [])
                if want:
                    ids = [i for i in ids if i in want]
                if a.limit:
                    ids = ids[:a.limit]
                for ident in ids:
                    for attire in (range(a.max_attires) if kind == 'wrestler' else [0]):
                        key = '%s_%d_%d' % (kind, ident, attire) if kind == 'wrestler' else '%s_%d' % (kind, ident)
                        if key in done:
                            continue
                        try:
                            item = run_item(v, kind, ident, attire, shots_dir)
                        except Exception as e:  # viewer died / hung: record, restart, go on
                            item = {'key': key, 'kind': kind, 'id': ident, 'attire': attire, 'ok': False,
                                    'seconds': 0.0, 'load': {'error': 'viewer: %s' % e}}
                            v.start()
                        # an attire past the last one is the end of this wrestler, not a failure
                        err = str(item['load'].get('error') or '')
                        # (only the viewer's own "no_attire" answer ends a wrestler; any other load error is a failure)
                        if kind == 'wrestler' and attire > 0 and item['load'].get('no_attire'):
                            break
                        results.append(item)
                        done.add(key)
                        print('%-18s %s %6.2f s  meshes %s  warnings %d  %s' % (
                            key, 'ok ' if item.get('ok') else 'BAD', item['seconds'], item['load'].get('meshes'),
                            len(item['load'].get('warnings') or []), err[:80]), flush=True)
                        if len(results) % 10 == 0:
                            json.dump(results, open(res_path, 'w'), indent=1)
        finally:
            json.dump(results, open(res_path, 'w'), indent=1)
            v.stop()
        print('sweep wall time %.0f s' % (time.time() - t_all))
    for p in sheets(out_dir, results):
        print('sheet', p)
    print(report(out_dir, results))


if __name__ == '__main__':
    sys.exit(main())
