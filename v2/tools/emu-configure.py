#!/usr/bin/env python3
"""Point the emulator's copy of Hermes 2 at a server, without opening the settings page.

    pebble kill && python3 tools/emu-configure.py http://127.0.0.1:8642 test-key && pebble install --emulator emery

The emulator keeps a plugin's localStorage and secure storage as plain files; this writes them.
Run it while the emulator is stopped.
"""
import dbm.dumb
import json
import os
import sys

UUID = '16f5cba8-2d99-4173-9e41-6d19458e1b5a'


def persist_dir():
    base = os.path.expanduser('~/.pebble-sdk')
    if not os.path.exists(base):
        base = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'pebble-sdk')
    versions = sorted(d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d, 'emery')))
    if not versions:
        sys.exit('Run the emery emulator once first.')
    return os.path.join(base, versions[-1], 'emery')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    url, key = sys.argv[1], sys.argv[2]
    root = persist_dir()
    os.makedirs(os.path.join(root, 'localstorage'), exist_ok=True)
    os.makedirs(os.path.join(root, 'plugin-secure-storage'), exist_ok=True)
    store = dbm.dumb.open(os.path.join(root, 'localstorage', UUID), 'c')
    cfg = json.loads(store['hp2.cfg']) if 'hp2.cfg' in store else {}
    cfg['url'] = url
    cfg.update(json.loads(sys.argv[3]) if len(sys.argv) > 3 else {})
    store['hp2.cfg'] = json.dumps(cfg)
    store.close()
    path = os.path.join(root, 'plugin-secure-storage', UUID + '.json')
    values = {}
    if os.path.exists(path):
        with open(path) as f:
            values = json.load(f)
    values['apiKey'] = key
    with open(path, 'w') as f:
        json.dump(values, f)
    print('configured', root)


main()
