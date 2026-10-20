/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/playback_protocol.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
bool horizon=false, failPublish=false, failRestore=false;
bool suffix(const char* value,const char* tail) {
    const auto n=std::strlen(value),m=std::strlen(tail);
    return n>=m && std::strcmp(value+n-m,tail)==0;
}
void require(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
uint64_t readId(const std::string& path) {
    FILE* file=std::fopen(path.c_str(),"rb");
    require(file!=nullptr,"expected queue file is missing");
    NcmQueueHeader header{};
    NcmQueueEntry entry{};
    const bool read=std::fread(&header,sizeof(header),1,file)==1 &&
        std::fread(&entry,sizeof(entry),1,file)==1;
    std::fclose(file);
    require(read && std::memcmp(header.magic,"NCMQUEUE",8)==0 &&
        header.version==NCM_PROTOCOL_VERSION && header.count==1,"queue content was corrupted");
    return entry.id;
}
}
extern "C" int __real_rename(const char*,const char*);
extern "C" int __wrap_rename(const char* source,const char* destination) {
    if(horizon && access(destination,F_OK)==0) { errno=EEXIST;return -1; }
    if(failPublish && suffix(source,".tmp")) { errno=EIO;return -1; }
    if(failRestore && suffix(source,".bak")) { errno=EACCES;return -1; }
    return __real_rename(source,destination);
}
int main() {
    char temporary[]="/tmp/ncm-queue-replace-XXXXXX";
    char* directory=mkdtemp(temporary);
    if(!directory)return 2;
    const std::string base=directory,path=base+"/queue.nsq";
    try {
        NcmQueueEntry entry{};
        entry.id=1;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==0,"initial queue write failed");
        entry.id=2;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==0 && readId(path)==2,"POSIX replacement failed");
        horizon=true;entry.id=3;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==0 && readId(path)==3,"Horizon EEXIST replacement failed");
        require(!std::filesystem::exists(path+".bak"),"completed replacement retained unnecessary backup");
        failPublish=true;entry.id=4;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==-1 && errno==EIO,"publication error was not preserved");
        require(readId(path)==3,"failed publication lost the old queue");
        require(!std::filesystem::exists(path+".tmp"),"failed publication left temporary data");
        failRestore=true;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==-1 && errno==EIO,"failed restore changed publication error");
        require(!std::filesystem::exists(path) && readId(path+".bak")==3,"failed restore lost the recoverable backup");
        failPublish=failRestore=false;
        require(ncmWriteQueueFile(path.c_str(),&entry,1)==0 && readId(path)==4,"recovery publication failed");
        require(!std::filesystem::exists(path+".bak"),"recovery did not retire stale backup");
        require(ncmWriteQueueFile(path.c_str(),&entry,NCM_MAX_QUEUE+1)==-1 && errno==EINVAL && readId(path)==4,"invalid queue modified the current queue");
        std::filesystem::remove_all(base);
        std::puts("PASS queue replacement: POSIX / Horizon EEXIST / publish rollback / failed-restore backup / recovery / invalid input");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL %s (fixtures %s)\n",e.what(),directory);
        return 1;
    }
}
