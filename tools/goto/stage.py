"""S101 lane 38: drives the stage push against the running game (the live channel, --wwe2k14_live NAME).

    py -I tools/goto/stage.py [--name main] stage                      build the stage from the main menu (`!scene stage`)
    py -I tools/goto/stage.py [--name main] play ID [CHAR[/ATTIRE]...] [arena=N]   push scene bank ID into the ready stage
    py -I tools/goto/stage.py [--name main] leave                       S101 lane 72: `!scene leave` = the editor's own cancel; wait for 'leave: back at the main menu'
    py -I tools/goto/stage.py [--name main] off                         `!scene off`
    py -I tools/goto/stage.py [--name main] wait TEXT [--timeout S]     wait for a [scene][director] log line containing TEXT (from the last stage / play / off verb sent, else from this call)

`stage` answers at once; wait for 'stage: ready' (`wait "stage: ready"`) before `play`. The same requests tools/goto/resolve.py's executor sends for the
plan of `scene:ID[:CHARS]`; this is for pushing the next scene into a stage that is already up (a WSD / Universe / entrance 1-699 bank holds the stage).
Proof lines: tools/goto/stage_log.py. Exit 0 = the game accepted / the line came; 1 = refused / timed out; 2 = no game.
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'live'))
import live  # noqa: E402
import resolve  # noqa: E402


def _mark_file(name):
    return os.path.join(live.OUT, 'stage_mark_%s.json' % name)


MARK_MAX_AGE_S = 120  # a mark older than this is stale (a wait for something the last verb did not cause would match an older line)


def mark(name, verb=''):
    """S101 lane 59 (Check E, run 7066: 4 of 4 waits timed out): the run-log size just BEFORE a verb is sent. `play: dispatched` is logged ~20-50 ms after the
    verb's answer, i.e. before the next `wait` process has started; a wait that only read what came after its own start never saw it. wait reads from this mark."""
    try:
        run = int(live.parse(live.request(name, '!status', connect_wait=5.0))[0]['run'])
        with open(_mark_file(name), 'w', encoding='utf-8') as f:
            json.dump({'run': run, 'offset': os.path.getsize(live.run_log(run)), 'verb': verb, 'time': time.time()}, f)
    except Exception:  # noqa: BLE001 - no mark = wait falls back to its own start
        try:
            os.remove(_mark_file(name))
        except OSError:
            pass


def start_offset(name, run, size_now, now=None, max_age=MARK_MAX_AGE_S):
    """Where a wait starts reading: the last verb's mark when it is for this run, not past the end and younger than max_age seconds, else the log size now."""
    now = time.time() if now is None else now
    try:
        with open(_mark_file(name), encoding='utf-8') as f:
            m = json.load(f)
        if int(m['run']) == run and 0 <= int(m['offset']) <= size_now and 0 <= now - float(m['time']) <= max_age:
            return int(m['offset'])
    except (OSError, ValueError, KeyError):
        pass
    return size_now


def scan(path, start, text):
    """-> the first [scene][director] line holding `text` at or after byte `start` of the log, or None."""
    with open(path, 'rb') as f:
        f.seek(start)
        data = f.read().decode('utf-8', errors='replace')
    for ln in data.splitlines():
        if '[scene][director]' in ln and text in ln:
            return ln.strip()
    return None


def send(name, text):
    mark(name, text)
    try:
        fields, _ = live.parse(live.request(name, text, connect_wait=15.0))
    except Exception as x:  # noqa: BLE001 - the channel raises several kinds
        print('no game answering on "%s": %s' % (name, x))
        return 2
    note = fields.get('scene', fields.get('error', '(no scene line in the answer)'))
    print('%s -> status %s: %s' % (text, fields.get('status'), note))
    return 0 if fields.get('status') == 'ok' and note.startswith(('fired', 'armed', 'disarmed')) else 1


def wait(name, text, timeout):
    run = int(live.parse(live.request(name, '!status'))[0]['run'])
    path = live.run_log(run)
    start = start_offset(name, run, os.path.getsize(path))
    end = time.time() + timeout
    while True:
        ln = scan(path, start, text)
        if ln:
            print(ln[-260:])
            return 0
        if time.time() >= end:
            break
        time.sleep(0.5)
    print('the game never logged "%s" within %d s' % (text, timeout))
    return 1


def main(argv):
    name = 'main'
    if argv[:1] == ['--name'] and len(argv) > 2:
        name, argv = argv[1], argv[2:]
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == 'stage' and not rest:
        return send(name, '!scene stage')
    if cmd == 'off' and not rest:
        return send(name, '!scene off')
    if cmd == 'leave' and not rest:  # S101 lane 72
        return send(name, '!scene leave')
    if cmd == 'play' and rest:
        return send(name, '!scene play ' + ' '.join(rest))
    if cmd == 'wait' and rest:
        timeout = 120
        if '--timeout' in rest:
            i = rest.index('--timeout')
            timeout = int(rest[i + 1])
            rest = rest[:i] + rest[i + 2:]
        return wait(name, ' '.join(rest), timeout)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
