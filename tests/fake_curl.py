#!/usr/bin/env python3
"""Test-only curl stand-in. Does not make network requests."""
import os, pathlib, sys
args=sys.argv[1:]
assert args[0]=='-q' and '--fail' in args and '--location' not in args and '-k' not in args
assert args[args.index('--proto')+1]=='=https'
assert args[-1]=='https://api.pushover.net/1/messages.json'
assert not any('T'*30 in a or 'U'*30 in a for a in args)
pathlib.Path(args[args.index('--output')+1]).write_text(os.environ.get('FAKE_RESPONSE','{"status":1,"request":"synthetic"}'))
raise SystemExit(int(os.environ.get('FAKE_EXIT','0')))
