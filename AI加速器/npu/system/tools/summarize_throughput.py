"""Summarize measured isolated RTL cost and physical DDR lower bounds, not board speed."""
import argparse,collections,csv,hashlib,json,math,os
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1]
ROOT=SYSTEM.parents[2]
def source_path(name):
 text=str(name).replace("\\","/")
 if os.name=="nt" and text.startswith("/mnt/") and len(text)>6 and text[5].isalpha() and text[6]=="/":
  return Path(text[5].upper()+":/"+text[7:])
 path=Path(text)
 return path if path.is_absolute() else ROOT/path
def sha(path):
 h=hashlib.sha256()
 with path.open("rb") as f:
  while part:=f.read(1<<20):h.update(part)
 return h.hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--budget',required=True,type=Path);p.add_argument('--census',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--clock-hz',type=int,default=162000000);p.add_argument('--codec-probe',type=Path,help='OPTIONAL stratified host-only lossless codec projection, never board speed');p.add_argument('--partial-sram',action='store_true',help='Only for source-matched SRAM census; this is an assumed traffic floor, never timing evidence')
 a=p.parse_args();budget=json.loads(a.budget.read_text());catalog=SYSTEM/'config/TENSOR_HEAD.JSON';tensors=[t for t in json.loads(catalog.read_text())['tensors'] if t.get('target_dtype')=='signed_W4_zero_point_0']
 if budget.get('passed') is not True or budget.get('synthetic_operands') is not True:p.error('need a passing isolated synthetic RTL budget')
 identity=budget.get('source_sha256',{})
 if not isinstance(identity,dict) or len(identity)<3:p.error('budget input identities missing')
 tracked=set()
 for name,expected in identity.items():
  source=source_path(name).resolve();tracked.add(source)
  snapshot=budget.get("source_snapshot",{}).get(name)
  bound=source_path(snapshot) if snapshot else source
  if not bound.is_file() or sha(bound)!=expected:p.error("budget bound input changed or missing: "+str(bound))
 if a.census.resolve() not in tracked or (SYSTEM/'rtl/npu_tensor_v2.sv').resolve() not in tracked:p.error('budget does not bind this census and tensor RTL input')
 if a.clock_hz<=0:p.error("--clock-hz must be positive")
 keys=('opcode','n','m','k','param0','param1')
 census_bound=next((source_path(budget.get("source_snapshot",{}).get(name,name)) for name in identity if source_path(name).resolve()==a.census.resolve()),a.census)
 with census_bound.open(newline='') as source:
  reader=csv.DictReader(source)
  if reader.fieldnames!=list(keys):p.error('unexpected census schema')
  counts=collections.Counter(tuple(int(row[k]) for k in keys) for row in reader)
 if not counts:p.error('empty tensor census')
 original_jobs=sum(counts.values())
 excluded=budget.get("exclusion",{}).get("opcodes",[])
 if excluded not in ([],[19]):p.error("only separately implemented opcode19 matrix fusion may be excluded")
 excluded_counts={key:count for key,count in counts.items() if key[0] in excluded}
 counts=collections.Counter({key:count for key,count in counts.items() if key[0] not in excluded})
 if sum(excluded_counts.values())!=budget.get("exclusion",{}).get("jobs",0):p.error("excluded call count inconsistent with census")
 if excluded and (budget.get("original_tensor_jobs")!=original_jobs or budget.get("measured_tensor_jobs")!=sum(counts.values())):p.error("remaining tensor call count inconsistent with census")
 rows=budget.get('rows',[])
 measured={tuple(r[k] for k in keys):r for r in rows}
 if len(measured)!=len(rows) or set(measured)!=set(counts):p.error('census shape and budget row sets differ')
 if any(row['calls']!=counts[key] or row['weighted_cycles']!=row['calls']*row['engine_cycles'] or row['engine_cycles']<=0 for key,row in measured.items()) or sum(row['weighted_cycles'] for row in rows)!=budget.get('weighted_cycles'):
  p.error('weighted budget calls, cycles or total inconsistent with census')
 byop=collections.Counter();reads=writes=0
 for key,count in counts.items():
  row=measured[key];byop[key[0]]+=count*row['engine_cycles'];reads+=count*row['reads'];writes+=count*row['writes']
 macs=sum(t['shape_export_NK'][0]*t['shape_export_NK'][1] for t in tensors);weights=sum(t['weight_bytes'] for t in tensors);scales=sum(t['scale_bytes'] for t in tensors);groups=sum(t['shape_export_NK'][0]*t['groups_per_row'] for t in tensors)
 # Raw matrix partials are two INT32 words per group, written once and reread by reduce.
 state=18*16*128*128*4;minimum=weights+scales+groups*16+2*state
 total=sum(byop.values());report={'scope':'isolated synthetic-operand RTL cycles weighted by actual single-token graph calls; physical traffic floor is separate','board_verified':False,'timing_closed':False,'quality_approved':False,'clock_hz_assumed':a.clock_hz,'tensor_jobs':sum(counts.values()),'tensor_weighted_cycles':total,'tensor_cycles_by_opcode':dict(byop.most_common()),'tensor_isolated_seconds_at_assumed_clock':total/a.clock_hz,'tensor_only_tokens_per_second_estimate':a.clock_hz/total,'tensor_ddr_reads':reads,'tensor_ddr_writes':writes,'matrix_macs':macs,'matrix_ideal_cycles_64_lanes':macs/64,'weight_bytes':weights,'scale_bytes':scales,'raw_partial_write_plus_reread_bytes':groups*16,'delta_state_read_plus_write_bytes':2*state,'ddr_traffic_floor_current_unfused_bytes':minimum,'physical_ddr_peak_bytes_per_second':4000000000,'ddr_only_tokens_per_second_ceiling_current_unfused':4000000000/minimum,'ten_tokens_per_second_ddr_peak_fraction_floor':minimum*10/4000000000,'limitations':['No CPU scheduling, cache maintenance, memcpy, activation traffic or long-context attention overhead in cycle estimate.','Synthetic operands do not validate model quality or actual DDR bandwidth.','Matrix ideal and tensor cycles are separate; they are not achieved end-to-end throughput.','Current raw partial traffic exceeds the bandwidth available for10 tokens/s even before other activation traffic.'],'source_sha256':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in (a.budget,a.census,catalog,Path(__file__))}}
 report["scope"]="Isolated synthetic-operand RTL cycles weighted by actual single-token graph calls; remaining tensor only when opcode19 excluded. Matrix fusion cost and physical traffic floor are separate."
 report["architecture"]=budget.get("architecture",{})
 report["budget_source_scope"]="immutable measurement snapshot" if budget.get("source_snapshot") else "current input files"
 report["original_tensor_jobs"]=original_jobs
 report["excluded_tensor_jobs"]=sum(excluded_counts.values())
 report["exclusion"]=budget.get("exclusion",{})
 report["heaviest_shapes"]=sorted(rows,key=lambda r:r["weighted_cycles"],reverse=True)[:12]
 grouped_nmk={}
 for row in rows:
  key=tuple(row[k] for k in ("opcode","n","m","k"))
  group=grouped_nmk.setdefault(key,dict(zip(("opcode","n","m","k"),key),calls=0,weighted_cycles=0,minimum_engine_cycles=row["engine_cycles"],maximum_engine_cycles=row["engine_cycles"]))
  group["calls"]+=row["calls"];group["weighted_cycles"]+=row["weighted_cycles"]
  group["minimum_engine_cycles"]=min(group["minimum_engine_cycles"],row["engine_cycles"]);group["maximum_engine_cycles"]=max(group["maximum_engine_cycles"],row["engine_cycles"])
 report["heaviest_opcode_nmk_groups"]=sorted(grouped_nmk.values(),key=lambda r:r["weighted_cycles"],reverse=True)[:12]
 report["tensor_cycles_and_fraction_by_opcode"]=[{"opcode":op,"cycles":cycles,"fraction":cycles/total,"calls":sum(count for key,count in counts.items() if key[0]==op)} for op,cycles in byop.most_common()]
 ideal_matrix=math.ceil(macs/64)
 report["serial_tensor_plus_ideal_matrix"]={"cycles_per_token":total+ideal_matrix,"tokens_per_second_ceiling_at_assumed_clock":a.clock_hz/(total+ideal_matrix),"scope":"Conditional on serial matrix/tensor scheduling, one MAC per lane per clock, no matrix stalls, no CPU/cache/MMIO cost; not achievable runtime evidence."}
 report["clock_scenarios"]=[{"clock_hz_assumed":hz,"tensor_only_tokens_per_second_ceiling":hz/total,"serial_tensor_plus_ideal_matrix_tokens_per_second_ceiling":hz/(total+ideal_matrix)} for hz in sorted({100000000,125000000,150000000,162000000,a.clock_hz})]
 report["serial_cpu_and_memory_extra_cycle_headroom"]=[{"target_tokens_per_second":target,"total_cycle_budget":a.clock_hz/target,"cycles_left_after_tensor_and_ideal_matrix":a.clock_hz/target-total-ideal_matrix} for target in (2,3,4,5)]
 report["limitations"]=["No CPU scheduling, cache maintenance, memcpy, activation traffic or long-context attention overhead in cycle estimate.","Synthetic operands and BFM stalls do not validate model quality or actual DDR bandwidth.","Opcode19 exclusion requires independent fusion implementation/correctness evidence. Fused matrix work is not free and is not timed here.","Clock values are assumptions, not routed timing closure or FPGA measurements.","Tensor-only and serial-ideal rates are conditional budget ceilings; no end-to-end usable rate is established."]
 if excluded:
  fused_groups=sum(key[1]*key[2]*count for key,count in excluded_counts.items())
  if fused_groups!=groups:p.error("excluded raw groups do not cover catalog exactly; cannot assume full partial traffic removal")
  fused_floor=minimum-groups*16
  report["matrix_fusion_traffic_boundary"]={"scope":"Conditional on direct matrix-scale fusion eliminating every excluded raw partial DDR roundtrip; includes weights/scales and recurrent state only, omits activation/output/cache traffic.","excluded_raw_groups":fused_groups,"raw_partial_roundtrip_bytes_removed":groups*16,"external_ddr_floor_bytes_per_token":fused_floor,"ddr_peak_bytes_per_second":4000000000,"ddr_only_tokens_per_second_ceiling":4000000000/fused_floor,"ddr_efficiency_scenarios":[{"fraction_of_peak":fraction,"ddr_only_tokens_per_second_ceiling":4000000000*fraction/fused_floor} for fraction in (0.5,0.6,0.7,0.8,1.0)]}
 if a.partial_sram:
  sram_floor=minimum-groups*16
  report['experimental_partial_sram']={'external_ddr_traffic_floor_bytes_per_token':sram_floor,'ddr_only_theoretical_ceiling_tokens_per_second':4000000000/sram_floor,'note':'SRAM routing assumes entire raw partial roundtrip eliminated; model numeric and physical runtime are separate evidence'}
 if a.codec_probe:
  if not a.partial_sram:p.error('--codec-probe requires --partial-sram; never drop partial traffic in the default DDR path')
  codec=json.loads(a.codec_probe.read_text())
  pinned=json.loads((SYSTEM/'config/model_assets.json').read_text())['files']['HEADW4.BIN']['sha256']
  if codec.get('package_sha256')!=pinned or codec.get('generator_sha256')!=sha(SYSTEM/'tools/probe_w4_codec.py') or codec.get('model_sha256_verified') is not True or codec.get('training_regions')!=4 or codec.get('holdout_regions')!=4 or codec.get('model_numeric_verified') is not False or codec.get('FPGA_decoder_implemented') is not False:
   p.error('codec report not bound to the verified generator/model and unimplemented FPGA decoder')
  regions=codec.get('regions',[])
  if codec.get('block_bytes')!=65536 or not isinstance(regions,list) or len(regions)!=8 or codec.get('blocks_roundtrip_verified')!=128 or any(r.get('blocks')!=16 or r.get('raw_bytes')!=1048576 or r.get('aligned_bytes',-1)%512!=0 or r.get('encoded_bytes',0)>r.get('aligned_bytes',-1) for r in regions):
   p.error('codec sample block sizes or alignment inconsistent')
  if regions[0]['logical_start']<0 or regions[-1]['logical_start']+1048576>weights or any(regions[i]['logical_start']+1048576>regions[i+1]['logical_start'] or regions[i]['logical_start']%65536 for i in range(7)):p.error('codec stratified regions not disjoint/aligned')
  if codec.get('sample_raw_bytes')!=sum(r['raw_bytes'] for r in regions) or codec.get('sample_encoded_bytes')!=sum(r['encoded_bytes'] for r in regions) or codec.get('sample_512B_aligned_bytes')!=sum(r['aligned_bytes'] for r in regions) or codec.get('sample_index_bytes')!=128*8:p.error('codec aggregate sample counters inconsistent')
  hold=regions[4:];holdraw=sum(r['raw_bytes'] for r in hold);holdbytes=sum(r['aligned_bytes']+r['blocks']*8 for r in hold)
  ratio=holdbytes/holdraw
  compressed=math.ceil(weights*holdbytes/holdraw)
  if not 0<ratio<1 or codec.get('holdout_effective_ratio_including_index')!=ratio or codec.get('optimistic_global_weight_bytes')!=compressed or codec.get('optimistic_SRAM_DDR_traffic_bytes_per_token')!=scales+2*state+compressed:p.error('codec holdout ratio or projected bytes inconsistent')
  floor=scales+2*state+compressed
  report['experimental_lossless_codec']={'holdout_aligned_indexed_ratio':codec['holdout_effective_ratio_including_index'],'optimistic_ddr_floor_bytes_per_token':floor,'optimistic_ddr_ceiling_tokens_per_second':4000000000/floor,'ten_tps_required_ddr_peak_fraction_floor':floor*10/4000000000,'tensor_cycles_unchanged_lower_bound':total,'tensor_cycles_to_remove_for_ten_tps_even_with_infinite_matrix_and_cpu':max(0,total-a.clock_hz//10),'tensor_only_cycles_reduction_factor_required':total/(a.clock_hz/10),'tensor_only_seconds_at_assumed_clock':total/a.clock_hz,'single_session_ten_tokens_per_second_possible_with_current_tensor_engine':total<=a.clock_hz/10,'FPGA_decoder_implemented':False,'full_package_codec_verified':False,'quality_approved':False,'physical_timing_closed':False,'source_sha256':hashlib.sha256(a.codec_probe.read_bytes()).hexdigest()}
  report['source_sha256'][str(a.codec_probe)]=sha(a.codec_probe)
  report['source_sha256'][str(SYSTEM/'tools/probe_w4_codec.py')]=codec['generator_sha256']
  report['source_sha256'][str(SYSTEM/'config/model_assets.json')]=sha(SYSTEM/'config/model_assets.json')
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
 printed={k:report[k] for k in ("scope","tensor_jobs","excluded_tensor_jobs","tensor_weighted_cycles","tensor_only_tokens_per_second_estimate","serial_tensor_plus_ideal_matrix")}
 if excluded:printed["matrix_fusion_traffic_boundary"]=report["matrix_fusion_traffic_boundary"]
 else:printed.update({k:report[k] for k in ("ddr_traffic_floor_current_unfused_bytes","ddr_only_tokens_per_second_ceiling_current_unfused")})
 print(json.dumps(printed,indent=2))
if __name__=='__main__':main()
