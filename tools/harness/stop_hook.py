"""Session 102 lane F2: Claude Code Stop hook for the main session (RENDERER_ARC_PT2.md section 9 item 9).

Hook format used: the Claude Code hooks documentation (code.claude.com/docs/en/hooks), from knowledge, not read from thirdparty:
  stdin  = JSON {"session_id", "transcript_path", "cwd", "hook_event_name": "Stop", "stop_hook_active": bool,
                 optionally "last_assistant_message"}; when that field is absent the last assistant text is read from the
                 transcript JSONL (lines {"type":"assistant","message":{"content":[{"type":"text","text":...}]}}).
  stdout = {"decision": "block", "reason": "..."} to refuse the stop; nothing / exit 0 to let it stop.
This script ALWAYS exits 0 (never 2: exit 2 would be read as a block). Every exception is logged and means "pass".

Blocks when
  (1) READY.md lists ready lanes and fewer than MIN_RUNNING (6) lanes run, or
  (2) the turn ENDS with a question to Jarrod about work the plan lets the main session decide (merge / start / run / build / review-fix /
      lane choice). Questions about which thing he meant, how a picture looks, quoted questions, and anything about release / push /
      a player default / runs beyond the grant / deletion pass. Measured on S101's transcript: tools\\harness\\stop_hook_measure.py.
Never blocks forever (S1):
  - the same (lane, state) table 3 blocks in a row -> pass, and keep passing until the (lane, state) key changes (Notes do not count);
  - a second counter ignores the key: stop_hook_active true and 6 blocks in a row -> pass.
Off switch: while out\\s102\\stop_hook.off exists the hook passes. State is per session_id: out\\s102\\stop_hook_state.json; log:
out\\s102\\stop_hook.log. Env overrides for tests: S102_READY_FILE, S102_HOOK_DIR.
"""
import json, os, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[2]
MIN_RUNNING = 6
MAX_BLOCKS = 3        # same (lane, state) key
MAX_ACTIVE_BLOCKS = 6  # consecutive blocks while stop_hook_active, whatever the key

ASK = re.compile(r"(\?\s*$|\bshould i\b|\bshall i\b|\bdo you want\b|\bwould you like\b|\bwant me to\b|\bokay to\b|\bok to\b|\bmay i\b|\bcan i go ahead\b|"
                 r"\bready (for me )?to (merge|start|run|build)\b)", re.I)
# work the plan lets the main session decide (the question must name one of these)
WORK = re.compile(r"\b(merge[sd]?|merging|start(ed|ing)?|launch(ed|ing)?|run(s|ning)?|build(s|ing)?|rebuild|fix(es|ed|ing)?|review[- ]?fix|"
                  r"dispatch|land(ing)?|integrate|lane|lanes|builder|reviewer|brief|next step|continue|proceed|go ahead|queue)\b", re.I)
# topics that pass (allowed waits on Jarrod, clarifying questions, pictures he judges)
ALLOWED = re.compile(r"(\brelease[sd]?\b|\bpush(ed|ing)?\b|\bpublish|\bplayer default|\bdefault (for|that) players|\bnew default\b|"
                     r"\bflip(ping)? the default\b|\b(game|surface) runs? (beyond|over)|\bbeyond (the|this|your) (plan|grant|budget)|"
                     r"\bover budget\b|\bextra (game |surface )?runs?\b|\bmore (game |surface )?runs\b|\bdelet(e|ing|ion)\b)", re.I)
CLARIFY = re.compile(r"(\bwhich (one|thing|do you mean|did you mean)\b|\bdid you mean\b|\bdo you mean\b|\bwhat do you mean\b|\bwhich of (these|the)\b|"
                     r"\blooks? (right|ok|okay|good|better|wrong|off|fine)\b|\b(picture|sheet|screenshot|frame|image|colou?r|look)s?\b)", re.I)


QUOTED = re.compile('`[^`]*`|"[^"]*"|“[^”]*”')


def hook_dir():
    return Path(os.environ.get('S102_HOOK_DIR') or ROOT / 'out' / 's102')


def log(msg):
    try:
        d = hook_dir()
        d.mkdir(parents=True, exist_ok=True)
        with open(d / 'stop_hook.log', 'a', encoding='utf-8') as f:
            f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
    except Exception:
        pass


def last_assistant_text(data):
    if isinstance(data.get('last_assistant_message'), str):
        return data['last_assistant_message']
    p = data.get('transcript_path')
    if not p or not Path(p).is_file():
        return ''
    last = ''
    with open(p, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                o = json.loads(line)
            except ValueError:
                continue
            m = o.get('message') if isinstance(o, dict) else None
            if o.get('type') == 'assistant' and isinstance(m, dict):
                c = m.get('content')
                t = c if isinstance(c, str) else ' '.join(b.get('text', '') for b in (c or []) if isinstance(b, dict) and b.get('type') == 'text')
                if t.strip():
                    last = t
    return last


def forbidden_ask(text):
    """The ending question (a string) when the turn ENDS with a question to Jarrod about plan-decidable work; else None."""
    t = re.sub(r'```.*?```', ' ', text, flags=re.S)           # code blocks are not questions
    t = QUOTED.sub(' ', t)  # quoted questions are not asked
    lines = [ln.strip() for ln in t.strip().splitlines() if ln.strip()]
    if not lines:
        return None
    last = lines[-1]
    sents = [s for s in re.split(r'(?<=[.?!])\s+', last) if s.strip()]
    # the turn must END with the question: the final sentence of the final line
    s = sents[-1] if sents else last
    if not ASK.search(s):
        return None
    # allowed / clarifying topics are judged on the question itself; the work words may come from the sentence before it ("Ready? Merge T1.")
    if ALLOWED.search(s) or CLARIFY.search(s):
        return None
    prev = sents[-2] if len(sents) > 1 else ''
    if not WORK.search(s) and not WORK.search(prev):
        return None
    return s.strip()


def decide(data):
    """Return (block_reason or None, key); key identifies READY.md's (lane, state) pairs for the no-forever rule."""
    import ready_lanes
    rf = os.environ.get('S102_READY_FILE') or str(ready_lanes.DEFAULT)
    reasons = []
    key = 'none'
    try:
        rows, problems = ready_lanes.parse(rf)
        for p in problems:
            log('READY.md problem: %s' % p)
        c = ready_lanes.counts(rows)
        key = ready_lanes.digest_states(rows)
        if c['ready'] > 0 and c['running'] < MIN_RUNNING:
            names = ', '.join(r['lane'] for r in rows if r['state'] == 'ready')
            reasons.append('READY.md lists %d ready lane(s) (%s) and only %d running (minimum %d). Start the ready lanes now, update READY.md, '
                           'then continue.' % (c['ready'], names, c['running'], MIN_RUNNING))
    except Exception as e:
        log('cannot read/parse READY.md (%s: %s): not blocking on lanes' % (type(e).__name__, e))
    try:
        ask = forbidden_ask(last_assistant_text(data))
    except Exception as e:
        log('ask rule failed (%s: %s): passing' % (type(e).__name__, e))
        ask = None
    if ask:
        reasons.append('Your turn ends asking Jarrod to decide work the plan lets you decide: "%s". Decide it yourself per CLAUDE.md, log it, '
                       'and keep working.' % ask[:200])
    return ('\n'.join(reasons) if reasons else None), key


def step(st, sid, reason, key, active):
    """Pure state machine. st = whole state dict; returns (block_reason or None). Mutates st[sid]."""
    s = st.get(sid) or {}
    if reason is None:
        st[sid] = {'blocks': 0, 'key': key, 'consec': 0, 'passing_key': None}
        return None
    if s.get('passing_key') == key:                      # (c) keep passing until the (lane, state) key changes
        return None
    n = s.get('blocks', 0) + 1 if s.get('key') == key else 1
    consec = (s.get('consec', 0) + 1) if active else 1
    if consec >= MAX_ACTIVE_BLOCKS:
        log('PASS after %d blocks in a row with stop_hook_active (key ignored): %s' % (consec, reason.replace('\n', ' | ')))
        st[sid] = {'blocks': 0, 'key': key, 'consec': 0, 'passing_key': key}
        return None
    if n > MAX_BLOCKS:
        log('PASS after %d blocks in a row with READY.md (lane, state) unchanged: %s' % (MAX_BLOCKS, reason.replace('\n', ' | ')))
        st[sid] = {'blocks': 0, 'key': key, 'consec': 0, 'passing_key': key}
        return None
    st[sid] = {'blocks': n, 'key': key, 'consec': consec, 'passing_key': None}
    log('BLOCK %d/%d (consec %d/%d): %s' % (n, MAX_BLOCKS, consec, MAX_ACTIVE_BLOCKS, reason.replace('\n', ' | ')))
    return reason


def run():
    if (hook_dir() / 'stop_hook.off').exists():
        return None
    try:
        raw = sys.stdin.buffer.read().decode('utf-8', errors='replace')
        data = json.loads(raw) if raw.strip() else {}
    except Exception as e:
        log('bad stdin (%s): passing' % e)
        return None
    if not isinstance(data, dict):
        return None
    reason, key = decide(data)
    sid = str(data.get('session_id') or 'nosession')
    sp = hook_dir() / 'stop_hook_state.json'
    try:
        st = json.loads(sp.read_text(encoding='utf-8'))
        if not isinstance(st, dict):
            st = {}
    except Exception:
        st = {}
    out = step(st, sid, reason, key, bool(data.get('stop_hook_active')))
    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(st), encoding='utf-8')
    except Exception as e:
        log('cannot write state (%s)' % e)
    return out


def main():
    try:
        out = run()
        if out:
            print(json.dumps({'decision': 'block', 'reason': out}))
    except BaseException as e:
        log('hook crashed (%s: %s): passing' % (type(e).__name__, e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
