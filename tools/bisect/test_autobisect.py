"""Offline self-test of autobisect.py's search logic (no game): py tools\\bisect\\test_autobisect.py
A fake prober decides each probe's outcome from a rule (OR of culprit flags, an AND interaction, a commit index),
so the flags search (split / each, interactions, the rest check) and the commit binary search (the tool's own
commit_search) can be checked against known answers; plus the preset override, the crash check without a log, a bad
log regex and --until inside a repeat block."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autobisect as ab  # noqa: E402


class FakeProber:
    def __init__(self, rule):
        self.rule = rule
        self.cache = {}
        self.history = []
        self.ref = None
        self.n_launched = 0

    def probe(self, flags, tag, exe=None, reuse=True):
        key = tuple(sorted(flags))
        if reuse and key in self.cache:
            return self.cache[key]
        self.n_launched += 1
        v = self.rule(set(flags))
        p = dict(tag=tag, flags=list(flags), run='%03d' % self.n_launched, value=v, detail='fake', flags_seen={})
        self.cache[key if reuse else key + (tag,)] = p
        self.history.append(p)
        return p


def args_for(cands, strategy='split', aa=False, base='', minimize=False):
    return argparse.Namespace(base=base, preset=None, no_preset_base=False, flags=' '.join(cands), strategy=strategy,
                              aa=aa, dry_run=False, minimize=minimize)


def run(cands, rule, strategy='split', aa=False, base='', minimize=False):
    pr = FakeProber(rule)
    res = {}
    try:
        ab.bisect_flags(args_for(cands, strategy, aa, base, minimize), pr, lambda s: None, res)
    except ab.ProbeError as e:   # main() does the same
        res['error'] = str(e)
    return res, pr.n_launched


def main():
    C = ['--f%d=1' % i for i in range(8)]
    fails = 0

    def expect(name, got, want):
        nonlocal fails
        ok = got == want
        fails += 0 if ok else 1
        print('%-48s %s  got %s%s' % (name, 'ok  ' if ok else 'FAIL', got, '' if ok else '  want %s' % (want,)))

    for strategy in ('split', 'each'):
        res, n = run(C, lambda s: '--f2=1' in s, strategy)
        expect('%s: one culprit f2 (%d probes)' % (strategy, n), (res['found'], res['interactions']), (['--f2=1'], []))
        res, n = run(C, lambda s: '--f1=1' in s or '--f6=1' in s, strategy)
        expect('%s: two culprits f1, f6 (%d probes)' % (strategy, n), res['found'], ['--f1=1', '--f6=1'])
        expect('%s: rest checked, no change' % strategy, (res.get('rest') or {}).get('changed'), False)
    # AND interaction across the halves: f1 and f5 together only.
    res, n = run(C, lambda s: '--f1=1' in s and '--f5=1' in s)
    expect('split: interaction f1+f5 (%d probes)' % n, (res['found'], len(res['interactions'])), ([], 1))
    res, n = run(C, lambda s: '--f1=1' in s and '--f5=1' in s, 'each')
    expect('each: interaction f1+f5 -> the whole set', (res['found'], len(res['interactions'])), ([], 1))
    for strategy in ('split', 'each'):
        res, n = run(C, lambda s: '--f1=1' in s and '--f5=1' in s, strategy, minimize=True)
        expect('%s --minimize: interaction -> f1+f5 (%d probes)' % (strategy, n), res['interactions'],
               [['--f1=1', '--f5=1']])
    # OR culprit plus an AND pair elsewhere: f0 alone, f3+f6 together.
    res, n = run(C, lambda s: '--f0=1' in s or ('--f3=1' in s and '--f6=1' in s), minimize=True)
    expect('split --minimize: f0 alone; rest f3+f6 changes', (res['found'], res['rest']['changed']), (['--f0=1'], True))
    expect('split --minimize: hidden pair minimized to f3+f6', res['interactions'], [['--f3=1', '--f6=1']])
    # Nothing changes: no bisect.
    res, n = run(C, lambda s: False)
    expect('nothing changes -> error', 'error' in res, True)
    # A/A noise: the reference itself flips (second reference run differs).
    flip = {'n': 0}

    def noisy(s):
        flip['n'] += 1
        return flip['n'] == 2
    res, n = run(C, noisy, aa=True)
    expect('A/A differs -> error, no bisect', ('A/A' in res.get('error', ''), n), (True, 2))
    # Base flag replaced by a candidate (merge_flags).
    expect('merge_flags replaces', ab.merge_flags(['--a=1', '--b=2'], ['--a=3']), ['--b=2', '--a=3'])
    # Commit search: the tool's own function (ab.commit_search, used by bisect_commits), every position, 1..9 commits.
    for n in range(1, 10):
        for first in range(n):
            asked = []

            def judge(i):
                asked.append(i)
                return i >= first
            got, err = ab.commit_search(n, judge)
            ok_steps = len(asked) <= max(0, n - 1).bit_length()
            expect('commit search n=%d: first changed #%d (%d probes)' % (n, first, len(asked)),
                   (got, err, ok_steps), (first, None, True))
    got = ab.commit_search(6, lambda i: None if i == 2 else i >= 4)
    expect('commit search: unjudgeable mid -> stops there', got, (None, 2))
    # Preset component overridden by --flags (review finding 7): the user value wins, and it is reported.
    real_parse = ab.parse_presets
    ab.parse_presets = lambda: {'p': dict(flags=['--res=3', '--msaa=0'], base='', source='fake')}
    try:
        pr, logged = FakeProber(lambda s: '--res=2' in s), []
        res = {}
        a = args_for(['--res=2', '--x=1'])
        a.preset = 'p'
        ab.bisect_flags(a, pr, logged.append, res)
        expect('preset overridden by --flags', (res['candidates'], any('overrides preset' in x for x in logged)),
               (['--msaa=0', '--res=2', '--x=1'], True))
    finally:
        ab.parse_presets = real_parse
    # crash check when the game died before making a log (review finding 6).
    chk = ab.Check('crash', 2.0, 0.5)
    v = chk.evaluate(dict(run=None, line='r.txt ->   EXITED after 0.1s, exit code -1073741511 (0xC0000139)  ',
                          lines=[]), None)[0]
    expect('crash: no log + non-zero EXITED -> crashed', v, True)
    v = chk.evaluate(dict(run=None, line='r.txt ->   KILLED after 720s (-KillAfterWait)', lines=[]), None)[0]
    expect('crash: no log + KILLED -> crashed', v, True)
    v = chk.evaluate(dict(run=None, line='no output from run_list.ps1', lines=[]), None)[0]
    expect('crash: nothing launched -> cannot judge', v, None)
    v = chk.evaluate(dict(run=None, line='r.txt ->   EXITED after 9.0s, exit code 0 (0x00000000)', lines=[]), None)[0]
    expect('crash: no log + exit 0 -> cannot judge', v, None)
    # Bad log regex: a clean message, not a traceback.
    try:
        ab.Check('log:(', 2.0, 0.5)
        got = 'no error'
    except SystemExit as e:
        got = 'SystemExit' if 'invalid regular expression' in str(e) else str(e)
    expect('log:( -> clean error', got, 'SystemExit')
    # --until inside a repeat block is refused.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        rp = os.path.join(td, 'r.txt')
        with open(rp, 'w', encoding='utf-8') as f:
            f.write('wait_frames 10\nrepeat 3\nscreenshot a\nend_repeat\nscreenshot b\nquit\n')
        try:
            ab.truncate_route(rp, 'a', td)
            got = 'cut'
        except SystemExit as e:
            got = 'refused' if 'repeat block' in str(e) else str(e)
        expect('--until label inside repeat -> refused', got, 'refused')
        dst = ab.truncate_route(rp, 'b', td)
        with open(dst, encoding='utf-8') as f:
            body = f.read()
        expect('--until label after the block -> cut', body.count('end_repeat') == 1 and 'quit' in body, True)
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
