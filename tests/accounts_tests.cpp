// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/accounts.hpp"
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace netease;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    require(failed, "Expected an account transaction failure");
}
static std::string jar(const char* fixture) {
    return "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\t" +
           std::string(fixture) + "\n";
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("cloudtune-accounts-test-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(directory);
        const auto mirror = (directory / "cookies.txt").string();
        const auto registry = (directory / "accounts.json").string();
        const auto alice = jar("synthetic-alice"), bob = jar("synthetic-bob");
        storage::write_file(mirror, alice);
        AccountStore store(directory.string());
        require(!store.registered() && store.list().empty() &&
                    storage::read_file(mirror) == alice,
                "Unverified legacy session was changed");
        store.save(11, "甲", alice);
        store.save(22, "乙", bob);
        require(store.list().size() == 2 && store.active_user_id() == 22 &&
                    storage::read_file(mirror) == bob, "Adding accounts failed");
        store.save(22, "乙改名", bob);
        require(store.list().size() == 2, "Repeated QR login duplicated a UID");
        store.switch_to(11);
        require(storage::read_file(mirror) == alice, "Switch did not publish selected session");
        rejects([&] { store.switch_to(999); });
        require(store.active_user_id() == 11, "Invalid selection changed active account");
        // Fail after the mirror was staged: account metadata cannot be committed.
        std::filesystem::create_directory(registry + ".new");
        rejects([&] { store.switch_to(22); });
        require(store.active_user_id() == 11 && storage::read_file(mirror) == alice,
                "Failed registry commit changed active credentials");
        std::filesystem::remove(registry + ".new");
        // Interrupted two-file publication: the committed registry repairs the mirror.
        storage::write_file(mirror, bob);
        AccountStore restarted(directory.string());
        require(restarted.active_user_id() == 11 && storage::read_file(mirror) == alice,
                "Restart did not restore the committed account");
        std::filesystem::rename(registry, registry + ".bak");
        AccountStore recovered(directory.string());
        require(recovered.active_user_id() == 11 && recovered.list().size() == 2,
                "Horizon backup recovery failed");
        recovered.remove(22);
        require(recovered.active_user_id() == 11 && recovered.list().size() == 1 &&
                    storage::read_file(mirror) == alice,
                "Removing another saved account changed active credentials");
        recovered.save(22, "乙", bob, false);
        recovered.remove(11);
        require(recovered.active_user_id() == 0 && recovered.list().size() == 1 &&
                    storage::read_file(mirror).find("synthetic-") == std::string::npos,
                "Removing active account exposed another user's session");
        recovered.switch_to(22);
        recovered.update_active(jar("synthetic-refreshed"));
        AccountStore refreshed(directory.string());
        require(refreshed.active_cookies().find("synthetic-refreshed") != std::string::npos,
                "Refreshed session was not saved");
        struct stat mode{};
        require(::stat(registry.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600 &&
                    ::stat(mirror.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600,
                "Session files have overly broad permissions");
        // Pending QR clients are in-memory; cancellation/destruction cannot publish them.
        {
            ApiClient pending;
            pending.adopt_session(alice);
            require(pending.session_cookies().find("synthetic-alice") != std::string::npos,
                    "In-memory session import/export failed");
        }
        require(storage::read_file(mirror) == refreshed.active_cookies(),
                "Pending login overwrote the active session");
        storage::write_file(registry, R"({"cookies":"secret-fixture", broken)");
        bool sanitized = false;
        try { AccountStore damaged(directory.string()); }
        catch (const std::exception& e) {
            sanitized = std::string(e.what()).find("secret-fixture") == std::string::npos;
        }
        require(sanitized, "Damaged registry exposed private data in an error");
        std::filesystem::remove_all(directory);
        std::cout << "Account migration, switch, removal, persistence, rollback, privacy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
