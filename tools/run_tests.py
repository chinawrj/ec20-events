#!/usr/bin/env python3
"""Build a host test binary with a non-network curl stub, then run integration tests."""
import os, pathlib, shlex, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[1]
(root/'build').mkdir(exist_ok=True)
binary=root/'build/ec20-events-test'
cc=shlex.split(os.environ.get('CC','cc'))
flags=shlex.split(os.environ.get('CFLAGS','-Os -std=c11 -Wall -Wextra -Werror'))
subprocess.run(cc+flags+['-DCURL_PATH="'+str(root/'tests/fake_curl.py')+'"',str(root/'src/main.c'),str(root/'src/sms.c'),'-o',str(binary)],check=True)
subprocess.run([sys.executable,str(root/'tests/test_events.py')],env=dict(os.environ,MOCK_CURL='1',TEST_BINARY=str(binary)),check=True)
