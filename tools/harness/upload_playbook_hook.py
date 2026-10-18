"""Session 29: Claude Code PostToolUse hook: upload PLAYBOOK.md / SESSIONNN_SUMMARY.md after every edit.

Configured in .claude/settings.local.json (gitignored; the upload address is personal, so it is an argument, not in
this file):  py tools\\harness\\upload_playbook_hook.py https://HOST/
Reads the hook's JSON on stdin; when the edited file is the repo's PLAYBOOK.md or a SESSIONNN[_N]_SUMMARY.md, PUTs it
(overwrite) to the address + its file name, appends one line to out/harness/upload_hook.log, and tells Claude the
HTTP status (201 / 204 = ok). Any other file: nothing. Never blocks the edit (exit 0 always).
"""
import json
import os
import re
import sys
import time
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = re.compile(r'^(PLAYBOOK\.md|SESSION\d+(_\d+)?_SUMMARY\.md)$')


def main():
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
    ok = status in ('200', '201', '204')
    print(json.dumps({'suppressOutput': ok, 'hookSpecificOutput': {
        'hookEventName': 'PostToolUse',
        'additionalContext': '%s uploaded to WebDAV: HTTP %s' % (name, status) if ok else
                             '%s upload to WebDAV FAILED (%s): upload by hand (memory upload-playbook-webdav)' % (name, status)}}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
