"""S106 C3: the state-verb registry (tools/check/verbs.json) as code.

What it answers: how a check puts the game in a state (the route lines, launch flags, the -Data mirror, the offline make command) and how a run's log
proves the state held (proof rows with min / max / after, fail regexes).

  py -I tools/check/verbs.py --selftest        every regex compiles; every seen line matches its verb; planted twins fail
  py -I tools/check/verbs.py --list            the verbs, their build and status
  py -I tools/check/verbs.py --expand bleed slot=0 level=3
  py -I tools/check/verbs.py --check bleed LOGFILE [k=v ...]   judge a run log against the verb's proof + fail rows

API (for C2's runner): load(), expand(name, **params) -> (script_lines, flags {switch: token}, data, make), needs_for(name, **params), conflicts(route_path, flags) -> colliding switches, check_log(name, text, **params) -> (ok, reasons). A parameter the verb does not declare, or out of range, raises.
Placeholders are <name>; only names the verb knows (params / defaults / derive) are replaced, so a regex's own '<' stays.
"""
# entry: expand
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REG = os.path.join(HERE, 'verbs.json')
_PH = re.compile(r'<([a-z_][a-z0-9_]*)>')


def load(path=REG):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def get(reg, name):
    for v in reg['verbs']:
        if v['name'] == name:
            return v
    raise KeyError('no verb %r' % name)


def _values(v, params):
    """The placeholder values: defaults, then params, range-checked, then derived ones (key, mask8, mask_u).
    S106 C3F (review B1): a key the verb does not declare (params / defaults / ranges / derive) raises; so does a non-numeric or out-of-range value."""
    known = set(v.get('params') or {}) | set(v.get('defaults') or {}) | set(v.get('ranges') or {}) | set(v.get('derive') or {})
    for k in params:
        if k not in known:
            raise ValueError('verb %s: unknown parameter %r (known: %s)' % (v['name'], k, ', '.join(sorted(known)) or 'none'))
    vals = dict(v.get('defaults') or {})
    vals.update(params)
    for k, (lo, hi) in (v.get('ranges') or {}).items():
        if k in vals:
            n = int(vals[k])
            if not lo <= n <= hi:
                raise ValueError('verb %s: %s=%s outside %d..%d' % (v['name'], k, vals[k], lo, hi))
            vals[k] = n
    if 'mask' in vals:
        if not re.fullmatch(r'[0-9A-Fa-f]{1,8}', str(vals['mask'])):
            raise ValueError('verb %s: mask=%r is not 1-8 hex digits' % (v['name'], vals['mask']))
        vals['mask'] = str(vals['mask'])  # as written: it names the mirror folder
        vals['mask_u'] = vals['mask'].upper()
        vals['mask8'] = '%08X' % int(vals['mask'], 16)
    if 'hash' in vals and not re.fullmatch(r'[0-9A-Fa-f]{16}', str(vals['hash'])):
        raise ValueError('verb %s: hash=%r is not 16 hex digits' % (v['name'], vals['hash']))
    for k, expr in (v.get('derive') or {}).items():
        e = _sub(expr, vals)
        if not re.fullmatch(r'[0-9+*\- ]+', e):
            raise ValueError('derive %s: unsafe expression %r' % (k, e))
        vals[k] = eval(e, {'__builtins__': {}}, {})  # digits and + * - only (checked above)
    return vals


def _sub(s, vals):
    return _PH.sub(lambda m: str(vals[m.group(1)]) if m.group(1) in vals else m.group(0), s)


def _fill(v, s, vals):
    """_sub, then raise if a <name> placeholder is still there (it would reach the script / the log regex unfilled)."""
    out = _sub(s, vals)
    m = _PH.search(out)
    if m:
        raise ValueError('verb %s: unfilled placeholder <%s> in %r' % (v['name'], m.group(1), s))
    return out


def _flags_keyed(v, flags):
    """--switch=value tokens -> {switch: token}; the same switch twice inside one verb raises (last wins silently in the game)."""
    out = {}
    for f in flags:
        k = f.split('=', 1)[0]
        if k in out:
            raise ValueError('verb %s: switch %s twice' % (v['name'], k))
        out[k] = f
    return out


def expand(name, reg=None, **params):
    """-> (script lines, launch flags {switch name: full token}, data mirror or None, make command or None), placeholders filled."""
    v = get(reg or load(), name)
    vals = _values(v, params)
    ap = v.get('apply') or {}
    return ([_fill(v, x, vals) for x in ap.get('script', [])], _flags_keyed(v, [_fill(v, x, vals) for x in ap.get('flags', [])]),
            _fill(v, ap['data'], vals) if ap.get('data') else None, _fill(v, ap['make'], vals) if ap.get('make') else None)


def needs_for(name, reg=None, **params):
    """The verb's `needs` (C1's `q save check` grammar) with the placeholders filled."""
    v = get(reg or load(), name)
    vals = _values(v, params)
    return [_fill(v, x, vals) for x in v.get('needs', [])]


def conflicts(route_path, flags):
    """Switches a verb adds that the route's own `# harness: flags` line already sets (a repeated switch: last wins, no log line). -> sorted switch names."""
    mine = set(flags)
    with open(route_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'#\s*harness:\s*flags\s+(.*)', line)
            if m:
                return sorted(mine & {t.split('=', 1)[0] for t in m.group(1).split() if t.startswith('--')})
    return []


def proof_rows(name, reg=None, **params):
    v = get(reg or load(), name)
    if v.get('proof_as'):
        v = get(reg or load(), v['proof_as'])
    vals = _values(v, params)
    rows = v.get('proof')
    if not isinstance(rows, list):
        return [], [_fill(v, f, vals) for f in v.get('fail', [])]
    return [dict(r, re=_fill(v, r['re'], vals)) for r in rows], [_fill(v, f, vals) for f in v.get('fail', [])]


def check_log(name, text, reg=None, **params):
    """Judge a log's text: every proof row's count within min..max (and after its predecessor), no fail regex matches. -> (ok, [reasons])."""
    rows, fails = proof_rows(name, reg, **params)
    why = []
    if not rows:
        return False, ['%s has no machine proof (proof is prose, not a regex)' % name]
    first = {}
    for i, r in enumerate(rows):
        ms = list(re.finditer(r['re'], text, re.M))
        n = len(ms)
        if n < r.get('min', 1):
            why.append('proof %d: %d match(es), need >= %d: %s' % (i, n, r.get('min', 1), r['re']))
        if 'max' in r and n > r['max']:
            why.append('proof %d: %d match(es), max %d: %s' % (i, n, r['max'], r['re']))
        if ms:
            first[i] = ms[0].start()
            if 'after' in r:
                prev = [j for j in range(i) if r['after'] in rows[j]['re'] and j in first]
                if not prev:
                    why.append('proof %d: no earlier row matched "%s"' % (i, r['after']))
                elif not any(m.start() > first[prev[0]] for m in ms):
                    why.append('proof %d: only before the row it must follow' % i)
    for f in fails:
        m = re.search(f, text, re.M)
        if m:
            why.append('fail regex matched: %s -> %r' % (f, m.group(0)[:100]))
    return not why, why


def _seen_regex(v, reg):
    s = v.get('seen')
    if not s or not s.get('line'):
        return None
    which = s.get('matches')
    if which == 'effect_title1':
        return v['effect_title1']['re']
    rows, _ = proof_rows(v['name'], reg)
    return rows[s.get('row', 0)]['re']


def selftest():
    reg = load()
    bad = []
    n_ok = 0

    def expect(cond, msg):
        nonlocal n_ok
        if cond:
            n_ok += 1
        else:
            bad.append(msg)

    for v in reg['verbs']:
        nm = v['name']
        try:
            expand(nm, reg)
            rows, fails = proof_rows(nm, reg)
            for rx in [r['re'] for r in rows] + fails:
                re.compile(rx)
            if 'effect_title1' in v:
                re.compile(v['effect_title1']['re'])
            expect(True, '')
        except Exception as e:  # noqa: BLE001
            expect(False, '%s: does not expand / compile: %s' % (nm, e))
            continue
        s = v.get('seen')
        if s and s.get('line'):
            rx = _seen_regex(v, reg)
            expect(re.search(rx, s['line']) is not None, '%s: seen line does not match its proof regex' % nm)
            for f in fails:
                expect(re.search(f, s['line']) is None, '%s: a fail regex matches its own seen line: %s' % (nm, f))
        for i, r in enumerate(rows):
            if r.get('sample'):
                expect(re.search(r['re'], r['sample']) is not None, '%s: proof %d does not match its sample (the new log text)' % (nm, i))
    # planted twins: each must FAIL its verb's regex (UNSAFE direction: a wrong state must never read as proven)
    def fails_row(name, line, row=0, **params):
        rows, _ = proof_rows(name, reg, **params)
        return re.search(rows[row]['re'], line) is None

    expect(fails_row('bleed', "[wwe2k14][director] script 'bleed 0 4' -> ok: bleed slot 0 level 4"), 'twin: bleed level 4 matched the level-3 proof')
    try:
        expand('bleed', reg, level=4)
        expect(False, 'twin: expand(bleed, level=4) was accepted')
    except ValueError:
        expect(True, '')
    expect(fails_row('attire', "[wwe2k14][scenario] participant 0 = id 397 pad 0 attire 1 set -1 (src '397:0:1') -> key 39702 mode 0 set byte 255"),
           'twin: attire key ID*100+2 matched the key ID*100+N*10+2 proof')
    expect(fails_row('caption_poke', '[wwe2k14][dethash] poke at frame 3930: 0xAF612254 00000001 -> 00000000 (test: the game is changed from here)'),
           'twin: the poke going 1 -> 0 matched')
    expect(fails_row('threads', '[attire_build] build step 0: chosen 13902 loaded 13902 mode 1 set 0 desc 0xAF480224: colours 902DCA9C47FA0DC3'),
           'twin: the threads line with the frozen hash matched the magenta proof')
    expect(fails_row('champion_build', '[wwe2k14][match] titles (bell): 0:322 t0/0, 1:324 t0/0 frame=1301'), 'twin: a titles line with no title matched t1')
    # S110 SEAT-B: the saved-mask log line is gone with the mask hook: dlc_mask has no machine proof, so no log may ever prove it (not even the old line)
    expect(not check_log('dlc_mask', '[wwe2k14][dlc] pack 6 re-offered to the save (saved mask 0000007C -> 0000003C)', reg)[0],
           'twin: dlc_mask was proven by a log line (it has no machine proof since SEAT-B)')
    # the run-level judge: log text built from the verbs' own seen lines, then broken
    lo = ("[wwe2k14][director] script 'lightsout 0' -> ok: lightsout slot 0\n"
          "state 0x83924778>452 <1065353216 reached after 3 frames\n"
          "state 0x83924778>452 1065353216 reached after 317 frames\n")
    ok, why = check_log('lightsout', lo, reg)
    expect(ok, 'check_log(lightsout) on a complete log failed: %s' % why)
    ok, why = check_log('lightsout', lo.replace('<1065353216 reached', 'X reached'), reg)
    expect(not ok, 'twin: lightsout log with no dim step passed')
    swapped = lo.splitlines()
    swapped[1], swapped[2] = swapped[2], swapped[1]
    ok, why = check_log('lightsout', '\n'.join(swapped), reg)
    expect(not ok, 'twin: lightsout restore BEFORE the dim passed (after rule)')
    ok, why = check_log('lightsout', lo + "state 0x83924778>452 1065353216 not reached within 600 frames\n", reg)
    expect(not ok, 'twin: lightsout log with a fail line passed')
    ok, why = check_log('bleed', "[wwe2k14][director] script 'bleed 0 3' -> ok: bleed slot 0 level 3\n" * 2, reg)
    expect(not ok, 'twin: bleed ok twice (max 1) passed')
    # S106 C3F twins (round-2 review): bad parameters must raise, never become a script line or a PASS
    def raises(fn, msg):
        try:
            fn()
            expect(False, msg)
        except (ValueError, KeyError):
            expect(True, '')

    raises(lambda: expand('bleed', reg, lvl=2), 'twin B1: expand(bleed, lvl=2) (a mistyped key) was accepted')
    raises(lambda: expand('momentum', reg, pct=150), 'twin B1: momentum pct=150 was accepted')
    raises(lambda: expand('stock', reg, n=12), 'twin B1: stock n=12 was accepted')
    raises(lambda: expand('passive', reg, slot=9), 'twin B1: passive slot=9 was accepted')
    raises(lambda: check_log('bleed', '', reg, lvl=2), 'twin B1: check_log(bleed, lvl=2) was accepted')
    _ph = dict(reg)
    _ph['verbs'] = [dict(x, apply={'script': ['wait_frames <nope>']}) if x['name'] == 'passive' else x for x in reg['verbs']]
    raises(lambda: expand('passive', _ph), 'twin B1: an unfilled placeholder reached the script')
    raises(lambda: expand('dlc_mask', reg, mask='7c; rm'), 'twin B1: a non-hex mask was accepted')
    raises(lambda: expand('attire', reg, seat=1), 'twin F1: attire took a seat (never applied: one flags token seats participant 0)')
    expect(expand('attire', reg)[1] == {'--wwe2k14_launch_participants': '--wwe2k14_launch_participants=397:0:1'},
           'twin F2: expand flags are not keyed by switch name (switch -> full token)')
    ro = os.path.join(HERE, '..', 'input_scripts', 'scenes', 's105_chk_lightsout.txt')
    expect(conflicts(ro, expand('attire', reg)[1]) == ['--wwe2k14_launch_participants'], 'twin F2: attire vs the lightsout route: no collision named')
    expect(conflicts(ro, expand('bleed', reg)[1]) == [], 'twin F2: a verb with no flags collided')
    expect(any(r.get('after') == "script 'bleed" for r in proof_rows('bleed', reg)[0]), 'twin F3: the blood row needs "after": script \'bleed')
    ok, why = check_log('bleed', "[liquid calls 1, blood 6]\n[wwe2k14][director] script 'bleed 0 3' -> ok: bleed slot 0 level 3\n", reg)
    expect(not ok, 'twin F3: a blood count BEFORE the bleed script line passed')
    expect(expand('dlc_mask', reg, mask='7c')[2] == 'userdata_mask7c_s105', 'twin F5: the mirror name lost its case')
    expect(needs_for('dlc_mask', reg, mask='7c') == ['dlc:mask=0x7C'], 'twin F5: needs is not dlc:mask=0x7C (equals)')
    expect(re.search(proof_rows('champion_build', reg)[0][0]['re'], '[wwe2k14][match] titles (bell): 0:322 t1/255, 1:324 t255/255 frame=1301') is not None,
           'twin F4: the realistic titles sample (t255/255 = no title) did not match')
    for b in bad:
        print('FAIL', b)
    print('verbs selftest: %d checks ok, %d failed' % (n_ok, len(bad)))
    return 1 if bad else 0


def _kv(args):
    out = {}
    for a in args:
        k, _, val = a.partition('=')
        out[k] = int(val) if re.fullmatch(r'-?[0-9]+', val) and k not in ('mask',) else val
    return out


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    if argv[0] == '--selftest':
        return selftest()
    reg = load()
    if argv[0] == '--list':
        for v in reg['verbs']:
            print('%-16s build %-4s %s%s' % (v['name'], v.get('build', '?'), v.get('status', 'ok'), ('  needs_lane ' + v['needs_lane']) if v.get('needs_lane') else ''))
        return 0
    if argv[0] == '--expand' and len(argv) >= 2:
        s, f, d, m = expand(argv[1], reg, **_kv(argv[2:]))
        print(json.dumps({'script': s, 'flags': f, 'data': d, 'make': m}, indent=1))
        return 0
    if argv[0] == '--check' and len(argv) >= 3:
        with open(argv[2], encoding='utf-8', errors='replace') as fh:
            ok, why = check_log(argv[1], fh.read(), reg, **_kv(argv[3:]))
        print('PASS' if ok else 'FAIL')
        for w in why:
            print(' ', w)
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
