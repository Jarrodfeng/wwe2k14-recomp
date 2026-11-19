#!/usr/bin/env python3
r"""S103 L12: --wwe2k14_dump_stage_page is OFF by default and costs nothing off (text lint of the three files that carry it; no build).
  py -I tools\scene\stage_page_lint.py
Checks: the cvar defaults to ""; reader.h only fills the frame list from it; live.cpp's QueueStagePage returns at once when StagePageDue is false (empty list);
PollAsync touches the dump's table only behind !stage_tags.empty(); the split decision reads the dump only through the same empty-list tests; QueueStagePage is called
from the two QueueAsync sites and nowhere else. Exit 1 on any FAIL."""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
live = open(os.path.join(ROOT, 'src', 'renderer_adapter', 'live.cpp'), encoding='utf-8').read()
reader = open(os.path.join(ROOT, 'src', 'renderer_adapter', 'reader.h'), encoding='utf-8').read()
fails = []


def need(cond, msg):
    print(('ok    ' if cond else 'FAIL  ') + msg)
    if not cond:
        fails.append(msg)


need(re.search(r'REXCVAR_DEFINE_STRING\(wwe2k14_dump_stage_page,\s*""', reader) is not None, 'cvar wwe2k14_dump_stage_page defaults to ""')
need(len(re.findall(r'REXCVAR_GET[(]wwe2k14_dump_stage_page[)]', reader)) == 1, 'reader.h reads the cvar once (the list)')
m = re.search(r'void QueueStagePage\([^)]*\) \{\s*if \(!StagePageDue\(guest_frame\)\) return;', live)
need(m is not None, 'QueueStagePage returns first thing when the list is empty / not reached')
m = re.search(r'bool StagePageDue\(uint64_t guest_frame\) \{.*?return !stage_pending\.empty\(\) && guest_frame >= stage_pending\.front\(\);', live, re.S)
need(m is not None, 'StagePageDue is false for an empty list')
need(re.search(r'if \(!stage_tags\.empty\(\)\) \{\s*// S103 L12', live) is not None, 'PollAsync reads the dump table only behind !stage_tags.empty()')
calls = re.findall(r'QueueStagePage\(frame, binder, message\.guest_frame\)', live)
need(len(calls) == 2, 'QueueStagePage called at the two QueueAsync sites only (found %d)' % len(calls))
need(len(re.findall(r'QueueAsync\(frame, binder, message\.guest_frame, message\.seq\)', live)) == 2, 'the two QueueAsync sites it sits beside are unchanged')
need('!stage_tags.empty() || StagePageDue(message.guest_frame)' in live, 'the split decision adds only the two empty-list tests')
print('stage_page_lint: %s' % ('FAIL' if fails else 'PASS'))
sys.exit(1 if fails else 0)
