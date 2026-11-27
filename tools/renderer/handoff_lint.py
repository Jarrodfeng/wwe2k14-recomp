#!/usr/bin/env python3
"""S102 T8 review F2: the hand-over's section list against ParseFrame's case ids and the reader's FrameTables (equal sets), with a planted twin.
Used by handoff_unit.py. lint(handoff_text, capture_reader_text, reader_text) -> list of problems (empty = clean)."""
import re

EXEMPT = {'kSecSurfaceRoles', 'kSecWhere'}   # ParseFrame binds them; the live reader never writes them
SECTION_LIKE = {'ops', 'targets', 'clears', 'draws', 'copies', 'presents', 'fixeds', 'dynamics', 'samplers', 'texture_slots', 'layouts', 'elements', 'constant_banks',
                'bool_loops', 'contents', 'copy_extras', 'bank_defs', 'bank_patches', 'origins', 'provenance', 'text_runs'}


def lint(handoff, capture_reader, reader):
    problems = []
    macro = handoff.split('#define WWE2K14_HANDOFF_TABLES', 1)[1].split('namespace rdr_adapter', 1)[0]
    rows = re.findall(r'X\((kSec\w+),\s*([\w:]+),\s*(\w+),\s*(\w+)\)', macro)
    if len(rows) < 20:
        problems.append('only %d rows found in WWE2K14_HANDOFF_TABLES' % len(rows))
    sec_ids = {r[0] for r in rows} | set(re.findall(r'f\(uint32_t\((kSec\w+)\)', handoff))
    parse_ids = set(re.findall(r'case (kSec\w+): Bind\(', capture_reader)) - EXEMPT
    for i in sorted(parse_ids - sec_ids):
        problems.append('ParseFrame binds %s but the hand-over does not list it' % i)
    for i in sorted(sec_ids - parse_ids):
        problems.append('the hand-over lists %s but ParseFrame does not bind it' % i)
    m = re.search(r'struct FrameTables \{(.*?)\n\};', reader, re.S)
    members = set(re.findall(r'std::(?:vector<[^;]+?>|string)\s+(\w+);', m.group(1))) if m else set()
    if not members:
        problems.append('FrameTables not found in reader.h')
    for r in rows:
        if r[2] not in members:
            problems.append('FrameTables has no member %s (hand-over table %s)' % (r[2], r[0]))
    for extra in ('thin_facts', 'strings'):
        if extra not in members:
            problems.append('FrameTables has no member ' + extra)
    names = {r[2] for r in rows}
    for n in sorted((members & SECTION_LIKE) - names):
        problems.append('FrameTables member %s is not in the hand-over table' % n)
    return problems


if __name__ == '__main__':
    import os, sys
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
    rd = lambda p: read_path(os.path.join(root, p))
    pr = lint(rd('src/renderer_adapter/frame_handoff.h'), rd('src/renderer_adapter/capture_reader.h'), rd('src/renderer_adapter/reader.h'))
    print('\n'.join(pr) or 'lint clean')
    sys.exit(1 if pr else 0)
