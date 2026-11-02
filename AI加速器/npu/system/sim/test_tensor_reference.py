#!/usr/bin/env python3
"""Independent Fraction oracle and reviewable tensor-v2 DDR fixtures.

Run in WSL: python3 test_tensor_reference.py --output /tmp/tensor_fixtures.json
Only the LUT knots are shared with C; rounding, saturation, graph operation
order, layout data and expected memory are computed independently in Python.
"""
import argparse
import ctypes as C
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile

SW = Path(__file__).resolve().parent.parent / "sw"
FIELDS = "opcode n m k a b c out state param0 param1 param2 stride_a stride_b stride_out flags".split()

class Job(C.Structure):
    _fields_ = [(s, C.c_uint32) for s in FIELDS]

class Mapping(C.Structure):
    _fields_ = [("data", C.c_void_p), ("capacity", C.c_size_t), ("ddr_base", C.c_uint32)]

class Numeric(C.Structure):
    _fields_ = [(s, C.c_uint32) for s in "status domain_clamps saturations underflows invalid_inputs zero_vectors".split()]

class Stats(C.Structure):
    _fields_ = [("error", C.c_uint32), ("result", C.c_uint32), ("numeric", Numeric)]

class Span(C.Structure):
    _fields_ = [(s,C.c_uint32) for s in ('offset','bytes','access')]

class Layout(C.Structure):
    _fields_ = [('spans',Span*6),('span_count',C.c_uint32),('scratch_bytes',C.c_uint32)]

def validation_checks(lib):
    """Malformed descriptors must leave all DDR bytes and query outputs intact."""
    lib.npu_tensor_query.argtypes=[C.POINTER(Job),C.POINTER(Layout)]
    lib.npu_tensor_query.restype=C.c_uint32
    base={f:0 for f in FIELDS};base.update(opcode=1,n=64,a=0,out=512)
    cases=[({'opcode':0},2),({'n':0},3),({'n':4097},3),
           ({'opcode':10,'n':3},3),({'opcode':11,'m':0},3),
           ({'opcode':12,'k':9},3),({'opcode':13,'n':129},3),
           ({'opcode':14,'n':33,'k':1},3),({'opcode':17,'n':1,'k':4097},3),
           ({'opcode':18,'n':1,'k':257},3),
           ({'opcode':8,'param1':2},4),({'opcode':11,'m':1,'param0':2},4),
           ({'opcode':13,'param0':1073741825},4),
           ({'opcode':14,'n':2,'k':1,'param0':2147483648},4),
           ({'stride_a':4},5),({'stride_b':4},5),({'stride_out':4},5),({'flags':1},5),
           ({'a':1},6),({'out':4},6),({'out':0x40000000},7),
           ({'out':0x3fffffe0},7),({'out':32},8),({'out':0},8),
           ({'opcode':12,'n':8,'k':2,'b':256,'state':0},8),
           ({'n':0,'flags':1,'out':1},3),({'flags':1,'out':1},5),
           ({'out':2048},9)]
    memory=(C.c_ubyte*1024)(*([0xa5]*1024));mapping=Mapping(C.cast(memory,C.c_void_p),1024,0)
    for changes,error in cases:
        d=dict(base);d.update(changes);j=Job(**d);st=Stats();before=bytes(memory)
        rc=lib.npu_tensor_reference(C.byref(j),C.byref(mapping),C.byref(st))
        assert rc==error and st.error==error,(changes,rc,error)
        assert bytes(memory)==before,('invalid descriptor wrote DDR',changes)
        layout=Layout();C.memset(C.byref(layout),0x5a,C.sizeof(layout));old=bytes(layout)
        q=lib.npu_tensor_query(C.byref(j),C.byref(layout))
        if error!=9:assert q==error and bytes(layout)==old,('query changed output',changes,q)
        else:assert not q
    # Read-only input aliases are valid, and a nonzero mapping DDR base must
    # translate offsets exactly rather than treating them as CPU pointers.
    d=dict(base);d.update(opcode=2,n=1,a=32,b=32,out=64);j=Job(**d)
    struct.pack_into('<i',memory,0,7);shifted=Mapping(C.cast(memory,C.c_void_p),1024,32);st=Stats()
    assert lib.npu_tensor_reference(C.byref(j),C.byref(shifted),C.byref(st))==0
    assert struct.unpack_from('<i',memory,32)[0]==14
    print(f'TENSOR_VALIDATION_PASS malformed={len(cases)} no_DDR_writes=true read_alias=true shifted_mapping=true')

def raw_w4_checks(lib):
    """Opcode19 independent exact rational scale decode plus tagged DDR guards."""
    rng=random.Random(0x1919);fixtures=[];valid=invalid=0
    def make(n,m,maximum,seed):
        stride=((m*8+31)//32)*32+(32 if seed&1 else 0)
        d={f:0 for f in FIELDS};d.update(opcode=19,n=n,m=m,a=32,stride_a=stride,param0=maximum)
        aend=32+(n-1)*stride+m*8;d['b']=(aend+63)//32*32;d['out']=(d['b']+n*m*2+63)//32*32
        memory=bytearray([0xa5]*(d['out']+n*4+32));oracle=Oracle();out=[]
        for r in range(n):
            terms=[]
            for g in range(m):
                partial=([-(1<<31),(1<<31)-1,-24576,-8192,0,8192,24576][(r*m+g+seed)%7]
                         if seed<10 else rng.randrange(-(1<<31),1<<31))
                bits=([1,0x3ff,0x400,0x3c00,0x7bff][(r*m+g+seed)%5]
                      if seed<10 else rng.randrange(1,0x7c00))
                struct.pack_into('<Ii',memory,d['a']+r*stride+g*8,r|(g<<16),partial)
                struct.pack_into('<H',memory,d['b']+(r*m+g)*2,bits)
                e=(bits>>10)&31;mant=(1024 if e else 0)+(bits&1023);shift=e-11 if e else -10
                num,den=mant*maximum,127
                if shift>=0:num<<=shift
                else:den<<=-shift
                coef=oracle.sat(oracle.rne(num,den))
                terms.append(oracle.rne(partial*coef,1<<14))
            out.append(oracle.sat(oracle.sum64(terms)))
        expected=bytearray(memory)
        for r,x in enumerate(out):struct.pack_into('<i',expected,d['out']+r*4,x)
        want={f:getattr(oracle,f) for f,_ in Numeric._fields_}
        return d,memory,expected,want
    def check(name,d,initial,expected,error,want):
        memory=(C.c_ubyte*len(initial)).from_buffer_copy(initial);mapping=Mapping(C.cast(memory,C.c_void_p),len(initial),0);stats=Stats();job=Job(**d)
        rc=lib.npu_tensor_reference(C.byref(job),C.byref(mapping),C.byref(stats))
        assert rc==error and stats.error==error,(name,rc,error)
        assert bytes(memory)==expected,(name,'DDR memory mismatch')
        actual={f:getattr(stats.numeric,f) for f,_ in Numeric._fields_};assert actual==want,(name,actual,want)
        fixtures.append(dict(name=name,descriptor=[d[f] for f in FIELDS],ddr_base=0,initial_hex=initial.hex(),expected_hex=expected.hex(),expected_error=error,expected_result=0,expected_numeric=want))
    for seed,(n,m) in enumerate([(3,5),(1,1),(7,32),(33,31)]):
        for maximum in (0,1,127,65536,0x80000000):
            d,initial,expected,want=make(n,m,maximum,seed)
            check(f'raw19_{n}_{m}_{maximum}',d,initial,expected,0,want);valid+=1
    d,initial,expected,want=make(4096,32,65536,19)
    check('raw19_max4096x32',d,initial,expected,0,want);valid+=1
    d,initial,_,_=make(3,5,65536,3);zero={f:0 for f,_ in Numeric._fields_}
    for bits in (0,0x8000,0x8001,0x7c00,0x7e00,0xfc00):
        data=bytearray(initial);struct.pack_into('<H',data,d['b']+(3*5-1)*2,bits)
        want=dict(zero);want.update(status=8,invalid_inputs=1)
        check(f'raw19_badscale_{bits}',d,data,data,4,want);invalid+=1
    for tag in (0,0xffffffff,2|(3<<16)):
        data=bytearray(initial);struct.pack_into('<I',data,d['a']+2*d['stride_a']+4*8,tag)
        check(f'raw19_badtag_{tag}',d,data,data,12,zero);invalid+=1
    for changes,error in [({'m':0},3),({'m':33},3),({'stride_a':0},5),({'stride_a':48},5),
                          ({'stride_b':32},5),({'stride_out':32},5),({'c':32},4),({'param1':1},4),
                          ({'param2':1},4),({'param0':0x80000001},4),
                          ({'n':4096,'stride_a':0xffffffe0},7),({'out':32},8)]:
        bad=dict(d);bad.update(changes)
        check('raw19_invalid_'+str(changes),bad,initial,initial,error,zero);invalid+=1
    print(f'TENSOR_RAW_W4_FRACTION_PASS valid={valid} invalid_prewrite={invalid} max_rows=4096 max_groups=32 diagnostics=exact full_DDR_compare=true')
    return fixtures

def table(name, filename):
    text = (SW / filename).read_text()
    return [int(x) for x in re.search(r"\b" + name + r"\[\d+\]\s*=\s*\{([^}]+)\}", text).group(1).split(",") if x.strip()]

LUT = {op: table("npu_fixed_lut_" + name + "_q16", "npu_fixed_lut_q16_generated.h")
       for op, name in ((4, "sigmoid"), (5, "silu"), (6, "softplus"))}
EXP = table("npu_fixed_lut_exp_negative_q30", "npu_fixed_lut_generated.h")

class Oracle:
    def __init__(self):
        self.status = self.domain_clamps = self.saturations = self.underflows = self.invalid_inputs = self.zero_vectors = 0

    def flag(self, bit):
        self.status |= bit
        name = {1: "domain_clamps", 2: "saturations", 4: "underflows", 32: "zero_vectors"}[bit]
        setattr(self, name, getattr(self, name) + 1)

    def sat(self, x, bits=32):
        low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        if x < low or x > high:
            self.flag(2)
        return min(high, max(low, x))

    def rne(self, numerator, denominator):
        value = round(Fraction(numerator, denominator))
        if numerator and not value:
            self.flag(4)
        return value

    def sum64(self, values):
        total = 0
        for x in values:
            total = self.sat(total + x, 64)
        return total

    def exp(self, x):
        x = self.rne(x, 32)
        if x < -32768 or x > 0:
            self.flag(1)
        x = min(0, max(-32768, x))
        i, r = divmod(-x, 8)
        return EXP[i] if i == 4096 else round(Fraction(EXP[i] * (8-r) + EXP[i+1] * r, 8))

    def softmax(self, a):
        mx = max(a)
        e = [self.exp(x - mx) for x in a]
        return [self.rne(x * 65536, sum(e)) for x in e]

    def activation(self, op, x):
        mag = abs(x)
        if mag > 1048576:
            self.flag(1)
            return (0 if x < 0 else 65536) if op == 4 else (0 if x < 0 else x)
        i, r = divmod(mag, 1024)
        tab = LUT[op]
        y = tab[i] if i == 1024 else round(Fraction(tab[i] * (1024-r) + tab[i+1] * r, 1024))
        return ((65536-y) if op == 4 else y-mag) if x < 0 else y

    def execute(self, d, a, b, c, state):
        op, n, k = d["opcode"], d["n"], d["k"]
        out, result = [], 0
        if op == 1: out = a[:]
        elif op == 2: out = [self.sat(x+y) for x,y in zip(a,b)]
        elif op == 3: out = [self.sat(self.rne(x*y, 1<<16)) for x,y in zip(a,b)]
        elif op in (4,5,6): out = [self.activation(op,x) for x in a]
        elif op == 7: out = [self.rne(self.exp(x),1<<14) for x in a]
        elif op == 8:
            mean = round(Fraction(sum(x*x for x in a),n))
            root = math.isqrt(mean + (d["param0"] or 4295))
            for x,w in zip(a,b):
                norm = self.rne(x*65536,root)
                value = self.sat(self.rne(norm*(w+(65536 if d["param1"] else 0)),65536),64)
                out.append(self.sat(value))
        elif op == 9: out = self.softmax(a)
        elif op == 10:
            h=n//2
            first=[self.sat(self.rne(self.sum64([a[i]*b[i],-a[i+h]*c[i]]),1<<30)) for i in range(h)]
            second=[self.sat(self.rne(self.sum64([a[i+h]*b[i],a[i]*c[i]]),1<<30)) for i in range(h)]
            out=first+second
        elif op == 11:
            for i in range(n):
                terms=[self.rne(a[i*d['m']+t]*b[i*d['m']+t],1<<14) for t in range(d['m'])]
                acc=self.sum64(terms)
                if d['param0']: acc=self.sum64([acc,c[i]])
                out.append(self.sat(acc))
        elif op == 12:
            updated=[]
            for i in range(n):
                taps=state[i*(k-1):(i+1)*(k-1)]+[a[i]]
                out.append(self.sat(self.rne(self.sum64(taps[t]*b[i*k+t] for t in range(k)),65536)))
                updated.extend(taps[1:])
            state=updated
        elif op in (13,20):
            state=[self.sat(self.rne(x*d['param0'],1<<30)) for x in state]
            delta=[]
            for t in range(n):
                mem=self.sat(self.rne(self.sum64(state[i*n+t]*b[i] for i in range(n)),1<<24))
                delta.append(self.sat(self.rne((c[t]-mem)*d['param1'],1<<30)))
            state=[self.sat(self.sum64([state[i*n+t], self.rne(b[i]*delta[t],256)])) for i in range(n) for t in range(n)]
            out=[self.sat(self.rne(self.sum64(state[i*n+t]*a[i] for i in range(n)),1<<(16 if op==20 else 24))) for t in range(n)]
        elif op == 14:
            scores=[]
            for i in range(n):
                dot=self.sum64(a[t]*b[i*k+t] for t in range(k))
                scores.append(self.sat(self.sat(self.rne(dot*d['param0'],1<<46),64)))
            p=self.softmax(scores)
            out=[self.sat(self.rne(self.sum64(p[i]*c[i*k+t] for i in range(n)),65536)) for t in range(k)]
        elif op == 15:
            result=max(abs(x) for x in a)
            if not result: self.flag(32)
            out=[self.rne(x*127,result) if result else 0 for x in a]
        elif op == 16:
            result=a.index(max(a));out=[result]
        elif op == 17:
            out=[self.sat(self.rne(self.sum64(a[i*k+t]*b[t] for t in range(k)),65536)) for i in range(n)]
        elif op == 18:
            out=[self.sat(self.rne(self.sum64(a[i]*b[i*k+t] for i in range(n)),65536)) for t in range(k)]
        return out,state,result

def lengths(d):
    op,n,m,k=d['opcode'],d['n'],d['m'],d['k']
    na,nb,nc,ns=n,0,0,0
    if op in (2,3,8): nb=n
    if op==10: nb=nc=n//2
    if op==11: na=nb=n*m;nc=n if d['param0'] else 0
    if op==12: nb=n*k;ns=n*(k-1)
    if op in (13,20): nb=nc=n;ns=n*n
    if op==14: na=k;nb=nc=n*k
    if op==17: na=n*k;nb=k
    if op==18: nb=n*k
    return na,nb,nc,ns

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    parser.add_argument('--extended-only',action='store_true',help='Only maximum dimensions and multi-step state; do not repeat the324 default cases')
    parser.add_argument('--raw-only',action='store_true',help='Only additive fused rawW4 opcode19 and its invalid guards')
    args=parser.parse_args()
    fixtures=[]
    rng=random.Random(0x4e505632)
    with tempfile.TemporaryDirectory(prefix='tensor-v2-') as work:
        so=Path(work)/'libtensor.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','-O2',str(SW/'npu_tensor_reference.c'),str(SW/'npu_fixed_math.c'),'-o',str(so)],check=True)
        lib=C.CDLL(str(so));lib.npu_tensor_reference.argtypes=[C.POINTER(Job),C.POINTER(Mapping),C.POINTER(Stats)];lib.npu_tensor_reference.restype=C.c_uint32
        if args.raw_only:fixtures=raw_w4_checks(lib)
        else:validation_checks(lib)
        for op in (() if args.raw_only else (*range(1,19),20)):
            modes=([18,19] if op in (12,13,17,18,20) else [18]) if args.extended_only else range(18)
            for mode in modes:
                d={f:0 for f in FIELDS};d.update(opcode=op,n=6 if op==10 else 7,m=3,k=5,param0=1073741824 if op in (13,14) else 0,param1=1073741824 if op==13 else 0)
                if mode>=18:
                    d['n']=4096
                    if op==11:d['m']=32
                    if op==12:d['n']=1024;d['k']=8
                    if op in (13,20):d['n']=128
                    if op in (14,18):d['n']=32;d['k']=256
                    if op==17:d['n']=32;d['k']=4096 if mode==18 else 2048
                    if mode==19 and op in (12,13,20):d['n']=8
                if mode%3==1:
                    if op==8:d['param1']=1;d['param0']=1
                    if op==11:d['param0']=1
                    if op in (13,20):d['param0']=rng.randrange(1<<30);d['param1']=rng.randrange(1<<30)
                    if op==14:d['param0']=rng.randrange(1<<31)
                na,nb,nc,ns=lengths(d)
                cursor=32
                for field,count in zip(('a','b','c','state'),(na,nb,nc,ns)):
                    d[field]=cursor;cursor+=(count*4+31)//32*32
                d['out']=cursor;capacity=cursor+max(4096,4*d['n'],4*d['k'])
                samples=[-(1<<31),(1<<31)-1,-3,-1,0,1,3,32768,-32768,65536,-65536,1048576,-1048577]
                def values(count):
                    if mode==0:return [0]*count
                    if mode<6:return [samples[(i+mode)%len(samples)] for i in range(count)]
                    bound=(1<<31) if mode%2 else 262144
                    return [rng.randrange(-bound,bound) for _ in range(count)]
                a,b,c,state=(values(count) for count in (na,nb,nc,ns))
                initial=bytearray([0xa5]*capacity)
                for field,vals in zip(('a','b','c','state'),(a,b,c,state)):
                    for i,v in enumerate(vals):struct.pack_into('<i',initial,d[field]+i*4,v)
                oracle=Oracle();out,newstate,result=oracle.execute(d,a,b,c,state)
                expected=bytearray(initial)
                for i,v in enumerate(out):
                    if op==15:expected[d['out']+i]=v&255
                    else:struct.pack_into('<i',expected,d['out']+4*i,v)
                for i,v in enumerate(newstate):struct.pack_into('<i',expected,d['state']+4*i,v)
                memory=(C.c_ubyte*capacity).from_buffer_copy(initial);mapping=Mapping(C.cast(memory,C.c_void_p),capacity,0);stats=Stats();job=Job(**d)
                error=lib.npu_tensor_reference(C.byref(job),C.byref(mapping),C.byref(stats))
                assert not error,(op,mode,error)
                assert bytes(memory)==expected,(op,mode,'memory',out,[struct.unpack_from('<i',bytes(memory),d['out']+4*i)[0] for i in range(min(len(out),10))])
                assert stats.result==result,(op,mode,'result',stats.result,result)
                actual={f:getattr(stats.numeric,f) for f,_ in Numeric._fields_}
                want={f:getattr(oracle,f) for f,_ in Numeric._fields_}
                assert actual==want,(op,mode,actual,want)
                fixtures.append(dict(name=f'op{op:02d}_case{mode:02d}',descriptor=[d[f] for f in FIELDS],ddr_base=0,initial_hex=initial.hex(),expected_hex=expected.hex(),expected_error=0,expected_result=result,expected_numeric=want))
                if mode==19 and op in (12,13,20):
                    # Feed persisted DDR state into three additional causal
                    # updates and compare every byte and per-call diagnostic.
                    for step in range(1,4):
                        previous=bytes(memory);oracle=Oracle()
                        out,newstate,result=oracle.execute(d,a,b,c,newstate)
                        expected=bytearray(previous)
                        for i,v in enumerate(out):struct.pack_into('<i',expected,d['out']+4*i,v)
                        for i,v in enumerate(newstate):struct.pack_into('<i',expected,d['state']+4*i,v)
                        error=lib.npu_tensor_reference(C.byref(job),C.byref(mapping),C.byref(stats))
                        assert not error and bytes(memory)==expected,(op,mode,step,'causal memory')
                        want={f:getattr(oracle,f) for f,_ in Numeric._fields_}
                        assert {f:getattr(stats.numeric,f) for f,_ in Numeric._fields_}==want,(op,mode,step,'causal diagnostics')
                        fixtures.append(dict(name=f'op{op:02d}_case{mode:02d}_step{step}',descriptor=[d[f] for f in FIELDS],ddr_base=0,initial_hex=previous.hex(),expected_hex=expected.hex(),expected_error=0,expected_result=result,expected_numeric=want))
        payload=dict(format='npu-tensor-v2-ddr-fixtures-1',numeric_version=0x20000,oracle='Python Fraction, independent operation order; original immutable LUT knots',fixtures=fixtures,source_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (SW/'npu_tensor_reference.c',SW/'npu_tensor_v2.h',SW/'npu_fixed_math.c',SW/'npu_fixed_lut_generated.h',SW/'npu_fixed_lut_q16_generated.h')})
        if args.output:
            args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(payload,separators=(',',':'))+'\n')
        print(f'TENSOR_FRACTION_PASS cases={len(fixtures)} opcodes={1 if args.raw_only else 19} diagnostics=exact full_DDR_compare=true')

if __name__=='__main__':main()
