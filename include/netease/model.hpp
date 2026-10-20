// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "netease/api.hpp"
#include "netease/library.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace netease {
enum class Page { Discover, Charts, Search, Library, Player, Queue, Account, Settings };
struct Snapshot {
    Page page = Page::Discover;
    std::string title = "发现音乐", subtitle = "在掌心，听见热爱。", notice, error;
    std::string nickname = "未登录", qr_url, qr_key, qr_message, search_query;
    std::string quality = "standard", proxy;
    uint64_t user_id = 0, collection_id = 0;
    MediaKind collection_kind = MediaKind::Playlist;
    bool subscribed = false, busy = false, more = false, collection = false;
    int search_type = 1, offset = 0, revision = 0;
    std::vector<Media> items, queue;
    std::vector<LyricLine> lyrics;
    std::set<uint64_t> likes;
    std::map<std::string, std::string> images;
};
class Model {
  public:
    explicit Model(std::string directory);
    ~Model();
    Snapshot snapshot() const;
    void start();
    void stop();
    void browse(Page page);
    void refresh();
    void search(std::string text, int type = 1, bool more = false);
    void open(Media item, bool remember_history = true);
    void back();
    void load_more();
    void library_section(int section, bool more = false);
    void daily();
    void fm();
    void new_albums();
    void category(std::string category, bool high_quality = false);
    void login_qr();
    void poll_qr();
    void send_captcha(std::string phone, std::string country);
    void login_phone(std::string phone, std::string country, std::string captcha);
    void logout();
    void like(uint64_t id);
    void subscribe_collection();
    void check_in();
    void intelligence(uint64_t current_track_id = 0);
    void fm_dislike(uint64_t id);
    void play(size_t index);
    void play_queue(size_t index);
    void lyrics_for(uint64_t id);
    void set_quality(std::string quality);
    void set_proxy(std::string proxy);
    void report(std::string message);
    void set_play_callback(std::function<void(const std::vector<Media> &, size_t)> callback);
    // Repeated visible-card requests are deduplicated; failed requests retry
    // after a delay instead of flooding the artwork worker.
    void ensure_artwork(const std::vector<Media> &visible);
    const std::string &directory() const {
        return directory_;
    }
    ApiClient &api() {
        return *api_;
    }

  private:
    friend struct ModelTestAccess;
    void enqueue(std::string label, std::function<void()> job, bool visible = true,
                 uint64_t view_generation = 0);
    void enqueue_view(std::string label, std::function<void()> job);
    bool current_view() const;
    void worker();
    void account();
    void restore_queue();
    bool apply_track_metadata(uint64_t queue_generation, const Media &song);
    Json intelligence_arguments(uint64_t current_track_id) const;
    void assign(std::vector<Media> items, std::string title, std::string subtitle,
                bool more = false);
    void cover_art(const std::vector<Media> &items);
    void save_settings();
    std::string directory_;
    std::unique_ptr<ApiClient> api_;
    mutable std::mutex mutex_;
    Snapshot state_;
    std::set<std::string> artwork_pending_;
    std::map<std::string, std::chrono::steady_clock::time_point> artwork_retry_after_;
    std::vector<Snapshot> history_;
    std::mutex jobs_mutex_;
    std::mutex settings_file_mutex_;
    std::condition_variable jobs_cv_;
    struct Job {
        bool visible;
        std::string label;
        std::function<void()> run;
        uint64_t view_generation;
    };
    std::deque<Job> jobs_;
    std::thread thread_;
    std::atomic<bool> stopping_{false}, qr_pending_{false};
    std::function<void(const std::vector<Media> &, size_t)> on_play_;
    std::atomic<uint64_t> lyric_id_{0}, view_generation_{0};
    uint64_t active_view_generation_ = 0; // Worker-thread only.
    std::size_t pending_visible_ = 0;     // Protected by mutex_.
    uint64_t queue_generation_ = 0;       // Protected by mutex_; rejects old metadata responses.
    std::atomic<int> library_mode_{0};
};
} // namespace netease
