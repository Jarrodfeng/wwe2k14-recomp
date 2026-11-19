r"""S102 CEN1: which census lines appeared in which phase of an in-run flip run (flip_line.py prints this; tools\tests\census_phase_test.py is the selftest).

The game (src\renderer_adapter\census_flush.h) logs, at every PerFrame switch's new seen value and at the run's end, one marker line
    [census] flush gen G phase end: NAME=V
and every lever's census site prints its line right after it (the flush). So a census line belongs to the phase that ENDED at the marker before it.
Markers are matched to phases one to one; only when there are more markers than phases (a spec that flips several switches at once) are markers with no census line between them merged. The k-th group ends the k-th phase of the
[ab_flip] phase list (the pre-phase, no -1, first). Lines between the run's last marker and the end of the log belong to the last phase.
A phase of the ON side (name != reference) with no census line is reported "NO CENSUS IN PHASE": the lever's census never printed, so "not separated" cannot be told from "never took"."""
import re

MARKER = re.compile(r'\[census\] flush gen (\d+) phase end: (.+?)\s*$')
CENSUS = re.compile(r'\[(?:rdr|owner|ko)\]\[[A-Za-z0-9_]+\]')
SKIP = ('[switch]', '[ab_flip]', '[census]', '[rdr][live][time]')


def read_events(path):
    """-> [('marker', text) | ('census', line)] in log order."""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MARKER.search(line)
            if m:
                out.append(('marker', m.group(2)))
            elif CENSUS.search(line) and not any(s in line for s in SKIP):
                out.append(('census', line.rstrip('\r\n')))
    return out


def group(events, n_phases=None):
    """-> (lines before the first marker, [(ended_text, [census lines])] one per marker). When there are MORE markers than phases (a spec that flips
    several switches at once logs one marker each), the markers with no census line between them are merged until the counts fit; never otherwise
    (a phase whose lever printed nothing must stay visible)."""
    head, groups = [], []
    for kind, text in events:
        if kind == 'marker':
            groups.append(([text], []))
        elif not groups:
            head.append(text)
        else:
            groups[-1][1].append(text)
    if n_phases is not None:
        i = 0
        while len(groups) > n_phases and i + 1 < len(groups):
            if not groups[i][1]:
                groups[i + 1][0][:0] = groups[i][0]
                del groups[i]
            else:
                i += 1
    return head, [(' + '.join(g[0]), g[1]) for g in groups]


def report(events, phases, ref):
    """phases = [(no, name)] in order (the pre-phase, no -1, first when the log has it). -> list of text lines."""
    head, groups = group(events, len(phases))
    out = []
    for i, (no, name) in enumerate(phases):
        ended, lines = groups[i] if i < len(groups) else ('(no marker: the run ended inside this phase)', [])
        side = 'reference' if name == ref else 'ON'
        if lines:
            out.append(f'census, phase {no} {name} [{side}] (ended: {ended}): {len(lines)} line(s)')
            for ln in lines:
                out.append('    ' + ln[ln.find('[rdr]') if '[rdr]' in ln else ln.find('[owner]') if '[owner]' in ln else 0:][:170])
        elif name != ref and no >= 0:
            out.append(f'census, phase {no} {name} [ON] (ended: {ended}): NO CENSUS IN PHASE (the lever\'s line never printed: "not separated" cannot be told from "never took")')
        else:
            out.append(f'census, phase {no} {name} [{side}] (ended: {ended}): none')
    return out
