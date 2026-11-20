r"""S104 lane CDT: what render state did the game use for this draw, per stage slot, in a stored capture. Offline, 0 game runs, read-only.

Four probes each wrote a throwaway script for this (z write / blend / alpha test / cull of a stage draw tied to an arena slot and its tokens); this is the one tool.
Reads the capture's fixed-state table (section 7, 72-byte rdr_adapter::DrawFixed) through tools\particles\pcap.py (`FIXED_DT`, `FrameData.fixeds()`), joins each
stage draw (model part at the stage sites 0x82C0A950 / 0x82C0B94C) to its arena slot with tools\arena\join_census.py (`match_draw`) against the player's own
pac read by tools\arena\arena_files.py, and groups it by the slot's tokens (`s`, `a`, `z`).

  py -I tools\arena\draw_states.py CAPTURE [--arena N] [--frames 30,60,90-99 | --frames-spread N] [--slots 45,46] [--tokens s,a,z,none] [--classes stage,particle,...]
  rows (default)  one row per draw: frame, draw index, class, site, slots, tokens, group, program, cull, front face, depth test / write / compare, blend, alpha test.
  --summary       per token group (s, a, s z, a z, z, none) the draws per distinct state; draws with an ambiguous join (hits in two groups) and draws with no join are
                  COUNTED and printed, never dropped.
  --order         the frame as coarse segments in op order: first / last draw index, class, site, group, depth write, blend, alpha test (all classes; filters ignored).
  --all DIR       the summary over every DIR\aNN_entrance / aNN_match (the arena id is the NN); an unreadable capture is reported with the reason (exit 1).
CAPTURE = a capture folder (frames\ and geometry.bin) or a case zip. Frames are capture frame numbers (frame_NNNNNN). --frames-spread N = N frames spread from 25 %
of the capture to its last (the ZW2 pick: 30, 60, 89, 119 of 120). --classes: `stage` (default: model part at the stage sites) or pcap class names (a class that is
not a stage draw has no slot join: status `nojoin`). --tokens keeps draws whose slots carry any listed token (`none` = the no-token group).
Exit 2 when the layout check (pcap.FIXED_DT against the headers' DrawFixed / DrawState) fails or an input cannot be read. Numbers, tokens and names only."""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'particles'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene'))
import pcap  # noqa: E402

SITE_A, SITE_B = 0x82C0A950, 0x82C0B94C  # join_census.SITE_A / SITE_B (asserted when the join is loaded)
HEADER_FIXED = os.path.join('src', 'renderer_adapter', 'adapter_frame.h')
HEADER_API = os.path.join('src', 'renderer', 'api.h')
CMP = ['NEVER', 'LESS', 'EQUAL', 'LEQUAL', 'GREATER', 'NOTEQUAL', 'GEQUAL', 'ALWAYS']  # VkCompareOp
BLEND = ['ZERO', 'ONE', 'SRC_COLOR', 'INV_SRC_COLOR', 'DST_COLOR', 'INV_DST_COLOR', 'SRC_ALPHA', 'INV_SRC_ALPHA', 'DST_ALPHA', 'INV_DST_ALPHA']  # VkBlendFactor


class LayoutError(Exception):
    """pcap.FIXED_DT no longer matches the headers' DrawFixed / DrawState."""


# ---------------------------------------------------------------- the header layout check -------------------------------------------------------------
_PRIM = {'uint8_t': 1, 'int8_t': 1, 'uint16_t': 2, 'int16_t': 2, 'uint32_t': 4, 'int32_t': 4, 'float': 4, 'int': 4, 'uint64_t': 8, 'int64_t': 8}


def _strip(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def _structs(text):
    """name -> body text of every `struct NAME { ... };` (braces matched, so `= {}` initialisers are fine)."""
    out = {}
    for m in re.finditer(r'\bstruct\s+(\w+)\s*\{', text):
        depth, j = 1, m.end()
        while j < len(text) and depth:
            depth += {'{': 1, '}': -1}.get(text[j], 0)
            j += 1
        out[m.group(1)] = text[m.end():j - 1]
    return out


def _members(body, consts):
    """[(type, name, count)] of a struct body: data members only (methods, statics, usings are skipped)."""
    out = []
    for stmt in re.sub(r'\{[^{}]*\}', '', body).split(';'):
        s = ' '.join(stmt.split())
        if not s or '(' in s or re.match(r'(static|constexpr|using|enum|typedef|friend)\b', s):
            continue
        m = re.match(r'(?:const\s+)?([\w:]+)\s+(.+)$', s)
        if not m:
            continue
        typ = m.group(1).split('::')[-1]
        for decl in m.group(2).split(','):
            decl = decl.split('=')[0].strip()
            d = re.match(r'(\w+)\s*(?:\[\s*(\w+)\s*\])?$', decl)
            if not d:
                raise LayoutError('cannot parse the member declaration %r' % s)
            cnt = 1
            if d.group(2):
                cnt = int(d.group(2)) if d.group(2).isdigit() else consts.get(d.group(2))
                if cnt is None:
                    raise LayoutError('array size %s is not a constant the check knows' % d.group(2))
            out.append((typ, d.group(1), cnt))
    return out


def _sizeof(typ, structs, consts, memo):
    """-> (size, align) with natural alignment."""
    if typ in _PRIM:
        return _PRIM[typ], _PRIM[typ]
    if typ in memo:
        return memo[typ]
    if typ not in structs:
        raise LayoutError('type %s is not one the check knows (a new member type in the header?)' % typ)
    off, al = 0, 1
    for t, _, c in _members(structs[typ], consts):
        s, a = _sizeof(t, structs, consts, memo)
        off = (off + a - 1) // a * a + s * c
        al = max(al, a)
    memo[typ] = ((off + al - 1) // al * al, al)
    return memo[typ]


def header_layout(fixed_text, api_text):
    """The flat layout the headers define for the 72-byte DrawFixed: [(name, offset, size, sub_names or None)]; DrawFixed's first member (the DrawState) is expanded;
    a repeated name gets `_2`."""
    ftext, atext = _strip(fixed_text), _strip(api_text)
    consts = {m.group(1): int(m.group(2)) for m in re.finditer(r'constexpr\s+\w+\s+(\w+)\s*=\s*(\d+)\s*;', atext)}
    structs = dict(_structs(atext))
    structs.update(_structs(ftext))
    if 'DrawFixed' not in structs or 'DrawState' not in structs:
        raise LayoutError('struct DrawFixed / DrawState not found in %s / %s' % (HEADER_FIXED, HEADER_API))
    memo = {}
    flat, off, seen = [], 0, set()

    def add(typ, name, cnt):
        nonlocal off
        s, a = _sizeof(typ, structs, consts, memo)
        off = (off + a - 1) // a * a
        n = name
        while n in seen:
            n += '_2'
        seen.add(n)
        subs = [m[1] for m in _members(structs[typ], consts)] if typ in structs else None
        flat.append((n, off, s * cnt, subs))
        off += s * cnt
    first = True
    for typ, name, cnt in _members(structs['DrawFixed'], consts):
        if first and typ == 'DrawState':
            for t2, n2, c2 in _members(structs['DrawState'], consts):
                add(t2, n2, c2)
        else:
            add(typ, name, cnt)
        first = False
    total = _sizeof('DrawFixed', structs, consts, memo)[0]
    return flat, total


def check_layout(dtype, fixed_text, api_text, section_size=72):
    """Raises LayoutError (with the first difference) unless `dtype` (pcap.FIXED_DT) has the headers' fields: names, order, offsets, sizes, nested names, total size."""
    flat, total = header_layout(fixed_text, api_text)
    if total != dtype.itemsize or total != section_size:
        raise LayoutError('DrawFixed is %d bytes in the headers, %d in pcap.FIXED_DT, %d expected for section 7' % (total, dtype.itemsize, section_size))
    names = list(dtype.names)
    if [f[0] for f in flat] != names:
        h = [f[0] for f in flat]
        bad = next((i for i in range(max(len(h), len(names))) if i >= len(h) or i >= len(names) or h[i] != names[i]), 0)
        raise LayoutError('field names / order differ at #%d: header %s, pcap.FIXED_DT %s' % (bad, h[bad] if bad < len(h) else '(none)', names[bad] if bad < len(names) else '(none)'))
    for n, off, size, subs in flat:
        fdt, foff = dtype.fields[n][0], dtype.fields[n][1]
        if foff != off or fdt.itemsize != size:
            raise LayoutError('field %s: header offset %d size %d, pcap.FIXED_DT offset %d size %d' % (n, off, size, foff, fdt.itemsize))
        if subs is not None:
            base = fdt.subdtype[0] if fdt.subdtype else fdt
            if list(base.names or ()) != subs:
                raise LayoutError('field %s members: header %s, pcap.FIXED_DT %s' % (n, subs, list(base.names or ())))


def run_layout_check(root=ROOT):
    try:
        with open(os.path.join(root, HEADER_FIXED), encoding='utf-8', errors='replace') as f:
            ftext = f.read()
        with open(os.path.join(root, HEADER_API), encoding='utf-8', errors='replace') as f:
            atext = f.read()
    except OSError as e:
        raise LayoutError('cannot read the headers (%s): the layout check cannot run' % e)
    check_layout(pcap.FIXED_DT, ftext, atext)


# ---------------------------------------------------------------- classification ---------------------------------------------------------------------
def group_of(tokens):
    """The token group of a slot's tokens: letters s / a, then z: 's', 'a', 's z', 'a z', 'z', 'none' (k, r, w, l(..) do not change the group)."""
    s, a, z = 's' in tokens, 'a' in tokens, 'z' in tokens
    lead = ' '.join(x for x, on in (('s', s), ('a', a)) if on)
    if lead:
        return lead + (' z' if z else '')
    return 'z' if z else 'none'


def classify(slots, flags):
    """slots = the joined draw's candidate slot ids, flags = {slot: set(tokens)} -> dict(group, status, slots, tokens, union).
    status: 'joined' (all candidate slots in one group), 'ambiguous' (two groups), 'unjoined' (no slot)."""
    sl = sorted(set(slots))
    if not sl:
        return dict(group='-', status='unjoined', slots=[], tokens='-', union=set())
    toks = {s: set(flags.get(s, set())) for s in sl}
    groups = {group_of(t) for t in toks.values()}
    tstr = ' '.join('%d:%s' % (s, ','.join(sorted(toks[s])) or '-') for s in sl)
    return dict(group=sorted(groups)[0] if len(groups) == 1 else 'ambig', status='joined' if len(groups) == 1 else 'ambiguous', slots=sl, tokens=tstr,
                union=set().union(*toks.values()))


def state_of(fx):
    """One FIXED_DT row -> a plain dict of the state values a probe asks about."""
    b = fx['blend'][0]
    return dict(program=int(fx['program']), cull=int(fx['cull']), ff=int(fx['front_face']), zt=int(fx['depth_test']), zw=int(fx['depth_write']), zc=int(fx['depth_compare']),
                bl=int(b['enable']), src=int(b['src_color']), dst=int(b['dst_color']), op=int(b['color_op']), at=int(fx['alpha_test']), af=int(fx['alpha_func']),
                ref=float(fx['alpha_ref']))


def _nm(table, v):
    return table[v] if 0 <= v < len(table) else str(v)


def state_text(st):
    bl = 'off' if not st['bl'] else '%s/%s op%d' % (_nm(BLEND, st['src']), _nm(BLEND, st['dst']), st['op'])
    at = 'off' if not st['at'] else '%s@%.4f' % (_nm(CMP, st['af']), st['ref'])
    return 'cull%d ff%d zt%d zw%d zc:%s bl:%s at:%s' % (st['cull'], st['ff'], st['zt'], st['zw'], _nm(CMP, st['zc']), bl, at)


def state_key(st, site):
    return 'zw%d zt%d zc:%s bl:%s at:%s site%s' % (st['zw'], st['zt'], _nm(CMP, st['zc']), 'off' if not st['bl'] else '%s/%s' % (_nm(BLEND, st['src']), _nm(BLEND, st['dst'])),
                                                  'off' if not st['at'] else '%s@%.4f' % (_nm(CMP, st['af']), st['ref']), site)


class Summary:
    """Counts over rows: draws per group per state key, and the draws that did not join (counted, never dropped)."""

    def __init__(self):
        self.frames = 0
        self.stage = self.joined = self.ambiguous = self.unjoined = self.nojoin = self.filtered = 0
        self.groups = collections.defaultdict(collections.Counter)
        self.zw = collections.defaultdict(collections.Counter)

    def add(self, rows, filtered=0):
        self.frames += 1
        self.filtered += filtered
        for r in rows:
            if r['status'] == 'nojoin':
                self.nojoin += 1
                continue
            self.stage += 1
            if r['status'] == 'unjoined':
                self.unjoined += 1
            elif r['status'] == 'ambiguous':
                self.ambiguous += 1
            else:
                self.joined += 1
            self.groups[r['group']][state_key(r['state'], r['site'])] += 1
            self.zw[r['group']][r['state']['zw']] += 1

    def merge(self, o):
        self.frames += o.frames
        for k in ('stage', 'joined', 'ambiguous', 'unjoined', 'nojoin', 'filtered'):
            setattr(self, k, getattr(self, k) + getattr(o, k))
        for g, c in o.groups.items():
            self.groups[g].update(c)
        for g, c in o.zw.items():
            self.zw[g].update(c)

    def lines(self, indent='  '):
        out = ['%sdraws considered %d: joined %d, ambiguous %d (hits in two groups), unjoined %d (no slot), not-a-stage-draw %d%s' % (
            indent, self.stage + self.nojoin, self.joined, self.ambiguous, self.unjoined, self.nojoin, ', filtered out %d' % self.filtered if self.filtered else '')]
        order = ['none', 'z', 's', 'a', 's a', 's z', 'a z', 's a z', 'ambig', '-']
        for g in sorted(self.groups, key=lambda x: order.index(x) if x in order else 99):
            n = sum(self.groups[g].values())
            out.append('%s%-7s n=%-4d zw %s' % (indent, 'unjoined' if g == '-' else g, n, dict(sorted(self.zw[g].items()))))
            for k, v in sorted(self.groups[g].items(), key=lambda kv: -kv[1]):
                out.append('%s    %5d  %s' % (indent, v, k))
        return out


# ---------------------------------------------------------------- reading a capture ------------------------------------------------------------------
class _Deps:
    """The join and the arena files, loaded on first use (the unit needs neither)."""
    jc = af = None
    arenas = {}

    @classmethod
    def load(cls):
        if cls.jc is None:
            sys.path.insert(0, HERE)
            import join_census
            import arena_files
            assert (join_census.SITE_A, join_census.SITE_B) == (SITE_A, SITE_B), 'join_census stage sites changed'
            cls.jc, cls.af = join_census, arena_files
        return cls

    @classmethod
    def arena(cls, aid):
        cls.load()
        if aid not in cls.arenas:
            a = cls.af.load_arena(aid)
            cls.arenas[aid] = (a, cls.jc.FileIndex(a), {})
        return cls.arenas[aid]


def arena_of(path, override=None):
    if override is not None:
        return override
    m = re.match(r'^a(\d+)_', os.path.basename(os.path.normpath(path)))
    if not m:
        raise pcap.PError('%s: the arena id is not in the folder name (aNN_...): pass --arena N' % path)
    return int(m.group(1))


def parse_frames(spec):
    out = []
    for part in spec.split(','):
        part = part.strip()
        if not part:
            continue
        m = re.match(r'^(\d+)-(\d+)$', part)
        out.extend(range(int(m.group(1)), int(m.group(2)) + 1) if m else [int(part)])
    return out


def pick_frames(cap, spec, spread):
    """-> [file index]: capture frame numbers from --frames, else `spread` indices spread from 25 % to the last frame (ZW2's pick)."""
    import numpy as np
    n = len(cap.frames)
    if spec:
        by_num = {int(re.search(r'frame_(\d+)', cap.frames[i]).group(1)): i for i in range(n)}
        missing = [x for x in parse_frames(spec) if x not in by_num]
        if missing:
            print('  frames not in the capture (skipped): %s' % missing[:12])
        return [by_num[x] for x in parse_frames(spec) if x in by_num]
    return sorted({int(round(x)) for x in np.linspace(n * 0.25, n - 1, spread)})


def draw_order(fr):
    di, _ = fr.draw_ops()
    seen = set(int(x) for x in di)
    return [int(x) for x in di] + [i for i in range(len(fr.draws)) if i not in seen]


def frame_rows(cap, fr, arena_id, classes=('stage',), all_classes=False):
    """Rows (dicts) of one frame in op order. Stage draws are joined to slots; every other class in `classes` is a 'nojoin' row. A frame without origins -> None."""
    d = _Deps.load()
    if not fr.has_origins:
        return None
    arena, fi, cache = _Deps.arena(arena_id)
    fx = fr.fixeds()
    rows = []
    for i in draw_order(fr):
        o = fr.origins[i]
        kn = pcap.CLASS_NAMES[int(o['klass'])] if int(o['klass']) < len(pcap.CLASS_NAMES) else str(int(o['klass']))
        site_v = int(o['pass_site'])
        stage = int(o['klass']) == d.jc.CLASS_MODEL_PART and site_v in (SITE_A, SITE_B)
        if not all_classes and not ((stage and 'stage' in classes) or kn in classes):
            continue
        site = 'A' if site_v == SITE_A else 'B' if site_v == SITE_B else '%08X' % site_v
        row = dict(i=i, klass=kn, site=site, status='nojoin', group='-', slots=[], tokens='-', union=set(), call=int(o['call_site']),
                   state=state_of(fx[int(fr.draws[i]['fixed'])]), ps=int(fr.draws[i]['ps_hash']))
        if stage:
            r = d.jc.match_draw(cap, fr, i, fi, cache)
            hits = r['hits'] or r['scan']
            row.update(classify([m.slot for m in hits], arena.flags))
        rows.append(row)
    return rows


def keep_row(r, slots, tokens):
    if slots and not (set(r['slots']) & slots):
        return False
    if tokens:
        if not ((r['union'] & tokens) or ('none' in tokens and r['group'] == 'none')):
            return False
    return True


def print_rows(label, rows):
    for r in rows:
        st = r['state']
        eff = pcap.effect_of(r['ps'])
        print('%s d%-5d %-10s %-2s slots=%-9s tok=%-18s grp=%-6s %s | prog=%s | %s' % (
            label, r['i'], r['klass'], r['site'], ','.join(map(str, r['slots'])) or '-', r['tokens'][:18], r['group'], r['status'] if r['status'] != 'joined' else '',
            eff if eff != '?' else '%016X' % r['ps'], state_text(st)))


def segments(rows):
    """Runs of like draws in op order -> [(first, last, n, key, slots)]; key = (class, site, group, zw, zt, blend, alpha test); the stage sites A and B (the mirrored copy)
    are one site, so a slot drawn through both is one run."""
    out = []
    for r in rows:
        st = r['state']
        key = (r['klass'], 'A/B' if r['site'] in ('A', 'B') else r['site'], r['group'], st['zw'], st['zt'], 'off' if not st['bl'] else '%s/%s' % (_nm(BLEND, st['src']), _nm(BLEND, st['dst'])),
               'off' if not st['at'] else '%s@%.4f' % (_nm(CMP, st['af']), st['ref']))
        if out and out[-1][3] == key:
            s = out[-1]
            s[1], s[2] = r['i'], s[2] + 1
            s[4].update(r['slots'])
        else:
            out.append([r['i'], r['i'], 1, key, set(r['slots'])])
    return out


def print_segments(label, segs):
    for first, last, n, k, slots in segs:
        sl = sorted(slots)
        print('%s %5d-%-5d n=%-4d %-10s site %-8s grp=%-6s zw%d zt%d bl:%s at:%s slots=%s' % (
            label, first, last, n, k[0], k[1], k[2], k[3], k[4], k[5], k[6], ','.join(map(str, sl[:12])) + ('..' if len(sl) > 12 else '') if sl else '-'))


def run_capture(path, a, quiet=False):
    """One capture -> a Summary (rows / order printed per frame as asked)."""
    arena_id = arena_of(path, a.arena)
    cap = pcap.Capture(path)
    classes = tuple(x.strip() for x in a.classes.split(',') if x.strip())
    slots = {int(x) for x in a.slots.split(',')} if a.slots else set()
    tokens = {x.strip() for x in a.tokens.split(',') if x.strip()} if a.tokens else set()
    summ = Summary()
    idxs = pick_frames(cap, a.frames, a.frames_spread)
    if not quiet:
        print('== %s arena %d frames %d, reading %d' % (os.path.basename(os.path.normpath(path)), arena_id, len(cap.frames), len(idxs)))
    for idx in idxs:
        fr = cap.frame(idx)
        num = int(re.search(r'frame_(\d+)', cap.frames[idx]).group(1))
        label = 'f%d' % num
        rows = frame_rows(cap, fr, arena_id, classes, all_classes=a.order)
        if rows is None:
            if not quiet:
                print('  %s: no origins (a capture without stage names): not read' % label)
            continue
        if a.order:
            print('== %s guest frame %d: %d draws' % (label, fr.guest_frame, len(rows)))
            print_segments('  ', segments(rows))
            continue
        kept = [r for r in rows if keep_row(r, slots, tokens)]
        if not (a.summary or a.all):
            print_rows(label, kept)
        fs = Summary()
        fs.add(kept, len(rows) - len(kept))
        summ.merge(fs)
        if a.summary and not quiet and len(idxs) > 1:
            print('  %s (guest %d)' % (label, fr.guest_frame))
            for ln in fs.lines('    '):
                print(ln)
    return summ


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--arena', type=int)
    ap.add_argument('--frames', default='')
    ap.add_argument('--frames-spread', type=int, default=4, metavar='N')
    ap.add_argument('--slots', default='')
    ap.add_argument('--tokens', default='')
    ap.add_argument('--classes', default='stage')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--order', action='store_true')
    ap.add_argument('--all', metavar='DIR')
    a = ap.parse_args(argv)
    if not a.capture and not a.all:
        ap.error('give a CAPTURE or --all DIR')
    try:
        run_layout_check()
    except LayoutError as e:
        print('draw_states: LAYOUT CHECK FAILED: %s\n  pcap.FIXED_DT (tools\\particles\\pcap.py) must be brought in line with DrawFixed (%s) and DrawState (%s) before this tool reads anything.' % (e, HEADER_FIXED, HEADER_API))
        return 2
    if a.all:
        names = sorted(n for n in os.listdir(a.all) if re.match(r'^a\d+_(entrance|match)$', n))
        if not names:
            print('draw_states: no aNN_entrance / aNN_match folder under %s' % a.all)
            return 2
        total, bad = Summary(), []
        for n in names:
            try:
                s = run_capture(os.path.join(a.all, n), a, quiet=True)
            except Exception as e:  # a capture that cannot be read is reported, never skipped silently
                bad.append((n, '%s: %s' % (type(e).__name__, e)))
                print('== %s UNREADABLE: %s' % (n, bad[-1][1]))
                continue
            total.merge(s)
            print('== %-14s frames %d  stage %4d joined %4d ambiguous %3d unjoined %4d | %s' % (
                n, s.frames, s.stage, s.joined, s.ambiguous, s.unjoined, '; '.join('%s %d zw%s' % (g, sum(c.values()), dict(sorted(s.zw[g].items()))) for g, c in sorted(s.groups.items()))))
        print('== TOTAL over %d captures (%d unreadable), %d frames' % (len(names) - len(bad), len(bad), total.frames))
        for ln in total.lines():
            print(ln)
        return 1 if bad else 0
    try:
        summ = run_capture(a.capture, a)
    except (pcap.PError, OSError) as e:
        print('draw_states: cannot read %s: %s' % (a.capture, e))
        return 2
    if a.summary:
        print('== TOTAL over %d frames' % summ.frames)
        for ln in summ.lines():
            print(ln)
    return 0


if __name__ == '__main__':
    sys.exit(main())
