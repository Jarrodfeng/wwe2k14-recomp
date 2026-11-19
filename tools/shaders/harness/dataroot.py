r"""Where out\ and game_data\ live: the main checkout when a tool runs from a worktree (git's common dir), else the repo itself.
Override with WWE2K14_DATA_ROOT. Used by check_effect*.py, shaderlib.py and shader_equiv.py (S103 T6). Nothing here reads game data."""
import os, subprocess

def data_root(repo):
    e = os.environ.get('WWE2K14_DATA_ROOT')
    if e:
        return e
    if os.path.isdir(os.path.join(repo, 'game_data')):
        return repo
    try:
        common = subprocess.run(['git', '-C', repo, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return repo
