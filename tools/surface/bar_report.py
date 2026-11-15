r"""S102 B1: the release bar from a Repeats job: median and range per scene, with each repeat's thermal state (burst_rules.py, R28).
  py -I tools\surface\bar_report.py RUN_DIR      (RUN_DIR holds frames_bryan, frames_bryan_r2 ... frames_brock ..., as bench.ps1 Repeats=N leaves them)
Per scene (the step15 / match / heavy window of each repeat): n read, raw fps median and range, the 110 %-normalised fps median and range (R28 exponent, unconfirmed
as a law), mean W and perf, thermal states of the repeats. THE BAR NUMBER is the median of the SUSTAINED repeats' raw fps (and the normalised one beside it);
no SUSTAINED repeat = 'NO BAR NUMBER'. Repeats refused by bench_line (device lost, a problem line) are listed and left out."""
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_line as bl  # noqa: E402
import burst_rules as br  # noqa: E402


def rng(v):
    return f'{min(v):.1f}-{max(v):.1f}'


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    d = Path(argv[0])
    for label, name in bl.STEPS:
        dirs = sorted(p for p in d.iterdir() if p.is_dir() and re.fullmatch(name + r'(_r\d+)?', p.name))
        if not dirs:
            continue
        rows, refused = [], []
        for p in dirs:
            try:
                s = bl.read_step(label, p, None, True)
            except bl.Refused as e:
                refused.append(f'{p.name}: {str(e)[:120]}')
                continue
            w = s['windows'][0]
            th = w.get('thermal')
            rows.append((p.name, w['n']['fps'], th))
        print(f'{label}: {len(rows)} of {len(dirs)} repeats read' + (f'; refused: {"; ".join(refused)}' if refused else ''))
        for nm, fps, th in rows:
            nrm = br.normalised(fps, th['perf']) if th and th.get('perf') else None
            print(f'  {nm:<26} {fps:5.1f} fps  ' + (f'{th["state"]:<9} {th["w"]:.1f} W {th["perf"]:.0f} %  norm110 {nrm:.1f}' if th and th.get('perf') else 'no heat record'))
        sus = [(f, br.normalised(f, t['perf'])) for _, f, t in rows if t and t['state'] == 'SUSTAINED']
        if rows:
            allf = [f for _, f, _ in rows]
            print(f'  all repeats: median {st.median(allf):.1f} ({rng(allf)})')
        if sus:
            f = [x for x, _ in sus]
            n = [y for _, y in sus]
            print(f'  BAR {label}: SUSTAINED {len(sus)} of {len(rows)}: median {st.median(f):.1f} fps (range {rng(f)}); normalised to 110 % median {st.median(n):.1f} ({rng(n)}; R28 exponent, unconfirmed as a law)')
        else:
            print(f'  NO BAR NUMBER for {label}: no SUSTAINED repeat (BURST / MIXED / COOL are not the bar\'s thermal state)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
