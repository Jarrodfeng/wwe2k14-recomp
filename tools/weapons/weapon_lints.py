#!/usr/bin/env python3
r"""weapon_lints (S103 PWI, the PW-1/2 review's findings 2 and 4): two cheap text lints that keep the weapons contract honest.

  frame      Every per-call row type that a contributor emits through TrackerLatch (a row struct with an `object` member, held in a `std::vector<Row>` of the family's FrameBuffer) has a `frame` member: a
             record can carry more than one closed frame (TrackerLatch), and a row that does not say its frame cannot be told from a newer one (WeaponCall was the case: the drawer needs it to draw only the
             newest frame). Every row type that exists today already has one; a NEW row type without `frame` fails.
  onereader  Only src\scene_live\weapon_mgr_reader.h reads the weapon manager's slot table: no other file under src\ uses kOffSlotId / kOffSlotMain / kOffSlotSecondary (src\renderer_scene\weapon_rows.h
             DEFINES them; tests under tools\ forge a manager with them and are not scanned). Two readers disagreed on the slot rules once (clamp vs refuse, id -1 vs < 0, mgr < 0x10000).

  py -I tools\weapons\weapon_lints.py --root .          scan the tree (exit 1 on an error)
  py -I tools\weapons\weapon_lints.py --selftest        planted twins: each bad snippet MUST be flagged, each good one must pass

Text scan, no compile."""
import argparse
import os
import re
import sys

OFFSETS = ('kOffSlotId', 'kOffSlotMain', 'kOffSlotSecondary')
READER_OK = ('src/renderer_scene/weapon_rows.h', 'src/scene_live/weapon_mgr_reader.h')


def walk(root, sub, exts):
    for dp, _, fns in os.walk(os.path.join(root, sub)):
        for fn in fns:
            if fn.endswith(exts):
                yield os.path.relpath(os.path.join(dp, fn), root).replace('\\', '/')


def read(root, rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8', errors='replace').read()
    except OSError:
        return ''


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def struct_body(text, name):
    m = re.search(r'\bstruct\s+' + re.escape(name) + r'\b[^;{]*\{', text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def row_types_through_latch(files):
    """{row type: where} for the vectors of the FrameBuffers of every file family that uses TrackerLatch<FrameBuffer"""
    latched = [f for f, t in files.items() if 'TrackerLatch<FrameBuffer' in t]
    out = {}
    for lf in latched:
        fam = os.path.dirname(lf)
        base = os.path.basename(lf).replace('_contrib.h', '')
        for f, t in files.items():
            if os.path.dirname(f) != fam or not os.path.basename(f).startswith(base[:6]):
                continue
            body = struct_body(t, 'FrameBuffer')
            if body is None:
                continue
            for ty in re.findall(r'std::vector<\s*(?:\w+::)*(\w+)\s*>', body):
                out.setdefault(ty, f)
    return out


def lint_frame(files, all_text):
    errs = []
    for ty, where in sorted(row_types_through_latch(files).items()):
        body = None
        for t in all_text.values():
            body = struct_body(t, ty)
            if body is not None:
                break
        if body is None:
            continue
        has_object = re.search(r'\bobject\b\s*(=|;|\[)', body) is not None
        if not has_object:
            continue   # a node pool / a flat table: not a per-call row
        has_frame = re.search(r'\bframe\w*\s*(=|;|,)', body) is not None
        if not has_frame:
            errs.append('%s: per-call row type %s (emitted through TrackerLatch) has no `frame` member: a record can carry more than one closed frame; add `uint32_t frame`' % (where, ty))
    return errs


def lint_one_reader(texts):
    errs = []
    for f, t in texts.items():
        if f in READER_OK:
            continue
        code = strip_comments(t)
        for nm in OFFSETS:
            for m in re.finditer(r'\b' + nm + r'\b', code):
                errs.append('%s:%d: %s is the slot table layout: only weapon_mgr_reader.h reads it (use weapons::ReadSlots)' % (f, code.count('\n', 0, m.start()) + 1, nm))
    return errs


def selftest():
    bad = 0

    def case(name, errs, want):
        nonlocal bad
        ok = (len(errs) > 0) == want
        print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  -> ' + ' | '.join(errs)))
        bad += 0 if ok else 1

    latch = {'src/scene_live/x_contrib.h': 'class C { TrackerLatch<FrameBuffer, kRing> latch_; };', 'src/scene_live/x_core.h': 'struct FrameBuffer { std::vector<w::XCall> calls; std::vector<RingNodeRows> nodes; };'}
    good_t = {'src/a.h': 'struct XCall { uint32_t object = 0; uint32_t frame = 0; };'}
    bad_t = {'src/a.h': 'struct XCall { uint32_t object = 0; uint32_t node_first = 0; };'}
    case('frame: a per-call row with a frame member passes', lint_frame(latch, good_t), False)
    case('frame: a per-call row WITHOUT a frame member fails (the review finding: WeaponCall before v2)', lint_frame(latch, bad_t), True)
    case('frame: a node pool (no object member) is not a per-call row', lint_frame(latch, {'src/a.h': 'struct XCall { uint32_t object; uint32_t frame; };\nstruct RingNodeRows { float rows[12]; };'}), False)
    case('onereader: the reader and the contract header may name the offsets', lint_one_reader({'src/scene_live/weapon_mgr_reader.h': 'x kOffSlotMain', 'src/renderer_scene/weapon_rows.h': 'kOffSlotId'}), False)
    case('onereader: a second reader (the PW-2 ReadMembers as it was) fails', lint_one_reader({'src/scene_live/weapon_rows_core.h': 'read_word(mgr + w::kOffSlotMain + 4 * i, main)'}), True)
    case('onereader: a comment naming the constant is not a read', lint_one_reader({'src/scene_live/y.h': '// kOffSlotSecondary is read in the reader'}), False)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        bad = selftest()
        print('weapon_lints selftest: %s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
        return 1 if bad else 0
    root = os.path.abspath(a.root)
    live = {f: strip_comments(read(root, f)) for f in walk(root, 'src/scene_live', ('.h',))}
    all_text = {f: strip_comments(read(root, f)) for f in walk(root, 'src', ('.h',))}
    if not live or not any('TrackerLatch<FrameBuffer' in t for t in live.values()):
        print('FAIL: no TrackerLatch contributor found under src/scene_live (the lint reads nothing: refusing)')
        return 1
    srcs = {f: read(root, f) for f in walk(root, 'src', ('.h', '.cpp'))}
    errs = lint_frame(live, all_text) + lint_one_reader(srcs)
    for e in errs:
        print('ERROR ' + e)
    types = sorted(row_types_through_latch(live))
    print('weapon_lints: %d error(s); per-call row types checked: %s' % (len(errs), ', '.join(types)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
