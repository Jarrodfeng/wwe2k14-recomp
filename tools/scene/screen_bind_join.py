#!/usr/bin/env python3
"""S103 lane M: joins the log families a mode-1 game-exe run prints (screen_planes_glue.h) and says which stage-screens model word equals which movie's handle word, which display set (size,
channels, titan slots) that is, and what the sets are.
  py -I tools\\scene\\screen_bind_join.py LOG [--selftest]
Reads  '[scene][screens] flush #N of display set 0xSET (guest frame F): channels [a,b] count C Y WxH chroma WxH display_index I handles [16 words] slots [...] tchan [...] movie 0xM words [...]'
and    '[scene][screens] stage-screens model 0xM part P (guest frame F, draw D[, first non-zero]): table 0xT count N entries [h:aux ...] behind [16 words] words [208 words]'.
A set's handle words are set+64*buf+4*w (w = 0..7: Y pair, Cb pair, Cr pair, A pair). ANY word of a model line (its object words, its table entries, the 16 words behind the table pointer) that equals ANY
non-zero handle word of a set is a match: printed with where it sits (word offset in the model, or table entry), the set, buffer, plane. Exit 0 = at least one match, 1 = none; the per-set and per-model
summaries are always printed (sizes, channels seen at any flush, the last 'slots' / 'tchan' text, whether any model table was ever non-zero)."""
import re
import sys

SET = re.compile(r'flush #(\d+) of display set 0x([0-9A-Fa-f]+) \(guest frame (\d+)\): channels \[([^\]]*)\] count (\d+) Y (\d+)x(\d+) chroma (\d+)x(\d+) display_index (\d+).*?handles \[([^\]]*)\] slots \[([^\]]*)\] tchan \[([^\]]*)\] movie 0x([0-9A-Fa-f]+) words \[([^\]]*)\]')
MODEL = re.compile(r'stage-screens model 0x([0-9A-Fa-f]+) part (\d+) \(guest frame (\d+), draw (\d+)[^)]*\): table 0x([0-9A-Fa-f]+) count (\d+) entries \[([^\]]*)\] behind \[([^\]]*)\] words \[([^\]]*)\]')
PLANES = ['Y.id', 'Y.aux', 'Cb.id', 'Cb.aux', 'Cr.id', 'Cr.aux', 'A.id', 'A.aux']


def join(lines):
    sets, handles = {}, {}
    for ln in lines:
        m = SET.search(ln)
        if not m:
            continue
        s = m.group(2).upper()
        d = sets.setdefault(s, {'channels': set(), 'size': '%sx%s' % (m.group(6), m.group(7)), 'chroma': '%sx%s' % (m.group(8), m.group(9)), 'slots': '', 'tchan': '', 'first': int(m.group(3)), 'movie': m.group(14).upper(), 'mwords': [], 'sizes': []})
        sz = '%sx%s@%d' % (m.group(6), m.group(7), int(m.group(3)))
        if not d['sizes'] or d['sizes'][-1].split('@')[0] != sz.split('@')[0]: d['sizes'].append(sz)   # every CHANGE is kept (A -> B -> A too)
        for c in m.group(4).split(','):
            if c.strip() and c.strip() != 'none':
                d['channels'].add(c.strip())
        d['slots'], d['tchan'] = m.group(12).strip(), m.group(13).strip()
        if m.group(14).strip('0'):
            d['movie'], d['mwords'] = m.group(14).upper(), m.group(15).split()
        for i, w in enumerate(m.group(11).split()):
            w = w.upper()
            if w not in ('00000000', 'FFFFFFFF'):
                handles.setdefault(w, set()).add((s, i // 8, PLANES[i % 8]))
    models = {}
    for ln in lines:
        m = MODEL.search(ln)
        if not m:
            continue
        mod = models.setdefault(m.group(1).upper(), {'parts': set(), 'nonzero': False, 'hits': {}, 'draws': 0})
        mod['parts'].add(int(m.group(2)))
        mod['draws'] = max(mod['draws'], int(m.group(4)))
        for e in m.group(7).split():
            h, aux = e.split(':')
            if h.strip('0') or aux.strip('0'):
                mod['nonzero'] = True
        where = [('table entry %d' % i, w.split(':')[0]) for i, w in enumerate(m.group(7).split())]
        where += [('behind[%d]' % i, w) for i, w in enumerate(m.group(8).split())]
        where += [('model+0x%X' % (4 * i), w) for i, w in enumerate(m.group(9).split())]
        for label, w in where:
            w = w.upper()
            if w in handles:
                mod['hits'].setdefault((label, w), set()).update(handles[w])
    out, hits = [], 0
    for s, d in sorted(sets.items(), key=lambda kv: kv[1]['first']):
        out.append('set 0x%s  Y %s chroma %s  channels seen [%s]  first logged frame %d  movie 0x%s  sizes seen (WxH@frame): %s' % (s, d['size'], d['chroma'], ','.join(sorted(d['channels'])) or 'none', d['first'], d['movie'], ' '.join(d['sizes'])))
        if d['slots']:
            out.append('    slots %s' % d['slots'])
            out.append('    tchan %s' % d['tchan'])
    for mod, d in models.items():
        out.append('model 0x%s parts %s draws seen %d, table ever non-zero: %s' % (mod, sorted(d['parts']), d['draws'], 'yes' if d['nonzero'] else 'no'))
        for (label, w), who in sorted(d['hits'].items()):
            for (s, b, plane) in sorted(who):
                out.append('  %s = %s = display set 0x%s buffer %d plane %s (%s, channels [%s])' % (label, w, s, b, plane, sets[s]['size'], ','.join(sorted(sets[s]['channels'])) or 'none'))
                hits += 1
    for ln in lines:
        if 'slot/channel map (guest frame' in ln:
            out.append('map ' + ln.split('[scene][screens] ')[-1][:400])
    if not models:
        out.append('no "stage-screens model" lines (mode 0, or no screens drawn)')
    if not sets:
        out.append('no "flush #N of display set" lines')
    return out, hits


def selftest():
    h1 = ' '.join(['AAAA0001', '00000010', 'AAAA0002', '00000011', 'AAAA0003', '00000012', '00000000', '00000000'] * 2)
    h2 = ' '.join(['BBBB0001', '00000020', 'BBBB0002', '00000021', 'BBBB0003', '00000022', '00000000', '00000000'] * 2)
    lines = [
        '[x] [scene][screens] flush #1 of display set 0x40400000 (guest frame 9): channels [none] count 2 Y 64x40 chroma 32x20 display_index 0 handles [%s] slots [] tchan [] movie 0x00000000 words []' % h1,
        '[x] [scene][screens] flush #3 of display set 0x40400000 (guest frame 12): channels [1] count 2 Y 64x40 chroma 32x20 display_index 0 handles [%s] slots [ s2=6/1,mv40300000,set40400000] tchan [ c1->s2] movie 0x40300000 words [1 2]' % h1,
        '[x] [scene][screens] flush #3 of display set 0x40500000 (guest frame 12): channels [2] count 2 Y 1280x280 chroma 640x144 display_index 1 handles [%s] slots [] tchan [] movie 0x00000000 words []' % h2,
        '[x] [scene][screens] stage-screens model 0xAA000000 part 0 (guest frame 9, draw 1): table 0xAA100000 count 1 entries [00000000:00000000] behind [%s] words [%s]' % (' '.join(['0'] * 16), ' '.join(['00000000'] * 208)),
        '[x] [scene][screens] stage-screens model 0xAA000000 part 0 (guest frame 40, draw 30, first non-zero): table 0xAA100000 count 2 entries [BBBB0002:00000000 11110000:00000000] behind [%s] words [%s]' % (' '.join(['0'] * 16), ' '.join(['00000000'] * 10 + ['AAAA0003'] + ['00000000'] * 197)),
    ]
    out, hits = join(lines)
    print('\n'.join(out))
    ok = (hits >= 2 and any('table entry 0 = BBBB0002' in o and 'buffer 0 plane Cb.id' in o and 'channels [2]' in o for o in out)
          and any('model+0x28 = AAAA0003' in o and 'plane Cr.id' in o and 'channels [1]' in o for o in out) and any('Y 1280x280' in o for o in out))
    print('selftest', 'OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], encoding='utf-8', errors='replace') as f:
        out, hits = join(f.read().splitlines())
    print('\n'.join(out))
    sys.exit(0 if hits else 1)
