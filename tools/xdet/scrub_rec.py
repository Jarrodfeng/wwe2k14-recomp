"""Copy a replay recording (.rec, src/replay.h) for committing / shipping (CLAUDE.md rule 1 exception, Session 77):
the '# modified:' comment lines lose host path values (they name the user's folders), as src/replay.h writes them
since Session 77. Playback reads no comment line, so the copy plays the same. Written with LF (replay format).
  py tools/xdet/scrub_rec.py out/replays/xdet_cena_taker_pad.rec tools/xdet/refs/xdet_cena_taker_pad.rec"""
import os
import re
import sys

PATH_VALUE = re.compile(r'^(# modified: \S+ = )(.*)$')


def is_host_path(v):
    return ':\\' in v or ':/' in v or v.startswith('/') or v.startswith('\\')


def main():
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.exists(dst):
        print('exists: %s (never overwritten)' % dst)
        return 2
    lines = open(src, encoding='utf-8', newline='').read().replace('\r\n', '\n').split('\n')
    out, scrubbed = [], 0
    for line in lines:
        m = PATH_VALUE.match(line)
        if m and is_host_path(m.group(2)):
            line = m.group(1) + '<path>'
            scrubbed += 1
        elif not line.startswith('#') and ('C:\\' in line or '/Users/' in line or '\\Users\\' in line):
            print('a non-comment line holds a host path, not copied: %s' % line)
            return 1
        out.append(line)
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print('%s -> %s: %d path value(s) removed' % (src, dst, scrubbed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
