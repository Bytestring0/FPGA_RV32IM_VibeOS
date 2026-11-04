"""Fail closed on board smoke readiness; do not confuse it with board qualification."""
import argparse,hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
SYSTEM=Path(__file__).resolve().parents[1]
def source_path(name,label):
 text=str(name).replace('\\','/')
 if text.startswith('/mnt/') and len(text)>6 and text[5].isalpha() and text[6]=='/':
  return Path(text[5].upper()+':/'+text[7:])
 path=Path(text)
 return path if path.is_absolute() else (SYSTEM if label=='quality' else ROOT)/path
def digest(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  while chunk:=f.read(1<<20):h.update(chunk)
 return h.hexdigest()
def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('system','firmware','pds','quality','bitstream'):
  p.add_argument('--'+name+'-report',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True)
 a=p.parse_args(argv);issues=[]
 def load(path,label):
  if not path.is_file():issues.append(label+': report missing');return {}
  try:return json.loads(path.read_text(encoding='utf-8'))
  except (ValueError,OSError) as exc:issues.append(label+': unreadable: '+str(exc));return {}
 sim=load(a.system_report,'system');fw=load(a.firmware_report,'firmware')
 phy=load(a.pds_report,'physical');quality=load(a.quality_report,'quality')
 bit_report=load(a.bitstream_report,'bitstream')
 if sim.get('passed') is not True:issues.append('CPU+NPU+Cache+DDR simulation missing/failing')
 if sim.get('cpu_mode')!='original':issues.append('system test used rewritten compatibility CPU, not production RTL')
 if fw.get('passed') is not True:issues.append('Agent firmware build missing/failing')
 if phy.get('mode')!='pnr' or phy.get('passed') is not True:issues.append('current RTL PDS place-and-route missing/failing')
 if phy.get('all_constrained_paths_met') is not True:issues.append('full constrained setup/hold timing is unproven')
 if not phy.get('synthesis_resources') or phy.get('queue4_cdc_audit',{}).get('exit_code')!=0:issues.append('resource or four-credit DDR CDC proof missing')
 if fw.get('clock_hz')!=phy.get('clock_mhz',-1)*1000000:issues.append('Agent firmware clock differs from physical build')
 for key in ('partial_sram_enabled','matrix_lanes','ddr_outstanding','tensor_parallel_raw','tensor_parallel_delta'):
  if sim.get(key)!=phy.get(key) or (key=='partial_sram_enabled' and fw.get(key)!=phy.get(key)):issues.append('hardware configuration differs: '+key)
 for label,path,report in (('firmware',a.firmware_report,fw),('simulation',a.system_report,sim)):
  bound=phy.get(label+'_binding')
  if not path.is_file() or not isinstance(bound,dict) or not bound.get('report'):
   issues.append('physical build lacks matching '+label+' identity')
  elif Path(bound['report']).resolve()!=path.resolve() or bound.get('sha256')!=digest(path):
   issues.append('physical build '+label+' digest mismatch')
 fwhex=fw.get('artifacts',{}).get('firmware.hex',{}).get('sha256')
 if not fwhex or not phy.get('firmware_sha256') or fwhex!=phy['firmware_sha256']:issues.append('physical firmware.hex digest differs from Agent artifact')
 if quality.get('quality_approved') is not True or quality.get('official_bf16_comparison') is not True:issues.append('model accuracy against official BF16 is not approved')
 if quality.get('model_result',{}).get('execution_passed') is not True:issues.append('full model quality execution evidence is missing')
 for key in ('precise_norm','precise_reduce','partial_sram_enabled'):
  if not isinstance(quality.get(key),bool) or not isinstance(fw.get(key),bool) or quality[key]!=fw[key]:issues.append('quality/firmware numerical policy mismatch: '+key)
 if fw.get('external_assets',{}).get('verified') is not True:issues.append('firmware external model assets not verified at build')
 inputs=quality.get('input_sha256',{})
 for label,filename in (('weights','HEADW4.BIN'),('cpu','CPU_RAW.BIN'),('catalog','HDTENSOR.JSN')):
  asset=fw.get('external_assets',{}).get('files',{}).get(label,{})
  deployed=fw.get('model_deployment',{}).get(label,{})
  target=ROOT/'Data'/filename
  quality_hashes=[recorded for name,recorded in inputs.items() if source_path(name,'quality').resolve()==target.resolve()]
  if len(quality_hashes)!=1 or not asset.get('path') or source_path(asset['path'],'firmware').resolve()!=target.resolve() or not asset.get('sha256') or asset['sha256']!=quality_hashes[0] or deployed.get('sha256')!=quality_hashes[0] or not target.is_file() or target.stat().st_size!=asset.get('bytes') or digest(target)!=quality_hashes[0]:
   issues.append('quality inputs differ from deployed model asset: '+filename)
 if fw.get('tokenizer_verified_at_build') is not True:issues.append('firmware tokenizer was not verified at build')
 for label,report in (('system',sim),('firmware',fw),('physical',phy),('quality',quality)):
  identities=report.get('source_sha256')
  if not isinstance(identities,dict) or not identities:issues.append(label+': source identity missing');continue
  for name,expected in identities.items():
   path=source_path(name,label)
   if not path.is_file() or digest(path)!=expected:
    issues.append(label+': source changed since report: '+name);break
 vendor=phy.get('vendor_source_sha256',{})
 if not isinstance(vendor,dict) or not vendor:issues.append('PDS vendor IP source identity missing')
 else:
  for name,expected in vendor.items():
   source=source_path(name,'physical')
   if not source.is_file() or digest(source)!=expected:
    issues.append('PDS vendor IP source changed: '+name);break
 for key,filename in (('generated_rtl_sha256','ag_npu_board.sv'),('constraint_sha256','board.fdc'),('firmware_sha256','firmware.hex')):
  source=a.pds_report.resolve().parent/filename
  if not source.is_file() or not phy.get(key) or digest(source)!=phy[key]:issues.append('PDS frozen input changed: '+filename)
 # Candidate receipt is emitted by finish_tensor_bitstream.py, not the PNR
 # report. Require the exact same routed project, inputs and receipt bytes.
 run=a.pds_report.resolve().parent
 if a.bitstream_report.resolve()!=run/'bitstream_candidate.json':
  issues.append('bitstream candidate report belongs to a different physical run')
 if bit_report.get('passed') is not True or bit_report.get('internal_timing_passed') is not True:
  issues.append('completed source-matched bitstream candidate is missing')
 if a.pds_report.is_file() and bit_report.get('routed_report_sha256')!=digest(a.pds_report):
  issues.append('bitstream receipt does not bind exact routed PDS report')
 for key in ('firmware_sha256','generated_rtl_sha256','constraint_sha256'):
  if not bit_report.get(key) or bit_report.get(key)!=phy.get(key):
   issues.append('bitstream differs from PDS '+key)
 for key,filename in (('route_sha256','place_route/ag_npu_board_top_pnr.adf'),('timing_sha256','report_timing/ag_npu_board_top.rtr')):
  artifact=run/filename
  if not artifact.is_file() or bit_report.get(key)!=digest(artifact):
   issues.append('routed artifact missing/mismatched: '+filename)
 bit=bit_report.get('bitstream')
 expected_bit=run/'generate_bitstream/ag_npu_board_top.sbit'
 if not isinstance(bit,dict) or not bit.get('path') or not expected_bit.is_file() or Path(bit['path']).resolve()!=expected_bit or bit.get('bytes')!=expected_bit.stat().st_size or bit.get('sha256')!=digest(expected_bit):
  issues.append('source-matched downloadable bitstream with SHA256 is missing')
 result={'ready_for_board_smoke':not issues,'board_qualified':False,'scope':'pre-board smoke prerequisites only; no hardware run or contest qualification performed here','issues':issues,'reports':{k:str(getattr(a,k+'_report')) for k in ('system','firmware','pds','quality')}}
 a.output.parent.mkdir(parents=True,exist_ok=True)
 a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+chr(10),encoding='utf-8')
 print(json.dumps(result,ensure_ascii=False,indent=2))
 return 0 if not issues else 2
if __name__=='__main__':raise SystemExit(main())
