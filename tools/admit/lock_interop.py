"""admit selftest case 12 helper: take or test a slot file's lock exactly the way the gate's Python side does (tools\\gate\\sched.py GpuSlots:
os.open O_RDWR|O_CREAT, lseek to 0, msvcrt.locking LK_NBLCK on 1 byte; the record from byte 1).

  py -I tools\\admit\\lock_interop.py hold FILE   -> prints "locked" (or "busy"), keeps the lock until stdin reaches end-of-file
  py -I tools\\admit\\lock_interop.py try FILE    -> prints "free" (it could lock byte 0, then unlocked) or "busy"

Started only by `admit selftest`; no loops, no sleeps (stdin.read blocks)."""
import json
import msvcrt
import os
import sys


def main():
    mode, path = sys.argv[1], sys.argv[2]
    fd = os.open(path, os.O_RDWR | os.O_CREAT)
    try:
        os.lseek(fd, 0, 0)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            print('busy', flush=True)
            return 0
        if mode == 'try':
            os.lseek(fd, 0, 0)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            print('free', flush=True)
            return 0
        os.lseek(fd, 1, 0)
        os.ftruncate(fd, 1)
        os.write(fd, json.dumps({'pid': os.getpid(), 'check': 'interop', 'kind': 'compile'}).encode())
        print('locked', flush=True)
        sys.stdin.read()
        os.lseek(fd, 0, 0)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        return 0
    finally:
        os.close(fd)


if __name__ == '__main__':
    sys.exit(main())
