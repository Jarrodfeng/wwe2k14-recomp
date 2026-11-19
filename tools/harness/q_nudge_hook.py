r"""PostToolUse hook (Grep | Bash | PowerShell): after a raw code search, tell the agent which q command answers it (Session 103).

Why: tools\harness\q_usage_audit.py over this session's 409 agents (2026-11-17 21:2x): reviewers used q for 4 % of their code
searches, Sonnet builders / probes 8 %, other Opus agents 21 %, although CLAUDE.md, the builder brief and the agent files say "q first".
A written rule did not hold. Jarrod, 2026-11-17: "we should make sure that rule stands for probes, reviewers, gates, and everything else".

NON-BLOCKING: the search runs and returns as usual; the hook only adds a short note to the agent's context (PostToolUse
additionalContext), at most once every NUDGE_EVERY raw searches per session, so it nags without flooding. Fails open (any error =
no output). Never blocks anything.

    py -I tools\harness\q_nudge_hook.py --selftest
"""
import json
import os
import re
import sys
import tempfile

NUDGE_EVERY = 4
CODE_PATH = re.compile(r'(src|generated|docs|thirdparty|tools)[\\/]')
RAW_CMD = re.compile(r'(^|[|;&(\s])(grep|rg|findstr|Select-String|sed\s+-n|awk)\b')
Q_RE = re.compile(r'tools[\\/]+q[\\/]+q\.py')
SUB_ADDR = re.compile(r'\bsub_[0-9A-Fa-f]{8}\b|\b0x8[0-9A-Fa-f]{7}\b')


def is_raw_search(tool, inp):
    if tool == 'Grep':
        p = str(inp.get('path', '')) + ' ' + str(inp.get('glob', ''))
        return not p.strip() or bool(CODE_PATH.search(p)) or 'src' in p or 'generated' in p
    if tool in ('Bash', 'PowerShell'):
        c = inp.get('command', '') or ''
        return bool(RAW_CMD.search(c)) and bool(CODE_PATH.search(c)) and not Q_RE.search(c)
    return False


def suggestion(tool, inp):
    text = inp.get('pattern', '') if tool == 'Grep' else inp.get('command', '')
    if SUB_ADDR.search(text or '') or 'generated' in (text or '') + str(inp.get('path', '')):
        return ('game code: `py tools\\q\\q.py decomp sub_X [--fields|--callers N]`, `q field OFF [--global G|--vtable V] [--writes]`, '
                '`q callers|callees|str|addr`')
    return ('our code: `py tools\\q\\q.py ours fn|def|refs|callers|callees|writes|reads|covers NAME` (one call, seconds); '
            'lodged facts: `q fact WORDS`')


def main():
    if '--selftest' in sys.argv:
        return selftest()
    try:
        d = json.load(sys.stdin)
        tool = d.get('tool_name', '')
        inp = d.get('tool_input') or {}
        if not is_raw_search(tool, inp):
            return 0
        sid = re.sub(r'[^\w-]', '', str(d.get('session_id', 'x')))[:64] or 'x'
        state = os.path.join(tempfile.gettempdir(), 'wwe_q_nudge_' + sid)
        n = 0
        try:
            n = int(open(state).read().strip() or 0)
        except (OSError, ValueError):
            pass
        n += 1
        try:
            open(state, 'w').write(str(n))
        except OSError:
            pass
        if n % NUDGE_EVERY != 1:
            return 0
        msg = ('q first (CLAUDE.md Agents): this raw search may be one q call. Try ' + suggestion(tool, inp) +
               '. If q cannot answer it, say which question in your hand-back (it becomes a q entry). Audit: tools\\harness\\q_usage_audit.py.')
        print(json.dumps({'hookSpecificOutput': {'hookEventName': 'PostToolUse', 'additionalContext': msg}}))
    except Exception:
        return 0
    return 0


def selftest():
    raw = [('Grep', {'pattern': 'g_on', 'path': 'src/renderer_scene'}), ('Bash', {'command': 'grep -rn LightsSig src/'}),
           ('PowerShell', {'command': 'Select-String -Path src\\x.cpp -Pattern foo'}), ('Grep', {'pattern': 'sub_82BAE370'})]
    not_raw = [('Bash', {'command': 'py tools\\q\\q.py ours refs g_on'}), ('Bash', {'command': 'git log --oneline'}),
               ('Bash', {'command': 'grep -c PASS out/gate/x.log'}), ('Read', {'file_path': 'src/x.cpp'})]
    bad = [x for x in raw if not is_raw_search(*x)] + [x for x in not_raw if is_raw_search(*x)]
    if 'decomp' not in suggestion('Grep', {'pattern': 'sub_82BAE370'}) or 'ours' not in suggestion('Grep', {'pattern': 'g_on', 'path': 'src'}):
        bad.append('suggestion')
    for b in bad:
        print('FAIL', b)
    print('PASS q_nudge_hook selftest' if not bad else 'FAIL %d' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
