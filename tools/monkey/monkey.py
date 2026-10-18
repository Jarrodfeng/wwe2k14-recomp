"""Session 30 (#7): monkey soak test: seeded random episodes on live games, crash triage, a summary.

  py tools/monkey/monkey.py --seed 1 --minutes 20 [--name monkey1] [--episode-s 240] [--kinds menus,match]

Each episode starts a fresh fast-forwarded live game (tools/live/live.py: frozen user data, run lock shared slot, the
window behind other windows, auto-bookmarks on hitches) and plays one of:
  menus  random presses in the menus (D-pad, A, B, LB / RB), now and then a jump with a navigation snippet; never A on
         QUIT TO DESKTOP; after each press it reads menu / seq / text, so it knows where it is;
  match  a Quick Match (--wwe2k14_launch=@quick) of a random known type (0, 3, 13, 14, 37, 39, 49) with random wrestlers
         from db/ids/characters.json, entrances skipped, random presses in the ring (the menus are taken down by the
         match: the Session 28 crash path);
then quits the game (a match episode's random presses may leave the match and build another one in the menus:
the Session 28 crash path; Jarrod watched one, soak run 2285). A game that stops answering is a crash or hang: the run gets tools/triage/triage.py, and the next
episode starts. Every batch is in the run log, so `py tools/live/live.py save-route NAME --run NNN` replays an episode.

Run 3 at once (one per run-lock lane) with different --seed / --name; the same seed makes the same choices (the game's
timing still varies). Output: out/harness/monkey/<date_time>_seed<N>/summary.json + monkey.log (local only).
"""
import argparse
import datetime
import json
import os
import random
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import live  # noqa: E402

# type -> wrestlers: the PLAY menus' own types (db/ids/match_types.json: 13 TRIPLE THREAT, 14 FATAL 4-WAY, ...) and 37
# (6-pack, captured). (Session 27's 1 / 2 also take 3 / 4 wrestlers, run 2290; the menus use 13 / 14.)
QUICK_TYPES = {0: 2, 3: 4, 13: 3, 14: 4, 37: 6, 39: 6, 49: 2}
NAV = ['menu_to_play', 'menu_to_options', 'menu_to_creations']
STATE = re.compile(r'state (\w+) = 0x[0-9A-F]+ \((-?\d+)\)')


def characters():
    d = json.load(open(os.path.join(REPO, 'db', 'ids', 'characters.json'), encoding='utf-8'))
    ids = []
    for e in d.get('entries', []):
        # Wrestlers only: valets (managers) in wrestler slots make odd matches (Session 30 soak run 2285: Naomi, Cameron).
        if (isinstance(e, dict) and str(e.get('id', '')).isdigit() and e.get('kind') == 'wrestler'
                and e.get('confidence', 'high') in ('high', 'medium')):
            ids.append(int(e['id']))
    return sorted(set(ids)) or [324, 343]


class Monkey:
    def __init__(self, a, out_dir):
        self.a = a
        self.rng = random.Random(a.seed)
        self.out_dir = out_dir
        self.logf = open(os.path.join(out_dir, 'monkey.log'), 'w', encoding='utf-8')
        self.summary = {'seed': a.seed, 'name': a.name, 'episodes': [], 'crashes': [], 'screens': {}}
        self.chars = characters()

    def say(self, text):
        line = '%s %s' % (time.strftime('%H:%M:%S'), text)
        print(line, flush=True)
        self.logf.write(line + '\n')
        self.logf.flush()

    def batch(self, lines, timeout=60):
        try:
            fields, log, _ = live.run_batch(self.a.name, lines, timeout)
        except live.NoGame:
            return None, {}
        st = {}
        for ln in log:
            m = STATE.search(ln)
            if m:
                st[m.group(1)] = int(m.group(2))
        return fields.get('status'), st

    def alive(self):
        try:
            live.parse(live.request(self.a.name, '!status', connect_wait=3.0))
            return True
        except live.NoGame:
            return False

    def run_number(self):
        try:
            fields, _ = live.parse(live.request(self.a.name, '!status', connect_wait=1.0))
            return fields.get('run')
        except live.NoGame:
            return None

    def note_screen(self, st):
        key = 'seq %s menu %s' % (st.get('seq'), st.get('menu'))
        self.summary['screens'][key] = self.summary['screens'].get(key, 0) + 1

    def menus_episode(self, end):
        steps = 0
        while time.time() < end:
            status, st = self.batch(['log_state seq', 'log_state menu', 'log_state cursor'])
            if status is None:
                return steps, 'no answer'
            self.note_screen(st)
            r = self.rng.random()
            if r < 0.08 and st.get('menu') == 1:
                lines = ['include %s' % self.rng.choice(NAV)]
            else:
                button = self.rng.choices(['DOWN', 'UP', 'LEFT', 'RIGHT', 'A', 'B', 'LB', 'RB'],
                                          weights=[5, 4, 2, 2, 3, 3, 1, 1])[0]
                if button == 'A' and st.get('menu') == 1 and st.get('cursor') == 8:
                    button = 'UP'  # never QUIT TO DESKTOP
                lines = ['press %s' % button, 'wait_frames %d' % self.rng.randint(10, 40)]
            if self.batch(lines)[0] is None:
                return steps, 'no answer'
            steps += 1
        return steps, 'time'

    def match_episode(self, end):
        steps = 0
        status, st = self.batch(['wait_state seq 9 3600 30', 'log_state seq'], timeout=240)
        if status is None:
            return steps, 'no answer before the match'
        if st.get('seq') != 9:
            return steps, 'the match did not start (seq %s)' % st.get('seq')
        while time.time() < end:
            button = self.rng.choice(['A', 'B', 'X', 'Y', 'RT', 'LT', 'L_LEFT', 'L_RIGHT', 'L_UP', 'L_DOWN'])
            status, st = self.batch(['press %s %d' % (button, self.rng.randint(3, 30)),
                                     'wait_frames %d' % self.rng.randint(5, 60), 'log_state seq'])
            if status is None:
                return steps, 'no answer'
            self.note_screen(st)
            steps += 1
        return steps, 'time'

    def episode(self, k, kind):
        extra = ''
        info = {'k': k, 'kind': kind}
        if kind == 'match':
            t = self.rng.choice(sorted(QUICK_TYPES))
            ids = self.rng.sample(self.chars, QUICK_TYPES[t])
            extra = ('--wwe2k14_launch=@quick --wwe2k14_quick_type=%d --wwe2k14_launch_participants=%s '
                     '--wwe2k14_launch_skip_entrances=true' % (t, ','.join(map(str, ids))))
            info.update({'type': t, 'participants': ids})
        # Menu episodes at normal speed: fast-forwarded menus drop short presses (they read input on real time), so a
        # fast menu monkey mostly stood still on the main menu (Jarrod, soak 20:47). Match episodes stay fast-forwarded.
        text = live.start(self.a.name, fast=(kind == 'match'), extra=extra)
        run = self.run_number()
        info['run'] = run
        self.say('episode %d (%s) %s: %s' % (k, kind, extra or '', text))
        if not text.startswith(('started', 'already')):
            info['end'] = 'did not start'
            self.summary['episodes'].append(info)
            return
        self.batch(['wait_state boot_step >=6 3600'], timeout=300)
        end = min(time.time() + self.a.episode_s, self.deadline)
        steps, why = (self.match_episode if kind == 'match' else self.menus_episode)(end)
        info.update({'steps': steps, 'end': why})
        if why.startswith('no answer') and not self.alive():
            info['crash'] = True
            verdict = subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'triage', 'triage.py'), str(run)],
                                     capture_output=True, text=True).stdout.splitlines()[:3]
            self.summary['crashes'].append({'run': run, 'episode': k, 'kind': kind, 'triage': verdict})
            self.say('episode %d: the game stopped answering: %s' % (k, ' / '.join(verdict)))
        else:
            try:
                live.request(self.a.name, 'quit', connect_wait=2.0)
            except live.NoGame:
                pass
            for _ in range(60):  # wait for the slot to be free before the next game starts
                if not self.alive():
                    break
                time.sleep(1)
        self.summary['episodes'].append(info)
        self.say('episode %d done: %d steps, %s' % (k, steps, why))

    def run(self):
        self.deadline = time.time() + self.a.minutes * 60
        kinds = self.a.kinds.split(',')
        k = 0
        while time.time() < self.deadline - 30:
            k += 1
            self.episode(k, self.rng.choice(kinds))
        with open(os.path.join(self.out_dir, 'summary.json'), 'w', encoding='utf-8') as f:
            json.dump(self.summary, f, indent=2)
        self.say('monkey seed %d: %d episodes, %d crashes; %d screens seen -> %s' % (
            self.a.seed, len(self.summary['episodes']), len(self.summary['crashes']), len(self.summary['screens']),
            os.path.join(self.out_dir, 'summary.json')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--minutes', type=float, default=20)
    ap.add_argument('--name', default='')
    ap.add_argument('--episode-s', type=float, default=240)
    ap.add_argument('--kinds', default='menus,match')
    a = ap.parse_args()
    a.name = a.name or 'monkey%d' % a.seed
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
    out_dir = os.path.join(REPO, 'out', 'harness', 'monkey', '%s_seed%d' % (stamp, a.seed))
    os.makedirs(out_dir, exist_ok=True)
    Monkey(a, out_dir).run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
