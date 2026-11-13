r"""Lane 158 (review B3 / S2): make the leg-3b route (a created wrestler in a normal match) for ONE real CAW id, and print the exact commands. Offline; starts nothing.

  py -I tools\census\roster_caw_route.py CAWID [--selftest]

CAWID is the created superstar's character id, 0-99 (the "100 CAS ids", docs\formats\chetc_roster_record.md), read from the editor's list / roster screen after leg 3a.
There is NO default: the template (scenes\s101_roster_caw_match.tmpl) is not a route. Writes out\s101\routes\s101_roster_caw_match_<ID>.txt (game-free text) and prints
  1. the run_list command, with a STAMPED case dir (a rerun can never read an older zip);
  2. the proof command: roster_proof with --picks 343 --caw --forbid-bodies 324 --expect-seat 0=<ID> --run-log <the run's log>.
The proof refuses the pass unless the run log says "participant 0 = id <ID>" and no "skipped" line, Bryan (ch324) is not in the hook's BODY list, and the unexplained body is ours.
"""
import datetime
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
TEMPLATE = HERE / 'tools' / 'input_scripts' / 'scenes' / 's101_roster_caw_match.tmpl'


def parse_id(text):
    """-> int 0-99 or raises ValueError (no default, no placeholder)."""
    if text is None or not text.strip().isdigit():
        raise ValueError(f'CAWID must be a number 0-99, got {text!r}')
    n = int(text)
    if not 0 <= n <= 99:
        raise ValueError(f'CAWID {n}: created superstars are ids 0-99 (an id outside 0-499 is skipped by the launcher and leaves Bryan in the seat; 100-499 are stock wrestlers)')
    return n


def make(cid, main, stamp):
    body = TEMPLATE.read_text(encoding='utf-8')
    if '@CAWID@' not in body:
        raise ValueError('template lost its @CAWID@ marker')
    out = Path(main) / 'out' / 's101' / 'routes' / f's101_roster_caw_match_{cid}.txt'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(body.replace('@CAWID@', str(cid)), encoding='utf-8')
    case = Path(main) / 'out' / 's101' / 'runs' / f'caw_match_{cid}_{stamp}'
    run = (f"& .\\tools\\bench\\run_list.ps1 -Scripts @('{out}') -Data userdata_caw_s101 "
           f"-Extra '--wwe2k14_case=true --wwe2k14_case_dir={case}' -WaitSeconds 600")
    proof = (f"py -I tools\\census\\roster_proof.py {case}\\<case zip> --frames 60,150,240,300 --arm both --picks 343 --caw --forbid-bodies 324 "
             f"--expect-seat 0={cid} --run-log <out\\build\\win-amd64-release\\logs\\wwe2k14_N.log of that run>")
    return out, run, proof


def selftest():
    ok = True
    for t, want in (('0', 0), ('42', 42), ('99', 99), ('100', None), ('-1', None), ('', None), ('x', None), (None, None), ('999', None)):
        try:
            got = parse_id(t)
        except ValueError:
            got = None
        good = got == want
        ok = ok and good
        print(('ok   ' if good else 'FAIL ') + f'parse_id({t!r}) = {got}')
    body = TEMPLATE.read_text(encoding='utf-8')
    good = body.count('@CAWID@') == 1 and TEMPLATE.suffix == '.tmpl'
    ok = ok and good
    print(('ok   ' if good else 'FAIL ') + 'the template carries one @CAWID@ marker and is not a .txt route (nothing can run it raw)')
    print('roster_caw_route selftest: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--selftest':
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        cid = parse_id(sys.argv[1])
    except ValueError as x:
        print('REFUSED: ' + str(x))
        return 2
    import roster_materials as RM
    out, run, proof = make(cid, RM.main_checkout(), datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    print(f'route: {out}\nrun:   {run}\nproof: {proof}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
