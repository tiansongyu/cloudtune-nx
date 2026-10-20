// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/accounts.hpp"
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace netease {
namespace {
const std::string empty_session = "# Netscape HTTP Cookie File\n# CloudTune NX session. Keep private.\n\n";
uint64_t account_id(const Json& value) {
    if (!value.is_number_unsigned() &&
        (!value.is_number_integer() || value.get<int64_t>() < 0))
        throw std::runtime_error("invalid account id");
    return value.get<uint64_t>();
}
Json read_registry(const std::string& directory) {
    const auto path = directory + "/accounts.json";
    const auto data = storage::read_file(path);
    if (data.empty() && !std::filesystem::exists(path) && !std::filesystem::exists(path + ".bak")) {
        if (std::filesystem::exists(path + ".pending") ||
            std::filesystem::exists(path + ".pending.bak"))
            throw std::runtime_error(
                "Saved account initialization was interrupted; restore the account archive "
                "or the previous cookie session before removing accounts.json.pending");
        return Json(); // Legacy cookie jars have no canonical archive yet.
    }
    try {
        auto registry = Json::parse(data);
        if (registry.at("version") != 1 || !registry.at("accounts").is_array() ||
            registry.at("accounts").size() > 32)
            throw std::runtime_error("invalid registry");
        std::set<uint64_t> ids;
        const auto active = account_id(registry.at("active_user_id"));
        for (const auto& item : registry.at("accounts")) {
            const auto id = account_id(item.at("user_id"));
            const auto& nickname = item.at("nickname").get_ref<const std::string&>();
            const auto& cookies = item.at("cookies").get_ref<const std::string&>();
            if (!id || !ids.insert(id).second || nickname.size() > 1024 ||
                cookies.empty() || cookies.size() > 128 * 1024)
                throw std::runtime_error("invalid account");
        }
        if (active && !ids.count(active))
            throw std::runtime_error("missing active account");
        return registry;
    } catch (...) {
        // JSON errors may embed cookie values; replace them with a safe message.
        throw std::runtime_error("Saved account registry is damaged; restore accounts.json from a backup");
    }
}
}
void AccountStore::validate_registry(const std::string& directory) {
    (void)read_registry(directory);
}
std::string AccountStore::playback_cookies(const std::string& directory) {
    const auto registry = read_registry(directory);
    if (registry.is_null())
        return storage::read_file(directory + "/cookies.txt");
    const auto active = account_id(registry.at("active_user_id"));
    if (!active)
        return empty_session;
    for (const auto& item : registry.at("accounts"))
        if (account_id(item.at("user_id")) == active)
            return item.at("cookies").get<std::string>();
    // read_registry already rejects references to a missing active account.
    throw std::runtime_error("Saved account was not found");
}
AccountStore::AccountStore(std::string directory) : directory_(std::move(directory)) {
    const auto registry = read_registry(directory_);
    if (registry.is_null())
        return; // Preserve legacy cookies until the server confirms their owner.
    active_ = account_id(registry.at("active_user_id"));
    for (const auto& item : registry.at("accounts"))
        sessions_.push_back({{account_id(item.at("user_id")), item.at("nickname").get<std::string>()},
                             item.at("cookies").get<std::string>()});
    registered_ = true;
    const auto committed = active_cookies();
    if (storage::read_file(directory_ + "/cookies.txt") != committed)
        storage::write_file(directory_ + "/cookies.txt", committed);
}
std::vector<SavedAccount> AccountStore::list() const {
    std::vector<SavedAccount> accounts;
    for (const auto& session : sessions_)
        accounts.push_back(session.account);
    return accounts;
}
std::string AccountStore::cookies_for(const std::vector<Session>& sessions, uint64_t id) {
    if (!id)
        return empty_session;
    for (const auto& session : sessions)
        if (session.account.user_id == id)
            return session.cookies;
    throw std::runtime_error("Saved account was not found");
}
std::string AccountStore::active_cookies() const {
    return cookies_for(sessions_, active_);
}
void AccountStore::commit(std::vector<Session> sessions, uint64_t active) {
    Json accounts = Json::array();
    for (const auto& session : sessions)
        accounts.push_back({{"user_id", session.account.user_id},
                            {"nickname", session.account.nickname}, {"cookies", session.cookies}});
    const auto serialized = Json{{"version", 1}, {"active_user_id", active}, {"accounts", accounts}}.dump(2) + "\n";
    if (serialized.size() > 7 * 1024 * 1024)
        throw std::runtime_error("Saved account registry is too large");
    const auto mirror = directory_ + "/cookies.txt";
    const auto before = storage::read_file(mirror);
    const auto after = cookies_for(sessions, active);
    const auto pending = directory_ + "/accounts.json.pending";
    const bool initializing = !registered_;
    auto clear_pending = [&] {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        std::filesystem::remove(pending + ".bak", ignored);
    };
    // A failed first publication has no canonical archive to protect its old
    // identity. Mark the transaction before changing the legacy cookie jar so
    // a crash or failed rollback cannot promote an uncommitted mirror on restart.
    if (initializing) {
        // A previous failed rollback may have left this same object alive.
        // Never treat its uncommitted mirror as the baseline of another retry.
        (void)read_registry(directory_);
        storage::write_file(pending, "CloudTune NX account initialization pending\n");
    }
    try {
        if (before != after)
            storage::write_file(mirror, after);
        storage::write_file(directory_ + "/accounts.json", serialized);
    } catch (...) {
        if (before != after) {
            try {
                storage::write_file(mirror, before.empty() ? empty_session : before);
            } catch (...) {
                // On restart the last committed registry repairs the mirror.
                // Do not reveal any serialized cookie data in diagnostics.
            }
        }
        if (initializing) {
            try {
                const auto recovered = storage::read_file(mirror);
                if (recovered == before || (before.empty() && recovered == empty_session))
                    clear_pending();
            } catch (...) {
                // Keep the marker until the previous identity is known to be restored.
            }
        }
        throw;
    }
    sessions_ = std::move(sessions);
    active_ = active;
    registered_ = true;
    if (initializing)
        clear_pending(); // Cleanup failure must not reject a committed transaction.
}
void AccountStore::save(uint64_t id, std::string nickname, std::string cookies, bool activate) {
    if (!id || nickname.size() > 1024 || cookies.empty() || cookies.size() > 128 * 1024)
        throw std::runtime_error("Invalid account session");
    auto sessions = sessions_;
    auto found = std::find_if(sessions.begin(), sessions.end(),
                             [id](const Session& s) { return s.account.user_id == id; });
    Session session{{id, std::move(nickname)}, std::move(cookies)};
    if (found == sessions.end()) {
        if (sessions.size() >= 32)
            throw std::runtime_error("At most 32 accounts can be saved; remove an account first");
        sessions.push_back(std::move(session));
    } else
        *found = std::move(session);
    commit(std::move(sessions), activate ? id : active_);
}
void AccountStore::switch_to(uint64_t id) {
    if (!id)
        throw std::runtime_error("Saved account was not found");
    (void)cookies_for(sessions_, id);
    commit(sessions_, id);
}
void AccountStore::remove(uint64_t id) {
    auto sessions = sessions_;
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [id](const Session& s) { return s.account.user_id == id; }),
                   sessions.end());
    if (sessions.size() == sessions_.size())
        throw std::runtime_error("Saved account was not found");
    // Removing the active account leaves a signed-out state; never implicitly
    // expose another person's library by selecting the first remaining account.
    commit(std::move(sessions), id == active_ ? 0 : active_);
}
void AccountStore::update_active(std::string cookies) {
    if (!active_ || cookies.empty())
        return;
    if (cookies.size() > 128 * 1024)
        throw std::runtime_error("Account session is too large");
    auto sessions = sessions_;
    for (auto& session : sessions)
        if (session.account.user_id == active_) {
            if (session.cookies == cookies)
                return;
            session.cookies = std::move(cookies);
            break;
        }
    commit(std::move(sessions), active_);
}
} // namespace netease
