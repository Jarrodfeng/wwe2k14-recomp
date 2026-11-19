#!/usr/bin/env python3
"""S103 lane SGF lint: every LatchSnapshot field a scope-2 (entry half) part sets must be reset by LatchSnapshot::Clear()
(src/scene_live/crowd_contrib.h) AND merged by the entry_ok merge in Contributor::Latch (crowd_contrib.cpp).

Why (Opus review, lane SG finding 1): the entry half (floor, props, holders, pictures, the hand map) is read on the draw thread into entry_snap_, the latch half (scope 1)
leaves those fields default; a field the merge forgets reads as its default at the latch (the hand-map census was wrong live), and a field Clear() forgets leaks the last frame.

Method: fields = the `out.<field>` names touched inside the entry-half reader functions of crowd_latch_reader.h (ReadFloor, ReadProps, ReadPictures, ReadSingles,
HandTypeMap, ComputeHandMap, ResetHandMap, PushProp, NoteSoft, MarkUnread, RollbackSoft) minus the declared INPUTS. Each must appear in Clear() and in the merge block.
--plant drop_merge  removes one field's merge line in memory: must FAIL (the lane runs it inverted).   --plant drop_clear  the same for Clear().
Always run with `py -I`, from the repo root.
"""
import re
import sys

READER = 'src/scene_live/crowd_latch_reader.h'
HEADER = 'src/scene_live/crowd_contrib.h'
CONTRIB = 'src/scene_live/crowd_contrib.cpp'
FUNCS = ['ReadFloor', 'ReadProps', 'ReadPictures', 'ReadSingles', 'HandTypeMap', 'ComputeHandMap', 'ResetHandMap', 'PushProp', 'NoteSoft', 'MarkUnread', 'RollbackSoft']
INPUTS = {'hand_cache', 'node', 'scope', 'screen_cam', 'served', 'entry_ring', 'want_palettes'}   # set by the contributor before the read, never by a part


def body(text, name):
    m = re.search(r'inline\s+[\w:<>&\s]*?\b' + name + r'\s*\(', text)
    if not m:
        return None
    i = text.index('{', m.end())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def main():
    plant = None
    if '--plant' in sys.argv:
        plant = sys.argv[sys.argv.index('--plant') + 1]
    reader = open(READER, encoding='utf-8').read()
    header = open(HEADER, encoding='utf-8').read()
    contrib = open(CONTRIB, encoding='utf-8').read()
    fields = set()
    errs = []
    for f in FUNCS:
        b = body(reader, f)
        if b is None:
            errs.append('reader function %s not found' % f)
            continue
        fields |= set(re.findall(r'\bout\.([a-z_][a-z_0-9]*)', b))
    fields -= INPUTS
    sm = re.search(r'struct LatchSnapshot \{(.*?)\n\};', header, re.S)
    if not sm:
        errs.append('struct LatchSnapshot not found')
        decl = ''
    else:
        decl = sm.group(1)
    cm = re.search(r'void Clear\(\)\s*\{(.*?)\}\s*(?:\n|$)', decl, re.S)
    clear = cm.group(1) if cm else ''
    if not cm:
        errs.append('LatchSnapshot::Clear not found')
    a = contrib.find('if (entry_ok && !why) {')
    b = contrib.find('if (why) {', a)
    merge = contrib[a:b] if a >= 0 and b > a else ''
    if not merge:
        errs.append('entry_ok merge block not found')
    if plant in ('drop_merge', 'drop_clear') and fields:
        victim = sorted(fields)[0]
        pat = re.compile(r'[^\n]*\b' + victim + r'\b[^\n]*\n')
        if plant == 'drop_merge':
            merge = pat.sub('', merge)
        else:
            clear = re.sub(r'\b' + victim + r'\b', 'x_', clear)
    for fld in sorted(fields):
        if not re.search(r'\b' + fld + r'\b', decl):
            errs.append('out.%s is not a LatchSnapshot field' % fld)
            continue
        if not re.search(r'\b' + fld + r'\b', clear):
            errs.append('field %s is set by an entry-half part but LatchSnapshot::Clear() does not reset it' % fld)
        if not re.search(r'\b' + fld + r'\b', merge):
            errs.append('field %s is set by an entry-half part but the entry_ok merge does not copy it' % fld)
    if errs:
        for e in errs:
            print('FAIL', e)
        return 1
    if '-q' not in sys.argv:
        print('ok: %d fields: %s' % (len(fields), ', '.join(sorted(fields))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
