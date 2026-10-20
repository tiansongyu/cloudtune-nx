// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace netease {
struct SavedAccount {
    uint64_t user_id = 0;
    std::string nickname;
};

// The registry is the committed source of truth. cookies.txt is its compatibility
// mirror for the independent playback service; secrets never enter Snapshot.
class AccountStore {
  public:
    explicit AccountStore(std::string directory);
    std::vector<SavedAccount> list() const;
    uint64_t active_user_id() const { return active_; }
    std::string active_cookies() const;
    bool registered() const { return registered_; }
    void save(uint64_t id, std::string nickname, std::string cookies, bool activate = true);
    void switch_to(uint64_t id);
    void remove(uint64_t id);
    void update_active(std::string cookies);

  private:
    struct Session {
        SavedAccount account;
        std::string cookies;
    };
    void commit(std::vector<Session> sessions, uint64_t active);
    static std::string cookies_for(const std::vector<Session>& sessions, uint64_t id);
    std::string directory_;
    std::vector<Session> sessions_;
    uint64_t active_ = 0;
    bool registered_ = false;
};
} // namespace netease
