r"""PreToolUse hook (Grep | Bash | PowerShell): the q-first gate (Session 103, 2026-11-17).

Why: q_usage_audit.py over 409 agents: reviewers used q for 4 % of their code searches, Sonnet 8 %, other Opus 21 %; after a written
rule, a non-blocking nudge and self-refreshing indexes, the PWI builder still ran 0 q calls in 85 minutes. Jarrod: "Is there maybe a tool
we could build that forces it to acknowledge q? Obviously it would still need to grep from time to time".

Rule, per session (state in %TEMP%\wwe_qgate_<session>.json):
  * a q command (any `tools\q\q.py ...`) is always allowed and resets the counter;
  * `q.py missed "<question>"` (logs the question to out\harness\q_missed.log) also grants 3 raw searches;
  * a RAW code search (Grep tool, or grep / rg / findstr / Select-String / sed -n / awk / cat / head / tail over src\ tools\ docs\
    generated\ thirdparty\) is refused while the session has made no q call yet, and again after 6 raw searches in a row with no q call
    in between; granted credits are spent first.
Fails open on any error. Never blocks a q command, a build, a gate, git, or a read of logs / out\.

    py -I tools\harness\q_gate_hook.py --selftest
"""
import json
import os
import re
import sys
import tempfile

RAW_LIMIT = 6
MISSED_CREDITS = 3
CODE_PATH = re.compile(r'(?:^|[\s"\'=(/\\])(src|tools|docs|generated|thirdparty)[\\/]')
RAW_CMD = re.compile(r'(^|[|;&(\s])(grep|rg|findstr|Select-String|sed\s+-n|awk|cat|head|tail)\b')
Q_RE = re.compile(r'tools[\\/]+q[\\/]+q\.py')
MISSED_RE = re.compile(r'tools[\\/]+q[\\/]+q\.py\s+missed\b')

MSG_FIRST = ("q first (CLAUDE.md Agents; tools\\harness\\q_gate_hook.py): this session has not asked q yet, so raw code searches are "
             "refused. Ask q: `py tools\\q\\q.py ours fn|def|refs|callers|callees|writes|reads|covers NAME` (our code), "
             "`q decomp sub_X --fields`, `q field OFF`, `q const N`, `q fact WORDS`, `q fn|callers|str sub_X` (game). If q cannot answer "
             "your question, run `py tools\\q\\q.py missed \"<the question>\"` (it is logged as a q gap) and you get 3 raw searches.")
MSG_AGAIN = ("q first: {n} raw code searches in a row with no q call. Ask q again (see q.py --help), or log what q cannot answer: "
             "`py tools\\q\\q.py missed \"<the question>\"` (grants 3 raw searches).")


def is_raw(tool, inp):
    if tool == 'Grep':
        p = str(inp.get('path', '')) + ' ' + str(inp.get('glob', ''))
        return not p.strip() or bool(CODE_PATH.search(' ' + p)) or p.strip().startswith(('src', 'tools', 'docs', 'generated'))
    if tool in ('Bash', 'PowerShell'):
        c = inp.get('command', '') or ''
        if Q_RE.search(c):
            return False
        # judged per pipeline / list segment: a segment is a raw search only when IT starts with a search tool AND names a code path
        # (S103: `py edit.py docs/x.md && ... | tail -1` was counted as a search because `tail` and `docs/` sat in different segments)
        for seg in re.split(r'\|\||&&|[|;\n]', c):
            seg = seg.strip()
            seg = re.sub(r'^(cd\s+\S+|timeout\s+\d+)\s*', '', seg).strip()
            if re.match(r'(grep|rg|findstr|Select-String|sed\s+-n|awk|cat|head|tail|Get-Content)\b', seg):
                # a cat that only WRITES (`cat >> docs/x.md <<'EOF'`) reads nothing: drop output redirects and heredoc markers first (tee is a write: never matched)
                body = re.sub(r'\d?>>?\s*\S+|<<-?\s*[\'"]?\w+[\'"]?', ' ', seg)
                if CODE_PATH.search(' ' + body):
                    return True
        return False
    return False


def decide(state, tool, inp):
    """Returns (allow, message, new_state)."""
    st = dict(state)
    c = (inp.get('command', '') or '') if tool in ('Bash', 'PowerShell') else ''
    if Q_RE.search(c):
        st['q'] = st.get('q', 0) + 1
        st['raw_since_q'] = 0
        if MISSED_RE.search(c):
            st['credits'] = st.get('credits', 0) + MISSED_CREDITS
        return True, None, st
    if not is_raw(tool, inp):
        return True, None, st
    if st.get('credits', 0) > 0:
        st['credits'] -= 1
        st['raw_since_q'] = st.get('raw_since_q', 0) + 1
        return True, None, st
    if st.get('q', 0) == 0:
        return False, MSG_FIRST, st
    if st.get('raw_since_q', 0) >= RAW_LIMIT:
        return False, MSG_AGAIN.format(n=st['raw_since_q']), st
    st['raw_since_q'] = st.get('raw_since_q', 0) + 1
    return True, None, st


def main():
    if '--selftest' in sys.argv:
        return selftest()
    try:
        d = json.load(sys.stdin)
        tool = d.get('tool_name', '')
        inp = d.get('tool_input') or {}
        sid = re.sub(r'[^\w-]', '', str(d.get('session_id', 'x')))[:64] or 'x'
        path = os.path.join(tempfile.gettempdir(), 'wwe_qgate_' + sid + '.json')
        try:
            state = json.load(open(path, encoding='utf-8'))
        except (OSError, ValueError):
            state = {}
        ok, msg, st = decide(state, tool, inp)
        try:
            json.dump(st, open(path, 'w', encoding='utf-8'))
        except OSError:
            pass
        if not ok:
            print(msg, file=sys.stderr)
            return 2
    except Exception:
        return 0
    return 0


def selftest():
    bad = []
    st = {}
    g = ('Grep', {'pattern': 'g_on', 'path': 'src/renderer_scene'})
    ok, _, st = decide(st, *g)
    if ok: bad.append('first raw search allowed before any q')
    ok, _, st = decide(st, 'Bash', {'command': 'git log --oneline -3'})
    if not ok: bad.append('git refused')
    ok, _, st = decide(st, 'Bash', {'command': 'grep -c PASS out/gate/x.log'})
    if not ok: bad.append('log read refused')
    ok, _, st = decide(st, 'Bash', {'command': 'py tools\\q\\q.py ours def ApplyStartSettings'})
    if not ok or st.get('q') != 1: bad.append('q call not counted')
    for i in range(RAW_LIMIT):
        ok, _, st = decide(st, *g)
        if not ok: bad.append(f'raw {i + 1} refused within the limit')
    ok, _, st = decide(st, *g)
    if ok: bad.append('raw past the limit allowed')
    ok, _, st = decide(st, 'Bash', {'command': 'py tools/q/q.py missed "who stores t_call_r4"'})
    if not ok or st.get('credits') != MISSED_CREDITS: bad.append('missed did not grant credits')
    for i in range(MISSED_CREDITS):
        ok, _, st = decide(st, 'Bash', {'command': 'grep -rn t_call_r4 src/'})
        if not ok: bad.append(f'credit {i + 1} not honoured')
    ok, _, st = decide({}, 'Bash', {'command': 'sed -n 10,20p src/x.cpp'})
    if ok: bad.append('sed over src allowed before q')
    ok, _, st = decide({}, 'PowerShell', {'command': 'Select-String -Path tools\\gate\\gate.py -Pattern only'})
    if ok: bad.append('Select-String over tools allowed before q')
    for cmd, want in (("cat >> docs/x.md <<'EOF'", False), ('cat docs/x.md', True),
                      ('cat src/a.cpp > out/b.txt', True), ('tee -a docs/x.md', False)):
        if is_raw('Bash', {'command': cmd}) != want:
            bad.append(f'is_raw({cmd!r}) != {want}')
    for b in bad:
        print('FAIL', b)
    print('PASS q_gate_hook selftest' if not bad else f'FAIL {len(bad)}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
