"""Session 102 F2: how often would the stop hook's ask rule have blocked a real session's turn ends?

  py -I tools\\harness\\stop_hook_measure.py TRANSCRIPT.jsonl [--examples 10]

A turn end = an assistant entry with text and no tool_use block, followed by a user entry (or the file's end). The same
forbidden_ask() the hook uses is applied to its text. Prints turn ends, blocks, the rate and example blocks.
"""
import argparse, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stop_hook


def turn_ends(path):
    pending = None
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                o = json.loads(line)
            except ValueError:
                continue
            t = o.get('type')
            m = o.get('message') if isinstance(o.get('message'), dict) else {}
            if t == 'assistant':
                c = m.get('content')
                blocks = c if isinstance(c, list) else [{'type': 'text', 'text': c or ''}]
                text = ' '.join(b.get('text', '') for b in blocks if isinstance(b, dict) and b.get('type') == 'text').strip()
                if any(isinstance(b, dict) and b.get('type') == 'tool_use' for b in blocks):
                    pending = None
                elif text:
                    pending = text
            elif t == 'user':
                c = m.get('content')
                is_result = isinstance(c, list) and any(isinstance(b, dict) and b.get('type') == 'tool_result' for b in c)
                if not is_result and pending is not None:
                    out.append(pending)
                    pending = None
    if pending:
        out.append(pending)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('transcript')
    ap.add_argument('--examples', type=int, default=10)
    a = ap.parse_args()
    ends = turn_ends(a.transcript)
    blocks = [(e, stop_hook.forbidden_ask(e)) for e in ends]
    blocks = [(e, q) for e, q in blocks if q]
    print('turn ends %d, blocked %d (%.1f %%)' % (len(ends), len(blocks), 100.0 * len(blocks) / max(1, len(ends))))
    step = max(1, len(blocks) // max(1, a.examples))
    for e, q in blocks[::step][:a.examples]:
        print('  - %s' % q.replace('\n', ' ')[:200])
    return 0 if ends else 1


if __name__ == '__main__':
    sys.exit(main())
