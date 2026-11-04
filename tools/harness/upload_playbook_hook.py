"""Session 29: Claude Code PostToolUse hook: upload STATUS.md / SESSIONNN_SUMMARY.md to Jarrod's WebDAV after every edit.

Configured in .claude/settings.local.json (gitignored; the upload address is personal, so it is an argument, not in
this file):  py tools\\harness\\upload_playbook_hook.py https://HOST/
Reads the hook's JSON on stdin; when the edited file is the repo's STATUS.md or a SESSIONNN[_N]_SUMMARY.md, PUTs it
(overwrite) to the address + its file name, appends one line to out/harness/upload_hook.log, and tells Claude the
HTTP status (201 / 204 = ok). Any other file: nothing. Never blocks the edit (exit 0 always).

Session 86 (docs rework, Jarrod: "STATUS + the current summary on every edit; the slimmed PLAYBOOK only at session
end"): PLAYBOOK.md is no longer uploaded per edit. At a session's end:
  py tools\\harness\\upload_playbook_hook.py --end [https://HOST/]
uploads PLAYBOOK.md, STATUS.md and the newest SESSIONNN_SUMMARY.md (the address defaults to the hook's own, read from
.claude/settings.local.json) and prints one line per file.
"""
import json
import os
import re
import sys
import time
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = re.compile(r'^(STATUS\.md|SESSION\d+(_\d+)?_SUMMARY\.md)$')
SUMMARY = re.compile(r'^SESSION(\d+)(?:_(\d+))?_SUMMARY\.md$')


def upload(path, base_url):
    """PUT one file; returns the HTTP status as text ('201') or 'failed: ...'; logs it."""
    name = os.path.basename(path)
    status = 'error'
    try:
        # curl.exe, the command the memory's manual upload uses: the server answers Python's urllib with 403 (run
        # 18:39, Session 29) and curl with 201.
        p = subprocess.run(['curl.exe', '-sS', '-o', os.devnull, '-w', '%{http_code}', '-T', path, base_url + name],
                           capture_output=True, text=True, timeout=30)
        status = p.stdout.strip() or ('failed: ' + p.stderr.strip()[:200])
    except Exception as e:  # report, never block the edit
        status = 'failed: %s' % e
    try:
        os.makedirs(os.path.join(REPO, 'out', 'harness'), exist_ok=True)
        with open(os.path.join(REPO, 'out', 'harness', 'upload_hook.log'), 'a', encoding='utf-8') as log:
            log.write('%s  %s  %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), name, status))
    except OSError:
        pass
    return status


def hook_url():
    """The address the hook is configured with (.claude/settings.local.json, the arg after this script's path)."""
    try:
        with open(os.path.join(REPO, '.claude', 'settings.local.json'), encoding='utf-8') as f:
            settings = json.load(f)
    except (OSError, ValueError):
        return None
    for group in settings.get('hooks', {}).get('PostToolUse', []):
        for h in group.get('hooks', []):
            args = h.get('args') or []
            for i, a in enumerate(args[:-1]):
                if os.path.basename(a) == os.path.basename(__file__):
                    return args[i + 1]
    return None


def end_of_session(argv):
    base_url = (argv[0] if argv else hook_url() or '').rstrip('/') + '/'
    if base_url == '/':
        print('no address: pass it, or configure the hook in .claude/settings.local.json')
        return 1
    summaries = []
    for name in os.listdir(REPO):
        m = SUMMARY.match(name)
        if m:
            summaries.append(((int(m.group(1)), int(m.group(2) or 0)), name))
    files = ['PLAYBOOK.md', 'STATUS.md'] + ([max(summaries)[1]] if summaries else [])
    bad = 0
    for name in files:
        path = os.path.join(REPO, name)
        if not os.path.exists(path):
            print('%s: missing' % name)
            bad += 1
            continue
        status = upload(path, base_url)
        ok = status in ('200', '201', '204')
        bad += not ok
        print('%s: HTTP %s%s' % (name, status, '' if ok else '  FAILED'))
    return 1 if bad else 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--end':
        return end_of_session(sys.argv[2:])
    if len(sys.argv) < 2:
        return 0
    base_url = sys.argv[1].rstrip('/') + '/'
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return 0
    path = (data.get('tool_input') or {}).get('file_path') or (data.get('tool_response') or {}).get('filePath') or ''
    if not path:
        return 0
    path = os.path.abspath(path)
    name = os.path.basename(path)
    if not NAMES.match(name) or os.path.normcase(os.path.dirname(path)) != os.path.normcase(REPO):
        return 0
    status = upload(path, base_url)
    ok = status in ('200', '201', '204')
    print(json.dumps({'suppressOutput': ok, 'hookSpecificOutput': {
        'hookEventName': 'PostToolUse',
        'additionalContext': '%s uploaded to WebDAV: HTTP %s' % (name, status) if ok else
                             '%s upload to WebDAV FAILED (%s): upload by hand (memory upload-playbook-webdav)' % (name, status)}}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
