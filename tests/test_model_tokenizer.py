"""Compare the portable C tokenizer with the independent HF Rust tokenizer.
The reference dependency and all outputs are confined to --out on E.
"""
from pathlib import Path
import argparse,json,os,random,struct,subprocess,sys,zipfile,hashlib,unicodedata
p=argparse.ArgumentParser();p.add_argument('--tokenizer',type=Path,required=True);p.add_argument('--asset',type=Path,required=True);p.add_argument('--reference-wheel',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1];out=a.out.resolve();assert str(out).lower().startswith(('e:', '/mnt/e/'));out.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(a.reference_wheel) as z:z.extractall(out/'reference-python')
sys.path.insert(0,str(out/'reference-python'))
from tokenizers import Tokenizer
doc=json.loads(a.tokenizer.read_text(encoding='utf-8'))
refs=[Tokenizer.from_str(json.dumps(doc)),Tokenizer.from_str(json.dumps(doc))]
# Removing added-token records would renumber later IDs in the Rust loader.
# Use its actual special-token parsing switch while preserving the vocabulary.
refs[0].encode_special_tokens=True
refs[1].encode_special_tokens=False
texts=['','19+23','计算7*8并保存结果','请设置一个10秒计时器','你好，世界！','a\r\n  b\t\n','  hello   world  ',"I'm WE'RE foo's bar't baz'll 'ſabc",'<|im_start|>user\n你好<|im_end|>\n<think></think><tool_call>','e\u0301 a\u0308\u0301 \u212b \u1100\u1161\u11a8','a'*3000,'👨\u200d👩\u200d👦 １２٣Ⅷ','\x00\x01\x1c\x1f\u0085\u00a0\u2028']
random.seed(7210)
alphabet='abcDEF你好吗世界0123 +-*/.\r\n\t\u0301\u0327\u2003\u00a0'
texts += [''.join(random.choice(alphabet) for _ in range(random.randrange(1,180))) for _ in range(200)]
texts += [chr(cp) for cp in range(0x110000) if unicodedata.decomposition(chr(cp)) and not unicodedata.decomposition(chr(cp)).startswith('<')][::11]
cases=[]
for text in texts:
    for special in (0,1):cases.append((text,special,refs[special].encode(text,add_special_tokens=False).ids))
fixture=out/'cases.bin'
with fixture.open('wb') as f:
    f.write(struct.pack('<I',len(cases)))
    for text,special,ids in cases:
        data=text.encode('utf-8');f.write(struct.pack('<III',len(data),special,len(ids)));f.write(data);f.write(struct.pack('<'+'I'*len(ids),*ids))
(out/'cases.json').write_text(json.dumps([{'text':t,'special':s,'ids':ids} for t,s,ids in cases],ensure_ascii=False),encoding='utf-8')
def lp(path):
    s=str(path).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:] if os.name=='nt' else s
prefix=['wsl.exe','--exec'] if os.name=='nt' else []
binary=out/'tokenizer_test'
cmd=prefix+['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+lp(root/'sw/model'),lp(root/'tests/model_tokenizer_host.c'),lp(root/'sw/model/qw_tokenizer.c'),lp(root/'sw/model/qw_sha256.c'),'-o',lp(binary)]
run=subprocess.run(cmd,capture_output=True);(out/'compile.log').write_bytes(run.stdout+run.stderr);run.check_returncode()
run=subprocess.run(prefix+[lp(binary),lp(a.asset.resolve()),lp(fixture)],capture_output=True);(out/'run.log').write_bytes(run.stdout+run.stderr);sys.stdout.buffer.write(run.stdout+run.stderr);run.check_returncode()
(out/'manifest.json').write_text(json.dumps({'cases':len(cases),'status':'PASS','source_tokenizer_sha256':hashlib.sha256(a.tokenizer.read_bytes()).hexdigest(),'asset_sha256':hashlib.sha256(a.asset.read_bytes()).hexdigest(),'reference_wheel_sha256':hashlib.sha256(a.reference_wheel.read_bytes()).hexdigest(),'sanitizers':['ASan','UBSan']},indent=2),encoding='utf-8')
