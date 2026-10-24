#!/usr/bin/env python3
"""Install only our files on the verified EC20; secrets supplied as files, never argv values."""
import argparse, base64, contextlib, hashlib, io, pathlib, shlex, sys
from usb_adb import ADB
PROJECT=pathlib.Path(__file__).resolve().parents[1]
def shell(command):
    # ADB.shell uses sys.stdout.buffer; capture so file transfer payloads never reach logs.
    binary=io.BytesIO(); stream=io.TextIOWrapper(binary,encoding='utf-8')
    with contextlib.redirect_stdout(stream):
        dev=ADB()
        try:dev.shell(command)
        finally:dev.close()
    stream.flush(); data=binary.getvalue().decode('utf-8',errors='replace');stream.detach();return data

def checked(command):
    marker='__EC20_RESULT__'
    output=shell('('+command+'); r=$?; printf "\\n'+marker+'%s\\n" "$r"')
    if marker+'0' not in output:raise RuntimeError('Device operation failed (details withheld to protect private data)')
    return output.split(marker)[0].strip()

def upload(source,remote,mode='600'):
    payload=source.read_bytes(); q=shlex.quote(remote)
    checked('umask 077; : > '+q+'.b64')
    encoded=base64.b64encode(payload).decode()
    for i in range(0,len(encoded),2400):checked("printf '%s' '"+encoded[i:i+2400]+"' >> "+q+'.b64')
    checked('base64 -d '+q+'.b64 > '+q+'.new && chmod '+mode+' '+q+'.new && mv '+q+'.new '+q+' && rm '+q+'.b64')
    actual=checked('sha256sum '+q).split()[0]
    if actual!=hashlib.sha256(payload).hexdigest():raise RuntimeError('Upload checksum mismatch')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--key-dir',type=pathlib.Path,help='Private directory containing app-token and user-key; omitted means preserve installed keys')
    p.add_argument('--install-service',action='store_true')
    a=p.parse_args()
    if a.key_dir:
        for name in ('app-token','user-key'):
            b=(a.key_dir/name).read_bytes()
            if len(b)!=30 or not b.isalnum():p.error('Each credential file must contain exactly 30 ASCII alphanumeric bytes')
    checked('umask 077; mkdir -p /data/ec20-events; chmod 700 /data/ec20-events')
    # Preserve existing service files; refuse to overwrite an unrelated init script.
    checked('if [ -f /etc/init.d/ec20-events ]; then grep -q "ec20-events managed" /etc/init.d/ec20-events && /etc/init.d/ec20-events stop; fi')
    upload(PROJECT/'build/ec20-events-arm','/data/ec20-events/ec20-events','700')
    if a.key_dir:
        for name in ('app-token','user-key'):upload(a.key_dir/name,'/data/ec20-events/'+name)
    print(checked('/data/ec20-events/ec20-events version'))
    if a.install_service:
        upload(PROJECT/'ec20-events.init','/etc/init.d/ec20-events','755')
        checked('for n in 2 3 4 5; do test ! -e /etc/rc$n.d/S99ec20-events || test "$(readlink /etc/rc$n.d/S99ec20-events)" = ../init.d/ec20-events || exit 1; ln -sf ../init.d/ec20-events /etc/rc$n.d/S99ec20-events; done; for n in 0 1 6; do test ! -e /etc/rc$n.d/K01ec20-events || test "$(readlink /etc/rc$n.d/K01ec20-events)" = ../init.d/ec20-events || exit 1; ln -sf ../init.d/ec20-events /etc/rc$n.d/K01ec20-events; done; sync; /etc/init.d/ec20-events start')
        print('SysV service installed and start requested; cold-boot validation is separate.')
    print(checked('/data/ec20-events/ec20-events status'))
if __name__=='__main__':main()
