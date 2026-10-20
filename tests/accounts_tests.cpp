// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/accounts.hpp"
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
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
static void initial_publication_tests(const std::filesystem::path& root) {
    const auto alice = jar("synthetic-legacy-alice"), bob = jar("synthetic-new-bob");
    const auto recovery = root / "pending";
    const auto registry = (recovery / "accounts.json").string();
    const auto pending = registry + ".pending";
    const auto mirror = (recovery / "cookies.txt").string();
    storage::write_file(mirror, alice);
    storage::write_file(pending, "Account initialization pending\n");
    rejects([&] { AccountStore::validate_registry(recovery.string()); });
    rejects([&] { AccountStore::playback_cookies(recovery.string()); });
    rejects([&] { AccountStore interrupted(recovery.string()); });
    require(storage::read_file(mirror) == alice && !std::filesystem::exists(registry),
            "Incomplete initialization was treated as a writable legacy profile");
    std::filesystem::rename(pending, pending + ".bak");
    rejects([&] { AccountStore::playback_cookies(recovery.string()); });
    storage::write_file(registry, Json{
        {"version", 1}, {"active_user_id", 11},
        {"accounts", Json::array({{{"user_id", 11}, {"nickname", "Alice"}, {"cookies", alice}}})}
    }.dump());
    require(AccountStore::playback_cookies(recovery.string()) == alice &&
                AccountStore(recovery.string()).active_user_id() == 11,
            "A residual initialization marker blocked a valid recovered archive");

    const auto ordinary = root / "rollback";
    const auto ordinary_mirror = (ordinary / "cookies.txt").string();
    const auto ordinary_registry = (ordinary / "accounts.json").string();
    storage::write_file(ordinary_mirror, alice);
    AccountStore rollback(ordinary.string());
    std::filesystem::create_directory(ordinary_registry + ".new");
    rejects([&] { rollback.save(22, "Bob", bob); });
    require(storage::read_file(ordinary_mirror) == alice &&
                !std::filesystem::exists(ordinary_registry + ".pending") &&
                AccountStore::playback_cookies(ordinary.string()) == alice,
            "Successful first-write rollback did not restore legacy access");
    std::filesystem::remove(ordinary_registry + ".new");
    std::filesystem::create_directory(ordinary_registry + ".pending.new");
    rejects([&] { rollback.save(22, "Bob", bob); });
    require(storage::read_file(ordinary_mirror) == alice &&
                !std::filesystem::exists(ordinary_registry),
            "Mirror changed before the initialization marker was durable");

    const auto damaged = root / "double-failure";
    const auto damaged_mirror = (damaged / "cookies.txt").string();
    const auto damaged_registry = (damaged / "accounts.json").string();
    storage::write_file(damaged_mirror, alice);
    AccountStore store(damaged.string());
    require(::mkfifo((damaged_registry + ".new").c_str(), 0600) == 0,
            "Cannot create account transaction fault fixture");
    bool failed = false;
    {
        std::thread writer([&] {
            try { store.save(22, "Bob", bob); }
            catch (...) { failed = true; }
        });
        // Release the blocked archive fopen and join even if an assertion fails.
        // This small synthetic archive fits in the FIFO without a reader drain.
        struct ReleaseWriter {
            std::thread& thread;
            std::string fifo;
            ~ReleaseWriter() {
                const int descriptor = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
                if (thread.joinable())
                    thread.join();
                if (descriptor >= 0)
                    ::close(descriptor);
            }
        } release{writer, damaged_registry + ".new"};
        bool staged = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!staged && std::chrono::steady_clock::now() < deadline) {
            // The writer holds storage's mutex while waiting in fopen.
            std::ifstream file(damaged_mirror);
            staged = std::string((std::istreambuf_iterator<char>(file)), {}) == bob;
            if (!staged)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(staged, "Account transaction did not stage its mirror");
        std::filesystem::create_directory(damaged_mirror + ".new"); // Also fail rollback.
    }
    require(failed && !store.registered() && store.active_user_id() == 0 &&
                !std::filesystem::exists(damaged_registry) &&
                !std::filesystem::exists(damaged_mirror + ".bak") &&
                storage::read_file(damaged_mirror) == bob &&
                std::filesystem::exists(damaged_registry + ".pending"),
            "Double failure did not preserve its initialization guard");
    rejects([&] { AccountStore::playback_cookies(damaged.string()); });
    rejects([&] { AccountStore blocked(damaged.string()); });
    const auto pending_before_retry = storage::read_file(damaged_registry + ".pending");
    rejects([&] { store.save(33, "Charlie", jar("synthetic-retry-charlie")); });
    require(std::filesystem::exists(damaged_registry + ".pending") &&
                storage::read_file(damaged_registry + ".pending") == pending_before_retry &&
                storage::read_file(damaged_mirror) == bob &&
                !std::filesystem::exists(damaged_registry),
            "Same-object retry discarded the uncommitted-session guard");
    rejects([&] { AccountStore::playback_cookies(damaged.string()); });
    // Recovery removes the guard only after the original session is restored.
    std::filesystem::remove(damaged_mirror + ".new");
    storage::write_file(damaged_mirror, alice);
    std::filesystem::remove(damaged_registry + ".pending");
    require(AccountStore::playback_cookies(damaged.string()) == alice,
            "Restoring the original legacy session did not recover playback");
    AccountStore recovered(damaged.string());
    recovered.save(11, "Alice", alice);
    require(recovered.registered() && recovered.active_user_id() == 11 &&
                !std::filesystem::exists(damaged_registry + ".pending"),
            "Successful first account publication left an initialization guard");
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("cloudtune-accounts-test-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(directory);
        initial_publication_tests(directory / "initial-publication");
        const auto mirror = (directory / "cookies.txt").string();
        const auto registry = (directory / "accounts.json").string();
        const auto alice = jar("synthetic-alice"), bob = jar("synthetic-bob");
        storage::write_file(mirror, alice);
        AccountStore::validate_registry(directory.string());
        require(!std::filesystem::exists(registry),
                "Read-only validation created a legacy registry");
        require(AccountStore::playback_cookies(directory.string()) == alice &&
                    !std::filesystem::exists(registry),
                "Read-only playback lost or changed the legacy session");
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
        AccountStore::validate_registry(directory.string());
        require(storage::read_file(mirror) == bob,
                "Read-only validation repaired the mirror");
        require(AccountStore::playback_cookies(directory.string()) == alice &&
                    storage::read_file(mirror) == bob,
                "Playback used an uncommitted mirror from another account");
        AccountStore restarted(directory.string());
        require(restarted.active_user_id() == 11 && storage::read_file(mirror) == alice,
                "Restart did not restore the committed account");
        std::filesystem::rename(registry, registry + ".bak");
        require(AccountStore::playback_cookies(directory.string()) == alice &&
                    !std::filesystem::exists(registry),
                "Playback did not read the committed backup without modifying it");
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
        storage::write_file(mirror, bob); // Interrupted/stale mirror after sign-out.
        require(AccountStore::playback_cookies(directory.string()).find("synthetic-") ==
                    std::string::npos && storage::read_file(mirror) == bob,
                "Signed-out playback activated an account from the stale mirror");
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
        rejects([&] { AccountStore::validate_registry(directory.string()); });
        rejects([&] { AccountStore::playback_cookies(directory.string()); });
        const auto committed_mirror = storage::read_file(mirror);
        storage::write_file(registry, "");
        rejects([&] { AccountStore empty(directory.string()); });
        rejects([&] { AccountStore::validate_registry(directory.string()); });
        rejects([&] { AccountStore::playback_cookies(directory.string()); });
        require(storage::read_file(mirror) == committed_mirror,
                "Zero-byte registry activated or overwrote its mirror");
        std::filesystem::rename(registry, registry + ".bak");
        rejects([&] { AccountStore empty_backup(directory.string()); });
        rejects([&] { AccountStore::validate_registry(directory.string()); });
        rejects([&] { AccountStore::playback_cookies(directory.string()); });
        require(storage::read_file(mirror) == committed_mirror,
                "Zero-byte backup was treated as a legacy account");
        std::filesystem::remove_all(directory);
        std::cout << "Account migration, switch, removal, persistence, rollback, privacy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
