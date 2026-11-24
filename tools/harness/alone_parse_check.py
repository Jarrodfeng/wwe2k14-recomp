"""S106 lane C5: the cheap static check of the files the alone request passes through (no run lock, no admit state, no game).

  py -I tools\\harness\\alone_parse_check.py

1. every PowerShell file of the change parses (tools\\runlock.ps1, tools\\harness\\runlock_alone.ps1, tools\\bench\\run_list.ps1, tools\\admit\\Admit.ps1);
2. tools\\bench\\run_list.ps1 declares `[switch]$Alone` EXACTLY (tools\\check\\check.py's preflight refuses a run_list without it) and maps a `alone: NO` refusal to exit 3;
3. tools\\harness\\runlock.py compiles and ensure_lock() takes `alone` and passes `-Alone` to `runlock.ps1 run`;
4. docs\\harness\\runlock.md and docs\\agents\\admit.md carry the "Alone (S106 C5)" / "alone pause" sections.
Exit 0 = all hold; 1 = one does not (each is printed). Planted twin: `--plant no_switch` reads run_list.ps1 with the switch removed and must exit 1.
"""
import os
import re
import subprocess
import sys

# entry: main
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    plant = len(sys.argv) > 2 and sys.argv[1] == '--plant' and sys.argv[2]
    bad = []
    ps = ['tools/runlock.ps1', 'tools/harness/runlock_alone.ps1', 'tools/bench/run_list.ps1', 'tools/admit/Admit.ps1']
    files = ','.join("'%s'" % os.path.join(ROOT, p) for p in ps)
    script = ("$bad = 0; foreach ($f in @(%s)) { $e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$null, [ref]$e); "
              "foreach ($x in $e) { Write-Output ($f + ':' + $x.Extent.StartLineNumber + ': ' + $x.Message); $bad++ } }; exit $(if ($bad) { 1 } else { 0 })") % files
    r = subprocess.run(['pwsh', '-NoProfile', '-Command', script], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        bad.append('PowerShell parse errors: ' + r.stdout.strip()[:400])
    rl = read('tools/bench/run_list.ps1')
    if plant == 'no_switch':
        rl = rl.replace('[switch]$Alone,', '')
    if not re.search(r'\[switch\]\$Alone\b', rl):
        bad.append('run_list.ps1 has no [switch]$Alone (check.py preflight refuses it)')
    if 'alone: NO' not in rl:
        bad.append('run_list.ps1 does not map the `alone: NO` refusal to exit 3')
    rp = read('tools/harness/runlock.py')
    try:
        compile(rp, 'tools/harness/runlock.py', 'exec')   # (no .pyc written)
    except SyntaxError as e:
        bad.append('runlock.py does not compile: %s' % e)
    if not re.search(r'def ensure_lock\([^)]*alone=False', rp) or "'-Alone'" not in rp:
        bad.append('runlock.py ensure_lock has no alone parameter / does not pass -Alone')
    if 'Alone (S106 C5' not in read('docs/harness/runlock.md') or 'The alone pause (S106 C5' not in read('docs/agents/admit.md'):
        bad.append('the doc sections "Alone (S106 C5)" / "The alone pause (S106 C5)" are missing')
    for b in bad:
        print('FAIL ' + b)
    print('alone_parse_check: %s' % ('FAIL (%d)' % len(bad) if bad else 'all hold'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
