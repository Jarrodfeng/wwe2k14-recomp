"""S111 SHOTDIET: the capture readers find and read .webp (the default screenshot format now) next to .png / .bmp. Offline: PIL writes the pictures (lossless WebP).

  py -I tools\\shot\\captures_webp_selftest.py          all cases, exit 0 = pass
  py -I tools\\shot\\captures_webp_selftest.py --tools DIR   read the tools from DIR (an export of the tools\\ folder before this lane: the planted twin, every webp line must FAIL)
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = sys.argv[sys.argv.index('--tools') + 1] if '--tools' in sys.argv else os.path.dirname(HERE)
fails = []


def check(name, ok):
    print('%s %s' % ('ok  ' if ok else 'FAIL', name))
    if not ok:
        fails.append(name)


def load(rel):
    p = os.path.join(TOOLS, *rel.split('/'))
    sys.path.insert(0, os.path.dirname(p))
    spec = importlib.util.spec_from_file_location('m_' + os.path.basename(p)[:-3], p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


from PIL import Image  # noqa: E402

with tempfile.TemporaryDirectory() as d:
    cap = os.path.join(d, 'captures')
    logs = os.path.join(d, 'logs')
    os.makedirs(cap)
    os.makedirs(logs)
    im = Image.new('RGB', (64, 36), (10, 120, 200))
    for lab in ('a', 'b'):
        im.save(os.path.join(cap, '042_%s.webp' % lab), lossless=True)
    im.save(os.path.join(cap, '042_c.png'))
    im.save(os.path.join(cap, '043_a.webp'), lossless=True)
    # a truncated WebP (a copy cut short) and a whole one
    whole = open(os.path.join(cap, '042_a.webp'), 'rb').read()
    open(os.path.join(cap, '042_cut.webp'), 'wb').write(whole[:len(whole) // 2])
    # lossless: the pixels read back equal what was written
    check('webp is lossless (PIL round trip)', list(Image.open(os.path.join(cap, '042_a.webp')).convert('RGB').getdata()) == list(im.getdata()))

    sd = load('bench/shot_diff.py')
    got = sd.captures_of(cap, '042')
    check('shot_diff.captures_of finds .webp and .png', {'a', 'b', 'c', 'cut'} <= set(got) and got['a'].endswith('.webp'))

    rg = load('regress/regress.py')
    check('regress.shot_path prefers .webp', rg.shot_path(cap, '042', 'a').endswith('042_a.webp'))
    check('regress.bmp_problem: a whole WebP is complete', rg.bmp_problem(os.path.join(cap, '042_a.webp')) is None)
    check('regress.bmp_problem: a cut WebP is refused', (rg.bmp_problem(os.path.join(cap, '042_cut.webp')) or '').startswith('truncated WebP'))

    sr = load('bench/shot_refs.py')
    check('shot_refs.capture_path prefers .webp, falls back to .png', sr.capture_path(cap, '042', 'a').endswith('.webp') and sr.capture_path(cap, '042', 'c').endswith('.png'))

    ro = load('check/readout.py')
    m = ro.SHOT_RE.search('[wwe2k14][input] screenshot saved: C:\\x\\captures\\042_a.webp (64x36) guest frame 5')
    check('check.readout.SHOT_RE reads a .webp line', bool(m) and m.group(1).endswith('042_a.webp'))

    rr = load('bench/run_read.py')
    found = rr.shots(logs, '042')[1]
    check('run_read.shots lists the .webp pictures', {n for n, _ in found} >= {'042_a.webp', '042_b.webp'})

    pp = load('native/parity_pair.py')
    check('parity_pair.shots maps .webp labels', {'a', 'b'} <= set(pp.shots(cap, '042', '')))

    # det_compare's pairing is inline in main(); its rule is the same listing: label = the name without its extension
    labels = {fn[len('042_'):fn.rindex('.')] for fn in os.listdir(cap) if fn.startswith('042_') and fn.endswith(('.bmp', '.png', '.webp'))}
    check('det_compare-style listing keeps the full label of a .webp', 'a' in labels and 'a.webp' not in labels)

    # S111 SHOTDIET review: the live readers the lane missed (owned_check -Poison, the sweeps, the census, the tron sheet). 043 holds only a .webp, so a reader that globs *.png finds nothing.
    ms = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'native', 'magenta_scan.py'), '--dir', d, '043'], capture_output=True, text=True)
    check('magenta_scan (owned_check -Poison) reads a .webp-only run', ms.returncode == 0 and '043_a.webp' in ms.stdout)
    sw = load('sweep/results.py')
    m = sw.RX_SHOT.search('[wwe2k14][input] screenshot saved: C:\\x\\captures\\042_a.webp (64x36) guest frame 5')
    check('sweep.results.RX_SHOT reads a .webp line', bool(m) and m.group(1).endswith('042_a.webp'))
    six = load('sweep/six_results.py')
    six.CAPS = Path(cap)
    check('sweep.six_results._shot prefers .webp', str(six._shot('042_a')).endswith('042_a.webp') and str(six._shot('042_c')).endswith('042_c.png'))
    cr = open(os.path.join(TOOLS, 'census', 'run.py'), encoding='utf-8').read()
    cc = open(os.path.join(TOOLS, 'census', 'common.py'), encoding='utf-8').read()
    check('census: run.py globs .webp shots, common.py names the shot .webp', "_*.webp'" in cr and "'%03d_%s.webp'" in cc)
    ts = open(os.path.join(TOOLS, 'trons', 'sheet.py'), encoding='utf-8').read()
    check('trons.sheet lists .webp', "('webp', 'png', 'bmp')" in ts)
    sb = open(os.path.join(TOOLS, 'surface', 'bench.ps1'), encoding='utf-8').read()
    check('surface.bench copies .webp route shots', "'.webp', '.png', '.jpg'" in sb)
    td = load('harness/tidy.py')
    check('tidy counts a .webp as an image', td.type_of('042_a.webp') == 'images')

print('PASS' if not fails else 'FAIL (%d)' % len(fails))
sys.exit(1 if fails else 0)
