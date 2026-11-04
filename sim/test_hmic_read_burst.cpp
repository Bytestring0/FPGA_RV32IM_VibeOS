#include "Vag_hmic_read_burst.h"
#include "verilated.h"
#include <deque>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
struct Beat{uint32_t address;bool last;unsigned due;};
int main(int argc,char**argv){
 Verilated::commandArgs(argc,argv);Vag_hmic_read_burst d;
 std::deque<uint32_t> accepted;std::deque<Beat> returns;
 std::vector<uint32_t> input;
 for(unsigned group=0;group<64;group++){
  uint32_t base=group*2048+((group%3)==0?1000:0);
  for(unsigned i=0;i<32;i++)input.push_back(base+i*8);
  input.push_back(base+512);input.push_back(base+640);
 }
 unsigned sent=0,received=0,commands=0,burst_commands=0,cycles=0,max_beats=0;
 uint32_t random=0x12349ba7;bool held=false;uint32_t held_addr=0,held_len=0;
 try{
  d.reset_n=0;d.enable=1;d.in_arvalid=0;d.out_arready=0;d.out_rvalid=0;
  for(unsigned i=0;i<4;i++){d.clk=0;d.eval();d.clk=1;d.eval();}d.reset_n=1;
  while(received<input.size()&&cycles<200000){
   random^=random<<13;random^=random>>17;random^=random<<5;
   d.clk=0;d.in_arvalid=sent<input.size()&&(random&7)!=0&&accepted.size()<32;
   d.in_araddr=sent<input.size()?input[sent]:0;
   d.out_arready=(random%5)!=0;
   bool reply=!returns.empty()&&returns.front().due<=cycles&&(random%7)!=0;
   d.out_rvalid=reply;d.out_rlast=reply&&returns.front().last;d.out_rid=0;
   for(unsigned i=0;i<8;i++)d.out_rdata[i]=reply?(returns.front().address^(i*0x13579bdu)):0;
   d.eval();
   if(held&&(!d.out_arvalid||d.out_araddr!=held_addr||d.out_arlen!=held_len))throw std::runtime_error("burst command changed under backpressure");
   held=d.out_arvalid&&!d.out_arready;held_addr=d.out_araddr;held_len=d.out_arlen;
   bool take=d.in_arvalid&&d.in_arready,issue=d.out_arvalid&&d.out_arready;
   if(take){accepted.push_back(input[sent]);sent++;}
   if(issue){
    unsigned beats=unsigned(d.out_arlen)+1;commands++;burst_commands+=beats>1;if(beats>max_beats)max_beats=beats;
    if(beats>16||(d.out_araddr>>10)!=((d.out_araddr+(beats-1)*8)>>10))throw std::runtime_error("burst crossed 4KiB boundary");
    unsigned due=returns.empty()?cycles+3:returns.back().due+1;
    for(unsigned i=0;i<beats;i++)returns.push_back({uint32_t(d.out_araddr)+i*8,i==beats-1,due+i});
   }
   if(reply){
    if(!d.in_rvalid||!d.in_rlast||d.in_rid||accepted.empty()||accepted.front()!=returns.front().address)throw std::runtime_error("ordered burst return mismatch");
    for(unsigned i=0;i<8;i++)if(d.in_rdata[i]!=(accepted.front()^(i*0x13579bdu)))throw std::runtime_error("burst data mismatch");
    returns.pop_front();accepted.pop_front();received++;
   }
   d.clk=1;d.eval();cycles++;
  }
  if(received!=input.size()||!accepted.empty()||!returns.empty()||burst_commands<64||max_beats<4)throw std::runtime_error("burst run failed to drain or coalesce");
  // Malformed LAST and unreserved response must propagate to bridge fault path.
  d.clk=0;d.in_arvalid=0;d.out_rvalid=1;d.out_rlast=1;d.eval();
  if(!d.in_rvalid||d.in_rlast)throw std::runtime_error("unexpected return accepted as valid LAST");
  // Reset discards reservations and prevents stale command emission.
  d.reset_n=0;d.clk=1;d.eval();d.clk=0;d.eval();d.reset_n=1;d.out_rvalid=0;d.eval();
  if(d.out_arvalid)throw std::runtime_error("stale command after reset");
  std::printf("HMIC_BURST_PASS requests=%u commands=%u multi_beat_commands=%u max_beats=%u cycles=%u ordered_data=1 boundary_4k=1 backpressure=1 unexpected_return=1 reset=1\n",received,commands,burst_commands,max_beats,cycles);
  return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"HMIC_BURST_FAIL %s sent=%u received=%u cycle=%u\n",e.what(),sent,received,cycles);return 1;}
}
