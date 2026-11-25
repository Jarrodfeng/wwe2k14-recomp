r"""S110 SCENEMOMENTS (docs\research\s110\SCENEMOMENTS.md section 3): the scene tool's LOG line, told against the error-level groups every clean run
prints. Read only by tools\scene\go.py's hand-back: tools\bench\run_read.py itself is unchanged for its other callers.

run_read groups error-level lines by err_norm (paths and numbers out, run_read.py:128-134); 3+ mixed error lines in a row on one thread are one
`burst: FIRST` group (:176-189); the groups never decide its exit without --strict-errors (:17-25). Every clean run of the tip exe printed the same
four at boot (runs 246-249: out\exe\tip-c9454aa1e\logs\wwe2k14_002..005.log, read 2026-11-25 with run_read --json): KNOWN below. Any other group
is NEW and named; so is a second memory dump in one run. PROBLEMS (run_read's own verdict) are always counted, and the first is named.
"""

# run_read's group key -> the most times a clean run shows it (0 = any number). Each one is a boot error nobody has fixed yet (its own lane), not a
# thing this list fixes: the heap allocator, one memory dump, the UPDATE:\ device, an unimplemented kernel query.
KNOWN = {
    'BaseHeap::Alloc failed to find contiguous range': 0,
    'burst: Memory Dump': 1,
    'ResolvePath(<path>) failed - device not found': 3,   # UPDATE:\ once, \Device twice (runs 246-249); err_norm makes every path <path>
    'NtQueryInformationFile(XFileXctdCompressionInformation) unimplemented': 0,
}


def new_groups(groups):
    """The error groups (run_read --json `error_groups`) that are not known boot noise."""
    return [g for g in groups if g.get('key') not in KNOWN or (KNOWN[g['key']] and g.get('count', 0) > KNOWN[g['key']])]


def summary(rr, log, text_of=lambda s: s):
    """run_read's JSON -> the hand-back's LOG line. `text_of` shortens a log line to its message (go.py game_text)."""
    probs = rr.get('problems') or []
    groups = rr.get('error_groups') or []
    new = new_groups(groups)
    p = 'PROBLEMS %d' % len(probs)
    if probs:
        p += ' (first: %s)' % text_of(probs[0].get('text', ''))[:160]
    if new:
        e = 'NEW error groups %d: %s%s' % (len(new), ' | '.join('"%s" x%d from line %d' % (g.get('example', '')[:100], g.get('count', 0), g.get('line_no', 0))
                                                                for g in new[:3]), ' ...' if len(new) > 3 else '')
        if len(groups) > len(new):
            e += ' (+%d known noise)' % (len(groups) - len(new))
    elif groups:
        e = 'errors: known noise only (%d groups)' % len(groups)
    else:
        e = 'errors: none'
    return 'LOG      %s | %s | SHORTFALLS %d | UNSORTED %d (counted, not judged)  (full: py -I tools\\bench\\run_read.py %s)' % (
        p, e, len(rr.get('shortfalls') or []), len(rr.get('unsorted') or []), log)
