// Shape-weighted RTL cycle estimate; synthetic operands, actual model call counts.
#define main tensor_regression_main
#include "test_tensor_v2.cpp"
#undef main
#include <map>
#include <tuple>
#include <sstream>
int main(int argc,char**argv){
 Verilated::commandArgs(argc,argv);
 try{
  if(argc!=2)throw std::runtime_error("expected tensor_jobs.csv");
  std::ifstream input(argv[1]);if(!input)throw std::runtime_error("missing census");
  using Key=std::tuple<unsigned,unsigned,unsigned,unsigned,unsigned,unsigned>;
  std::map<Key,unsigned> calls;std::string line;std::getline(input,line);
  while(std::getline(input,line)){
   std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
   unsigned op,n,m,k,p0,p1;if(!(row>>op>>n>>m>>k>>p0>>p1))throw std::runtime_error("invalid census row");
   calls[{op,n,m,k,p0,p1}]++;
  }
  Bench b;b.mem.resize(8<<20,0xa5);uint64_t total=0;
  puts("opcode,n,m,k,param0,param1,calls,engine_cycles,reads,writes,weighted_cycles");
  for(const auto &entry:calls){
   npu_tensor_job j{};std::tie(j.opcode,j.n,j.m,j.k,j.param0,j.param1)=entry.first;
   j.a=0x1000;j.b=0x200000;j.c=0x400000;j.out=0x600000;j.state=0x700000;
   if(j.opcode==19||j.opcode==21){j.c=0;j.stride_a=((j.m*8+31)/32)*32;}
   npu_tensor_layout layout{};if(npu_tensor_query(&j,&layout))throw std::runtime_error("invalid synthetic descriptor");
   for(unsigned i=0;i<layout.span_count;i++)if(layout.spans[i].access&1){
    auto &s=layout.spans[i];for(unsigned at=0;at<s.bytes;at+=4)put(b.mem,s.offset+at,int32_t((at*719u+7919u)%131071u)-65535);
   }
   if(j.opcode==19||j.opcode==21)for(unsigned r=0;r<j.n;r++)for(unsigned g=0;g<j.m;g++){
    put(b.mem,j.a+r*j.stride_a+g*8,(g<<16)|r);put(b.mem,j.a+r*j.stride_a+g*8+4,int(r*719u+g*997u)%32000-16000);
    uint16_t scale=0x2400;memcpy(b.mem.data()+j.b+(r*j.m+g)*2,&scale,2);
   }
   auto reads=b.reads,writes=b.writes;b.start(j);b.finish();
   if(b.d.error)throw std::runtime_error("budget execution error op"+std::to_string(j.opcode));
   uint64_t weighted=uint64_t(b.d.cycle_count)*entry.second;total+=weighted;
   printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu\n",j.opcode,j.n,j.m,j.k,j.param0,j.param1,entry.second,b.d.cycle_count,b.reads-reads,b.writes-writes,(unsigned long long)weighted);
  }
  fflush(stdout);fprintf(stderr,"TENSOR_SHAPE_BUDGET_PASS shapes=%zu weighted_cycles=%llu synthetic_operands=true board_verified=false\n",calls.size(),(unsigned long long)total);return 0;
 }catch(const std::exception&e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
