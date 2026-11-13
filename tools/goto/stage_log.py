"""S101 lane 38: reads a run log for the stage push's proof lines (Check E) and says which scenes got how far.

    py -I tools/goto/stage_log.py LOGFILE [--expect ID[,ID...]]

The game logs (src/scene_director.h, all `[wwe2k14][scene][director]`):
    menu: scene 4025 requested ...           the stage request (the old `menu` verb with mode 2)
    stage: ready R=... ctx(read)=4025 ...    the stage holds bank 4025 and waits for a push
    play: pushed scene ID ...                the handler's tail ran (sub_82A30F08 returned, M+40 restored)
    play: committed ...                      the stage's command took the pending context (write++)
    play: dispatched scene ID (...) ... G+64=N   (S101 lane 72: no class verdict here, the new command object is not built yet; run 7072 read vtable 0 on this line)
    play: command class of scene ID: vtable V (story class ... | not the story class ...)   first frame the vtable is non-zero (lane 72); older logs had it on the dispatched line
    leave: requested ... / leave +N Q+16=.. P+4=.. M+20=.. H=.. L=.. M=.. listener=.. PG+88=.. seq=.. / leave: back at the main menu (...) | leave: did not complete
    play: scene list done after scene ID ... what followed a scene that ended
    menu: playback finished ...              the OLD verb's end line; the stage never logs it (run 7058: the stage held for ever)
Exit 0 when every id in --expect was dispatched (or, with none given, when the stage was ready); 1 otherwise. Reads only our own log text.
"""
import re
import sys

LINE = re.compile(r'\[scene\]\[director\] (?P<text>.*)$')
FRAME = re.compile(r'frame=(\d+)')


def analyse(lines):
    """-> dict: requested (frame|None), ready (frame|None), pushes [{id, frame}], committed [frame], dispatched [{id, frame, story}], done [{id, frame}],
    finished (the old verb's 'playback finished' line was seen), refused [text]."""
    r = {'requested': None, 'ready': None, 'pushes': [], 'committed': [], 'dispatched': [], 'done': [], 'finished': False, 'refused': [], 'left': []}
    for ln in lines:
        m = LINE.search(ln)
        if not m:
            continue
        t = m.group('text')
        fm = FRAME.search(t)
        frame = int(fm.group(1)) if fm else None
        if t.startswith('menu: scene 4025 requested') or t.startswith('stage: requested'):
            r['requested'] = frame if r['requested'] is None else r['requested']
        elif t.startswith('stage: ready'):
            r['ready'] = frame
        elif t.startswith('play: pushed scene '):
            r['pushes'].append({'id': int(t.split()[3]), 'frame': frame})
        elif t.startswith('play: committed'):
            r['committed'].append(frame)
        elif t.startswith('play: dispatched scene '):
            r['dispatched'].append({'id': int(t.split()[3]), 'frame': frame, 'story': 'story class: holds' in t})
        elif t.startswith('play: command class of scene '):  # S101 lane 72: the class line after the dispatch
            sid = int(t.split()[5].rstrip(':'))
            for d in reversed(r['dispatched']):
                if d['id'] == sid:
                    d['story'] = 'story class: holds' in t
                    break
        elif t.startswith('leave: back at the main menu'):
            r['left'].append({'frame': frame, 'clean': t.startswith('leave: back at the main menu (')})
        elif t.startswith('play: scene list done after scene '):
            r['done'].append({'id': int(t.split()[6]), 'frame': frame})
        elif t.startswith('menu: playback finished'):
            r['finished'] = True
        elif ': refused' in t or t.startswith(('stage: the main menu never left', 'play: the scene manager went away', 'leave: did not complete')):
            r['refused'].append(t[:200])
    return r


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path, expect = argv[0], []
    if '--expect' in argv:
        expect = [int(x) for x in argv[argv.index('--expect') + 1].split(',')]
    try:
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    except OSError as x:
        print('cannot read %s: %s' % (path, x))
        return 2
    r = analyse(lines)
    print('stage requested at frame %s; ready at frame %s' % (r['requested'], r['ready']))
    for p in r['pushes']:
        print('pushed scene %d (frame %s)' % (p['id'], p['frame']))
    for d in r['dispatched']:
        print('dispatched scene %d (frame %s, %s)' % (d['id'], d['frame'], 'story class: holds' if d['story'] else 'not the story class'))
    for d in r['done']:
        print('scene list done after scene %d (frame %s)' % (d['id'], d['frame']))
    for t in r['refused']:
        print('problem: ' + t)
    got = {d['id'] for d in r['dispatched']}
    missing = [i for i in expect if i not in got]
    if missing:
        print('NOT dispatched: %s' % missing)
        return 1
    if not expect and r['ready'] is None:
        print('the stage never became ready')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
