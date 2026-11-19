"""S103 KS1 lint: the props light read is behind --wwe2k14_rdr_props_map, the section id 65 is used once, and the reader reads the LIGHT-1 words.
Source-only, 0 game runs.  py -I tools/scene/ks1_lint.py   (exit 0 = ok; --plant NAME: exit 0 only when the lint FAILS: ungated / offthread / dup_id / light2_offsets)"""
import os, re, sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def text(rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    plant = sys.argv[sys.argv.index('--plant') + 1] if '--plant' in sys.argv else ''
    contrib, reader, rows = text('src/scene_live/inputs_contrib.h'), text('src/scene_live/props_light_read.h'), text('src/renderer_scene/props_light_rows.h')
    if plant == 'ungated':
        contrib = contrib.replace('rdr_scene::props_map::On()', 'true')
    if plant == 'offthread':
        contrib = contrib.replace('latch_thread && ', '')
    if plant == 'light2_offsets':
        reader = reader.replace('2304u', '1296u')
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += 0 if ok else 1

    m = re.search(r'if \(latch_thread && rdr_scene::props_map::On\(\)\)\s*\{[^\n]*\n\s*const scene_inputs::PropsWhy pw = scene_inputs::ReadPropsLight\(', contrib)
    check(bool(m), 'ReadPropsLight is called only inside `if (latch_thread && rdr_scene::props_map::On())` (inputs_contrib.h)')
    check(len(re.findall(r'ReadPropsLight\(', contrib)) == 1, 'exactly one call site of ReadPropsLight in the contributor')
    check('const uint32_t far_off = light2 ? 1296u : 2304u, near_off = light2 ? 1300u : 2308u;' in reader, 'the reader takes the light-1 words (module + 2304 / + 2308) unless the planted twin is on')
    check('kPropsRecOff = 1312' in reader, 'the light-1 record is module + 1312')
    ids = []
    for dp, _, fs in os.walk(os.path.join(root, 'src')):
        for f in fs:
            if f.endswith(('.h', '.cpp')):
                with open(os.path.join(dp, f), encoding='utf-8', errors='replace') as fh:
                    s = fh.read()
                ids += [(f, n) for n in re.findall(r'\bkSc\w+\s*=\s*65\b', s)]
    if plant == 'dup_id':
        ids.append(('x.h', 'kScDup = 65'))
    check(len(ids) == 1 and ids[0][0] == 'props_light_rows.h', 'section id 65 is defined once (%s)' % ', '.join('%s:%s' % i for i in ids))
    if plant:
        print('plant %s: %s' % (plant, 'caught' if bad else 'NOT caught'))
        return 0 if bad else 1
    return 1 if bad else 0


sys.exit(main())
