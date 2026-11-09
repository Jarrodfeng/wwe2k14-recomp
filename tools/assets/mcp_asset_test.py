"""Session 97: exercise the MCP asset_* tools without an MCP client (calls tools\\live\\mcp_server.py's call() the way
the server does). Starts a hidden viewer, loads a wrestler, lists motions, poses the idle at a frame with a shot, goes
back to the bind pose, loads an arena with a shot, stops. Prints each tool's text result (images by path only).

  py -I tools\\assets\\mcp_asset_test.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'live'))
import mcp_server  # noqa: E402


def show(tool, args):
    out, is_err = mcp_server.call(tool, args)
    parts = []
    for p in out or []:
        if p.get('type') == 'text':
            parts.append(p['text'][:300])
        else:
            parts.append('<%s>' % p.get('type'))
    print('%s %s -> %s %s' % (tool, json.dumps(args), 'ERROR' if is_err else 'ok', ' | '.join(parts)))
    return not is_err


def main():
    ok = True
    ok &= show('asset_start', {})
    ok &= show('asset_load', {'kind': 'wrestler', 'id': 107, 'attire': 0})
    ok &= show('asset_motions', {})
    ok &= show('asset_motion', {'name': 'idle', 'frame': 30, 'shot': 'front'})
    ok &= show('asset_motion', {'frame': 10, 'pose': 'bind'})
    ok &= show('asset_load', {'kind': 'arena', 'id': 5})
    ok &= show('asset_view', {'preset': 'ring'})
    # Session 98: scenes (actors on spots)
    ok &= show('asset_place', {'wrestler': 161, 'spot': 'stage', 'id': 'orton', 'shot': True, 'width': 640})
    ok &= show('asset_place', {'wrestler': 107, 'spot': 'ramp', 't': 0.5, 'id': 'kane', 'motion': '00000000', 'top': 40, 'act': 23110, 'frame': 40})
    ok &= show('asset_actors', {'spot_cam': 'ramp', 't': 0.5, 'width': 640})
    ok &= show('asset_motion', {'actor': 'kane', 'frame': 10})
    ok &= show('asset_actors', {'remove': 'orton'})
    ok &= show('asset_actors', {'clear': True})
    show('asset_stop', {})
    print('ALL OK' if ok else 'SOME FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
