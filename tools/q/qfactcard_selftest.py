"""Self-test of qfactcard (S111 Lane A FACTCARD): `py -I tools/q/qfactcard_selftest.py`   (0 game runs, offline, invented rows only)

A. Site parsing: `decomp sub_X +OFF / +OFF2, +A..+B`, `sub_X+0xOFF`, absolute listing addresses, the owner rule (a bare sub_X after an offset is a callee),
   prose offsets (`char+5076`, `in_r3 + 2736`) are not sites.
B. The card on a small fixture: rows citing nearby offsets merge into ONE line (`xN notes`, `rev`), [L] rows only under `leads:`, DISPUTED when two [C]
   rows on one typed key name it differently (never merged), STALE for an explicit offset past the end, a superseded row struck; caps (<= 8 header lines
   of <= 160 chars, one comment <= 100 chars); `--facts none|all`; the comment lands before the decomp line holding the offset; asm window marks,
   one fact per site / per caller; the provider API (a registered provider's facts print, a failing one shows).
C. Planted twins that must FAIL the same checks: a card that drops a site group, one that merges two disagreeing rows, one that prints [L] as a block comment,
   one where another provider's comment outranks the claim headers (B24, S111 VCALL review).
D. (S111 FACTCARD review) the site parser on the S111 evidence shapes that put a comment on the wrong block: a vtable slot / field offset (`vtable +124`,
   `record +0x90`, `(the +680 handle`) is not a site; `sub_Y +0xC` and `sub_Y ... (decomp +0x0..+0x2C)` belong to sub_Y; Lane B's typed rows
   (`kind` / `key` / `name` / `site` fields) are placed by their `site` / own address, DISPUTED on `key` across functions; the comment sits on the
   shown row's own cited offset; the function's own row before a row about another function (labelled); the budget spends nothing on a comment that
   cannot be placed; the hook through qdecomp.run (decomp_selftest's synthetic listing).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfactcard as qf  # noqa: E402

F = 0x82000000        # an invented function start
SIZE = 0x600


def row(note, mark, claim, evidence, off='0x0', func=F, **kw):
    r = {'address': f'0x{func:08X}', 'function': f'0x{func:08X}', 'offset': off, 'mark': mark, 'claim': claim, 'evidence': evidence,
         'note': f'docs/research/s111/{note}.md'}
    r.update(kw)
    return r


ROWS = [
    row('AAA', 'C', 'shown word tests the node', f'decomp sub_{F:08X} +0x70 / +0x7C'),
    row('BBB', 'C', 'node is behind the shown word', f'decomp sub_{F:08X} +0x70..+0x84; q asm sub_{F:08X}+0x7C', rev=True),
    row('CCC', 'C', 'created word picks one body call', f'decomp sub_{F:08X} +0x328..+0x34C (r3 = char+5076 / +5072 test, vtable +120 at +0x50C)'),
    row('DDD', 'C', 'created word again, same site', f'decomp sub_{F:08X} +0x330 / +0x33C, +0x34C'),
    row('EEE', 'L', 'maybe the cache replays the list', f'decomp sub_{F:08X} +0x330; decomp sub_{F:08X}+0x4B4'),
    row('FFF', 'C', 'the whole function draws the node', 'decomp sub_%08X --fields' % F),
    row('GGG', 'C', 'one field rule', f'decomp sub_{F:08X} +0x200', off='0x200', kind='field', key='vt:0x82000010+5076', name='created_flag'),
    row('HHH', 'C', 'the same field, another name (in another function)', 'asm sub_82003000 +0x4', off='0x4', func=F + 0x3000,
        kind='field', key='vt:0x82000010+5076', name='dead_flag'),
    row('NNN', 'C', 'a typed site row', f'decomp sub_{F:08X} +0x100 / +0x440', kind='site', key=f'sub_{F:08X}+0x440', site=f'0x{F + 0x440:08X}'),
    row('PPP', 'C', 'the cluster after an [L] point', f'decomp sub_{F:08X} +0x4C8'),
    row('III', 'C', 'a site past the end', 'x', off='0x9000'),
    row('JJJ', 'C', 'ENVCUBE-style absolute citation', f'TEXDIFF (0x{F + 0x1A0:08X}..0x{F + 0x1B8:08X}: 7 sites)'),
    row('KKK', 'C', 'old claim', f'decomp sub_{F:08X} +0x300'),
    row('LLL', 'C', 'new claim', f'decomp sub_{F:08X} +0x300', supersedes='docs/research/s111/KKK.md +0x300'),
    row('MMM', 'C', 'other function cites this one', 'decomp sub_%08X +0x3C0 (sub_%08X is a callee)' % (F, F + 0x1000), func=F + 0x2000),
]
BODY = ['@82000000  [listing :1]', '  +14  call#14 sub_82001000(r3=in_r3)', '  +70  r11 = u32 [in_r3-976]',
        'if ([in_r3-976] != 0) {   // +78, skips to @82000100', '    +7C  r11 = u32 [in_r3-968]', '  +200  r9 = 1', '  +328  r8 = u32 [in_r3+3876]',
        '  +330  r7 = 2', '  +1A0  r6 = 5', '  +3C0  r5 = 9'] + ['  (padding line, so the byte budget of 15% leaves room)'] * 300


def checks():
    """-> [failure text]"""
    bad = []

    def ck(name, cond, detail=''):
        if not cond:
            bad.append(f'{name} {detail}')

    qf.set_rows(ROWS)
    # A. parsing
    ps = qf.parse_sites(f'decomp sub_{F:08X} +0x70 / +0x7C, +0x328..+0x34C (char+5076 / +5072); fn sub_{F + 8:08X} +A0 sub_{F + 0x10:08X} +B0', F)
    ck('A1 two owners', (F, 0x70, 0x70, True) in ps and (F + 8, 0xA0, 0xA0, False) in ps, str(ps))
    ck('A2 a callee after an offset is not an owner', (F + 0x10, 0xB0, 0xB0, False) not in ps and (F + 8, 0xB0, 0xB0, False) in ps, str(ps))
    ck('A3 prose offsets are not sites', not any(lo in (0x5076, 0x5072) for _o, lo, _h, _s in ps), str(ps))
    ck('A4 range', (F, 0x328, 0x34C, True) in ps, str(ps))
    ck('A5 absolute', (F, 0x1A0, 0x1B8, False) in qf.parse_sites(f'x (0x{F + 0x1A0:08X}..0x{F + 0x1B8:08X})', F))
    ck('A6 sub_X+0xOFF', (F, 0x4B4, 0x4B4, True) in qf.parse_sites(f'q asm sub_{F:08X}+0x4B4', F))
    # B. the card
    fs = qf.collect(F, None, SIZE, 'decomp', 'card')
    hdr = qf.header_lines(fs, 'card')
    ck('B1 header caps', 1 <= len(hdr) <= qf.HEADER_LINES and all(len(l) <= qf.HEADER_CHARS for l in hdr), str(hdr))
    sites = [f for f in fs if f.off is not None and f.tag.startswith('C')]
    near70 = [f for f in sites if f.off == 0x70]
    ck('B2 two rows on +0x70 / +0x7C are ONE group, x2, rev', len(near70) == 1 and ' x2' in near70[0].text and near70[0].tag == 'C rev', str(near70))
    near328 = [f for f in sites if 0x328 <= f.off <= 0x34C]
    ck('B3 created-word rows merge into one group x2 (+0x328..+0x34C)', len(near328) == 1 and near328[0].hi == 0x34C and ' x2' in near328[0].text, str(near328))
    ck('B4 [L] only under leads', any(f.tag == 'leads' and 'EEE' in f.text for f in fs) and not any('EEE' in f.text for f in sites), str([f.text for f in sites]))
    dis = [f for f in fs if f.tag == 'DISPUTED']
    ck('B5 DISPUTED names both, never merged', len(dis) == 1 and 'created_flag' in dis[0].text and 'dead_flag' in dis[0].text, str(dis))
    ck('B6 STALE for the explicit offset past the end', any(f.tag == 'STALE' and 'III' in f.text for f in fs), str([f.text for f in fs if f.tag == 'STALE']))
    ck('B7 the absolute citation is a site', any(f.off == 0x1A0 and f.hi == 0x1B8 for f in sites), str([(f.off, f.hi) for f in sites]))
    ck('B8 superseded row struck, newer shown', any(f.off == 0x300 and 'LLL' in f.text for f in sites) and not any('KKK' in f.text for f in sites))
    ck('B9 function-level fact', any(f.tag == 'C' and f.off is None and 'FFF' in f.text for f in fs))
    ck('B10 cross-citation reaches the cited function', any('MMM' in f.text for f in sites if f.off == 0x3C0), str([f.text for f in sites]))
    hdr_text = ' '.join(hdr)
    ck('B11 DISPUTED / STALE rank into the header', 'DISPUTED' in hdr_text and 'STALE' in hdr_text, hdr_text)
    h2, body = qf.apply(list(BODY), fs, 'card')
    ck('B12 comment before the line holding the offset', any(l.strip().startswith('; [C') for l in body) and
       next(i for i, l in enumerate(body) if '+70 ' in l and 'r11' in l) - 1 == next(i for i, l in enumerate(body) if l.strip().startswith('; [C')), str(body))
    ck('B13 comment cap', all(len(l.strip()) <= qf.COMMENT_CHARS for l in body if l.strip().startswith('; ')), str(body))
    ck('B14 no comment is an [L] row', not any('EEE' in l for l in body), str(body))
    ck('B15 --facts none prints nothing', qf.apply(list(BODY), fs, 'none')[0] == [] and qf.apply(list(BODY), fs, 'none')[1] == BODY)
    fa = qf.collect(F, None, SIZE, 'decomp', 'all')
    ha, ba = qf.apply(list(BODY), fa, 'all')
    ck('B16 --facts all lists every [C] row of the +0x328 group', sum('CCC' in l or 'DDD' in l for l in ba) >= 2, str(ba))
    wm = qf.window_marks(fs, 0x60, 0x90, 'card')
    ck('B17 asm window marks', len(wm) == 1 and wm[0][0] == 0x70 and ('AAA' in wm[0][1] or 'BBB' in wm[0][1]), str(wm))
    ck('B18 site fact / fn fact', qf.site_fact(fs, 0x338, 'card') and 'created' in qf.site_fact(fs, 0x338, 'card') and qf.site_fact(fs, 0x10, 'card') is None
       and 'FFF' in qf.fn_fact(fs, 'card'))
    ck('B19 a function with no claim prints nothing', qf.collect(F + 0x500, None, 64) == [] or qf.header_lines(qf.collect(F + 0x500, None, 64), 'card') == [])
    tiny = BODY[:10]
    ht, bt = qf.apply(list(tiny), fs, 'card')
    added = sum(len(l) + 1 for l in ht) + sum(len(l) + 1 for l in bt) - sum(len(l) + 1 for l in tiny)
    ck('B22 byte budget: a 10-line body gains <= 300 bytes (+ the first fact)', added <= qf.BUDGET_MIN + qf.HEADER_CHARS, str(added))
    big_added = sum(len(l) + 1 for l in h2) + sum(len(l) + 1 for l in body) - sum(len(l) + 1 for l in BODY)
    ck('B23 byte budget: the card is <= 15% of the plain output', big_added <= 0.15 * sum(len(l) + 1 for l in BODY), str(big_added))
    # B24 (S111 VCALL review): a tight budget; another provider's comment must not take the place of the claim headers
    tb = ['  +4  r3 = 1', '  +14  call#14 sub_82001000(r3=in_r3)'] + ['  pad'] * 8
    pf = [qf.Fact('facts', 'F' * 100, None, None, 90), qf.Fact('leads', 'L' * 100, None, None, 60), qf.Fact('sites', 'S' * 40, None, None, 70),
          qf.Fact('vcall', 'slot +8 [L] seen 1: ' + 'V' * 80, 0x14, None, 50, 9)]
    hb, _bb = qf.apply(list(tb), pf, 'card')
    ck('B24 a provider comment never evicts the facts / sites / leads headers',
       all(any(l.startswith(f'; {k}:') for l in hb) for k in ('facts', 'sites', 'leads')), str(hb))
    # providers
    qf.register('when', lambda ctx: [qf.Fact('when', 'group 1, the load wait', None, None, 85), qf.Fact('enum', 'r4=33 is cat 33', 0x14, None, 50, 9)])
    fp = qf.collect(F, None, SIZE, 'decomp', 'card')
    hp, bp = qf.apply(list(BODY), fp, 'card')
    ck('B20 a registered provider prints (header + site comment)', any('; [when]' in l for l in hp) and any('r4=33' in l for l in bp), str(hp))
    qf.register('boom', lambda ctx: 1 / 0)
    ck('B21 a failing provider is shown', any('provider failed' in l for l in qf.header_lines(qf.collect(F, None, SIZE, 'decomp', 'card'), 'card')))
    qf.register('boom', lambda ctx: [])
    qf.register('when', lambda ctx: [])
    review_checks(ck)
    return bad


def review_checks(ck):
    """D. the S111 FACTCARD review's cases (the evidence texts are the S111 rows that misplaced a comment, with invented addresses)"""
    sites = lambda ev, own=F: [(o, lo) for o, lo, _h, _s in qf.parse_sites(ev, own)]
    ps = sites(f'decomp sub_{F:08X} +0x328..+0x34C (char+5076 / +5072 test, vtable +120 at +0x50C), +0x430 / +0x4D0 vtable +124, +0x504 vtable +52')
    ck('D1 a vtable slot is not a site', not any(lo in (0x120, 0x124, 0x52) for _o, lo in ps) and (F, 0x50C) in ps and (F, 0x504) in ps, str(ps))
    ps = sites(f'UNIFY3.md review (sub_{F:08X}: the technique is [r17+20] = record +0x90 at 0x{F + 0xD08:08X}); else vtable +60 -> [+20]')
    ck('D2 a record field / vtable +60 is not a site; the absolute address is', [lo for _o, lo in ps] == [0xD08], str(ps))
    ps = sites(f'decomp sub_{F:08X} +D98 (cntlzw(count - 1) >> 5 & 1), +D9C..+DEC (the +680 handle, VS float recorder); asm 0x{F + 0xB30:08X} (the model word +368 indexed)')
    ck('D3 a field offset in prose is not a site; nested parentheses keep the chain', sorted(lo for _o, lo in ps) == [0xB30, 0xD98, 0xD9C], str(ps))
    ps = sites(f'decomp sub_{F:08X} --callers: one caller, sub_82009000 +0xC tail with r3 = [0x83930050]')
    ck('D4 `sub_Y +OFF` belongs to sub_Y', ps == [(0x82009000, 0xC)], str(ps))
    ps = sites(f'decomp sub_{F:08X} +0x18C sub_82BA7838(r4 = 0), body draw +0x2A8; sub_82BA7868 reads the same slot (decomp +0x0..+0x2C)')
    ck('D5 a verb with no function names the one the segment named', (F, 0x18C) in ps and (F, 0x2A8) in ps and (0x82BA7868, 0x0) in ps
       and (F, 0x0) not in ps, str(ps))
    ps = sites(f'decomp sub_{F:08X} +70..+84 ([node-976] / [node-968] = char+224 / +232), +36C (component_find_by_tag node-48), +3D8 / +478')
    ck('D6 the chain of decomp offsets survives a parenthesis', sorted(lo for _o, lo in ps) == [0x70, 0x36C, 0x3D8, 0x478], str(ps))
    ps = sites(f'q callers sub_82BB75A0 (184 sites, none in sub_{F:08X}); +0x7C04 sub_82BD54D0 restore', F)
    ck('D7 offsets after `callers sub_X` have no owner', ps == [], str(ps))
    fs = qf.collect(F, None, SIZE, 'decomp', 'card')
    cs = [f for f in fs if f.off is not None and f.tag.startswith('C')]
    ck('D8 a typed site row is placed by its `site` field only', any(f.off == 0x440 and 'NNN' in f.text for f in cs)
       and not any('NNN' in f.text for f in cs if f.off != 0x440), str([(hex(f.off), f.text[:20]) for f in cs]))
    dis = [f for f in fs if f.tag == 'DISPUTED']
    ck('D9 DISPUTED on Lane B `key` across functions', len(dis) == 1 and 'vt:0x82000010+5076' in dis[0].text and 'HHH' in dis[0].text, str(dis))
    p4 = [f for f in cs if 'PPP' in f.text]
    ck('D10 the comment sits on the shown row\'s own offset, not on an [L] point before it', len(p4) == 1 and p4[0].off == 0x4C8, str(p4))
    G = F + 0x6000
    qf.set_rows([row('QQQ', 'C', 'own row on the cluster', f'decomp sub_{G:08X} +0x40', func=G),
                 row('SSS', 'C', 'a row about another function, newer', f'decomp sub_{G:08X} +0x44', func=G + 0x1000),
                 row('TTT', 'C', 'a row about another function citing past the end', f'decomp sub_{G:08X} +900', func=G + 0x2000)])
    fm = qf.collect(G, None, 0x100, 'decomp', 'card')
    c40 = [f for f in fm if f.off is not None and f.tag.startswith('C')]
    ck('D11 the function\'s own row before a row about another function', len(c40) == 1 and 'QQQ' in c40[0].text, str(c40))
    fa = qf.collect(G, None, 0x100, 'decomp', 'all')
    ck('D12 a row about another function says whose it is', any('SSS' in f.text and f'sub_{G + 0x1000:08X}' in f.text for f in fa), str(fa))
    ck('D13 a row about another function whose cited site is past the end is no fact here', not any('TTT' in f.text for f in fm + fa if f.tag.startswith('C')),
       str(fm))
    qf.set_rows(ROWS)
    tiny = ['  +14  r3 = 1', '  +70  r11 = u32 [in_r3-976]', '  +7C  r11 = u32 [in_r3-968]']
    unplaced = [qf.Fact('C', 'x' * 95, 0x590 + 0x10 * k, None, 70, 50 - k) for k in range(3)] + [qf.Fact('C', 'placed' + 'y' * 90, 0x70, None, 70, 1)]
    _h, bt = qf.apply(list(tiny), unplaced, 'card')
    ck('D14 a comment with no line to sit on spends no budget', any('placed' in l for l in bt), str(bt))
    ck('D15 callers label: the fact is the caller\'s, at its call site', (qf.site_fact(fs, 0x338, 'card', 'caller +0x338: ') or '').startswith('; [C] caller +0x338: '),
       str(qf.site_fact(fs, 0x338, 'card', 'caller +0x338: ')))
    hook_check(ck)


def hook_check(ck):
    """the card through the real hook: qdecomp.run on decomp_selftest's synthetic listing (no game listing, no db)"""
    import decomp_selftest as ds
    import qdecomp
    e = ds.Env()
    try:
        base = 0x82001000
        e.add(base, ['lwz r11,112(r3)', 'cmpwi cr6,r11,0', 'beq cr6,<L>', 'lwz r10,8(r3)', 'stw r10,16(r3)', ('label', 'L'), 'li r3,1', 'blr'])
        qf.set_rows([row('HOOK', 'C', 'the word +112 gates the store', f'decomp sub_{base:08X} +0xC', func=base)])
        out = qdecomp.run(e.con, base, types.SimpleNamespace(raw_names=True)).split('\n')
        i = next((k for k, l in enumerate(out) if l.strip().startswith('; [C] HOOK')), None)
        ck('H1 qdecomp.run prints the comment before the line of the cited offset', i is not None and out[i + 1].strip().startswith('+C '), '\n'.join(out))
        off = qdecomp.run(e.con, base, types.SimpleNamespace(raw_names=True, facts='none'))
        ck('H2 --facts none: qdecomp.run prints no fact', 'HOOK' not in off, off)
    finally:
        e.close()
        qf.set_rows(ROWS)


def run_twin(name, patch):
    """the same checks with a broken card: must report failures"""
    saved = patch()
    try:
        bad = checks()
    finally:
        saved()
        qf.set_rows(None)
    print(f'{"ok  " if bad else "NOT CAUGHT"} twin {name}: {len(bad)} check(s) failed' + (f' (first: {bad[0][:70]})' if bad else ''))
    return bool(bad)


def twin_drop_group():
    orig = qf._clusters
    qf._clusters = lambda rs: [c for c in orig(rs) if c.lo != 0x70]
    return lambda: setattr(qf, '_clusters', orig)


def twin_merge_disputed():
    orig = qf._type_of
    qf._type_of = lambda r: orig(r)[:2] + ('',)
    return lambda: setattr(qf, '_type_of', orig)


def twin_lead_as_comment():
    orig = qf.Row.__init__

    def init(self, r, order, mt):
        orig(self, r, order, mt)
        if self.mark == 'L':
            self.mark = 'C'
    qf.Row.__init__ = init
    return lambda: setattr(qf.Row, '__init__', orig)


def twin_provider_outranks_headers():
    orig = qf._comment_rank
    qf._comment_rank = lambda f: 80 + min(f.weight, 99) / 100.0      # the rank before the S111 VCALL review: every block comment above every claim header
    return lambda: setattr(qf, '_comment_rank', orig)


def main():
    bad = checks()
    qf.set_rows(None)
    for b in bad:
        print('FAIL', b)
    if not bad:
        print('ok   all checks')
    if bad:
        return 1
    caught = [run_twin('card drops a site group', twin_drop_group), run_twin('merges two disagreeing rows', twin_merge_disputed),
              run_twin('[L] printed as a block comment', twin_lead_as_comment), run_twin('a provider comment evicts the claim headers', twin_provider_outranks_headers)]
    return 0 if all(caught) else 2


if __name__ == '__main__':
    sys.exit(main())
