#!/usr/bin/env python3
"""Synthetic data only. Never touches a modem, credentials, or the public network."""
import json, os, pathlib, pty, select, subprocess, tempfile, time, unittest
PROJECT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get('TEST_BINARY',str(PROJECT/'build/ec20-events'))).resolve()
def pdu(text='Hello', dcs=8, header=b'', septets=None):
    if septets is None: data=header+text.encode('utf-16-be'); udl=len(data)
    else:
        skip=(len(header)*8+6)//7; udl=skip+len(septets); packed=int.from_bytes(header,'little')
        for i,v in enumerate(septets): packed |= v << ((skip+i)*7)
        data=packed.to_bytes((udl*7+7)//8,'little')
    return (bytes([0,0x40 if header else 0,5,0x91,0x21,0x43,0xf5,0,dcs])+bytes(7)+bytes([udl])+data).hex().upper()
class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=pathlib.Path(self.tmp.name); self.root.chmod(0o700)
        self.env=dict(os.environ,EC20_EVENTS_DIR=str(self.root))
    def tearDown(self): self.tmp.cleanup()
    def call(self,*args): return subprocess.run([str(BIN),*args],env=self.env,capture_output=True,text=True)
    def test_unicode_and_surrogates(self):
        r=self.call('decode',pdu('你好，EC20 🚀')); self.assertEqual(r.returncode,0,r.stderr); self.assertEqual(r.stdout,'From: +12345\n你好，EC20 🚀\n')
    def test_gsm7_and_extensions(self):
        r=self.call('decode',pdu(dcs=0,septets=[72,101,108,108,111,32,27,40,27,101,27,41]));self.assertEqual(r.stdout,'From: +12345\nHello {€}\n')
    def test_multipart_padding(self):
        r=self.call('decode',pdu(dcs=0,header=bytes([5,0,3,42,2,1]),septets=[72,105]));self.assertEqual(r.stdout,'From: +12345 [part 1/2, ref 42]\nHi\n')
    def test_malformed_and_truncated(self):
        good=pdu('hi')
        for n in range(0,len(good),2):self.assertNotEqual(self.call('decode',good[:n]).returncode,0)
        self.assertNotEqual(self.call('decode','GG').returncode,0)
    def test_queue_limit_and_failed_flush(self):
        for i in range(100):self.assertEqual(self.call('enqueue','test',str(i)).returncode,0)
        self.assertNotEqual(self.call('enqueue','test','overflow').returncode,0)
        self.assertNotEqual(self.call('flush').returncode,0)
        self.assertEqual(len(list((self.root/'outbox').glob('event.*'))),100)
        self.assertEqual((self.root/'outbox').stat().st_mode&0o777,0o700)
    def test_sms_pty_and_restart_pending(self):
        self.call('status')
        (self.root/'pending'/'7').write_text('pending\n')
        master,slave=pty.openpty(); port=os.ttyname(slave)
        process=subprocess.Popen([str(BIN),'monitor',port],env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        commands=[];buf=b'';deadline=time.monotonic()+12
        try:
            while time.monotonic()<deadline:
                if select.select([master],[],[],0.2)[0]:
                    buf+=os.read(master,8192)
                    while b'\r' in buf:
                        raw,buf=buf.split(b'\r',1);cmd=raw.decode().strip();commands.append(cmd)
                        response={'AT+CNMI=2,1,0,0,0':'OK','AT+CMGF?':'+CMGF: 0\r\nOK','AT+CPMS?':'+CPMS: "SM",1,50,"MT",0,255,"MT",0,255\r\nOK','AT+CMGR=7':'+CMGR: 0,,20\r\n'+pdu('模拟短信 TEST')+'\r\nOK','AT+CPIN?':'+CPIN: READY\r\nOK','AT+CEREG?':'+CEREG: 0,2\r\nOK','AT+CGATT?':'+CGATT: 0\r\nOK'}.get(cmd,'ERROR')
                        os.write(master,('\r\n'+response+'\r\n').encode())
                messages=list((self.root/'outbox').glob('event.*/message'))
                if any('模拟短信 TEST' in m.read_text() for m in messages):break
            else:self.fail('No decoded SMS was queued; commands='+repr(commands))
            self.assertFalse((self.root/'pending'/'7').exists())
            self.assertTrue((self.root/'state'/'sms-7.pdu').exists())
            self.assertFalse(any('CMGL' in c or 'CMGD' in c for c in commands))
            other=self.call('monitor',port);self.assertEqual(other.returncode,4)
        finally:
            process.terminate();process.wait(timeout=35);process.stderr.close();os.close(master);os.close(slave)
        self.assertEqual(process.returncode,0)
        self.assertIn('collector=stopped',self.call('status').stdout)
class SenderTests(unittest.TestCase):
    setUp=Tests.setUp
    tearDown=Tests.tearDown
    call=Tests.call
    """Run explicitly with TEST_BINARY set to the curl-mocked build."""
    def credentials(self):
        for name,letter in [('app-token','T'),('user-key','U')]:
            p=self.root/name;p.write_text(letter*30);p.chmod(0o600)
    @unittest.skipUnless(os.environ.get('MOCK_CURL'), 'requires mock curl build')
    def test_sender_acceptance_and_retention(self):
        self.credentials()
        self.assertEqual(self.call('enqueue','test','test event').returncode,0)
        self.env['FAKE_EXIT']='60'
        self.assertNotEqual(self.call('flush').returncode,0)
        self.assertEqual(len(list((self.root/'outbox').glob('event.*'))),1)
        self.env['FAKE_EXIT']='0';self.env['FAKE_RESPONSE']='{"status":0,"errors":["no"]}'
        self.assertNotEqual(self.call('flush').returncode,0)
        self.env['FAKE_RESPONSE']='{"status":10}'
        self.assertNotEqual(self.call('flush').returncode,0)
        self.env['FAKE_RESPONSE']='{"errors":["status:1"]}'
        self.assertNotEqual(self.call('flush').returncode,0)
        self.env['FAKE_RESPONSE']='{"status":1,"request":"synthetic"}'
        self.assertEqual(self.call('flush').returncode,0)
        self.assertEqual(len(list((self.root/'outbox').glob('event.*'))),0)
    @unittest.skipUnless(os.environ.get('MOCK_CURL'), 'requires mock curl build')
    def test_insecure_key_is_refused(self):
        self.credentials();(self.root/'app-token').chmod(0o644)
        self.assertEqual(self.call('flush').returncode,5)
if __name__=='__main__':unittest.main(verbosity=2)
