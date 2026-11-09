"""Tiny client for the asset viewer's named pipe (wwe2k14_assets.exe). Standard library only.

    import assets_client; assets_client.send({"cmd": "stats"})
    py tools\\assets\\assets_client.py '{"cmd":"stats"}' [--pipe NAME] [--wait SECONDS]
"""
import json
import sys
import time

DEFAULT_PIPE = "wwe2k14_assets"


def _open(pipe, wait):
    path = "\\\\.\\pipe\\" + pipe
    deadline = time.time() + wait
    while True:
        try:
            return open(path, "r+b")
        except OSError:
            if time.time() >= deadline:
                raise
            time.sleep(0.1)


def send(cmd, pipe=DEFAULT_PIPE, wait=5.0):
    """Send one command dict, return the reply dict. Raises OSError if the viewer is not running."""
    f = _open(pipe, wait)
    try:
        f.write((json.dumps(cmd) + "\n").encode("utf-8"))
        f.flush()
        line = f.readline()
    finally:
        f.close()
    if not line:
        raise OSError("viewer closed the pipe without a reply")
    return json.loads(line.decode("utf-8"))


def main(argv):
    pipe, wait, args = DEFAULT_PIPE, 5.0, []
    i = 0
    while i < len(argv):
        if argv[i] == "--pipe":
            pipe = argv[i + 1]; i += 2
        elif argv[i] == "--wait":
            wait = float(argv[i + 1]); i += 2
        else:
            args.append(argv[i]); i += 1
    if not args:
        print(__doc__)
        return 2
    rc = 0
    for a in args:
        r = send(json.loads(a), pipe, wait)
        print(json.dumps(r))
        if not r.get("ok", False):
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
