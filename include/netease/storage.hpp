// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#ifndef __SWITCH__
#include <sys/stat.h>
#else
// devkitA64 newlib exports fileno but hides its declaration in strict C++ mode.
extern "C" int fileno(FILE*);
#endif

namespace netease::storage {
namespace detail {
inline std::mutex& file_mutex() { static std::mutex mutex; return mutex; }
inline void rename_new(const std::string& from,const std::string& to) {
    // Horizon/libnx rejects rename when the destination exists. Enforce the same
    // semantics on the host so regression tests exercise the Switch path.
    if(std::filesystem::exists(to))throw std::runtime_error("Save destination already exists: "+to);
    std::filesystem::rename(from,to);
}
template<class Rename>
void install_locked(const std::string& staged,const std::string& destination,Rename rename) {
    if(!std::filesystem::is_regular_file(staged))
        throw std::runtime_error("Completed save file is missing: "+staged);
    const auto backup=destination+".bak";
    if(std::filesystem::exists(destination)) {
        // An existing primary is the committed version. Any old backup is stale.
        // If its removal fails, abort before touching that primary.
        if(std::filesystem::exists(backup))std::filesystem::remove(backup);
        rename(destination,backup);
    }
    try {
        rename(staged,destination);
    } catch(...) {
        // A failed promotion must not discard the previous working file.
        // Even if rollback itself fails, readers recover from .bak.
        if(!std::filesystem::exists(destination)&&std::filesystem::exists(backup)) {
            try {rename(backup,destination);} catch(...) {}
        }
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(backup,ignored);
}
inline bool read_one(const std::string& path,std::string& result) {
    std::ifstream input(path,std::ios::binary);
    if(!input)return false;
    input.seekg(0,std::ios::end);
    const auto size=input.tellg();
    if(size<0||size>8*1024*1024)throw std::runtime_error("Saved settings/session file has an invalid size");
    result.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    if(size&&!input.read(result.data(),size))throw std::runtime_error("Cannot read saved settings/session");
    return true;
}
} // namespace detail

// Returns the last committed primary, or its backup if interrupted between the
// two Horizon rename operations. Read-only background clients never alter files.
inline std::string read_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(detail::file_mutex());
    std::string result;
    if(detail::read_one(path,result))return result;
    if(detail::read_one(path+".bak",result))return result;
    // Another process may have promoted the primary and removed its backup
    // between those opens (the background service is a read-only consumer).
    detail::read_one(path,result);
    return result;
}
inline void sync_file(FILE* file) {
    if(std::fflush(file)!=0||std::ferror(file))throw std::runtime_error("Could not flush completed file");
    // Some virtual filesystems do not expose fsync; all actual write failures
    // still abort before the previous destination is moved.
    if(::fsync(::fileno(file))!=0&&errno!=EINVAL&&errno!=ENOSYS)
        throw std::runtime_error("Could not synchronize completed file");
}
inline void install_file(const std::string& staged,const std::string& destination) {
    std::lock_guard<std::mutex> lock(detail::file_mutex());
    detail::install_locked(staged,destination,detail::rename_new);
}
inline void write_file(const std::string& path,const std::string& bytes) {
    std::lock_guard<std::mutex> lock(detail::file_mutex());
    const auto parent=std::filesystem::path(path).parent_path();
    if(!parent.empty())std::filesystem::create_directories(parent);
    const auto staged=path+".new";
    std::unique_ptr<FILE,decltype(&std::fclose)> file(std::fopen(staged.c_str(),"wb"),std::fclose);
    if(!file)throw std::runtime_error("Cannot create settings/session save file");
#ifndef __SWITCH__
    ::chmod(staged.c_str(),0600);
#endif
    try {
        if(std::fwrite(bytes.data(),1,bytes.size(),file.get())!=bytes.size())
            throw std::runtime_error("Could not write settings/session save");
        sync_file(file.get());
        auto* completed=file.release();
        if(std::fclose(completed)!=0)throw std::runtime_error("Could not close settings/session save");
        detail::install_locked(staged,path,detail::rename_new);
    } catch(...) {
        file.reset();
        std::remove(staged.c_str());
        throw;
    }
}
} // namespace netease::storage
