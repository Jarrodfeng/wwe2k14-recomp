"""qverbs_selftest: the S111 QVERBS verb registry (tools/q/verbs/*.toml, qverbs.py). 0 game runs, no index.
A PLANTED verb (a module + a verb file in a temp folder, WWE_Q_VERBS_EXTRA) is dispatched, listed in `q -h` / the first screen and caught as a typo target
WITHOUT editing q.py; its twin (no verb file) is the old hard-coded behaviour: an unknown word is a find question. Lints: every registered verb names an
existing module and a usage / routing slot that q.py really has a marker for, no verb is listed twice, a bad verb file is skipped with a warning and breaks
nothing else. Exit 1 = a check failed."""
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:300]))
    if not ok:
        fails.append(what)


def q(*args, extra=None):
    env = dict(os.environ)
    env.pop('WWE_Q_VERBS_EXTRA', None)
    if extra:
        env['WWE_Q_VERBS_EXTRA'] = extra
    r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout, r.stderr


def write(root, name, text):
    with open(os.path.join(root, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


PLANTED_PY = '''class PlantedError(Exception):
    pass


def main(argv):
    if argv[:1] == ['boom']:
        raise PlantedError('planted boom')
    if argv[:1] == ['pipe']:
        raise BrokenPipeError()
    print('planted ok ' + ' '.join(argv))
    return 0
'''
PLANTED_TOML = """name = "plantedverb"
module = "qplantedverb"
errors = ["qplantedverb.PlantedError"]

[[usage]]
text = '''  plantedverb WORDS            the planted verb of the selftest (qplantedverb.py)'''

[[route]]
text = '''- a planted question: plantedverb WORDS'''
"""


def planted():
    with tempfile.TemporaryDirectory() as d:
        write(d, 'qplantedverb.py', PLANTED_PY)
        write(d, 'plantedverb.toml', PLANTED_TOML)
        rc, so, se = q('plantedverb', 'a', 'b', extra=d)
        check('a planted verb is dispatched without editing q.py', rc == 0 and so.strip() == 'planted ok a b', (rc, so, se))
        rc, so, se = q('plantedverb', 'boom', extra=d)
        check('its own error class prints `q: MESSAGE` and exits 2', rc == 2 and 'q: planted boom' in se and not so, (rc, so, se))
        rc, so, se = q('plantedverb', 'pipe', extra=d)
        check('a closed pipe from the verb returns 0', rc == 0 and not se, (rc, so, se))
        rc, so, se = q('-h', extra=d)
        check('the planted verb\'s usage line is in `q -h`, before the common-notes paragraph', rc == 0 and '  plantedverb WORDS ' in so
              and so.index('plantedverb WORDS') < so.index('FUNC: sub_82CB1F50'), (rc, so[:200], se))
        rc, so, se = q(extra=d)
        check('its routing line is on the first screen', '- a planted question: plantedverb WORDS' in so, (rc, so[:200], se))
        rc, so, se = q('plantedvrb', extra=d)
        check('a typo of the planted verb gets the did-you-mean line', rc == 2 and 'did you mean `q plantedverb' in so, (rc, so[:300], se))
        # twin: the same call WITHOUT the verb file = the old hard-coded world: an unknown first word is a find question
        rc, so, se = q('plantedverb', 'a', 'b')
        check('twin: without the verb file the word is not a verb (answered as q find)', "'plantedverb' is not a q verb" in se, (rc, so[:200], se))
        rc, so, se = q('-h')
        check('twin: without the verb file `q -h` does not list it', 'plantedverb' not in so, so[:100])
        # a bad verb file: skipped with one warning, everything else still works
        write(d, 'broken.toml', 'name = "broken"\nmodule = \n')
        write(d, 'wrongname.toml', 'name = "other"\nmodule = "qplantedverb"\n')
        write(d, 'extrakey.toml', 'name = "extrakey"\nmodule = "qplantedverb"\nbogus = 1\n')
        rc, so, se = q('plantedverb', 'x', extra=d)
        check('bad verb files are skipped with a warning each; the good verb still runs', rc == 0 and so.strip() == 'planted ok x'
              and 'verb file broken.toml skipped' in se and 'verb file wrongname.toml skipped' in se and 'verb file extrakey.toml skipped' in se, (rc, so, se))


def lints():
    import qverbs
    entries, warns = qverbs.load(extra='')
    check('every verb file of tools/q/verbs loads (no warning)', not warns, warns)
    src = open(Q, encoding='utf-8').read()
    names = [e['name'] for e in entries]
    check('the registry holds the migrated verbs', len(names) >= 40 and 'slice' in names and 'fieldsdiff' in names, len(names))
    for e in entries:
        if importlib.util.find_spec(e['module']) is None:
            check('verb %s: module %s exists in tools/q' % (e['name'], e['module']), False)
        us, rs = e.get('usage_slot', 'new'), e.get('route_slot', 'new')
        if e.get('usage') and '{{usage:%s}}' % us not in src:
            check('verb %s: usage slot %s has a marker in q.py' % (e['name'], us), False)
        if e.get('route') and '{{route:%s}}' % rs not in src:
            check('verb %s: route slot %s has a marker in q.py' % (e['name'], rs), False)
    check('every verb\'s module, usage slot and route slot exist (no FAIL above)', not [f for f in fails if f.startswith('verb ')], fails)
    import q as Qm
    check('no verb is listed twice in VERBS (a verb file named like a hand-wired verb)', len(set(Qm.VERBS)) == len(Qm.VERBS),
          sorted(v for v in set(Qm.VERBS) if Qm.VERBS.count(v) > 1))
    check('every marker of q.py is filled or removed in the printed texts', '{{usage:' not in Qm.__doc__ and '{{route:' not in Qm.ROUTING, '')
    check('the registered usage lines reach `q -h` text (slice)', 'slice sub_X+OFF REG|[mem]' in Qm.__doc__ and 'slice sub_X+OFF REG|[mem] [--expr]' in Qm.ROUTING, '')


if __name__ == '__main__':
    lints()
    planted()
    print('\nqverbs_selftest: %d failed' % len(fails) if fails else '\nqverbs_selftest: all passed')
    sys.exit(1 if fails else 0)
