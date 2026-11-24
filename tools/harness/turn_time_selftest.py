r"""Selftest of turn_time.py: a synthetic transcript with known timestamps (T0 = 10:00:00):
  prompt 0 s
  turn 1  Read, block at 6 s, result 8 s                 model 6, tool 2   (usage: out 60, in 100, cache creation 1000)
  turn 2  q.py find + a script, blocks 12 s / 13 s, results 14 s / 20 s    model 5 (8 -> 13), tool 7 (the slowest; the SUM would be 8)
  turn 3  Write docs\research\x.md at 25 s, result 26 s  model 5, tool 1   (usage: out 200 of which 80 thinking, in 4, read 1100, creation 50)
  turn 4  SubagentHandback at 30 s, no result            model 4, tool 0
wall 30, model 20, tool 10, idle 0. A planted twin (the calls of a reply summed, not the slowest) must fail the same assertions.
    py -I tools\harness\turn_time_selftest.py"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import turn_time  # noqa: E402


def iso(s):
    return '2026-11-24T10:%02d:%02d.000Z' % (s // 60, s % 60)


def build(path):
    recs = [dict(type='user', timestamp=iso(0), message=dict(role='user', content='the brief'))]

    def asst(mid, t, block, usage=None):
        m = dict(id=mid, role='assistant', content=[block])
        if usage:
            m['usage'] = usage
        recs.append(dict(type='assistant', timestamp=iso(t), message=m))

    def res(tid, t):
        recs.append(dict(type='user', timestamp=iso(t), message=dict(role='user', content=[dict(type='tool_result', tool_use_id=tid, content='x')])))

    u1 = dict(input_tokens=100, cache_read_input_tokens=0, cache_creation_input_tokens=1000, output_tokens=60)
    u3 = dict(input_tokens=4, cache_read_input_tokens=1100, cache_creation_input_tokens=50, output_tokens=200, output_tokens_details=dict(thinking_tokens=80))
    asst('m1', 5, dict(type='thinking', thinking=''), u1)
    asst('m1', 6, dict(type='tool_use', id='t1', name='Read', input=dict(file_path='C:\\a\\b\\file.cpp')), u1)
    res('t1', 8)
    asst('m2', 12, dict(type='tool_use', id='t2', name='Bash', input=dict(command='py tools\\q\\q.py find Foo')))
    asst('m2', 13, dict(type='tool_use', id='t3', name='Bash', input=dict(command='py -I tools\\x\\run_it.py --go')))
    res('t2', 14)
    res('t3', 20)
    asst('m3', 25, dict(type='tool_use', id='t4', name='Write', input=dict(file_path='docs\\research\\s110\\x.md', content='the note begins here')), u3)
    res('t4', 26)
    asst('m4', 30, dict(type='tool_use', id='t5', name='SubagentHandback', input=dict(message='done')))
    with open(path, 'w', encoding='utf-8') as f:
        for r in recs:
            f.write(json.dumps(r) + '\n')


def check(a):
    """every summary number; raises AssertionError on the first that is wrong"""
    def eq(what, got, want):
        assert got == want, '%s: got %r, want %r' % (what, got, want)
    eq('wall', a['wall'], 30.0)
    eq('turns', a['n'], 4)
    eq('model per turn', [t['model'] for t in a['turns']], [6.0, 5.0, 5.0, 4.0])
    eq('tool per batch', [t['tool'] for t in a['turns']], [2.0, 7.0, 1.0, 0.0])
    eq('model total', a['model'], 20.0)
    eq('tool total', a['tool'], 10.0)
    eq('idle', a['idle'], 0.0)
    eq('model median / p90 / max', a['mstat'], (5.0, 6.0, 6.0))
    eq('tool median / p90 / max', a['tstat'], (2.0, 7.0, 7.0))
    eq('batches', a['nbatch'], 3)
    eq('output tokens', a['out'], 260)
    eq('turns with usage', a['out_turns'], 2)
    eq('tokens per s', round(a['tps'], 3), round(260 / 11.0, 3))
    eq('input first / mid / last', (a['in_first'], a['in_mid'], a['in_last']), (1100, 1154, 1154))
    eq('time before first tool call', a['pre_tool'], 6.0)
    eq('hand-back model time', a['hand_model'], 4.0)
    eq('thinking tokens of turn 3', a['turns'][2]['think'], 80)
    eq('categories', {k: (v[0], v[1]) for k, v in a['by_cat'].items()},
       {'lookups only (q / Read / Grep / Glob)': (1, 6.0), 'ran scripts (Bash / PowerShell)': (1, 5.0),
        'wrote the note (docs\\research)': (1, 5.0), 'final hand-back': (1, 4.0)})
    eq('kinds', {k: (v[0], v[1]) for k, v in a['by_kind'].items()},
       {'Read': (1, 2.0), 'sh:q find': (1, 1.0), 'sh:py run_it.py': (1, 7.0), 'Write': (1, 1.0)})
    eq('call summary', turn_time.summarize_call('Bash', dict(command='py tools\\q\\q.py find Foo')), 'q find Foo')
    eq('summary cap', len(turn_time.summarize_call('Bash', dict(command='x' * 200))), 60)


def main():
    fd, path = tempfile.mkstemp(suffix='.jsonl')
    os.close(fd)
    try:
        build(path)
        check(turn_time.analyse(path))
        a = turn_time.analyse(path)
        out = turn_time.rows(a)
        assert len(out) == 12
        caught = False
        try:
            check(turn_time.analyse(path, tool_agg=sum))       # the planted twin: tool time as the SUM of the parallel calls
        except AssertionError as e:
            caught = True
            twin = str(e)
        assert caught, 'the twin (tool time summed) was NOT caught'
        print('turn_time selftest: ok (every summary number checked; the sum-of-parallel-calls twin was caught: %s)' % twin)
        return 0
    finally:
        os.unlink(path)


if __name__ == '__main__':
    sys.exit(main())
