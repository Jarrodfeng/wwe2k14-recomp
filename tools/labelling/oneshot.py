"""Session 68: label exported batches without an agent loop (cost test).

    py tools\\labelling\\oneshot.py pack BATCH_DIR [BATCH_DIR ...] --out FILE
        One file: the one-shot prompt + every function's context file in index order (for one packed agent that
        reads it once and answers, no other tool calls).
    py tools\\labelling\\oneshot.py settle-pack BATCH_DIR ... --a A.json --b B.json --out FILE
        Settle prompt + per function its context, A's and B's entries (for one packed settle agent).
    py tools\\labelling\\oneshot.py judge-pack BATCH_DIR ... --a A.json --b B.json --s S.json --ref RESULT.json ...
        --out FILE
        Judge prompt + per function its context, the pilot's Opus reference R, A, B and the settled S.
    py tools\\labelling\\oneshot.py local BATCH_DIR [BATCH_DIR ...] --out FILE.json [--model M] [--url U]
        [--max-tokens N] [--limit N]
        One request per function to a local OpenAI-compatible server (LM Studio, default http://localhost:1234),
        same prompt as the system message; records seconds and token counts per function.

Batch directories are the export.py ones (index.md + 0x82XXXXXX.md). Outputs hold addresses and labels only and go
under out\\ (derived from the game: never committed).
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT = os.path.join(HERE, 'prompts', 'oneshot_v1.md')


def batch_addresses(d):
    with open(os.path.join(d, 'index.md'), encoding='utf-8') as f:
        return re.findall(r'^\s*(?:\d+\s+)?(0x[0-9A-F]{8})\b', f.read(), re.M)


def context(d, a):
    with open(os.path.join(d, a + '.md'), encoding='utf-8') as f:
        return f.read()


def prompt_text():
    with open(PROMPT, encoding='utf-8') as f:
        return f.read()


def cmd_pack(args):
    parts = [prompt_text(), '\n\n# The functions\n']
    n = 0
    for d in args.batches:
        for a in batch_addresses(d):
            parts.append(f'\n\n======== function {a} ========\n\n{context(d, a)}')
            n += 1
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(''.join(parts))
    print(f'{n} functions -> {args.out} ({os.path.getsize(args.out):,} bytes)')


def load_entries(path, key=None):
    """{address: entry} from a JSON array (labellers), an array of settle results (key='settled') or a pilot result
    file's R entries ({'R': {'entries': [...]}})."""
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    if isinstance(d, dict) and 'R' in d:
        d = d['R']['entries']
    out = {}
    for e in d:
        e = e.get(key, e) if key else e
        out[e['address'].upper().replace('0X', '0x')] = e
    return out


def entry_text(e):
    return json.dumps(e, indent=1, ensure_ascii=False) if e else '(missing)'


def cmd_settle_pack(args):
    a, b = load_entries(args.a), load_entries(args.b)
    with open(os.path.join(HERE, 'prompts', 'oneshot_settle_v1.md'), encoding='utf-8') as f:
        parts = [f.read(), '\n\n# The functions\n']
    n = 0
    for d in args.batches:
        for addr in batch_addresses(d):
            parts.append(f'\n\n======== function {addr} ========\n\n{context(d, addr)}\n\n'
                         f'-------- labeller A --------\n{entry_text(a.get(addr))}\n\n'
                         f'-------- labeller B --------\n{entry_text(b.get(addr))}\n')
            n += 1
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(''.join(parts))
    print(f'{n} functions -> {args.out} ({os.path.getsize(args.out):,} bytes)')


def cmd_judge_pack(args):
    a, b = load_entries(args.a), load_entries(args.b)
    s = load_entries(args.s, key='settled')
    r = {}
    for p in args.ref:
        r.update(load_entries(p))
    with open(os.path.join(HERE, 'prompts', 'oneshot_judge_v1.md'), encoding='utf-8') as f:
        parts = [f.read(), '\n\n# The functions\n']
    todo = [(d, addr) for d in args.batches for addr in batch_addresses(d)]
    if args.part:
        k, n_parts = (int(x) for x in args.part.split('/'))
        size = -(-len(todo) // n_parts)
        todo = todo[(k - 1) * size:k * size]
    n = 0
    for d, addr in todo:
        parts.append(f'\n\n======== function {addr} ========\n\n{context(d, addr)}\n\n'
                     f'-------- R (reference) --------\n{entry_text(r.get(addr))}\n\n'
                     f'-------- A --------\n{entry_text(a.get(addr))}\n\n'
                     f'-------- B --------\n{entry_text(b.get(addr))}\n\n'
                     f'-------- S (settled) --------\n{entry_text(s.get(addr))}\n')
        n += 1
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(''.join(parts))
    print(f'{n} functions -> {args.out} ({os.path.getsize(args.out):,} bytes)')


def cmd_score(args):
    """Tally judge results (one-shot judge v1 objects) like report.py does for the pilot."""
    fns = []
    for p in args.judged:
        with open(p, encoding='utf-8') as f:
            fns += json.load(f)['functions']
    print(f'{len(fns)} functions judged')
    rows = [('meaning match / compatible with R', lambda s: s.get('meaning') in ('match', 'compatible')),
            ('exact match with R', lambda s: s.get('meaning') == 'match'),
            ('meaning different', lambda s: s.get('meaning') == 'different'),
            ('summary fully correct', lambda s: s.get('correct') == 'correct'),
            ('minor errors', lambda s: s.get('correct') == 'minor_errors'),
            ('summary wrong', lambda s: s.get('correct') == 'wrong'),
            ('invented purpose', lambda s: s.get('invented') is True),
            ('confidence over / under', lambda s: s.get('conf') in ('over', 'under')),
            ('system ok', lambda s: s.get('system_ok') is True)]
    print(f'{"":38} {"A":>4} {"B":>4} {"S":>4}')
    for label, test in rows:
        print(f'{label:38} ' + ' '.join(f'{sum(1 for f in fns if test(f.get(k) or {})):4}' for k in 'ABS'))
    doubts = [f['address'] for f in fns if f.get('ref_doubt')]
    print(f'reference doubted: {len(doubts)} {doubts}')
    for f in fns:
        s = f.get('S') or {}
        if s.get('correct') != 'correct' or s.get('meaning') == 'different' or s.get('invented'):
            print(f'  S {f["address"]}: {s.get("meaning")}, {s.get("correct")}, {s.get("note", "")}')


def cmd_plan(args):
    """Bulk run: split each exported batch into packs that fit one Read, write per pack a label pack (labeller
    instructions + contexts), a settle pack (settle v2 instructions + contexts) and, for every 10th pack, a judge pack
    (listing-mode judge instructions + contexts); manifest = [[id, n_functions, kb, judged], ...] + addresses."""
    with open(PROMPT, encoding='utf-8') as f:
        p_label = f.read()
    with open(os.path.join(HERE, 'prompts', args.settle_prompt), encoding='utf-8') as f:
        p_settle = f.read()
    with open(os.path.join(HERE, 'prompts', 'oneshot_judge_listing_v1.md'), encoding='utf-8') as f:
        p_judge = f.read()
    os.makedirs(args.packs, exist_ok=True)
    manifest, addresses, k = [], {}, 0
    for wave in args.waves:
        for d in sorted(d for d in os.listdir(os.path.join(args.root, wave)) if d.startswith(wave + 'b')):
            bdir = os.path.join(args.root, wave, d)
            groups, cur, size = [], [], 0
            for a in batch_addresses(bdir):
                n = os.path.getsize(os.path.join(bdir, a + '.md'))
                if cur and size + n > args.max_kb * 1024:
                    groups.append(cur)
                    cur, size = [], 0
                cur.append(a)
                size += n
            if cur:
                groups.append(cur)
            for g in groups:
                k += 1
                pid = f'{wave}p{len([m for m in manifest if m[0].startswith(wave)]) + 1:03d}'
                body = ''.join(f'\n\n======== function {a} ========\n\n{context(bdir, a)}' for a in g)
                judged = 1 if k % 10 == 5 else 0
                for kind, head in (('label', p_label), ('settle', p_settle)) + ((('judge', p_judge),) if judged else ()):
                    with open(os.path.join(args.packs, f'{pid}.{kind}.md'), 'w', encoding='utf-8') as f:
                        f.write(head + f'\n\n# The functions ({len(g)})\n' + body)
                kb = os.path.getsize(os.path.join(args.packs, f'{pid}.label.md')) // 1024
                manifest.append([pid, len(g), kb, judged])
                addresses[pid] = g
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump({'packs': manifest, 'addresses': addresses}, f, indent=0)
    n = sum(m[1] for m in manifest)
    print(f'{len(manifest)} packs, {n} functions, {sum(m[3] for m in manifest)} judged packs '
          f'({sum(m[1] for m in manifest if m[3])} functions); label pack KB max {max(m[2] for m in manifest)} -> {args.out}')


def extract_json(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.S).strip()
    m = re.search(r'```(?:json)?\s*(.*?)```', text, re.S)
    if m:
        text = m.group(1)
    s = min([i for i in (text.find('['), text.find('{')) if i >= 0], default=-1)
    if s < 0:
        raise ValueError('no JSON in reply')
    val = json.JSONDecoder().raw_decode(text[s:])[0]
    return val[0] if isinstance(val, list) else val


def cmd_local(args):
    system = prompt_text()
    todo = [(d, a) for d in args.batches for a in batch_addresses(d)]
    if args.limit:
        todo = todo[:args.limit]
    out = {'model': args.model, 'url': args.url, 'prompt': os.path.basename(PROMPT), 'entries': [], 'runs': []}
    t_all = time.time()
    for i, (d, a) in enumerate(todo):
        body = {'model': args.model, 'temperature': 0.2, 'max_tokens': args.max_tokens,
                'messages': [{'role': 'system', 'content': system},
                             {'role': 'user', 'content': f'Label this one function ({a}). Answer with a JSON array '
                                                         f'holding its one entry.\n\n{context(d, a)}'}]}
        req = urllib.request.Request(args.url.rstrip('/') + '/v1/chat/completions', data=json.dumps(body).encode(),
                                     headers={'Content-Type': 'application/json'})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=args.timeout) as r:
                resp = json.load(r)
            text = resp['choices'][0]['message'].get('content') or ''
            usage = resp.get('usage', {})
            err = None
            try:
                entry = extract_json(text)
                entry['address'] = a
            except (ValueError, json.JSONDecodeError) as e:
                entry, err = None, f'parse: {e}'
        except Exception as e:  # noqa: BLE001 - record and go on
            text, usage, entry, err = '', {}, None, f'request: {e}'
        dt = time.time() - t0
        out['runs'].append({'address': a, 'seconds': round(dt, 1), 'prompt_tokens': usage.get('prompt_tokens'),
                            'completion_tokens': usage.get('completion_tokens'), 'error': err,
                            'raw': text if err else None})
        if entry:
            out['entries'].append(entry)
        print(f'{i + 1}/{len(todo)} {a} {dt:5.1f}s in {usage.get("prompt_tokens")} out '
              f'{usage.get("completion_tokens")} {err or entry.get("name")}', flush=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=1)
    out['seconds_total'] = round(time.time() - t_all, 1)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=1)
    print(f'done: {len(out["entries"])}/{len(todo)} entries in {out["seconds_total"]} s -> {args.out}')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('batches', nargs='+')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser('settle-pack')
    p.add_argument('batches', nargs='+')
    p.add_argument('--a', required=True)
    p.add_argument('--b', required=True)
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_settle_pack)
    p = sub.add_parser('judge-pack')
    p.add_argument('batches', nargs='+')
    p.add_argument('--a', required=True)
    p.add_argument('--b', required=True)
    p.add_argument('--s', required=True)
    p.add_argument('--ref', nargs='+', required=True, help='pilot result files with R entries')
    p.add_argument('--part', help='K/N: only the K-th of N equal parts of the functions')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_judge_pack)
    p = sub.add_parser('plan')
    p.add_argument('waves', nargs='+', help='wave names (x01 ...) under --root')
    p.add_argument('--root', default=os.path.join(HERE, '..', '..', 'out', 'harness', 'labelling', 's68'))
    p.add_argument('--packs', default=os.path.join(HERE, '..', '..', 'out', 'harness', 'labelling', 's68', 'packs'))
    p.add_argument('--max-kb', type=int, default=85, help='context bytes per pack (one Read)')
    p.add_argument('--settle-prompt', default='oneshot_settle_v3.md', help='file in prompts\\ (x01-x05 ran v2)')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_plan)
    p = sub.add_parser('score')
    p.add_argument('judged', nargs='+', help='judge result files (JSON objects with "functions")')
    p.set_defaults(fn=cmd_score)
    p = sub.add_parser('local')
    p.add_argument('batches', nargs='+')
    p.add_argument('--out', required=True)
    p.add_argument('--model', default='qwen3.8-27b')
    p.add_argument('--url', default='http://localhost:1234')
    p.add_argument('--max-tokens', type=int, default=6000)
    p.add_argument('--timeout', type=int, default=900)
    p.add_argument('--limit', type=int, default=0)
    p.set_defaults(fn=cmd_local)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
