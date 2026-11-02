"""Offline HF tokenizer.json -> bounded C runtime asset. Not a target dependency.
All large outputs must go to an explicit E-drive directory.
"""
import argparse,hashlib,json,struct,unicodedata
from pathlib import Path

def build(source:Path,out:Path):
    doc=json.loads(source.read_text(encoding='utf-8'));model=doc['model']
    assert model['type']=='BPE' and not model['dropout'] and not model['byte_fallback'] and not model['ignore_merges']
    assert doc['normalizer']=={'type':'NFC'}
    pattern=doc['pre_tokenizer']['pretokenizers'][0]['pattern']['Regex']
    assert pattern==r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+|\p{N}| ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
    alphabet=list(range(33,127))+list(range(161,173))+list(range(174,256))
    mapped=alphabet.copy();extra=0
    for byte in range(256):
        if byte not in alphabet:alphabet.append(byte);mapped.append(256+extra);extra+=1
    decode={chr(c):b for b,c in zip(alphabet,mapped)};encode={b:chr(c) for b,c in zip(alphabet,mapped)}
    vocab=model['vocab'];entries=[(0,0xffffffff,0)]*248320;data=bytearray();added=[]
    for piece,tid in vocab.items():
        raw=bytes(decode[c] for c in piece);entries[tid]=(len(data),len(raw),0);data.extend(raw)
    for token in doc['added_tokens']:
        assert not any(token[k] for k in ['single_word','lstrip','rstrip'])
        assert not token['normalized']
        tid=token['id'];raw=token['content'].encode('utf-8');entries[tid]=(len(data),len(raw),1 if token['special'] else 2);data.extend(raw);added.append(tid)
    slots=1
    while slots<len(model['merges'])*2:slots*=2
    pairs=[(0,0,0,0)]*slots
    for rank,pair in enumerate(model['merges']):
        left,right=pair.split(' ') if isinstance(pair,str) else pair
        l,r=vocab[left],vocab[right];tid=vocab[left+right];pos=((l*0x9e3779b1)^r)&(slots-1)
        while pairs[pos][0]:
            assert pairs[pos][:2]!=(l+1,r+1),'duplicate BPE pair'
            pos=(pos+1)&(slots-1)
        pairs[pos]=(l+1,r+1,rank,tid)
    ranges=[];ccc=[];decomp=[];seq=[];compose=[]
    whitespace={9,10,11,12,13,32,0x85,0xa0,0x1680,0x2028,0x2029,0x202f,0x205f,0x3000,*range(0x2000,0x200b)}
    first=0;prev=None
    for cp in range(0x110000):
        c=chr(cp);category=unicodedata.category(c);flags=(1 if category[0]=='L' else 2 if category[0]=='M' else 4 if category[0]=='N' else 0)|(8 if cp in whitespace else 0)
        if flags!=prev:
            if prev is not None:ranges.append((first,cp-1,prev))
            first=cp;prev=flags
        klass=unicodedata.combining(c)
        if klass:ccc.append((cp,klass))
        raw=unicodedata.decomposition(c)
        if raw and not raw.startswith('<'):
            values=[int(v,16) for v in raw.split()];decomp.append((cp,len(seq),len(values)));seq.extend(values)
            if len(values)==2 and unicodedata.normalize('NFC',''.join(map(chr,values)))==c:compose.append((*values,cp))
    ranges.append((first,0x10ffff,prev));compose.sort()
    blob=bytearray(128);header=[0]*32;header[:5]=[0x31544b51,1,0,len(entries),slots]
    def put(raw):
        while len(blob)%4:blob.append(0)
        offset=len(blob);blob.extend(raw);return offset
    def rows(values,width):return b''.join(struct.pack('<'+'I'*width,*v) for v in values)
    header[5]=put(rows(entries,3));header[6]=put(data);header[7]=len(data);header[8]=put(rows(pairs,4))
    header[9]=put(rows(ranges,3));header[10]=len(ranges);header[11]=put(rows(decomp,3));header[12]=len(decomp)
    header[13]=put(struct.pack('<'+'I'*len(seq),*seq));header[14]=len(seq);header[15]=put(rows(ccc,2));header[16]=len(ccc)
    header[17]=put(rows(compose,3));header[18]=len(compose)
    header[19]=put(struct.pack('<256I',*(vocab[encode[i]] for i in range(256))))
    header[20]=put(struct.pack('<'+'I'*len(added),*added));header[21]=len(added)
    v=unicodedata.unidata_version.split('.');header[22]=int(v[0])*10000+int(v[1])*100+int(v[2]);header[2]=len(blob)
    struct.pack_into('<24I',blob,0,*header[:24]);blob[96:128]=hashlib.sha256(source.read_bytes()).digest()
    out.mkdir(parents=True,exist_ok=True);(out/'TOKEN.BIN').write_bytes(blob)
    report={'schema':'qwen35-c-tokenizer-v1','bytes':len(blob),'sha256':hashlib.sha256(blob).hexdigest(),'source_tokenizer_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'unicode_version':unicodedata.unidata_version,'vocab_entries':len(vocab),'added_tokens':len(added),'merge_pairs':len(model['merges']),'pair_slots':slots,'regex':pattern,'runtime_dependencies':'C only; offline builder is not used on the target'}
    (out/'tokenizer_manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tokenizer',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    if not str(a.out.resolve()).lower().startswith(('e:', '/mnt/e/')):p.error('--out must be on E')
    print(json.dumps(build(a.tokenizer,a.out),ensure_ascii=False))
