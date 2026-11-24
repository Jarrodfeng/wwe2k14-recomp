#!/usr/bin/env python3
r"""Shell habits the written rules forbid, refused before they run (S110, 2026-11-22; called from no_long_sleep_hook.py's main).

Jarrod, S110: "Agents bypassing q and running sed and heredoc are problematic, your habits are bypassing direct instructions";
"let's go for your 4 proposals, last resort is right as they've bypassed a lot". This is a GUARD (CLAUDE.md rule 6: a band-aid on
model habit, standing in for agents that follow their definitions): the rules below were written in every agent definition and
were broken in every session measured (S110: 24+ q cuts, sed -i edits, heredoc scripts, shell reads of source files).

The four refusals, each with the thing to do instead:
  1. q output cut:   q.py ... | head / tail / Select-Object -First / -Last
  2. shell edits:    sed -i, perl -pi, or a redirect / Set-Content / Out-File into a source file under src, tools, docs, .claude
  3. inline scripts: python fed by a heredoc or a here-string, a file written by `cat > F <<EOF`, a `py -c` over 200 characters or lines
  4. shell reads:    sed -n / cat / head / tail / type / Get-Content of a source file under src, tools, docs

  py -I tools\harness\habit_hook.py --selftest
ENABLED below switches the whole thing; why_refused() never raises (the caller fails open)."""
import re
import sys

ENABLED = True           # on since 2026-11-22 22:07 (S110), after the lanes running when it was written had handed back

SRC = r'''(?:^|[\s"'=(\\/])(?:src|tools|docs|\.claude)[\\/][^\s"'|;&<>()]*\.(?:py|h|hpp|c|cpp|md|ps1|toml|json|txt)\b'''
PY = r'\b(?:py|python3?|python\.exe)\b'

MSG = {
    'cut': ("q output is never cut: every q list prints its total and the exact call for the rest, and a cut causes a re-ask. "
            "Run the q call without head / tail / Select-Object -First, or narrow the question (--in PATH, a more exact verb)."),
    'edit': ("Source files are edited with the Edit / Write tools, never through sed -i, perl -pi or a shell redirect: the tools "
             "check the match and the harness tracks the file."),
    'inline': ("A script is a FILE: create it with Write (in your scratch folder), then run it by path. No heredoc, no here-string "
               "piped to python, no long `py -c`: quoting through a shell has corrupted scripts before (S109: a silent \\b)."),
    'read': ("Read source files with `py tools\\q\\q.py multi \"show FILE:N-M\" \"show FILE2:N-M\" ...` (several ranges in one call) "
             "or the Read tool, not sed -n / cat / head / tail / Get-Content."),
}


def why_refused(cmd):
    """(kind, found) for the first forbidden habit in cmd, else None."""
    if not ENABLED or not cmd:
        return None
    # 1. q output cut
    m = re.search(r'q[\\/]q\.py(?:[^;&\n]|>&)*\|\s*(head|tail|Select-Object\s+-(?:First|Last)|select\s+-(?:First|Last))\b', cmd, re.I)
    if m:
        return 'cut', m.group(1)
    # 2. shell edits of source files
    m = re.search(r'\bsed\s+(?:-[a-zA-Z]+\s+)*-[a-zA-Z]*i\b|\bperl\s+-[a-zA-Z]*p[a-zA-Z]*i\b|\bperl\s+-[a-zA-Z]*i[a-zA-Z]*p\b', cmd)
    if m:
        return 'edit', m.group(0)
    m = re.search(r'(?:(?<![0-9&>])>>?|\b(?:Set-Content|Out-File|Add-Content)\b[^|;&\n]*?)\s*["\']?' + SRC.replace('(?:^|[\\s"\'=(\\\\/])', '(?:[A-Za-z]:)?[^\\s"\'|;&<>()]*?'), cmd, re.I)
    if m and '<<' not in cmd:
        return 'edit', m.group(0).strip()[:80]
    # 3. inline scripts
    for line in cmd.split('\n'):
        if re.search(r'<<-?\s*["\']?\w', line):
            if re.search(PY + r'[^|;&]*<<', line):
                return 'inline', 'python fed by a heredoc'
            if re.search(r'\b(?:cat|tee)\b', line) and re.search(r'>\s*[^&\s>]|\btee\s+\S', line):
                return 'inline', 'a file written through a heredoc'
    if re.search(r'''@["']\s*\n[\s\S]*?\n["']@\s*\|\s*''' + PY, cmd):
        return 'inline', 'a here-string piped to python'
    m = re.search(PY + r'''[^|;&\n]*?\s-c\s+(["'])''', cmd)
    if m:
        q = m.group(1)
        end = cmd.find(q, m.end())
        body = cmd[m.end():end if end >= 0 else len(cmd)]
        if len(body) > 200 or '\n' in body:
            return 'inline', 'py -c of %d characters' % len(body)
    # 4. shell reads of source files
    # the reader must be the COMMAND (start, or after | ; & ( or a newline), not a word inside another command's arguments
    # (S110: `q.py multi "ours text tail --in tools/q/x.py"` was refused as a `tail`)
    m = re.search(r'(?:^|[|;&(\n])\s*(sed\s+-n|cat|head|tail|type|Get-Content|gc)\b(?![^|;&\n]*<<)[^|;&\n]*?' + SRC, cmd, re.I)
    if m:
        return 'read', m.group(1)
    return None


def message(cmd):
    """The refusal text for cmd, or None."""
    try:
        r = why_refused(cmd)
    except Exception:
        return None
    if not r:
        return None
    return 'Refused by tools\\harness\\habit_hook.py (%s): %s' % (r[1], MSG[r[0]])


def selftest():
    global ENABLED
    ENABLED = True
    refuse = [
        ('cut', r'py tools\q\q.py callers 0x82BDB460 | Select-Object -First 8'),
        ('cut', 'py tools/q/q.py multi "ours refs X" | head -40'),
        ('cut', 'cd /c/x && py tools/q/q.py decomp sub_82000000 --fields 2>&1 | tail -20'),
        ('edit', "cd /c/wt && sed -i \"729s/a/b/\" tools/q/qx7.py"),
        ('edit', "sed -n -i '3d' docs/x.md"),
        ('edit', r'Set-Content tools\harness\x.py $text'),
        ('edit', 'echo "x = 1" >> src/a.h'),
        ('inline', "cd /c/wt && python - <<'EOF'\nimport re\nEOF"),
        ('inline', "cat > /tmp/s.py <<'EOF'\nprint(1)\nEOF"),
        ('inline', "cat <<EOF > scratch/s.py\nprint(1)\nEOF"),
        ('inline', 'py -c "' + 'x=1;' * 80 + '"'),
        ('inline', 'py -I -c "import json\nprint(1)"'),
        ('inline', "@'\nprint(1)\n'@ | py -"),
        ('read', 'cd /c/wt && sed -n 20,86p tools/q/build_index.py'),
        ('read', r'Get-Content docs\DECISIONS.md -Tail 6'),
        ('read', 'cat C:/Users/J/wwe2k14-recomp/src/coverage.h | wc -l'),
        ('read', 'head -50 tools/q/q.py'),
    ]
    allow = [
        r'py tools\q\q.py callers 0x82BDB460',
        'py tools/q/q.py multi "ours refs X" "show src/a.h:10-40"',
        r'py -I tools\harness\lane_time.py 2>&1 | Select-Object -Last 40',
        "git commit -q -m \"$(cat <<'EOF'\nS110: a message\n\nCo-Authored-By: Claude <noreply@anthropic.com>\nEOF\n)\"",
        'git commit -q -am "x" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"; git log --oneline -1',
        'py -c "print(1)"',
        r'py -I tools\q\qicall_selftest.py 2>&1 | Select-Object -Last 4',
        'cat out/s110/icall/build_index.log',
        r'Get-Content out\bench\run_1\log.txt -Tail 30',
        'git diff --stat s110...branch; git diff s110...branch -- tools/q/q.py',
        r'py tools\q\build_index.py > out\s110\build.log 2>&1',
        'sed -n 2527,2580p generated/default/wwe2k14_recomp.36.cpp',
        r'Copy-Item out\s110\feed\feed_fields.py tools\q\qfeed.py',
        'grep -n "vt_ok" tools/q/qx7.py',
        'ls tools/q/ | grep -i selftest',
        r'py tools\q\q.py multi "ours text lr --in tools/q/qseen.py" "ours text tail --in tools/q/qicall_check.py"',
        'py tools/q/q.py ours text cat head --in src/coverage.h',
    ]
    bad = []
    for kind, c in refuse:
        r = why_refused(c)
        if not r or r[0] != kind:
            bad.append(('should refuse as ' + kind, c, r))
    for c in allow:
        r = why_refused(c)
        if r:
            bad.append(('should allow', c, r))
    ENABLED = False
    if why_refused(refuse[0][1]) is not None:
        bad.append(('ENABLED = False must refuse nothing', refuse[0][1], None))
    for b in bad:
        print('FAIL', b[0], '|', repr(b[1])[:120], '->', b[2])
    print('habit_hook selftest: %d refuse, %d allow, %d problems' % (len(refuse), len(allow), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(selftest() if '--selftest' in sys.argv else 0)
