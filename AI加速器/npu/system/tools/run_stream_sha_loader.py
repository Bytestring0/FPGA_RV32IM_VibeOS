#!/usr/bin/env python3
"""Single small-fixture loader/SHA batch plus one host system build-only.
No full-model load, decode, SD write or board action.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib
SYSTEM=Path(__file__).resolve().parents[1]
MODEL=SYSTEM/'integration/agent_runtime_20261101/sw/model'
ROOT=SYSTEM.parents[2]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def digest(data):return ','.join('0x%02x'%b for b in hashlib.sha256(data).digest())
def array(name,data):return 'static const unsigned char '+name+'[]={'+','.join('0x%02x'%x for x in data)+'};\n'
def command(argv,log,cwd=SYSTEM):
    with log.open('w',encoding='utf-8') as file:
        result=subprocess.run([str(a) for a in argv],cwd=cwd,stdout=file,stderr=subprocess.STDOUT)
    if result.returncode:raise RuntimeError(f'{log}: exit {result.returncode}')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--toolchain',required=True,type=Path)
    a=p.parse_args();out=a.output.resolve()
    if out.exists() or not out.is_relative_to(SYSTEM/'build'):raise ValueError('fresh system/build output required')
    out.mkdir(parents=True)
    selected=[MODEL/n for n in ('qw_load.c','qw_sha256.c','qw_sha256.h')]
    protected=[SYSTEM/'sw'/n for n in ('npu_storage.c','npu_package.c')]+[MODEL/'vendor/npu_v1'/n for n in ('npu_storage.c','npu_package.c')]
    before={str(x.relative_to(SYSTEM)):sha(x) for x in selected+protected}
    result={'passed':False,'scope':'SMALL_SYNTHETIC_LOADER_CONTRACT_AND_HOST_BUILD_ONLY','no_model_decode':True,'no_board':True,'source_sha256':before}
    try:
        lengths=[0,1,55,56,63,64,65,8191,8192,8193,16384,32768,65536,65537,131101]
        data=bytes((i*37+(i>>3))&255 for i in range(max(lengths)))
        header=['struct digest_case {size_t bytes;unsigned char digest[32];};','static const struct digest_case digest_cases[]={']
        header += ['{%du,{%s}},'%(n,digest(data[:n])) for n in lengths];header.append('};')
        package=bytearray(2048);package[:8]=b'AGNPUPK\0'
        struct.pack_into('<HHIIIQQQIIII',package,8,1,512,0,1,64,512,1024,2048,0,1,0x01020304,0)
        name='projection.weight';name_id=2166136261
        for byte in name.encode():name_id=((name_id^byte)*16777619)&0xffffffff
        struct.pack_into('<IIIIIIQQQQII',package,512,name_id,0,1,128,128,64,1024,64,1536,2,0,0)
        package[1024:1088]=bytes(range(64));package[1536:1538]=b'\x00\x3c'
        struct.pack_into('<I',package,48,zlib.crc32(package)&0xffffffff)
        cpu=bytes((i*13+7)&255 for i in range(8192))
        catalog=json.dumps({'tensors':[{'name':name,'target_dtype':'signed_W4_zero_point_0'},
            {'name':'token_embd.weight','target_dtype':'BF16','source_shape':[2,1024],'source_raw_offset':0,'source_raw_bytes':4096},
            {'name':'output_norm.weight','target_dtype':'F32','source_shape':[1024],'source_raw_offset':4096,'source_raw_bytes':4096}]},separators=(',',':')).encode()
        bad=bytearray(package);bad[1024]^=1
        for name,blob in [('package_data',package),('cpu_data',cpu),('catalog_data',catalog)]:header.append(array(name,blob))
        for name,blob in [('package_sha',package),('cpu_sha',cpu),('catalog_sha',catalog),('package_tail_sha',package+b'TAIL!'),('package_bad_crc_sha',bad)]:header.append('static const unsigned char '+name+'[32]={'+digest(blob)+'};')
        (out/'stream_sha_fixtures.h').write_text('\n'.join(header)+'\n',encoding='ascii')
        cc=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
            '-fno-omit-frame-pointer','-I',MODEL,'-I',SYSTEM/'sw','-I',out,
            SYSTEM/'sim/test_stream_sha_loader.c',MODEL/'qw_sha256.c',MODEL/'qw_json.c',
            MODEL/'vendor/npu_v1/npu_storage.c',MODEL/'vendor/npu_v1/npu_package.c','-o',out/'test_stream_sha_loader']
        command(cc,out/'compile.log');command([out/'test_stream_sha_loader'],out/'test.log')
        text=(out/'test.log').read_text()
        if 'PASS_STREAM_SHA_LOADER' not in text:raise RuntimeError('loader assertions missing')
        result['loader_test']=text.strip();result['padding_and_chunk_lengths']=lengths
        tc=a.toolchain/'bin/riscv-none-elf-'
        command([str(tc)+'gcc','-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-std=c11','-ffreestanding','-fno-builtin',
                 '-fstack-usage','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DQW_ENABLE_TENSOR_V2',
                 '-I',MODEL,'-I',MODEL/'freestanding_include','-I',SYSTEM/'sw','-c',MODEL/'qw_load.c','-o',out/'qw_load.o'],out/'rv32_compile.log')
        command([str(tc)+'nm','-u',out/'qw_load.o'],out/'rv32_undefined.log')
        undefined=(out/'rv32_undefined.log').read_text()
        if any(word in undefined for word in (' malloc',' calloc',' realloc',' free')):raise RuntimeError('allocator introduced into loader')
        command([sys.executable,SYSTEM/'tools/run_tensor_model_v2.py','--output',out/'host_build','--build-only'],out/'host_build.log')
        host=json.loads((out/'host_build/report.json').read_text())
        if host.get('compile_exit')!=0 or not host.get('build_only') or not host.get('sources_unchanged'):raise RuntimeError('host build-only failed')
        result['host_build_only']=True;result['host_input_sha256']=host['input_sha256']
        result['sources_unchanged']=all(sha(SYSTEM/name)==value for name,value in before.items())
        result['passed']=result['sources_unchanged']
        if not result['passed']:raise RuntimeError('source changed during batch')
    except BaseException as exc:
        result['failure']=repr(exc);raise
    finally:(out/'report.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(text.strip(),flush=True);print('PASS_STREAM_SHA_BATCH',out/'report.json',flush=True)
if __name__=='__main__':main()
