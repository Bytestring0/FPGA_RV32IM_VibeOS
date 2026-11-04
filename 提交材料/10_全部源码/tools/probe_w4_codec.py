"""Bounded package-verified W4-only Huffman block probe, not deployment code."""
import argparse,collections,hashlib,heapq,json,math,struct,time
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1]
HEADER=b'AGNPUPK\0'
def load_payload_spans(path):
 pin=json.loads((SYSTEM/'config/model_assets.json').read_text())['files']['HEADW4.BIN']
 if path.stat().st_size!=pin['bytes']:raise ValueError('package size mismatch')
 checksum=hashlib.sha256()
 with path.open('rb') as f:
  while data:=f.read(4<<20):checksum.update(data)
 if checksum.hexdigest()!=pin['sha256']:raise ValueError('package SHA mismatch')
 with path.open('rb') as f:
  header=f.read(512)
  if header[:8]!=HEADER or struct.unpack_from('<H',header,8)[0]!=1:raise ValueError('package header mismatch')
  count,record_bytes=struct.unpack_from('<II',header,16)
  table=struct.unpack_from('<Q',header,24)[0]
  if not 0<count<4096 or record_bytes!=64 or table!=512:raise ValueError('package index mismatch')
  spans=[];scale=0;f.seek(table)
  for _ in range(count):
   r=f.read(64);woff,wlen,soff,slen=struct.unpack_from('<QQQQ',r,24)
   if not wlen or woff+wlen>pin['bytes'] or soff+slen>pin['bytes']:raise ValueError('invalid weight span')
   spans.append((woff,wlen));scale+=slen
 if sum(s for _,s in spans)!=375947264 or scale!=11748352:raise ValueError('unexpected graph weight/scale totals')
 return spans,checksum.hexdigest()
def read_logical(f,spans,position,length):
 data=bytearray()
 for offset,n in spans:
  if position>=n:position-=n;continue
  take=min(length-len(data),n-position)
  f.seek(offset+position);data.extend(f.read(take));position=0
  if len(data)==length:return bytes(data)
 raise ValueError('truncated weight sampling')
def canonical_lengths(counts):
 heap=[(count,i,[i]) for i,count in enumerate(counts)];heapq.heapify(heap)
 lengths=[0]*16;seq=16
 while len(heap)>1:
  a,b=heapq.heappop(heap),heapq.heappop(heap)
  for i in a[2]+b[2]:lengths[i]+=1
  heapq.heappush(heap,(a[0]+b[0],seq,a[2]+b[2]));seq+=1
 next_code=0;previous_width=0;codes={}
 for width,symbol in sorted((lengths[i],i) for i in range(16)):
  next_code<<=width-previous_width
  codes[symbol]=(next_code,width);next_code+=1;previous_width=width
 return lengths,codes
def encode(raw,codes):
 out=bytearray();bits=0;hold=0
 for b in raw:
  for symbol in (b&15,b>>4):
   code,width=codes[symbol];hold=(hold<<width)|code;bits+=width
   while bits>=8:
    bits-=8;out.append((hold>>bits)&255);hold&=(1<<bits)-1
 if bits:out.append(hold<<(8-bits))
 return bytes(out)
def decode(encoded,codes,bytes_expected):
 lookup={(c,n):v for v,(c,n) in codes.items()};out=bytearray();code=0;width=0;pending=None
 for byte in encoded:
  for i in range(7,-1,-1):
   code=(code<<1)|((byte>>i)&1);width+=1
   symbol=lookup.get((code,width))
   if symbol is None:continue
   if pending is None:pending=symbol
   else:
    out.append(pending|(symbol<<4));pending=None
    if len(out)==bytes_expected:return bytes(out)
   code=width=0
 raise ValueError('truncated compressed stream')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--weights',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--block-bytes',type=int,default=65536)
 a=p.parse_args()
 if a.block_bytes<4096 or a.block_bytes>131072 or a.block_bytes&(a.block_bytes-1):p.error('block-bytes must be power of two 4096..131072')
 start=time.monotonic();spans,sha=load_payload_spans(a.weights)
 # Fit canonical code lengths using only the first four independent regions.
 counts=[0]*16
 with a.weights.open('rb') as f:
  for region in range(4):
   logical=((375947264-1048576)*region//7)&~(a.block_bytes-1)
   for byte,n in collections.Counter(read_logical(f,spans,logical,1048576)).items():
    counts[byte&15]+=n;counts[byte>>4]+=n
 lengths,codes=canonical_lengths(counts)
 weighted=0;unpacked=0;padded=0;blocks=0;index_bytes=0
 holdout_aligned=holdout_raw=holdout_index=0
 by_region=[]
 with a.weights.open('rb') as f:
  for region in range(8):
   logical=((375947264-1048576)*region//7)&~(a.block_bytes-1)
   sub={'logical_start':logical,'blocks':0,'raw_bytes':0,'encoded_bytes':0,'aligned_bytes':0}
   for delta in range(0,1048576,a.block_bytes):
    data=read_logical(f,spans,logical+delta,a.block_bytes)
    compressed=encode(data,codes)
    if decode(compressed,codes,len(data))!=data:raise ValueError('lossless roundtrip mismatch')
    aligned=(len(compressed)+511)&~511
    sub['blocks']+=1;sub['raw_bytes']+=len(data);sub['encoded_bytes']+=len(compressed);sub['aligned_bytes']+=aligned
    blocks+=1;unpacked+=len(data);weighted+=len(compressed);padded+=aligned;index_bytes+=8
    if region>=4:holdout_raw+=len(data);holdout_aligned+=aligned;holdout_index+=8
   by_region.append(sub)
 ratio=(holdout_aligned+holdout_index)/holdout_raw
 result={'scope':'4 training + 4 disjoint holdout x 1MiB stratified samples of W4 weight spans, 64KiB indexed blocks; host codec probe only','model_sha256_verified':True,'package_sha256':sha,'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'block_bytes':a.block_bytes,'symbol_code_lengths':lengths,'training_regions':4,'holdout_regions':4,'blocks_roundtrip_verified':blocks,'sample_raw_bytes':unpacked,'sample_encoded_bytes':weighted,'sample_512B_aligned_bytes':padded,'sample_index_bytes':index_bytes,'sample_effective_ratio_including_index':(padded+index_bytes)/unpacked,'holdout_effective_ratio_including_index':ratio,'optimistic_global_weight_bytes':math.ceil(375947264*ratio),'optimistic_SRAM_DDR_traffic_bytes_per_token':425444352-375947264+math.ceil(375947264*ratio),'regions':by_region,'seconds':round(time.monotonic()-start,3),'model_numeric_verified':False,'FPGA_decoder_implemented':False,'timing_closed':False,'board_verified':False,'limitations':['Random-access block decode bandwidth, LUTs/DRMs, state traffic, DDR arbitration and model equivalence unmeasured','This samples weight blocks, not an encoded full model package or official BF16 quality approval']}
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+chr(10))
 print(json.dumps({k:result[k] for k in ('blocks_roundtrip_verified','sample_effective_ratio_including_index','holdout_effective_ratio_including_index','optimistic_SRAM_DDR_traffic_bytes_per_token','seconds')},indent=2))
if __name__=='__main__':main()
