"""Session 91 step 7: the event bus's reader (src/event_bus.h, --wwe2k14_events=true).

  py tools/live/events.py [--run N] [--follow] [--kind seq,screen,...] [--last 40]

The game writes one JSON line per transition to out/build/win-amd64-release/logs/events_NNN.jsonl (NNN = the run's
log number): {"f": guest frame, "t": seconds, "kind": seq | screen | exit | msgbox | match | entrance | load, ...}.
Library: Events(run).read(since) -> new events; Events(run).wait(pred, timeout_s) -> the first event that matches
(pred: a callable or a dict of field values, e.g. {"kind": "match", "what": "clock_started"}).
"""
import argparse
import glob
import json
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')


def newest_run():
    files = glob.glob(os.path.join(LOGS, 'events_*.jsonl'))
    return int(max(files, key=os.path.getmtime).split('_')[-1].split('.')[0]) if files else None


class Events:
    def __init__(self, run=None):
        self.run = int(run) if run is not None else newest_run()
        self.path = os.path.join(LOGS, 'events_%03d.jsonl' % self.run) if self.run is not None else None
        self.pos = 0

    def read(self, since_start=False):
        """The events written since the last read (all of them with since_start)."""
        if since_start:
            self.pos = 0
        if not self.path or not os.path.exists(self.path):
            return []
        out = []
        with open(self.path, encoding='utf-8', errors='replace') as f:
            f.seek(self.pos)
            for line in f:
                if not line.endswith('\n'):
                    break  # a line still being written
                self.pos += len(line.encode('utf-8'))
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
        return out

    def skip_to_end(self):
        self.read()

    def wait(self, pred, timeout_s=60.0, poll_s=0.05):
        """The first new event matching pred (callable, or a dict of field values); None on timeout."""
        test = pred if callable(pred) else (lambda e: all(e.get(k) == v for k, v in pred.items()))
        end = time.time() + timeout_s
        while time.time() < end:
            for e in self.read():
                if test(e):
                    return e
            time.sleep(poll_s)
        return None


def snapshot(live_name='main', timeout_s=10.0):
    """The match snapshot (src/event_bus.h 'state' event): clock, referee mode / count, finish, wrestlers (slot, id,
    team, role, state 0 active / 1 apron / 4 out / 5 over the top, health / max). None outside a match."""
    import live  # tools/live
    fields, _ = live.parse(live.request(live_name, '!status'))
    ev = Events(int(fields.get('run')))
    ev.skip_to_end()
    live.run_batch(live_name, ['cvar wwe2k14_match_state_dump %d' % (int(time.time() * 1000) % 10**9),
                               'wait_frames 2'], 60)
    e = ev.wait({'kind': 'state'}, timeout_s)
    return e if e and e.get('in_match', True) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run', type=int)
    ap.add_argument('--follow', action='store_true')
    ap.add_argument('--kind', default='')
    ap.add_argument('--last', type=int, default=40)
    a = ap.parse_args()
    ev = Events(a.run)
    kinds = set(a.kind.split(',')) if a.kind else None
    show = lambda e: print(json.dumps(e), flush=True) if not kinds or e.get('kind') in kinds else None
    if not ev.path:
        print('no events file in %s' % LOGS)
        return 1
    for e in ev.read()[-a.last:]:
        show(e)
    while a.follow:
        for e in ev.read():
            show(e)
        time.sleep(0.1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
