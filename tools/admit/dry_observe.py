r"""The Python pools on the REAL admit.exe in observe mode (lane CQW; run by tools\admit\wiring_dry.ps1, which sets WWE_ADMIT_EXE / WWE_ADMIT_MODE=observe /
WWE_ADMIT_DIR (a temp state folder) / WWE_ADMIT_CONFIG). Each wired pool's kind at today's default want must get the full want (`observed`) and leave one
history row with queue_mode observe and a would_grant. 0 load: no pool is started."""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import admit_py  # noqa: E402

# (site, kind, today's want: the default the pool line sizes itself with, timeout_s as wired)
POOLS = [('tools/gate/game_syntax.py (the --changed pool)', 'syntax.worker', 8, 120),
         ('tools/gate/game_syntax.py (the fixed-list pool)', 'syntax.worker', 4, 120),
         ('tools/q/build_ours.py', 'q.parse_worker', 8, 300),
         ('tools/q/ours_overlay.py', 'q.parse_worker', 16, 20),
         ('tools/scene/keep_stream_off_link.py', 'cc.game_src', 12, 300),
         ('tools/perf/guest_flags_exp.py', 'cc.generated', 8, 300),
         ('tools/w3/diff/run.py', 'cc.game_src', 4, 300)]


def rows():
    h = Path(os.environ['WWE_ADMIT_DIR']) / 'history.tsv'
    lines = [x for x in h.read_text(encoding='utf-8').splitlines() if x.strip()] if h.exists() else []
    if len(lines) < 2:
        return []
    head = lines[0].split('\t')
    return [dict(zip(head, x.split('\t'))) for x in lines[1:]]


def main():
    bad = 0
    for site, kind, want, tmo in POOLS:
        before = len(rows())
        with admit_py.hold(kind, want, timeout_s=tmo) as n:
            pass
        new = rows()[before:]
        r = new[-1] if new else {}
        good = int(n) == want and n.why == 'observed' and r.get('queue_mode') == 'observe' and r.get('kind') == kind and str(r.get('would_grant', '')).isdigit()
        print(f'{"SAME" if good else "FAIL"}  observe mode, the REAL exe: {site}: {kind} x{want} -> {int(n)} ({n.why}); history row queue_mode '
              f'{r.get("queue_mode")}, outcome {r.get("outcome")}, would_grant {r.get("would_grant")} ({r.get("would_reason", "")})', flush=True)
        bad += 0 if good else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
