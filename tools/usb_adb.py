"""USB ADB transport for the verified EC20 2c7c:0125, interface 6. No credentials."""
import argparse
import struct
import sys
import time
import usb.core
import usb.util

VID, PID = 0x2C7C, 0x0125
STATUS = ['ATI', 'AT+CPIN?', 'AT+CFUN?', 'AT+CSQ', 'AT+CEREG?',
          'AT+COPS?', 'AT+CGATT?', 'AT+CGACT?', 'AT+QNWINFO',
          'AT+QCFG="usbnet"', 'AT+QCFG="usbcfg"', 'AT+QGPS?',
          'AT+CNMI?', 'AT+CMGF?']

class Device:
    def __init__(self, interface):
        devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))
        if len(devices) != 1:
            raise RuntimeError(f'Expected one EC20 2c7c:0125, found {len(devices)}. Check USB connection.')
        self.dev = devices[0]
        cfg = self.dev.get_active_configuration()
        try:
            itf = cfg[(interface, 0)]
        except IndexError as exc:
            raise RuntimeError(f'Interface {interface} is absent') from exc
        bulk = [e for e in itf if usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK]
        inep = next((e for e in bulk if e.bEndpointAddress & 128), None)
        self.ein = inep.bEndpointAddress if inep else None
        self.packet_size = inep.wMaxPacketSize if inep else 512
        self.eout = next((e.bEndpointAddress for e in bulk if not e.bEndpointAddress & 128), None)
        if self.ein is None or self.eout is None:
            raise RuntimeError('Interface does not have a bulk IN/OUT pair')
        usb.util.claim_interface(self.dev, interface)
    def close(self):
        usb.util.dispose_resources(self.dev)
    def write(self, data):
        count = self.dev.write(self.eout, data, timeout=3000)
        if count != len(data):
            raise RuntimeError('Short USB write')
    def read(self, timeout=500):
        return bytes(self.dev.read(self.ein, self.packet_size, timeout=timeout))
    def at(self, command, timeout=8):
        if not command.upper().startswith('AT') or any(c in command for c in '\r\n\x00'):
            raise ValueError('One AT command per argument is required')
        self.write((command+'\r').encode('ascii'))
        response = bytearray()
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            try:
                response.extend(self.read(400))
            except usb.core.USBTimeoutError:
                continue
            if len(response) > 1024*1024:
                raise RuntimeError('AT response exceeded 1 MiB')
            if any(t in response for t in (b'\r\nOK\r\n', b'\r\nERROR\r\n', b'+CME ERROR:', b'+CMS ERROR:')):
                return response.decode('utf-8', errors='replace')
        raise TimeoutError('AT response timed out: '+response.decode('utf-8', errors='replace'))

class ADB(Device):
    def __init__(self):
        super().__init__(6)
        self.buffer = bytearray()
    def send(self, cmd, a=0, b=0, payload=b''):
        code = int.from_bytes(cmd.encode(), 'little')
        self.write(struct.pack('<6I', code,a,b,len(payload),sum(payload)&0xffffffff,code^0xffffffff))
        if payload:
            self.write(payload)
    def readn(self, count):
        while len(self.buffer) < count:
            self.buffer.extend(self.read(10000))
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data
    def recv(self):
        c,a,b,n,s,m = struct.unpack('<6I', self.readn(24))
        if m != c^0xffffffff or n > 1024*1024:
            raise RuntimeError('Invalid ADB packet header')
        payload = self.readn(n)
        if s != sum(payload)&0xffffffff:
            raise RuntimeError('Invalid ADB checksum')
        return c.to_bytes(4,'little').decode('ascii'),a,b,payload
    def shell(self, command):
        self.send('CNXN',0x01000000,4096,b'host::\0')
        cmd,a,b,p = self.recv()
        if cmd != 'CNXN':
            raise RuntimeError(f'ADB handshake returned {cmd}; authentication is not implemented')
        service = ('shell:'+command+'\0').encode()
        if len(service)>4096:
            raise ValueError('Shell command is too long')
        self.send('OPEN',1,0,service)
        while True:
            cmd,a,b,p = self.recv()
            if cmd == 'WRTE':
                sys.stdout.buffer.write(p)
                sys.stdout.buffer.flush()
                self.send('OKAY',1,a)
            elif cmd == 'CLSE':
                self.send('CLSE',1,a)
                return
            elif cmd != 'OKAY':
                raise RuntimeError(f'Unexpected ADB message: {cmd}')
