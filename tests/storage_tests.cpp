// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <iostream>
#include <unistd.h>

using namespace netease;
static void require(bool ok,const char* reason) {
    if(!ok)throw std::runtime_error(reason);
}
int main() {
    try {
        const auto directory=std::filesystem::temp_directory_path()/
            ("netease-storage-test-"+std::to_string(getpid()));
        std::filesystem::create_directories(directory);
        const auto path=(directory/"settings.json").string();
        storage::write_file(path,"old");
        storage::write_file(path,"new");
        require(storage::read_file(path)=="new","Repeat save did not replace its destination");
        require(!std::filesystem::exists(path+".bak"),"Committed save retained obsolete backup");

        // Emulate power loss after old -> backup and before new -> primary.
        storage::detail::rename_new(path,path+".bak");
        require(storage::read_file(path)=="new","Interrupted save did not recover backup");
        storage::write_file(path,"recovered");
        require(storage::read_file(path)=="recovered"&&std::filesystem::exists(path),
                "Saving from recovered backup did not restore primary");

        const auto staged=(directory/"download.part").string();
        storage::write_file(staged,"new download");
        bool rejected=false;
        try {
            storage::detail::install_locked(staged,path,[&](const std::string& from,const std::string& to){
                if(from==staged)throw std::runtime_error("Injected promotion failure");
                storage::detail::rename_new(from,to);
            });
        }catch(...){rejected=true;}
        require(rejected&&storage::read_file(path)=="recovered"&&std::filesystem::exists(path),
                "Failed promotion did not roll back the old destination");
        rejected=false;
        try {
            storage::detail::install_locked(staged,path,[&](const std::string& from,const std::string& to){
                if(to==path)throw std::runtime_error("Injected promotion and rollback failure");
                storage::detail::rename_new(from,to);
            });
        }catch(...){rejected=true;}
        require(rejected&&!std::filesystem::exists(path)&&storage::read_file(path)=="recovered",
                "Failed rollback did not preserve a readable backup");
        storage::install_file(staged,path);
        require(storage::read_file(path)=="new download","Download replacement failed after backup recovery");

        const auto cookie_path=(directory/"cookies.txt").string();
        const std::string fixture="# Netscape HTTP Cookie File\n"
            "#HttpOnly_.music.163.com\tTRUE\t/\tTRUE\t2147483647\tMUSIC_U\tfixture-not-real\n"
            ".music.163.com\tTRUE\t/\tFALSE\t2147483647\t__csrf\tfixture-csrf\n";
        storage::write_file(cookie_path,fixture);
        ApiConfig config;config.cookie_file=cookie_path;
        {
            ApiClient client(config);
            client.flush_cookies();
            client.flush_cookies();
            require(storage::read_file(cookie_path).find("fixture-not-real")!=std::string::npos,
                    "Cookie export dropped the imported session");
        }
        storage::detail::rename_new(cookie_path,cookie_path+".bak");
        {
            ApiClient client(config);
            client.flush_cookies();
            require(std::filesystem::exists(cookie_path)&&
                    storage::read_file(cookie_path).find("fixture-not-real")!=std::string::npos,
                    "Cookie restart did not restore the interrupted save");
        }
        const auto before=storage::read_file(cookie_path);
        config.persist_cookies=false;
        {ApiClient reader(config);reader.clear_session();reader.flush_cookies();}
        require(storage::read_file(cookie_path)==before,"Background client overwrote the GUI session");
        config.persist_cookies=true;
        {ApiClient owner(config);owner.clear_session();}
        require(storage::read_file(cookie_path).find("fixture-not-real")==std::string::npos,
                "Logout did not persist removal of the session");
        std::filesystem::remove_all(directory);
        std::cout<<"Horizon-safe replacement / rollback / crash recovery / cookie persistence tests passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
