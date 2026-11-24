#!/usr/bin/env python3
r"""Pull a reader agent's final answer out of its transcript into a text file, for `q read store` (S110).
The sheet-reader has no Write tool by design (it answers in its final message), so the main session would otherwise
have to re-type the answer. Reads the transcript only; writes the one file named.

  py -I tools\q\qread_answer.py AGENT_ID OUT_FILE [--session SESSION_ID]     (default: the newest session with subagents)
"""
import glob
import json
import os
import sys

ROOT = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')


def longest_string(obj):
    best = ''
    if isinstance(obj, str):
        return obj
    if isinstance(obj, dict):
        obj = list(obj.values())
    if isinstance(obj, list):
        for v in obj:
            s = longest_string(v)
            if len(s) > len(best):
                best = s
    return best


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 2:
        print(__doc__)
        return 2
    agent, out = args
    if '--session' in argv:
        folders = [os.path.join(ROOT, argv[argv.index('--session') + 1], 'subagents')]
    else:
        folders = sorted(glob.glob(os.path.join(ROOT, '*', 'subagents')), key=os.path.getmtime, reverse=True)
    path = next((p for f in folders for p in [os.path.join(f, 'agent-%s.jsonl' % agent)] if os.path.exists(p)), None)
    if not path:
        print('no transcript for agent %s' % agent)
        return 1
    handback, last_text = '', ''
    for line in open(path, encoding='utf-8', errors='replace'):
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        msg = rec.get('message') or {}
        content = msg.get('content')
        if msg.get('role') != 'assistant' or not isinstance(content, list):
            continue
        for block in content:
            if not isinstance(block, dict):
                continue
            if block.get('type') == 'tool_use' and 'handback' in str(block.get('name', '')).lower():
                handback = longest_string(block.get('input') or {})
            elif block.get('type') == 'text' and len(block.get('text', '')) > len(last_text):
                last_text = block['text']   # the LONGEST text block: a reader may answer in one reply and end with a short "done"
    text = handback if len(handback) >= len(last_text) else last_text
    if not text.strip():
        print('no answer text found in %s' % path)
        return 1
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text if text.endswith('\n') else text + '\n')
    print('%s: %d bytes, %d "## " blocks (from %s)' % (out, len(text.encode('utf-8')), text.count('\n## ') + text.startswith('## '),
                                                    'the hand-back call' if text is handback else 'the last message'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
