"""qx7_time: cold / warm wall time of the QX4 verbs. Cold = the table is built by the call into a TEMP db (--db): the real out\\harness\\index\\vcall.s<VERSION>.db is never
deleted. py -I tools/q/qx7_time.py"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
Q = os.path.join(HERE, 'q.py')
tmp = tempfile.mkdtemp(prefix='qx7_time_')
vdb = os.path.join(tmp, 'vcall.db')
for label, args in (('vcall 0x34 (COLD: builds the table)', ['vcall', '0x34', '--limit', '1', '--db', vdb]), ('vcall 0x4C (warm)', ['vcall', '0x4C', '--limit', '1', '--db', vdb]),
                    ('nodes 36 (warm)', ['nodes', '36', '--db', vdb]),
                    ('ours refs 0x836F38D0 (address held in constants)', ['ours', 'refs', '0x836F38D0'])):
    t = time.time()
    subprocess.run([sys.executable, '-I', Q] + args, capture_output=True, text=True, timeout=600)
    print(f'{time.time() - t:6.2f} s  {label}')
for n in os.listdir(tmp):
    try:
        os.remove(os.path.join(tmp, n))
    except OSError:
        pass
try:
    os.rmdir(tmp)
except OSError:
    pass
