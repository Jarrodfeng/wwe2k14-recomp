# entry: main
r"""Selftest for agent_scratch_hook.py (folders under out\laneF only).  py -I tools\harness\agent_scratch_hook_selftest.py"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.join(HERE, 'agent_scratch_hook.py')
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', 'out', 'laneF', 'scratch_selftest'))


def run(payload, raw=None, env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    r = subprocess.run([sys.executable, '-I', HOOK], input=raw if raw is not None else json.dumps(payload),
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stdout


def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(ROOT)
    bad = []

    def expect(name, cond):
        print('ok  ' if cond else 'FAIL', name)
        if not cond:
            bad.append(name)

    # with scratchpad_dir
    sp = os.path.join(ROOT, 'pad')
    rc, out = run({'agent_id': 'ab12-X_y', 'session_id': 's1', 'scratchpad_dir': sp})
    want = os.path.abspath(os.path.join(sp, 'agents', 'ab12-X_y'))
    j = json.loads(out)
    ctx = j['hookSpecificOutput']['additionalContext']
    expect('scratchpad exit 0', rc == 0)
    expect('scratchpad folder made', os.path.isdir(want))
    expect('scratchpad json', j['hookSpecificOutput']['hookEventName'] == 'SubagentStart' and ctx.startswith('Your private scratch folder is ' + want + '.'))
    # without scratchpad_dir: TEMP redirected into out
    tmp = os.path.join(ROOT, 'tmp')
    os.makedirs(tmp)
    rc, out = run({'agent_id': 'zz9', 'session_id': 'sess 2'}, env_extra={'TEMP': tmp, 'TMP': tmp, 'TMPDIR': tmp})
    want2 = os.path.abspath(os.path.join(tmp, 'claude-agent-scratch', 'sess2', 'zz9'))
    expect('temp fallback folder made', rc == 0 and os.path.isdir(want2))
    expect('temp fallback json', want2 in json.loads(out)['hookSpecificOutput']['additionalContext'])
    # hostile id
    rc, out = run({'agent_id': '..\\..\\x', 'session_id': 's', 'scratchpad_dir': sp})
    want3 = os.path.abspath(os.path.join(sp, 'agents', 'x'))
    expect('hostile id stays inside', rc == 0 and os.path.isdir(want3) and not os.path.exists(os.path.join(ROOT, 'x')))
    expect('hostile id json path', want3 in json.loads(out)['hookSpecificOutput']['additionalContext'])
    # S108 review: Claude Code writes UTF-8 on stdin; a non-ASCII scratchpad_dir (a user profile like C:\Users\José) must not be
    # decoded with the cp1252 console code page into a different folder (C:\Users\JosÃ©)
    sp_u = os.path.join(ROOT, 'pad-\u00e9')
    r = subprocess.run([sys.executable, '-I', HOOK], input=json.dumps({'agent_id': 'u1', 'scratchpad_dir': sp_u}, ensure_ascii=False).encode('utf-8'),
                       capture_output=True)
    want4 = os.path.join(sp_u, 'agents', 'u1')
    expect('utf-8 scratchpad_dir', r.returncode == 0 and os.path.isdir(want4) and not os.path.exists(os.path.join(ROOT, 'pad-\u00c3\u00a9')))
    expect('utf-8 json path', r.returncode == 0 and want4 in json.loads(r.stdout.decode('ascii'))['hookSpecificOutput']['additionalContext'])
    # a relative scratchpad_dir would land in the hook's cwd (the repo): fall back to TEMP instead
    cwd = os.path.join(ROOT, 'cwd')
    os.makedirs(cwd)
    r = subprocess.run([sys.executable, '-I', HOOK], input=json.dumps({'agent_id': 'r1', 'session_id': 's3', 'scratchpad_dir': 'rel'}),
                       capture_output=True, text=True, cwd=cwd, env=dict(os.environ, TEMP=tmp, TMP=tmp, TMPDIR=tmp))
    expect('relative scratchpad_dir not used', r.returncode == 0 and not os.path.exists(os.path.join(cwd, 'rel'))
           and os.path.isdir(os.path.join(tmp, 'claude-agent-scratch', 's3', 'r1')))
    # errors print nothing, exit 0
    rc, out = run(None, raw='not json')
    expect('bad json: silent exit 0', rc == 0 and out == '')
    blocker = os.path.join(ROOT, 'blocker')
    open(blocker, 'w').write('file, not a folder')
    rc, out = run({'agent_id': 'q', 'scratchpad_dir': blocker})
    expect('unmakeable folder: silent exit 0', rc == 0 and out == '')
    print('PASS agent_scratch_hook selftest' if not bad else f'FAIL {bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
