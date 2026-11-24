r"""SubagentStart hook: a private scratch folder per agent (Session 108, lane F).

Claude Code passes hook JSON on stdin (agent_id, agent_type, session_id, cwd, hook_event_name, and from v2.1.257 scratchpad_dir).
Makes <scratchpad_dir>\agents\<agent_id>\ (no scratchpad_dir: %TEMP%\claude-agent-scratch\<session_id>\<agent_id>\) and prints the
additionalContext JSON telling the agent about it. agent_id / session_id are sanitised to [A-Za-z0-9_-]; a relative scratchpad_dir is
ignored (TEMP fallback); stdin is read as bytes so a UTF-8 path is not decoded with the cp1252 code page. Any error: print nothing, exit 0
(never blocks an agent).

    py -I tools\harness\agent_scratch_hook.py            (hook, JSON on stdin)
    py -I tools\harness\agent_scratch_hook_selftest.py
"""
import json
import os
import re
import sys
import tempfile


def clean(s, default):
    return re.sub(r'[^A-Za-z0-9_-]', '', str(s or ''))[:80] or default


def build(data):
    agent = clean(data.get('agent_id'), 'agent')
    sp = data.get('scratchpad_dir')
    if sp and isinstance(sp, str) and os.path.isabs(sp):   # a relative path would land in the hook's cwd (the repo)
        path = os.path.join(sp, 'agents', agent)
    else:
        path = os.path.join(tempfile.gettempdir(), 'claude-agent-scratch', clean(data.get('session_id'), 'session'), agent)
    path = os.path.abspath(path)
    os.makedirs(path, exist_ok=True)
    msg = (f'Your private scratch folder is {path}. Put every temporary file and helper script there, never in the shared '
           'scratchpad root; never run a script you did not write.')
    return {'hookSpecificOutput': {'hookEventName': 'SubagentStart', 'additionalContext': msg}}


def main():
    try:
        out = build(json.loads(sys.stdin.buffer.read()))   # bytes: json detects UTF-8; text stdin is cp1252 on Windows
        sys.stdout.write(json.dumps(out))
    except Exception:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
