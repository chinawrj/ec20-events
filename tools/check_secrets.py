#!/usr/bin/env python3
"""Check staged files and optional Git history against locally held secrets; never print secret values."""
import argparse,base64,pathlib,re,subprocess,sys,urllib.parse
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--key-dir',type=pathlib.Path,required=True)
p.add_argument('--history',action='store_true')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1]
def git(*args):return subprocess.check_output(['git','-C',str(root),*args])
secrets=[]
for name in ('app-token','user-key'):
    value=(a.key_dir/name).read_bytes().strip()
    if len(value)!=30 or not value.isalnum():raise SystemExit('Invalid credential file')
    secrets.extend([value,base64.b64encode(value),value.hex().encode(),urllib.parse.quote_from_bytes(value).encode()])
paths=[s.decode() for s in git('diff','--cached','--name-only','--diff-filter=ACMR','-z').split(b'\0') if s]
fail=[]
for name in paths:
    data=git('show',':'+name)
    if any(v in data for v in secrets):fail.append(name)
    if re.search(r'(^|/)(private|outbox|pending|state|evidence)(/|$)|(^|/)(app-token|user-key)$',name):fail.append(name+' (private path)')
if a.history:
    for row in git('rev-list','--objects','--all').splitlines():
        obj,*name=row.split(b' ',1)
        if git('cat-file','-t',obj.decode()).strip()!=b'blob':continue
        data=git('cat-file','blob',obj.decode())
        if any(v in data for v in secrets):fail.append((name[0].decode(errors='replace') if name else obj.decode())+' (history)')
if fail:
    print('FAILED: private material found in:');print('\n'.join(sorted(set(fail))));sys.exit(1)
print('PASS: no exact raw/base64/hex credential matches in staged files'+(' or Git history' if a.history else '')+'.')
print('This is an exact credential check, not a general personal-data detector.')
