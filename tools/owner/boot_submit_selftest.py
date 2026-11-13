# Lane 231 (LEAVE_THE_COMMAND_STREAM 3a): builds tools/owner/boot_submit_test.cpp (the shipped src/owner_boot_submit.h) and runs it: the honest arm must pass (exit 0),
# every planted fault must FAIL the check named for it (exit 1). Output: out/tools/boot_submit_test.exe (gitignored).
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'boot_submit_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
clang = r'C:\Program Files\LLVM\bin\clang++.exe'
b = subprocess.run([clang, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-o', str(exe), str(root / 'tools' / 'owner' / 'boot_submit_test.cpp')], capture_output=True, text=True, timeout=300)
if b.returncode:
    print(b.stdout + b.stderr)
    print('FAIL: build')
    sys.exit(1)
ok = True
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
print(r.stdout.strip())
if r.returncode != 0:
    ok = False
for plant, named in (('rptr_unwritten', 'boot.words_the_game_reads'), ('rptr_wrong', 'boot.words_the_game_reads'), ('wrong_block', 'boot.words_the_game_reads'),
                     ('scan_blind_mem_write', 'mem_write.words_the_game_reads'), ('scratch_unwritten', 'scratch.words_the_game_reads'), ('scratch_unwritten', 'boot7157.words_the_game_reads'), ('scan_blind_scratch', 'scratch_forms.go_to_original'), ('scan_blind_ib', 'ib.words_the_game_reads'),
                     ('scan_blind_cond_write_mem', 'cond_write_mem.words_the_game_reads'), ('scan_blind_bad', 'truncated.goes_to_original'),
                     ('no_register_writes', 'forced.scratch_writeback'), ('old_rule', 'prefix.then_segment'), ('scan_blind_event_elsewhere', 'event_write_elsewhere.words_the_game_reads')):
    p = subprocess.run([str(exe), 'plant', plant], capture_output=True, text=True, timeout=120)
    failed = [l for l in p.stdout.splitlines() if l.startswith('FAIL ') and named in l]
    caught = p.returncode == 1 and bool(failed)
    print(f'planted {plant}: {"caught by " + named if caught else "NOT CAUGHT by " + named + " (exit %d)" % p.returncode}')
    ok = ok and caught
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
