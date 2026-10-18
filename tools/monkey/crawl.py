"""Session 30 (#7): menu crawler: walk a menu tree by state through the live channel and record every entry.

  py tools/monkey/crawl.py [--root play] [--items 0-7] [--name main] [--start] [--quit]

Drives a live game (py tools/live/live.py start --fast, or --start here) with state-driven presses
(press_until_state: fast-forward safe). For the PLAY tree (the first target, Session 30): from the main menu it opens
PLAY (menu 2), then for each item (ONE ON ONE, TWO ON TWO, ...) its entry list (menu 7), and for each entry presses A,
records where it leads (the menu id), the screen text there, and backs out with B. Setup screens have menu id
2000 + the match type (checked on the three types known from match captures: NORMAL 0, TAG TEAM 3, HELL IN A CELL 49;
Session 30 live, run 2261), so each entry's match-type id is recorded without launching a match.

Output: out/harness/crawl/<date_time>_<root>/map.json (every list: menu id, labels from the screen text; every entry:
item, cursor, label, menu reached, type = menu - 2000 when menu >= 2000, screen text there) plus crawl.log. Labels come
from the game's text manager (src/screen_text.h): the list's labels in the order the game fetches them.
"""
import argparse
import datetime
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'live'))
import live  # noqa: E402

STATE = re.compile(r'line \d+(?: \([^)]*\))?: state (\w+) = 0x[0-9A-F]+ \((-?\d+)\)')
TEXT = re.compile(r'line \d+(?: \([^)]*\))?: screen text: (.*) \(frame \d+\)$')


class Crawler:
    def __init__(self, name, log):
        self.name = name
        self.logf = log

    def say(self, text):
        print(text, flush=True)
        self.logf.write(text + '\n')
        self.logf.flush()

    def batch(self, lines, timeout=120):
        fields, log, _ = live.run_batch(self.name, lines, timeout)
        states, text = {}, None
        for ln in log:
            m = STATE.search(ln)
            if m:
                states[m.group(1)] = int(m.group(2))
            m = TEXT.search(ln.rstrip())
            if m:
                text = m.group(1)
        return fields.get('status') == 'ok', states, text, log

    def labels(self, text):
        return [t.strip() for t in (text or '').split(' | ') if t.strip() and t.strip() != '(none yet)']

    def back_to(self, menu):
        """B until the list `menu` is open again (a setup screen may need more than one). False = lost."""
        for _ in range(3):
            ok, st, _, _ = self.batch(['press_until_state B menu %d 6 60' % menu, 'log_state menu'])
            if ok and st.get('menu') == menu:
                return True
        return False

    def walk(self, menu, path, labels, out, depth, max_depth, only=None):
        """Every entry of the open list `menu` (from cursor 0): A, record where it leads, back with B; a list one level
        down is walked too (up to max_depth). `only` = the cursors to visit at this level (None = all). False = lost."""
        self.batch(['press_until_state UP cursor 0 40 30 5 if_needed'])
        j = 0
        while j < 80:
            label = labels[j] if j < len(labels) else None
            if only is None or j in only:
                ok, st, text, _ = self.batch(['press_until_state A menu !=%d 4 60' % menu, 'wait_frames 20',
                                              'log_state menu', 'log_text'])
                reached = st.get('menu') if ok else None
                entry = {'path': path, 'list_menu': menu, 'cursor': j, 'label': label, 'menu': reached,
                         'type': reached - 2000 if reached is not None and reached >= 2000 else None,
                         'text': text if ok else None}
                out['entries'].append(entry)
                if reached is None:
                    self.say('%s%s / %d %s -> A does nothing here' % ('  ' * depth, path, j, label))
                elif reached < 1000 and depth < max_depth:
                    sub = self.labels(text)
                    out['lists'].append({'menu': reached, 'path': '%s > %s' % (path, label), 'labels': sub})
                    self.say('%s%s / %d %s -> list %d: %s' % ('  ' * depth, path, j, label, reached, ' | '.join(sub)))
                    if not self.walk(reached, '%s > %s' % (path, label), sub, out, depth + 1, max_depth):
                        return False
                    if not self.back_to(menu):
                        self.say('could not get back to list %d from list %d: stopping' % (menu, reached))
                        return False
                else:
                    self.say('%s%s / %d %s -> menu %s%s' % ('  ' * depth, path, j, label, reached,
                                                          '  (type %d)' % entry['type'] if entry['type'] is not None
                                                          else ''))
                    if not self.back_to(menu):
                        self.say('could not get back to list %d from menu %s: stopping' % (menu, reached))
                        return False
            if only is not None and j >= max(only):
                return True
            ok, st, text3, _ = self.batch(['press_until_state DOWN cursor %d 4 30' % (j + 1), 'wait_frames 10',
                                           'log_state cursor', 'log_text'])
            if not ok or st.get('cursor') != j + 1:
                return True  # the end of the list (DOWN no longer moves the cursor)
            # A long list scrolls: the label that came into view is fetched now (append the new ones in order).
            for lab in self.labels(text3):
                if lab not in labels:
                    labels.append(lab)
            j += 1
        return True

    def crawl_play(self, items, max_depth=2):
        out = {'root': 'play', 'lists': [], 'entries': []}
        ok, st, text, _ = self.batch(['include menu_to_play', 'wait_frames 30', 'log_state menu', 'log_text'])
        if not ok or st.get('menu') != 2:
            self.say('could not open PLAY (menu %s)' % st.get('menu'))
            return out
        play_labels = self.labels(text)
        out['lists'].append({'menu': 2, 'path': 'PLAY', 'labels': play_labels})
        self.say('PLAY: %s' % ' | '.join(play_labels))
        out['complete'] = self.walk(2, 'PLAY', play_labels, out, 0, max_depth, only=set(items))
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default='play', choices=['play'])
    ap.add_argument('--items', default='0-7', help='PLAY items to walk, e.g. 0-7 or 0,2,5')
    ap.add_argument('--name', default='main')
    ap.add_argument('--start', action='store_true', help='start a fast-forwarded live game first')
    ap.add_argument('--quit', action='store_true', help='close the game at the end')
    a = ap.parse_args()
    items = []
    for part in a.items.split(','):
        lo, _, hi = part.partition('-')
        items += list(range(int(lo), int(hi or lo) + 1))
    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M')
    out_dir = os.path.join(REPO, 'out', 'harness', 'crawl', '%s_%s' % (stamp, a.root))
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'crawl.log'), 'w', encoding='utf-8') as logf:
        c = Crawler(a.name, logf)
        if a.start:
            c.say(live.start(a.name, fast=True))
        ok, st, _, _ = c.batch(['wait_state boot_step >=6 3600', 'log_state menu'], timeout=300)
        fields, _ = live.parse(live.request(a.name, '!status'))
        run = fields.get('run')
        c.say('live run %s, menu %s' % (run, st.get('menu')))
        if st.get('menu') != 1:
            ok, st, _, _ = c.batch(['include back_to_menu', 'log_state menu'])
        result = c.crawl_play(items)
        result.update({'run': run, 'made': stamp, 'note': 'Session 30 menu crawler (tools/monkey/crawl.py); local only'})
        with open(os.path.join(out_dir, 'map.json'), 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2)
        typed = [e for e in result['entries'] if e['type'] is not None]
        c.say('done: %d entries (%d with a match type) -> %s' % (len(result['entries']), len(typed),
                                                                  os.path.join(out_dir, 'map.json')))
        if a.quit:
            try:
                live.request(a.name, 'quit', connect_wait=2.0)
            except live.NoGame:
                pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
