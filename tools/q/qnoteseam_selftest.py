"""Self-test of qnoteseam (S111 NOTESEAM: the lane notes and the seam rows in the fact card): `py -I tools/q/qnoteseam_selftest.py`   (0 game runs, offline,
invented notes / seams / rows in a temp folder)

A. notes: a note that names the function (sub_X or 0xX) is listed, its `## Answer` text and the count of functions it settled print; a note that lodged a
   claim AT the function comes first; EIGHTEEN notes name it and every one is accounted for (names listed + `+K more`), the rest named with the exact call
   `q ours text sub_X --docs`; `--facts all` = one line per note; a note naming another address is not listed.
B. seams: a seam claim row at the function / a game row inside it = the function's own seam first (with `ours=` and `(open N)`); `via NOTE` for a note that has
   a claim at the function and another claim naming a seam; a compound topic named as a WHOLE word of the function's claim text (`light sets`, plural ok)
   is listed, `reskin pass` does not name `skin_pass`; a seam of no relation is absent.
C. the card: every header line <= 160 chars; the claims' `facts` line outranks the notes / seams lines under a tight budget; a tiny function stays inside
   max(15%, 300 bytes) after its first fact.
D. planted twins that must FAIL the same checks: notes cut at six with no count (_fit drops silently), the line not fitted (the card's own cut eats names),
   the seam topic matched as a substring.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfactcard as qf  # noqa: E402
import qnoteseam as qn  # noqa: E402

F = 0x82100000
SIZE = 0x200
OTHER = 0x82200000
CALL = f'q ours text sub_{F:08X} --docs'


def row(note, mark, claim, func=F, **kw):
    r = {'address': f'0x{func:08X}', 'function': f'0x{func:08X}', 'offset': '0x0', 'mark': mark, 'claim': claim, 'evidence': 'x',
         'note': f'docs/research/s111/{note}.md'}
    r.update(kw)
    return r


ROWS = [
    row('N1', 'C', 'the composer copies one block', seam='pose_timing', ours='src/own.h:10'),
    row('N2', 'C', 'the light sets are picked here (the same light sets again)'),
    row('N3', 'C', 'a plain claim at the function'),
    row('N3', 'C', 'a claim of the same note elsewhere', func=OTHER, seam='slots', ours='src/slots.h:5'),
    row('N4', 'C', 'the reskin pass is not a seam word'),
    row('N5', 'C', 'another function of N1', func=OTHER + 4),
    row('N1', 'C', 'another function of N1', func=OTHER + 8),
]
NOTES = {      # name -> (mentions of the function, other text)
    'N1': (5, '# N1: the first note\n\n## Answer\n\nThe composer picks one view and hands it on.\n\n## Rest\nx\n'),
    'N2': (4, '# N2\n'), 'N3': (3, '# N3\n'), 'N4': (3, '# N4\n'), 'N5': (2, '# N5\n'), 'N6': (2, '# N6\n'), 'N7': (1, '# N7\n'), 'N8': (1, '# N8\n'),
    **{f'LONGNOTE_NAME_{i:02d}': (1, '# x\n') for i in range(10)},
}
NNOTES = len(NOTES)      # 18: the line cannot hold them all
SEAMS = {
    'pose_timing': {'summary': 's', 'game': [{'addr': f'0x{F + 8:08X}', 'what': 'w', 'mark': 'C', 'source': 'a.md'}], 'ours': [], 'open': ['a', 'b']},
    'slots': {'summary': 's', 'game': [], 'ours': [{'path': 'src/file_ours.h'}], 'open': []},
    'light_sets': {'summary': 's', 'game': [], 'ours': [{'path': 'src/ls.h'}], 'open': ['o']},
    'skin_pass': {'summary': 's', 'game': [], 'ours': [], 'open': ['o']},
    'unrelated': {'summary': 's', 'game': [{'addr': '0x82900000', 'what': 'w', 'mark': 'C', 'source': 'a.md'}], 'ours': [], 'open': []},
}


def build(root):
    d = os.path.join(root, 'docs', 'research', 's111')
    os.makedirs(d)
    for name, (n, body) in NOTES.items():
        with open(os.path.join(d, name + '.md'), 'w', encoding='utf-8') as f:
            f.write(body + ''.join(f'mention sub_{F:08X} here\n' for _ in range(n)))
    with open(os.path.join(d, 'ELSE.md'), 'w', encoding='utf-8') as f:
        f.write('# else\nonly sub_82300000 here\n')
    s = os.path.join(root, 'db', 'seams')
    os.makedirs(s)
    for t, v in SEAMS.items():
        with open(os.path.join(s, t + '.json'), 'w', encoding='utf-8') as f:
            json.dump({'topic': t, **v}, f)


def line_of(facts, tag):
    return [f.text for f in facts if f.tag == tag]


def checks():
    bad = []

    def ck(name, cond, detail=''):
        if not cond:
            bad.append(f'{name} {detail}')

    ctx = qf.Ctx(F, SIZE, None, 'decomp', 'card')
    nl = line_of(qn.notes_facts(ctx), 'notes')
    ck('A1 one notes line', len(nl) == 1, repr(nl))
    t = nl[0] if nl else ''
    ck('A2 answer text of the first note', 'The composer picks one' in t and 'settled 1' in t and t.startswith('N1 ('), t)
    ck('A3 a note with a claim AT the function before one with more mentions only', t.find('N2') < t.find('N5') if 'N5' in t else True, t)
    names = re.findall(r'\b(?:N[1-8]|LONGNOTE_NAME_\d\d)\b', t.split('(q ours')[0])
    more = re.search(r'\+(\d+) more \(' + re.escape(CALL) + r'\)', t)
    ck('A4 every note accounted for: names + K more == all', len(set(names)) + (int(more.group(1)) if more else 0) == NNOTES, t)
    ck('A5 the line fits the card (<= 160 with `; notes: `)', len(qf._fmt(qf.Fact('notes', t))) <= qf.HEADER_CHARS, str(len(t)))
    ck('A6 a note naming another address is not listed', 'ELSE' not in t, t)
    al = line_of(qn.notes_facts(qf.Ctx(F, SIZE, None, 'decomp', 'all')), 'notes')
    ck('A7 --facts all: one line per note, none cut', len(al) == NNOTES and all('mentions' in x for x in al), str(len(al)))
    ck('A8 no note at an unmentioned function', not qn.notes_facts(qf.Ctx(F + 0x400, SIZE, None, 'decomp', 'card')))

    sl = line_of(qn.seams_facts(ctx), 'seams')
    s = sl[0] if sl else ''
    ck('B1 the own seam first, with ours and open', s.startswith('pose_timing own ours=src/own.h:10 (open 2)'), s)
    ck('B2 via a note', 'slots via N3 ours=src/slots.h:5' in s, s)
    ck('B3 a compound topic named as a whole word (plural ok)', 'light_sets named ours=src/ls.h (open 1)' in s, s)
    ck('B4 `reskin pass` does not name `skin_pass`', 'skin_pass' not in s, s)
    ck('B5 an unrelated seam is absent', 'unrelated' not in s, s)
    ck('B6 the seam line fits the card', len(qf._fmt(qf.Fact('seams', s))) <= qf.HEADER_CHARS, str(len(s)))

    # C. the card
    facts = qf.collect(F, None, SIZE, 'decomp', 'card')
    body = ['@x  [listing :1]'] + ['  +%X  r3 = %d' % (i * 4, i) for i in range(0x300)]
    hdr, _out = qf.apply(body, facts, 'card')
    ck('C1 header lines <= 160 chars', all(len(h) <= qf.HEADER_CHARS for h in hdr), str([len(h) for h in hdr]))
    ck('C2 the notes and seams lines print', any(h.startswith('; notes:') for h in hdr) and any(h.startswith('; seams:') for h in hdr), str(hdr))
    ck('C3 the claims `facts` line is there', any(h.startswith('; facts:') for h in hdr), str(hdr))
    tiny = ['@x  [listing :1]', '  +0  r3 = 1']
    th, _o = qf.apply(tiny, facts, 'card')
    base = sum(len(l) + 1 for l in tiny)
    budget = max(int(base * qf.BUDGET_FRAC), qf.BUDGET_MIN)
    first = max((len(h) + 1 for h in th), default=0)
    ck('C4 a tiny function: the card stays inside max(15%, 300) after the first fact', sum(len(h) + 1 for h in th) <= max(budget, first), str(sum(len(h) + 1 for h in th)))
    ck('C5 notes / seams rank below the claim headers', not (th and not any(h.startswith('; facts:') for h in th) and any(h.startswith(('; notes:', '; seams:')) for h in th)), str(th))
    return bad


def run(label):
    with tempfile.TemporaryDirectory() as tmp:
        build(tmp)
        qn.set_root(tmp)
        qf.set_rows(ROWS)
        try:
            return checks()
        finally:
            qn.set_root(None)
            qf.set_rows(None)


def twin(name, patch):
    saved = {k: getattr(qn, k) for k in patch}
    for k, v in patch.items():
        setattr(qn, k, v)
    try:
        bad = run(name)
    finally:
        for k, v in saved.items():
            setattr(qn, k, v)
    return bad


def _fit_silent(label, items, rest_call, width):
    return (label + ' ' if label else '') + ' | '.join(items[:6])      # the old qbrief habit: the seventh note is gone, no count


def _fit_unfitted(label, items, rest_call, width):
    return (label + ' ' if label else '') + ' | '.join(items)      # no fit: the card's own clip cuts the tail, names vanish behind `...`


def _named_substring(topic, text):
    return topic.lower().replace('_', ' ') in str(text).lower() or topic.lower() in str(text).lower()


def main():
    bad = run('real')
    for b in bad:
        print('FAIL', b)
    if bad:
        return 1
    miss = []
    for name, patch in (('notes cut at six, no count', {'_fit': _fit_silent}), ('line not fitted', {'_fit': _fit_unfitted}),
                        ('seam topic as a substring', {'_named': _named_substring})):
        tb = twin(name, patch)
        print(f'twin "{name}": {"caught: " + "; ".join(b.split(" ")[0] for b in tb) if tb else "NOT CAUGHT"}')
        if not tb:
            miss.append(name)
    if miss:
        print('qnoteseam_selftest: planted twin(s) NOT caught:', ', '.join(miss))
        return 2
    print('qnoteseam_selftest: all checks pass, all 3 twins caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
