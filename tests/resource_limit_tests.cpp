/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_decoder.hpp"
#include "allocation_budget.hpp"
#include "../airplay/codec_limits.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace netease::playback;
static void check(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
static size_t read(void* context,void* data,size_t size) { return std::fread(data,1,size,static_cast<FILE*>(context)); }
static bool seek(void* context,int64_t offset,AudioDecoder::Origin origin) {
    return fseeko(static_cast<FILE*>(context),offset,
                  origin==AudioDecoder::Origin::Start?SEEK_SET:origin==AudioDecoder::Origin::Current?SEEK_CUR:SEEK_END)==0;
}
static bool tell(void* context,int64_t* offset) { *offset=ftello(static_cast<FILE*>(context));return *offset>=0; }
int main(int argc,char** argv) try {
    check(argc==3,"usage: resource-limits <oversized flac> <valid flac>");
    AllocationBudget budget;
    void* a=AllocationBudget::allocate(1024*1024,&budget);
    void* b=AllocationBudget::allocate(2*1024*1024,&budget);
    check(a && b,"normal allocations rejected");
    check(!AllocationBudget::allocate(2*1024*1024,&budget),"aggregate decoder budget exceeded");
    check(!AllocationBudget::resize(a,std::numeric_limits<size_t>::max(),&budget),"resize overflow");
    check(!AllocationBudget::allocate(std::numeric_limits<size_t>::max(),&budget),"allocation overflow");
    a=AllocationBudget::resize(a,512,&budget); check(a,"shrink failed");
    AllocationBudget::release(a,&budget); AllocationBudget::release(b,&budget);
    check(!budget.used && budget.peak<=AllocationBudget::Limit,"allocation accounting leaked");
    cloudtune::audio::configure_limits();
    check(!av_malloc(cloudtune::audio::MaximumAllocation+1),"FFmpeg oversized allocation allowed");
    AudioDecoder decoder;
    FILE* file=std::fopen(argv[1],"rb"); check(file,"oversized fixture missing");
    check(!decoder.open(read,seek,tell,file) && decoder.resource_limited(),"oversized FLAC seek table accepted");
    std::fclose(file);
    file=std::fopen(argv[2],"rb"); check(file,"valid fixture missing");
    check(decoder.open(read,seek,tell,file) && !decoder.resource_limited(),"failed allocation poisoned decoder");
    int16_t samples[2048]{};
    check(decoder.read(samples,1024)==1024,"normal high-resolution FLAC failed");
    decoder.close();std::fclose(file);
    std::puts("PASS: aggregate decoder budget, overflow, FFmpeg cap and actual oversized FLAC rejection/recovery");
} catch (const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;
}
