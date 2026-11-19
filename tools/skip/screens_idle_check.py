r"""S103 lane E7: why a match frame's screens are "incomplete" (run 26), offline, 0 game runs.

  py -I tools\skip\screens_idle_check.py CASE_OR_CAPTURE LOG [--plant movie_any]

The census said `screens_complete 3512 incomplete 3949 shown_without_row 3949` and `screens_live rows_bound 0 ... flushes 2857 copied 0 ... no_stage_skipped 2857`. The cause (docs\renderer\STAGE_LIVE_WRITERS.md
lane E7): the match plays NO movie, so nothing flushes a display set and no screen ever gets a row, while the gate waits for a row for every shown screen. Three facts, each checked:
  1. the stream: the case's draws hold no movie-class draw (DrawOrigin class 5) and no draw at the screen pass 0x82C0F4BC in ANY frame (the game draws no video plane: there is no picture to wait for);
  2. the log's last [screens_live ...]: flushes == no_stage_skipped and copied == 0 (every flush was before the stage existed: the boot logo; none in the match);
  3. the log's [stage_fx ...] census: screens_complete stops growing while incomplete does (the first line with the final complete count and the last differ in incomplete only).
--plant movie_any counts sprite-class (3) draws as movie: fact 1 must FAIL (the case has 4 UI sprite draws). Exit 1 on a failed fact."""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402

SITE_SCREEN = 0x82C0F4BC


def main(argv):
    plant = ''
    args = [a for a in argv[1:] if not a.startswith('--')]
    expect_fail = '--expect-fail' in argv
    if '--plant' in argv:
        plant = argv[argv.index('--plant') + 1]
        args = [a for a in args if a != plant]
    if len(args) != 2:
        print(__doc__.split('\n')[0])
        return 2
    cap = L.Capture(args[0])
    movie_klass = 3 if plant == 'movie_any' else 5
    movie = screen = frames = 0
    for fi in range(cap.n):
        f = cap.frame(fi)
        if not f.has_origins:
            continue
        frames += 1
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o:
                continue
            movie += 1 if o['klass'] == movie_klass else 0
            screen += 1 if o['pass_site'] == SITE_SCREEN else 0
    bad = 0
    ok1 = frames > 0 and movie == 0 and screen == 0
    print('%s 1 stream: %d frames with origins, movie-class draws %d, screen-pass draws %d' % ('PASS' if ok1 else 'FAIL', frames, movie, screen))
    bad += 0 if ok1 else 1
    text = open(args[1], encoding='utf-8', errors='replace').read()
    sl = re.findall(r'\[screens_live rows_bound (\d+) rows_no_frame (\d+) unbound_calls (\d+), flushes (\d+) copied (\d+) bytes (\d+) unbound_skipped (\d+) not_bound_set (\d+) bad_layout (\d+) unreadable (\d+) wrong_size_skipped (\d+) no_stage_skipped (\d+)', text)
    if not sl:
        print('FAIL 2 log: no [screens_live ...] line')
        return 1
    last = [int(x) for x in sl[-1]]
    flushes, copied, no_stage = last[3], last[4], last[11]
    ok2 = flushes > 0 and flushes == no_stage and copied == 0 and last[0] == 0
    print('%s 2 log: rows_bound %d, flushes %d, copied %d, no_stage_skipped %d (every flush was before the stage)' % ('PASS' if ok2 else 'FAIL', last[0], flushes, copied, no_stage))
    bad += 0 if ok2 else 1
    sf = re.findall(r'screens_complete (\d+) incomplete (\d+) shown_without_row (\d+)', text)
    comp = [int(a) for a, b, c in sf]
    inc = [int(b) for a, b, c in sf]
    ok3 = len(sf) >= 3 and comp[-1] == comp[-3] and inc[-1] > inc[-3]
    print('%s 3 log: screens_complete %s, incomplete %s over the last %d census lines (complete frozen, incomplete grows)' % ('PASS' if ok3 else 'FAIL', comp[-3:], inc[-3:], len(sf)))
    bad += 0 if ok3 else 1
    print('screens_idle_check: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return (0 if bad else 1) if expect_fail else (1 if bad else 0)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
