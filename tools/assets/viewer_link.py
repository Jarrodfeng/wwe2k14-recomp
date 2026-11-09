r"""Session 96: the link to wwe2k14_assets.exe (src\assets\view\): start it, send one JSON command per line on its
named pipe, read one JSON answer. Used by tools\assets\census.py and the MCP server (tools\live\mcp_server.py, asset_*).
"""
import json
import os
import subprocess
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe')
GAME = os.path.join(REPO, 'game_data', 'extracted')


class Viewer:
    def __init__(self, exe=EXE, game=GAME, name='main'):
        self.exe, self.game, self.name = exe, game, name
        self.proc = None
        self.pipe = None

    def start(self, hidden=True):
        self.stop()
        self.proc = subprocess.Popen([self.exe, '--game', self.game, '--pipe', self.name, '--size', '1280x720',
                                      '--parent', str(os.getpid())] + (['--hidden'] if hidden else []),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        path = r'\\.\pipe\%s' % self.name
        for _ in range(200):  # 20 s for the pipe to appear
            if self.proc.poll() is not None:
                raise RuntimeError('viewer exited at start (%s)' % self.proc.returncode)
            try:
                self.pipe = open(path, 'r+b', buffering=0)
                return
            except OSError:
                time.sleep(0.1)
        self._kill()
        raise RuntimeError('viewer pipe never appeared')

    def _kill(self):
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.kill()
        except OSError:
            pass

    def send(self, cmd, timeout=60.0):
        """One command, one answer. A watchdog kills the viewer if no full line arrives in `timeout` s (a blocking read
        cannot time out by itself); on ANY error the viewer is stopped (so a late reply can never desync the pipe) and
        the error is raised: the next send() starts a fresh viewer."""
        if self.pipe is None:
            self.start()
        proc = self.proc
        timer = threading.Timer(timeout, lambda: proc.kill() if proc and proc.poll() is None else None)
        timer.daemon = True
        try:
            timer.start()
            self.pipe.write((json.dumps(cmd) + '\n').encode())
            buf = b''
            while not buf.endswith(b'\n'):
                ch = self.pipe.read(1)
                if not ch:
                    raise RuntimeError('pipe closed (no answer within %.0f s, or the viewer exited)' % timeout)
                buf += ch
            return json.loads(buf.decode('utf-8', 'replace'))
        except BaseException:
            timer.cancel()
            self.stop()
            raise
        finally:
            timer.cancel()

    def stop(self):
        try:
            if self.pipe:
                self.pipe.write(b'{"cmd":"quit"}\n')
        except OSError:
            pass
        try:
            if self.pipe:
                self.pipe.close()
        except OSError:
            pass
        self.pipe = None
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


