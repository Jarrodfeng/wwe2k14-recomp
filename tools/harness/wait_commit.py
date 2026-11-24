r"""Marker wait: block until a git branch gets a new commit (Session 108, lane F).

Answers: "has BRANCH moved on from SHA yet?" without a sleep loop in the agent's command. The review-fixbuilder starts at the builder's
plan commit and calls this to wait for the builder's hand-back commit.

    py -I tools\harness\wait_commit.py --branch BRANCH --after SHA [--timeout SECONDS (default 540, max 580)] [--repo PATH] [--marker TEXT]

Returns when refs/heads/BRANCH points at a commit that is not SHA and has SHA as an ancestor (with --marker: returns the newest commit
that descends from SHA, is reachable from the branch and whose subject starts with TEXT; other commits are ignored). Worktrees share
refs, so any worktree can call it.
  COMMIT <sha> <subject>   exit 0
  TIMEOUT <current sha>    exit 3   (hard upper bound: --timeout, capped at 580 s)
  MOVED <sha>              exit 4   (branch no longer descends from SHA: reset / rebase)
  NO BRANCH                exit 4
  BAD AFTER <text>         exit 4   (--after is not a commit)
Polls `git rev-parse` every 1.5 s; no CPU otherwise.  Selftest: py -I tools\harness\wait_commit_selftest.py
"""
import argparse
import subprocess
import sys
import time

MAX_TIMEOUT = 580
STEP_S = 1.5


def git(repo, *a):
    try:   # a hung git must not overrun the hard upper bound: it reads as a failed call
        r = subprocess.run(['git', '-C', repo, *a], capture_output=True, text=True, timeout=15)
    except subprocess.TimeoutExpired:
        return 124, ''
    return r.returncode, r.stdout.strip()


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--branch', required=True)
    ap.add_argument('--after', required=True)
    ap.add_argument('--timeout', type=float, default=540)
    ap.add_argument('--repo', default='.')
    ap.add_argument('--marker', default=None)
    a = ap.parse_args(argv)
    timeout = max(0.0, min(a.timeout, MAX_TIMEOUT))
    rc, after = git(a.repo, 'rev-parse', '--verify', a.after + '^{commit}')
    if rc:
        print('BAD AFTER', a.after)   # not a commit in this repo (typo, tree / blob sha, ambiguous abbreviation)
        return 4
    deadline = time.monotonic() + timeout
    cur = after
    while True:
        rc, cur = git(a.repo, 'rev-parse', '--verify', 'refs/heads/' + a.branch)
        if rc:
            print('NO BRANCH')
            return 4
        if cur != after:
            if git(a.repo, 'merge-base', '--is-ancestor', after, cur)[0] != 0:
                print('MOVED', cur)
                return 4
            if a.marker is None:
                _, subj = git(a.repo, 'log', '-1', '--format=%s', cur)
                print('COMMIT', cur, subj)
                return 0
            # --ancestry-path: only commits that descend from SHA (a merged-in branch's own commits, e.g. another lane's, never match)
            _, out = git(a.repo, 'log', '--ancestry-path', '--format=%H %s', f'{after}..{cur}')
            for line in out.splitlines():
                sha, _, subj = line.partition(' ')
                if subj.startswith(a.marker):
                    print('COMMIT', sha, subj)
                    return 0
        if time.monotonic() >= deadline:
            print('TIMEOUT', cur)
            return 3
        time.sleep(min(STEP_S, max(0.05, deadline - time.monotonic())))


if __name__ == '__main__':
    sys.exit(main())
