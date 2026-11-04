"""Bounded, package-verified sampling of W4 payload only, never FP16 scales."""
import argparse,collections,hashlib,json,math,struct,time,zlib
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--weights',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--sample-bytes',type=int,default=1<<20);a=p.parse_args()
start=time.monotonic();asset=json.loads((Path(__file__).resolve().parents[1]/'config/model_assets.json').read_text())['files']['HEADW4.BIN']
size=a.weights.stat().st_size
if size!=asset['bytes']:p.error('package size differs from pinned HEADW4.BIN')
h=hashlib.sha256()
with a.weights.open('rb') as f:
 while chunk:=f.read(4<<20):h.update(chunk)
if h.hexdigest()!=asset['sha256']:p.error('package SHA256 differs from pinned HEADW4.BIN')
with a.weights.open('rb') as f:
 header=f.read(512)
 if header[:8]!=b'AGNPUPK\0' or struct.unpack_from('<H',header,8)[0]!=1:p.error('unexpected package magic/version')
 count=struct.unpack_from('<I',header,16)[0];record_bytes=struct.unpack_from('<I',header,20)[0]
 table=struct.unpack_from('<Q',header,24)[0]
 if not count or count>4096 or record_bytes!=64 or table!=512:p.error('unexpected package directory')
 records=[];f.seek(table)
 for i in range(count):
  record=f.read(64)
  woff,wlen,soff,slen=struct.unpack_from('<QQQQ',record,24)
  if not wlen or woff+wlen>size or soff+slen>size or (woff&511) or (soff&511):p.error('invalid package tensor span')
  records.append((woff,wlen,soff,slen))
 weight_bytes=sum(r[1] for r in records);scale_bytes=sum(r[3] for r in records)
 if weight_bytes!=375947264 or scale_bytes!=11748352:p.error('weight/scale layout differs from audited graph')
 hist=[0]*16;ratios=[]
 for index in range(8):
  logical=((weight_bytes-a.sample_bytes)*index//7)&~31
  # A sample may straddle tensors; read strictly within the indexed W4 spans.
  remain=a.sample_bytes;where=logical;data=bytearray()
  for offset,length,_,_ in records:
   if where>=length:where-=length;continue
   take=min(remain,length-where);f.seek(offset+where);chunk=f.read(take)
   if len(chunk)!=take:p.error('truncated indexed weight span')
   data.extend(chunk);remain-=take;where=0
   if not remain:break
  if remain:p.error('sample exceeded weight payload')
  compressed=len(zlib.compress(data,1));ratios.append({'logical_weight_offset':logical,'raw':len(data),'zlib_fast_bytes':compressed})
  for word,n in collections.Counter(data).items():hist[word&15]+=n;hist[word>>4]+=n
n=sum(hist);entropy=-sum((v/n)*math.log2(v/n) for v in hist if v)
# Signed INT4 values -4..3 have nibble codes 12..15 and 0..3.
in3=sum(hist[0:4])+sum(hist[12:16]);common=sorted(range(16),key=lambda x:(-hist[x],x))[:7];coverage=sum(hist[i] for i in common)/n;bits=3+4*(1-coverage)
result={'scope':'8 x 1MiB stratified samples from package-indexed W4 weights ONLY; FP16 scales excluded; entropy is not a codec','model_hash_verified':True,'package_sha256':h.hexdigest(),'package_bytes':size,'weight_bytes':weight_bytes,'scale_bytes_excluded':scale_bytes,'tensor_records':count,'nibble_counts':hist,'entropy_bits_per_weight':entropy,'signed_3bit_central8_coverage':in3/n,'seven_common_nibbles':common,'seven_common_coverage':coverage,'naive_seven_symbol_escape_bits_per_weight':bits,'zlib_fast_samples':ratios,'elapsed_seconds':round(time.monotonic()-start,3),'board_verified':False,'numeric_policy_changed':False,'work_remaining':['Build random-access lossless format and hardware DDR decompressor','Validate full-file weight identity and model quality','Measure placed resource and real memory bandwidth']}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+chr(10))
print(json.dumps({k:result[k] for k in ('weight_bytes','scale_bytes_excluded','entropy_bits_per_weight','seven_common_coverage','naive_seven_symbol_escape_bits_per_weight','elapsed_seconds')},indent=2))
