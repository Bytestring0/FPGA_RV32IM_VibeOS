// Host test: same bare-metal C algorithm, MMIO bus, two-flop sampled SPI BFM.
#include "Vsd_spi_test_wrapper.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static Vsd_spi_test_wrapper *dut;
static uint64_t edges=0, remove_edge=~uint64_t(0);
static uint64_t wire_bytes=0,command_counts[64]={0},hw_crc_reads=0;
static bool invalidate_crc_once=false;
#ifndef EXPECT_REJECT_CMD18
#define EXPECT_REJECT_CMD18 0
#endif
#define EXPECT_MULTI (SD_SPI_MULTIBLOCK && !EXPECT_REJECT_CMD18)
static unsigned monitor_bits=0,monitor_length=0;
static uint8_t monitor_shift=0,monitor_command=0;
static void tick() {
    bool was_clk=dut->spi_clk;
    dut->clk=0; dut->eval(); dut->clk=1; dut->eval();
    if(!was_clk && dut->spi_clk) {
        ++edges;
        if(dut->spi_cs_n) {monitor_bits=0;monitor_length=0;}
        else {
            monitor_shift=uint8_t((monitor_shift<<1)|dut->spi_mosi);
            if(++monitor_bits==8) {
                monitor_bits=0;++wire_bytes;
                if(monitor_length || (monitor_shift&0xc0)==0x40) {
                    if(!monitor_length)monitor_command=monitor_shift&0x3f;
                    if(++monitor_length==6) {++command_counts[monitor_command];monitor_length=0;}
                }
            }
        }
    }
    if(edges>=remove_edge) dut->card_present=0;
}
static uint32_t transaction(bool write,uint32_t reg,uint32_t value,bool expect_error=false) {
    dut->req_addr=0x10005000u+reg; dut->req_wdata=value;
    dut->req_write=write; dut->req_wstrb=write?15:0; dut->req_valid=1;
    unsigned guard=1000;
    while(!dut->req_ready && guard--) tick();
    if(!guard) std::abort();
    tick(); dut->req_valid=0; dut->req_write=0; dut->req_wstrb=0;
    guard=1000;
    while(!dut->rsp_valid && guard--) tick();
    if(!guard || bool(dut->rsp_error)!=expect_error) {
        std::printf("MMIO error reg=%x write=%u error=%u edges=%llu cmd17=%llu cmd18=%llu stops=%llu\n",reg,unsigned(write),dut->rsp_error,
            static_cast<unsigned long long>(edges),static_cast<unsigned long long>(command_counts[17]),static_cast<unsigned long long>(command_counts[18]),static_cast<unsigned long long>(command_counts[12]));
        std::abort();
    }
    uint32_t result=dut->rsp_rdata; tick(); return result;
}
extern "C" uint32_t sd_test_mmio_read(uintptr_t,uint32_t reg) {
    uint32_t value=transaction(false,reg,0);
    if(reg==0x2c) {++hw_crc_reads;if(invalidate_crc_once){invalidate_crc_once=false;value&=0xffff;}}
    return value;
}
extern "C" void sd_test_mmio_write(uintptr_t,uint32_t reg,uint32_t value) {transaction(true,reg,value);}
#include "../sw/sd_card.c"
#if defined(TEST_FAT32_SPI)
#include "../sw/npu_fat32.c"
static int fat_read(void *,uint32_t lba,uint32_t count,void *out) {
    return sd_card_read_blocks(0x10005000u,lba,count,out,200000u);
}
#endif
#ifndef SD_TEST_TIMEOUT
#define SD_TEST_TIMEOUT (50000u*((SD_SPI_DATA_DIVIDER+3u)/4u))
#endif
static void check(bool condition,const char *message) {
    if(!condition) {std::printf("FAIL %s\n",message); std::exit(1);}
}
static void verify(const uint8_t *image,unsigned lba,unsigned count) {
    for(unsigned s=0;s<count;++s) for(unsigned i=0;i<512;++i) {
        uint8_t expected=uint8_t((lba+s)*37u+i*13u+(i>>3));
        if(image[s*512+i]!=expected) {
            std::printf("FAIL data LBA%u index%u actual%02x expected%02x\n",lba+s,i,image[s*512+i],expected);
            std::exit(1);
        }
    }
}
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    dut=new Vsd_spi_test_wrapper;
    dut->card_present=1; dut->rst=1; dut->req_valid=0; dut->rsp_ready=1;
    tick(); tick(); dut->rst=0;
    const uint8_t cmd0[5]={0x40,0,0,0,0},cmd8[5]={0x48,0,0,1,0xaa};
    check(sd_crc7(cmd0,5)==0x95 && sd_crc7(cmd8,5)==0x87,"CRC7 known SD commands");
    check(sd_crc16(reinterpret_cast<const uint8_t *>("123456789"),9)==0x31c3,"CRC16 known vector");
    int rc=sd_card_init(0x10005000u,100000u);
    if(rc) {std::printf("FAIL init rc=%d\n",rc); return 2;}
    check(transaction(false,8,0)==SD_SPI_DATA_DIVIDER,"configured data divider");
#if SD_SPI_HW_CRC == 1
    check(bool(hardware_crc)==bool(transaction(false,0x20,0)&4),"hardware CRC follows capability");
#endif
#ifdef EXPECT_INVALID_CRC_VALID
    uint8_t invalid_buffer[1024]; invalidate_crc_once=true;
    rc=sd_card_read_blocks(0x10005000u,3,2,invalid_buffer,SD_TEST_TIMEOUT);
    check(rc==SD_E_BUS&&dut->spi_cs_n,"missing hardware CRC valid cannot bypass integrity");
#endif
#ifdef EXPECT_REJECT_BUSY_TIMEOUT
    uint8_t rejected_buffer[1024];
    rc=sd_card_read_blocks(0x10005000u,3,2,rejected_buffer,SD_TEST_TIMEOUT);
    check(rc==SD_E_TIMEOUT&&!initialized&&dut->spi_cs_n,"rejected CMD18 stuck busy requires reinit");
    check(command_counts[17]==0,"no CMD17 submitted while rejected card busy");
    std::puts("PASS rejected CMD18 bounded busy timeout");delete dut;return 0;
#endif
#if defined(TEST_FAT32_SPI)
    npu_fat32_fs fs;npu_fat32_file file;
    check(npu_fat32_mount(&fs,fat_read,nullptr,0)==0,"FAT mount through real SPI");
    check(npu_fat32_open_params(&fs,&file)==0&&file.size_bytes==2685&&fs.sectors_per_cluster==4,"fragmented FAT file shape");
    uint8_t loaded[2687];std::memset(loaded,0xa5,sizeof loaded);
    uint64_t initial_bytes=wire_bytes,initial17=command_counts[17],initial18=command_counts[18],initial12=command_counts[12];
    sd_card_stats cold_stats, warm_stats;
    sd_card_reset_stats(); sd_card_get_stats(&cold_stats);
    check(cold_stats.read_calls==0&&cold_stats.crc_mode==2,"reset preserves hardware CRC mode without reads");
    check(npu_fat32_read_exact_at(&file,0,loaded+1,2685)==0,"FAT full file via CMD18/CMD12");
    for(unsigned i=0;i<2685;i++)check(loaded[i+1]==uint8_t(i*43u+(i>>5)),"FAT fragmented payload bytes");
    check(loaded[0]==0xa5&&loaded[2686]==0xa5,"FAT arbitrary destination guards");
    check(command_counts[18]-initial18==1&&command_counts[12]-initial12==1&&command_counts[17]-initial17==2,"FAT cluster run and partial tail command counts");
    sd_card_get_stats(&cold_stats);
    check(cold_stats.read_calls>0&&cold_stats.blocks_requested>=6&&cold_stats.blocks_completed>=6&&
          cold_stats.cmd18==1&&cold_stats.cmd12==1&&cold_stats.cmd17==2&&
          cold_stats.crc_hardware==cold_stats.crc_verified,"FAT cold stats and hardware CRC");
    std::printf("SD_STATS {\"read_calls\":%llu,\"blocks_requested\":%llu,\"blocks_completed\":%llu,\"crc_verified\":%llu,\"crc_hardware\":%llu,\"crc_software\":%llu,\"cmd17\":%llu,\"cmd18\":%llu,\"cmd12\":%llu,\"fallback\":%llu,\"errors\":%llu,\"crc_mode\":%u}\n",
        (unsigned long long)cold_stats.read_calls,(unsigned long long)cold_stats.blocks_requested,
        (unsigned long long)cold_stats.blocks_completed,(unsigned long long)cold_stats.crc_verified,
        (unsigned long long)cold_stats.crc_hardware,(unsigned long long)cold_stats.crc_software,
        (unsigned long long)cold_stats.cmd17,(unsigned long long)cold_stats.cmd18,
        (unsigned long long)cold_stats.cmd12,(unsigned long long)cold_stats.cmd18_fallback,
        (unsigned long long)cold_stats.errors,unsigned(cold_stats.crc_mode));
    sd_card_reset_stats(); sd_card_get_stats(&warm_stats);
    check(warm_stats.read_calls==0&&warm_stats.blocks_requested==0&&warm_stats.crc_mode==2,"warm reset counters");
    std::printf("SD_TRANSPORT {\"multiblock\":1,\"fat_multi_read\":1,\"payload_bytes\":2685,\"blocks\":6,\"selected_spi_bytes\":%llu,\"cmd17\":2,\"cmd18\":1,\"stops\":1,\"data_divider\":%u,\"pio\":true,\"fragmented_next_cluster\":true}\n",
        static_cast<unsigned long long>(wire_bytes-initial_bytes),unsigned(SD_SPI_DATA_DIVIDER));
    check(npu_fat32_read_exact_at(&file,2048,loaded+1,637)==0,"FAT read after stop into fragmented next cluster");
    for(unsigned i=0;i<637;i++)check(loaded[i+1]==uint8_t((i+2048u)*43u+((i+2048u)>>5)),"FAT next-cluster bytes");
    // Expire during a FIFO burst: recovery must never submit TX while BUSY.
    rc=sd_card_read_blocks(0x10005000u,1062,2,loaded+1,1000u);
    check(rc==SD_E_TIMEOUT&&dut->spi_cs_n&&!initialized,"active burst deadline safely requires reinit");
    check(sd_card_init(0x10005000u,100000u)==SD_OK,"reinit after active burst deadline");
    check(npu_fat32_read_exact_at(&file,0,loaded+1,2685)==0,"FAT reload after burst timeout recovery");
    for(unsigned i=0;i<2685;i++)check(loaded[i+1]==uint8_t(i*43u+(i>>5)),"FAT reloaded bytes");
    std::puts("PASS actual divider16 FAT cluster4/fragmented/head-tail -> CMD18/CMD12 SPI PIO, CRC16, guards, active-burst-timeout recovery");
    delete dut;return 0;
#endif
    uint8_t storage[1026]; std::memset(storage,0xa5,sizeof storage);
    uint8_t *image=storage+1;
#ifdef EXPECT_STOP_BUSY_TIMEOUT
    rc=sd_card_read_blocks(0x10005000u,3,2,image,SD_TEST_TIMEOUT);
    check(rc==SD_E_TIMEOUT,"CMD12 R1b busy has bounded timeout");
    check(dut->spi_cs_n && !initialized,"failed stop invalidates initialization");
    check(sd_card_read_blocks(0x10005000u,7,1,image,SD_TEST_TIMEOUT)==SD_E_ARG,"failed stop requires reinit");
    check(sd_card_init(0x10005000u,100000u)==SD_OK,"reinit clears failed stream");
    check(sd_card_read_blocks(0x10005000u,7,1,image,SD_TEST_TIMEOUT)==SD_OK,"single read after failed stop reinit");
    verify(image,7,1);std::puts("PASS CMD12 R1b bounded busy timeout/reinitialization");delete dut;return 0;
#endif
#ifdef EXPECT_CRC_ERROR
    uint64_t fault_stops=command_counts[12];
    rc=sd_card_read_blocks(0x10005000u,3,EXPECT_MULTI?2:1,image,SD_TEST_TIMEOUT);
    check(rc==SD_E_CRC,"injected card CRC16 error reported");
    check(dut->spi_cs_n && !(transaction(false,4,0)&1u),"CRC error releases CS and busy");
    check(command_counts[12]-fault_stops==(EXPECT_MULTI?1u:0u),"CRC error attempts CMD12 before release");
#elif defined(EXPECT_TOKEN_TIMEOUT)
    uint64_t fault_stops=command_counts[12];
    rc=sd_card_read_blocks(0x10005000u,3,EXPECT_MULTI?2:1,image,SD_TEST_TIMEOUT);
    check(rc==SD_E_TIMEOUT,"missing token has bounded timeout");
    check(dut->spi_cs_n && !(transaction(false,4,0)&1u),"timeout releases CS and busy");
    check(command_counts[12]-fault_stops==(EXPECT_MULTI?1u:0u),"token timeout attempts CMD12 before release");
#endif
    rc=sd_card_read_blocks(0x10005000u,3,2,image,SD_TEST_TIMEOUT);
    check(rc==SD_OK,"read two blocks after optional fault"); verify(image,3,2);
    check(storage[0]==0xa5 && storage[1025]==0xa5,"arbitrary unaligned destination guards");
    uint8_t large_storage[4098];std::memset(large_storage,0xa5,sizeof large_storage);
    uint64_t before_bytes=wire_bytes,c17=command_counts[17],c18=command_counts[18],stops=command_counts[12];
    rc=sd_card_read_blocks(0x10005000u,11,8,large_storage+1,SD_TEST_TIMEOUT);
    check(rc==SD_OK,"read eight consecutive sectors");verify(large_storage+1,11,8);
    check(large_storage[0]==0xa5&&large_storage[4097]==0xa5,"eight sector destination guards");
    check(command_counts[17]-c17==(EXPECT_MULTI?0u:8u),"CMD17 wire count");
    check(command_counts[18]-c18==(EXPECT_MULTI?1u:0u),"CMD18 wire count");
    check(command_counts[12]-stops==(EXPECT_MULTI?1u:0u),"CMD12 wire count");
    std::printf("SD_TRANSPORT {\"multiblock\":%u,\"payload_bytes\":4096,\"blocks\":8,\"selected_spi_bytes\":%llu,\"cmd17\":%llu,\"cmd18\":%llu,\"stops\":%llu,\"data_divider\":%u,\"pio\":true}\n",
        unsigned(EXPECT_MULTI),static_cast<unsigned long long>(wire_bytes-before_bytes),
        static_cast<unsigned long long>(command_counts[17]-c17),static_cast<unsigned long long>(command_counts[18]-c18),static_cast<unsigned long long>(command_counts[12]-stops),unsigned(SD_SPI_DATA_DIVIDER));
    if(transaction(false,0x20,0)&4) {
        uint32_t last_crc=transaction(false,0x2c,0);
        check((last_crc&0x10000)&&uint16_t(last_crc)==sd_crc16(large_storage+1+7*512,512),"hardware CRC equals independent software CRC after trailer");
    }
#if SD_SPI_HW_CRC == 1
    check(hw_crc_reads>0 || !hardware_crc,"hardware path actually read CRC CSR");
#else
    check(hw_crc_reads==0,"forced software path never reads CRC CSR");
#endif
#if EXPECT_REJECT_CMD18
    check(command_counts[18]==1,"CMD18 unsupported cached until reinit");
#endif
    std::printf("SD_CRC {\"register_reads\":%llu,\"forced_software\":%u,\"cmd18_rejected\":%u}\n",static_cast<unsigned long long>(hw_crc_reads),unsigned(!SD_SPI_HW_CRC),unsigned(EXPECT_REJECT_CMD18));
    rc=sd_card_read_blocks(0x10005000u,7,1,image,SD_TEST_TIMEOUT);
    check(rc==SD_OK,"read distinct LBA7"); verify(image,7,1);
    // Fabric-cycle timeout has an independent hardware bound.
    transaction(true,8,65535); transaction(true,12,8); transaction(true,0,1);
    transaction(true,16,255);
    for(unsigned i=0;i<20;++i) tick();
    check((transaction(false,4,0)&9u)==8u && dut->spi_cs_n,"hardware timeout abort");
    if(transaction(false,0x20,0)&4)check(!(transaction(false,0x2c,0)&0x10000),"hardware timeout invalidates CRC");
    transaction(true,0,4); transaction(true,8,4); transaction(true,12,100000);
    check(sd_card_read_blocks(0x10005000u,7,1,image,SD_TEST_TIMEOUT)==SD_OK,"read recovery after hardware timeout");
    // Pull the card after command/token but before a 512-byte burst can finish.
    remove_edge=edges+500;
    rc=sd_card_read_blocks(0x10005000u,7,EXPECT_MULTI?2:1,image,SD_TEST_TIMEOUT);
    check(rc==SD_E_NO_CARD && dut->spi_cs_n,"card removal abort");
    if(transaction(false,0x20,0)&4)check(!(transaction(false,0x2c,0)&0x10000),"removal invalidates CRC");
    remove_edge=~uint64_t(0); dut->card_present=1; tick();
    check(sd_card_init(0x10005000u,100000u)==SD_OK,"reinitialize after card reinsertion");
    check(sd_card_read_blocks(0x10005000u,7,1,image,SD_TEST_TIMEOUT)==SD_OK,"read after reinsertion");
    verify(image,7,1);
    // Response must stay stable under rsp backpressure.
    dut->rsp_ready=0; dut->req_valid=1; dut->req_write=0; dut->req_addr=0x1000501c;
    tick(); dut->req_valid=0;
    check(dut->rsp_valid && dut->rsp_rdata==0x53445049,"pending ID response");
    for(unsigned i=0;i<5;++i) {tick();check(dut->rsp_valid && !dut->req_ready && dut->rsp_rdata==0x53445049,"stable blocked response");}
    dut->rsp_ready=1; tick();
    transaction(false,0x100,0,true); // reserved CSR must fault, no side effects
    std::puts("PASS real C driver: MISO2FF/configured divider, SDSC/SDHC addressing, LBA3/4/7, unaligned PIO, CRC/timeout/removal recovery, bus backpressure");
    delete dut; return 0;
}
