#!/usr/bin/env python3
import ctypes as C
import pathlib
import struct
import subprocess
import zlib

s=pathlib.Path(__file__).resolve().parents[1]
out=s/'build/uart_asset_20261104'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(s/'sw/ag_asset_loader.c'),'-o',str(out/'loader.so')],check=True)
lib=C.CDLL(str(out/'loader.so'))
RX=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_ubyte),C.c_uint32)
TX=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_ubyte),C.c_size_t)
class Transport(C.Structure):
    _fields_=[('user',C.c_void_p),('receive',RX),('send',TX)]
lib.ag_asset_receive.argtypes=[C.POINTER(Transport),C.c_void_p,C.c_uint32]
lib.ag_asset_receive.restype=C.c_int

def packet(offset,data):
    p=struct.pack('<IHH',offset,len(data),0)+data
    return p+struct.pack('<I',zlib.crc32(p))

def run(data,packets=None,whole=None,extra_header=None):
    if packets is None:packets=b''.join(packet(i,data[i:i+256]) for i in range(0,len(data),256))
    stream=extra_header or struct.pack('<III',0x31525341,len(data),zlib.crc32(data) if whole is None else whole)
    stream+=packets
    pos=0;acks=[]
    @RX
    def receive(_,p,timeout):
        nonlocal pos
        assert timeout==10000
        if pos==len(stream):return -1
        p[0]=stream[pos];pos+=1;return 0
    @TX
    def send(_,p,n):
        assert n==5
        acks.append(struct.unpack('<BI',bytes(p[:n])));return 0
    t=Transport(None,receive,send)
    target=(C.c_ubyte*(len(data)+32))(*([0xa5]*(len(data)+32)))
    rc=lib.ag_asset_receive(C.byref(t),C.byref(target,16),len(data))
    assert bytes(target[:16])==b'\xa5'*16 and bytes(target[-16:])==b'\xa5'*16
    return rc,bytes(target[16:-16]),acks

data=bytes((i*37+11)&255 for i in range(777))
rc,result,acks=run(data)
assert rc==0 and result==data and acks[-1]==(4,len(data))
first=packet(0,data[:256]);bad=bytearray(first);bad[-1]^=1
sequence=bytes(bad)+first+first+packet(512,data[512:768])+packet(256,data[256:512])+packet(512,data[512:768])+packet(768,data[768:])
rc,result,acks=run(data,sequence)
assert rc==0 and result==data and (0x15,0) in acks and (0x15,256) in acks
assert acks.count((6,256))==2
assert run(data,whole=0)[0]==-4
assert run(data,packets=first[:-3])[0]==-2
assert run(data,extra_header=struct.pack('<III',0x31525341,len(data)+1,0))[0]==-3
print('ASSET_LOADER_PASS packets CRC32 retry duplicate ordered_offsets truncation exact_size whole_asset guard_bytes')
