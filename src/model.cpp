// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/model.hpp"
#include "netease/playback_protocol.h"
#include "netease/storage.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace netease {
namespace {
const Json empty_array = Json::array();
const Json &array_at(const Json &root, std::initializer_list<const char *> keys) {
    for (auto k : keys)
        if (root.contains(k) && root[k].is_array())
            return root[k];
    return empty_array;
}
Json object_at(const Json &root, const char *key) {
    return root.contains(key) && root[key].is_object() ? root[key] : Json::object();
}
std::string cache_name(const std::string &url) {
    // Stable FNV-1a, not std::hash (whose persistence is unspecified).
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    std::ostringstream s;
    s << std::hex << h;
    return s.str() + ".img";
}
uint64_t little_endian(const unsigned char *bytes, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
bool read_queue(const std::string &path, std::vector<Media> &queue) {
    // Parse bounded bytes rather than casting untrusted SD contents to an ABI
    // struct. Validate before allocating the entry buffer.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const auto length = input.tellg();
    constexpr size_t header_size = sizeof(NcmQueueHeader), entry_size = sizeof(NcmQueueEntry);
    static_assert(header_size == 16, "Queue header ABI changed");
    if (length < static_cast<std::streamoff>(header_size) ||
        length > static_cast<std::streamoff>(header_size + NCM_MAX_QUEUE * entry_size))
        return false;
    input.seekg(0);
    unsigned char header[header_size]{};
    if (!input.read(reinterpret_cast<char *>(header), header_size) ||
        std::memcmp(header, "NCMQUEUE", 8) != 0 ||
        little_endian(header + 8, 4) != NCM_PROTOCOL_VERSION)
        return false;
    const auto count = little_endian(header + 12, 4);
    if (count > NCM_MAX_QUEUE ||
        length != static_cast<std::streamoff>(header_size + count * entry_size))
        return false;
    std::vector<Media> restored;
    restored.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        unsigned char entry[entry_size]{};
        if (!input.read(reinterpret_cast<char *>(entry), entry_size))
            return false;
        if (little_endian(entry + 12, 4) != 0)
            return false;
        Media song;
        song.id = little_endian(entry, 8);
        song.duration_ms = static_cast<uint32_t>(little_endian(entry + 8, 4));
        auto field = [&](size_t offset, size_t size, std::string &target) {
            const auto *begin = reinterpret_cast<const char *>(entry + offset);
            const auto *end = static_cast<const char *>(std::memchr(begin, 0, size));
            if (!end)
                return false;
            target.assign(begin, end);
            return true;
        };
        if (!field(offsetof(NcmQueueEntry, title), sizeof(NcmQueueEntry::title), song.title) ||
            !field(offsetof(NcmQueueEntry, artist), sizeof(NcmQueueEntry::artist), song.artist) ||
            !field(offsetof(NcmQueueEntry, album), sizeof(NcmQueueEntry::album), song.album) ||
            !field(offsetof(NcmQueueEntry, url), sizeof(NcmQueueEntry::url), song.url))
            return false;
        restored.push_back(std::move(song));
    }
    queue = std::move(restored);
    return true;
}
} // namespace
Model::Model(std::string directory) : directory_(std::move(directory)) {
    std::filesystem::create_directories(directory_ + "/artwork");
    try {
        accounts_ = std::make_unique<AccountStore>(directory_);
        sync_accounts();
    } catch (const std::exception& error) {
        accounts_error_ = error.what();
        state_.error = accounts_error_;
    }
    try {
        const auto saved = storage::read_file(directory_ + "/settings.json");
        if (!saved.empty()) {
            auto j = Json::parse(saved);
            state_.quality = json_text(j, "quality", "standard");
            state_.proxy = json_text(j, "proxy");
        }
    } catch (...) {
    }
    ApiConfig c;
    // An unreadable canonical registry must not silently activate its mirror
    // or overwrite it while the user is recovering the account archive.
    if (accounts_)
        c.cookie_file = directory_ + "/cookies.txt";
    else
        c.persist_cookies = false;
    c.proxy = state_.proxy;
    c.timeout_seconds = 15;
#ifdef __SWITCH__
    c.ca_bundle = "romfs:/certs/cacert.pem";
#endif
    api_ = std::make_unique<ApiClient>(c);
    thread_ = std::thread([this] { worker(); });
}
Model::~Model() {
    try {
        stop();
    } catch (...) {
    }
}
Snapshot Model::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = state_;
    if (!accounts_error_.empty())
        result.error = accounts_error_;
    return result;
}
void Model::stop() {
    if (stopping_.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.clear();
    }
    jobs_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    try {
        api_->flush_cookies();
        if (accounts_)
            accounts_->update_active(api_->session_cookies());
    } catch (const std::exception &error) {
        // Persistence errors contain filesystem reasons/paths, never cookie data.
        std::cerr << "Session save failed during shutdown: " << error.what() << '\n';
        std::lock_guard<std::mutex> lock(mutex_);
        state_.error = error.what();
    }
}
void Model::enqueue(std::string label, std::function<void()> job, bool visible,
                    uint64_t generation) {
    if (stopping_)
        return;
    if (visible) {
        std::lock_guard<std::mutex> l(mutex_);
        ++pending_visible_;
        state_.busy = true;
        state_.notice = label;
        state_.error.clear();
    }
    {
        std::lock_guard<std::mutex> l(jobs_mutex_);
        Job entry{visible, std::move(label), std::move(job), generation, account_generation_};
        // User operations retain FIFO order and precede queued artwork.
        if (visible)
            jobs_.insert(
                std::find_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return !j.visible; }),
                std::move(entry));
        else
            jobs_.push_back(std::move(entry));
    }
    jobs_cv_.notify_one();
}
void Model::enqueue_view(std::string label, std::function<void()> job) {
    const auto generation = ++view_generation_;
    enqueue(std::move(label), std::move(job), true, generation);
}
bool Model::current_view() const {
    return active_account_generation_ == account_generation_.load() &&
           (!active_view_generation_ || active_view_generation_ == view_generation_.load());
}
void Model::worker() {
    while (!stopping_) {
        Job job;
        {
            std::unique_lock<std::mutex> l(jobs_mutex_);
            jobs_cv_.wait(l, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_)
                break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        active_view_generation_ = job.view_generation;
        active_account_generation_ = job.account_generation;
        if (current_view()) {
            try {
                job.run();
                // Service responses may rotate cookies. Keep the committed
                // registry current as well as the playback compatibility mirror.
                if (accounts_ && accounts_->active_user_id())
                    accounts_->update_active(api_->session_cookies());
            } catch (const std::exception &e) {
                if (job.visible && current_view()) {
                    std::lock_guard<std::mutex> l(mutex_);
                    state_.error = e.what();
                    state_.notice = "请求失败，请重试";
                }
            }
        }
        active_view_generation_ = 0;
        if (job.visible) {
            std::lock_guard<std::mutex> l(mutex_);
            if (pending_visible_)
                --pending_visible_;
            state_.busy = pending_visible_ != 0;
            state_.revision++;
        }
    }
}
void Model::report(std::string message) {
    std::lock_guard<std::mutex> l(mutex_);
    state_.notice = std::move(message);
}
void Model::start() {
    restore_queue();
    browse(Page::Discover);
    enqueue(
        "",
        [this] {
            try {
                account();
            } catch (...) {
            }
        },
        false);
}
void Model::restore_queue() {
    if (!accounts_)
        return;
    std::vector<Media> queue;
    const auto path = directory_ + "/queue.nsq";
    // A previous GUI may have exited during Horizon's two-rename publication.
    // A second primary read also covers a concurrent backup promotion.
    if (!read_queue(path, queue) && !read_queue(path + ".bak", queue) && !read_queue(path, queue))
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.queue.empty())
        return;
    state_.queue = std::move(queue);
    ++queue_generation_;
    ++state_.revision;
}
bool Model::apply_track_metadata(uint64_t generation, const Media &song) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != queue_generation_)
        return false;
    bool updated = false;
    for (auto &item : state_.queue)
        if (item.id == song.id) {
            // Keep the handoff URL and duration: the active stream may be a preview.
            item.title = song.title;
            item.artist = song.artist;
            item.album = song.album;
            item.cover = song.cover;
            updated = true;
        }
    if (updated) {
        if (state_.page == Page::Queue && !state_.collection)
            for (auto &item : state_.items)
                if (item.id == song.id) {
                    item.title = song.title;
                    item.artist = song.artist;
                    item.album = song.album;
                    item.cover = song.cover;
                }
        ++state_.revision;
    }
    return updated;
}
void Model::account() {
    if (!accounts_)
        return;
    auto j = api_->login_status();
    if (!current_view())
        return;
    auto p = object_at(j, "profile");
    if (p.empty())
        p = object_at(object_at(j, "data"), "profile");
    auto uid = json_id(p, "userId");
    if (uid && accounts_) {
        accounts_->save(uid, json_text(p, "nickname", "网易云用户"), api_->session_cookies());
        sync_accounts();
    }
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (state_.user_id != uid || !uid)
            state_.likes.clear();
        state_.user_id = uid;
        state_.nickname = json_text(p, "nickname", "未登录");
    }
    if (uid)
        try { load_likes(true); } catch (...) {}
}
void Model::load_likes(bool refresh) {
    if (likes_loaded_ && !refresh)
        return;
    const auto uid = snapshot().user_id;
    if (!uid)
        return;
    const auto ids = api_->call("user_song_id_list", {{"uid", uid}});
    std::set<uint64_t> likes;
    for (const auto &id : array_at(ids, {"ids"}))
        if (id.is_number_unsigned() || id.is_number_integer())
            likes.insert(id.get<uint64_t>());
    if (!current_view())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.likes = std::move(likes);
    likes_loaded_ = true;
}
void Model::assign(std::vector<Media> items, std::string title, std::string subtitle, bool more) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (std::this_thread::get_id() == thread_.get_id() && !current_view())
            return;
        state_.items = items;
        state_.title = std::move(title);
        state_.subtitle = std::move(subtitle);
        state_.more = more;
        state_.notice = items.empty() ? "这里暂时没有内容" : "";
        state_.revision++;
    }
    cover_art(items);
}
void Model::cover_art(const std::vector<Media> &items) {
    ensure_artwork(items);
}
void Model::ensure_artwork(const std::vector<Media> &visible) {
    if (stopping_)
        return;
    for (size_t i = 0; i < std::min<size_t>(visible.size(), 40); ++i) {
        const auto url = visible[i].cover;
        if (url.empty())
            continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.images.count(url) || artwork_pending_.count(url))
                continue;
            auto retry = artwork_retry_after_.find(url);
            if (retry != artwork_retry_after_.end() &&
                retry->second > std::chrono::steady_clock::now())
                continue;
            artwork_pending_.insert(url);
        }
        enqueue(
            "",
            [this, url] {
                bool relevant = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto &m : state_.items)
                        if (m.cover == url) {
                            relevant = true;
                            break;
                        }
                    if (!relevant)
                        for (const auto &m : state_.queue)
                            if (m.cover == url) {
                                relevant = true;
                                break;
                            }
                    if (!relevant || state_.images.count(url)) {
                        artwork_pending_.erase(url);
                        return;
                    }
                }
                auto path = directory_ + "/artwork/" + cache_name(url);
                try {
                    if (!std::filesystem::exists(path))
                        api_->download(url + (url.find('?') == std::string::npos ? "?" : "&") +
                                           "param=300y300",
                                       path);
                    std::lock_guard<std::mutex> l(mutex_);
                    state_.images[url] = path;
                    state_.revision++;
                    artwork_pending_.erase(url);
                    artwork_retry_after_.erase(url);
                } catch (...) {
                    // Artwork is optional; the next visible request may retry later.
                    std::lock_guard<std::mutex> lock(mutex_);
                    artwork_pending_.erase(url);
                    artwork_retry_after_[url] =
                        std::chrono::steady_clock::now() + std::chrono::seconds(30);
                }
            },
            false);
    }
}
void Model::browse(Page page) {
    ++view_generation_;
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.page = page;
        state_.collection = false;
        state_.offset = 0;
        state_.error.clear();
        history_.clear();
    }
    if (page == Page::Discover) {
        category("全部");
        return;
    }
    if (page == Page::Charts) {
        enqueue_view("正在获取排行榜", [this] {
            auto j = api_->call("toplist");
            assign(parse_collections(array_at(j, {"list"}), MediaKind::Playlist), "排行榜",
                   "发现此刻最受欢迎的音乐");
        });
        return;
    }
    if (page == Page::Library) {
        library_section(0);
        return;
    }
    if (page == Page::Search) {
        auto s = snapshot();
        if (!s.search_query.empty())
            search(s.search_query, s.search_type);
        else
            assign({}, "搜索", "按 A 输入关键词 · 单曲 / 歌单 / 专辑 / 歌手 / 歌词");
        return;
    }
    if (page == Page::Queue) {
        assign(snapshot().queue, "播放队列", "选择歌曲即可继续播放");
        return;
    }
    if (page == Page::Account) {
        assign({}, "我的账号", "使用网易云音乐手机 App 扫码登录");
        return;
    }
    if (page == Page::Settings) {
        assign({}, "设置", "账号、音质、网络与运行信息");
        return;
    }
    if (page == Page::Player) {
        std::lock_guard<std::mutex> l(mutex_);
        state_.title = "正在播放";
        state_.notice.clear();
    }
}
void Model::refresh() {
    auto s = snapshot();
    if (s.collection) {
        Media m;
        m.id = s.collection_id;
        m.kind = s.collection_kind;
        m.title = s.title;
        open(m, false);
    } else
        browse(s.page);
}
void Model::category(std::string category, bool high) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.collection = false;
    }
    enqueue_view("正在发现音乐", [this, category, high] {
        auto j = api_->call(high ? "top_song_list_highquality" : "top_song_list",
                            {{"cat", category}, {"order", "hot"}, {"limit", 40}});
        assign(parse_collections(array_at(j, {"playlists"}), MediaKind::Playlist),
               high ? "精品歌单" : "发现音乐", "为每一次冒险，找到合适的旋律 · " + category);
    });
}
void Model::new_albums() {
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.collection = false;
    }
    enqueue_view("正在获取新碟", [this] {
        auto j = api_->call("new_albums", {{"limit", 40}});
        assign(parse_collections(array_at(j, {"albums"}), MediaKind::Album), "新碟上架",
               "听见新的声音");
    });
}
void Model::search(std::string text, int type, bool more) {
    if (text.empty())
        return;
    int offset = more ? snapshot().offset + 30 : 0;
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.page = Page::Search;
        state_.search_query = text;
        state_.search_type = type;
        state_.collection = false;
    }
    enqueue_view("正在搜索", [this, text, type, offset, more] {
        auto j = object_at(api_->search(text, type, offset, 30), "result");
        std::vector<Media> items;
        if (type == 1 || type == 1006)
            items = parse_songs(array_at(j, {"songs"}));
        else if (type == 10)
            items = parse_collections(array_at(j, {"albums"}), MediaKind::Album);
        else if (type == 100)
            items = parse_collections(array_at(j, {"artists"}), MediaKind::Artist);
        else if (type == 1000)
            items = parse_collections(array_at(j, {"playlists"}), MediaKind::Playlist);
        bool has_more = items.size() == 30;
        if (!current_view())
            return;
        if (more) {
            auto old = snapshot().items;
            old.insert(old.end(), items.begin(), items.end());
            items = std::move(old);
        }
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!current_view())
                return;
            state_.offset = offset;
        }
        assign(std::move(items), "搜索 · " + text, "单曲 / 歌单 / 专辑 / 歌手 / 歌词", has_more);
    });
}
void Model::open(Media item, bool remember_history) {
    if (item.kind == MediaKind::Song)
        return;
    if (remember_history) {
        std::lock_guard<std::mutex> l(mutex_);
        history_.push_back(state_);
    }
    enqueue_view("正在打开 " + item.title, [this, item] {
        Json j;
        std::vector<Media> songs;
        bool subscribed = false, more = false;
        if (item.kind == MediaKind::Playlist) {
            j = api_->playlist(item.id);
            auto p = object_at(j, "playlist");
            songs = parse_songs(array_at(p, {"tracks"}));
            // The service may only return a partial track body. Preserve all IDs and
            // page song/detail explicitly instead of silently truncating a library.
            std::vector<uint64_t> ids;
            for (const auto &t : array_at(p, {"trackIds"})) {
                auto id = json_id(t, "id");
                if (id)
                    ids.push_back(id);
            }
            if (!ids.empty()) {
                std::map<uint64_t, Media> indexed;
                for (auto &song : songs)
                    indexed[song.id] = std::move(song);
                std::vector<uint64_t> missing;
                for (auto id : ids)
                    if (!indexed.count(id))
                        missing.push_back(id);
                for (size_t n = 0; n < missing.size(); n += 200) {
                    if (stopping_ || !current_view())
                        return;
                    auto end = std::min(missing.size(), n + 200);
                    std::vector<uint64_t> batch(missing.begin() + n, missing.begin() + end);
                    auto parsed = parse_songs(array_at(api_->song_details(batch), {"songs"}));
                    for (auto &song : parsed)
                        indexed[song.id] = std::move(song);
                }
                songs.clear();
                songs.reserve(ids.size());
                for (auto id : ids) {
                    auto found = indexed.find(id);
                    if (found != indexed.end())
                        songs.push_back(found->second);
                    else {
                        Media unavailable;
                        unavailable.id = id;
                        unavailable.title = "暂不可用的歌曲 #" + std::to_string(id);
                        unavailable.artist = "服务未返回曲目信息";
                        songs.push_back(std::move(unavailable));
                    }
                }
            }
            subscribed = p.contains("subscribed") && p["subscribed"].is_boolean() &&
                         p["subscribed"].get<bool>();
        } else if (item.kind == MediaKind::Album) {
            j = api_->call("album", {{"album_id", item.id}});
            songs = parse_songs(array_at(j, {"songs"}));
            try {
                auto dynamic = api_->call("album_detail_dynamic", {{"album_id", item.id}});
                subscribed = dynamic.contains("isSub") && dynamic["isSub"].is_boolean() &&
                             dynamic["isSub"].get<bool>();
            } catch (...) {
            }
        } else if (item.kind == MediaKind::Artist) {
            j = api_->call("singer_all_songs", {{"id", item.id}, {"limit", 100}});
            songs = parse_songs(array_at(j, {"songs"}));
            more = songs.size() == 100;
        } else if (item.kind == MediaKind::Radio) {
            j = api_->call("radio_program", {{"rid", item.id}, {"limit", 100}});
            songs = parse_songs(array_at(j, {"programs"}));
            more = songs.size() == 100;
        }
        if (!current_view())
            return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!current_view())
                return;
            state_.collection = true;
            state_.collection_id = item.id;
            state_.collection_kind = item.kind;
            state_.subscribed = subscribed;
            state_.offset = 0;
        }
        assign(std::move(songs), item.title,
               item.artist.empty() ? "A 播放 · X 收藏 / 取消收藏" : item.artist, more);
    });
}
void Model::back() {
    ++view_generation_;
    Snapshot old;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (history_.empty())
            return;
        old = std::move(history_.back());
        history_.pop_back();
        state_.items = old.items;
        state_.title = old.title;
        state_.subtitle = old.subtitle;
        state_.collection = old.collection;
        state_.collection_id = old.collection_id;
        state_.collection_kind = old.collection_kind;
        state_.page = old.page;
        state_.offset = old.offset;
        state_.more = old.more;
        state_.subscribed = old.subscribed;
        state_.error.clear();
        state_.notice.clear();
        state_.revision++;
    }
}
void Model::load_more() {
    auto s = snapshot();
    if (!s.more || s.busy)
        return;
    if (s.page == Page::Search && !s.collection) {
        search(s.search_query, s.search_type, true);
        return;
    }
    if (s.collection &&
        (s.collection_kind == MediaKind::Artist || s.collection_kind == MediaKind::Radio)) {
        enqueue_view("正在加载更多", [this, s] {
            auto j = api_->call(s.collection_kind == MediaKind::Artist ? "singer_all_songs"
                                                                       : "radio_program",
                                {{"id", s.collection_id},
                                 {"rid", s.collection_id},
                                 {"offset", s.offset + 100},
                                 {"limit", 100}});
            auto list = parse_songs(array_at(j, {"songs", "programs"}));
            bool more = list.size() == 100;
            if (!current_view())
                return;
            auto items = s.items;
            items.insert(items.end(), list.begin(), list.end());
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (!current_view())
                    return;
                state_.offset = s.offset + 100;
            }
            assign(items, s.title, s.subtitle, more);
        });
    } else if (s.page == Page::Library)
        library_section(library_mode_, true);
}
void Model::library_section(int section, bool more) {
    if (section < 0 || section > 4)
        return;
    library_mode_ = section;
    auto s = snapshot();
    if (!s.user_id) {
        assign({}, "我的音乐", "登录后查看喜欢的音乐、歌单、专辑、电台和云盘");
        return;
    }
    const int offset = more ? s.offset + 100 : 0;
    enqueue_view("正在加载我的音乐", [this, section, uid = s.user_id, offset, more] {
        if (snapshot().user_id != uid)
            return;
        if (section == 1)
            load_likes(!more);
        std::vector<Media> items;
        std::string title;
        bool has_more = false;
        if (section == 0) {
            title = "我的歌单";
            auto j =
                api_->call("user_song_list", {{"uid", uid}, {"offset", offset}, {"limit", 100}});
            items = parse_collections(array_at(j, {"playlist"}), MediaKind::Playlist);
            has_more = items.size() == 100;
        }
        if (section == 1) {
            title = "我喜欢的音乐";
            auto ids = snapshot().likes;
            std::vector<uint64_t> v(ids.begin(), ids.end());
            if (static_cast<size_t>(offset) < v.size()) {
                auto end = std::min(v.size(), static_cast<size_t>(offset) + 100);
                auto j =
                    api_->song_details(std::vector<uint64_t>(v.begin() + offset, v.begin() + end));
                items = parse_songs(array_at(j, {"songs"}));
                has_more = end < v.size();
            }
        }
        if (section == 2 || section == 3) {
            title = section == 2 ? "收藏专辑" : "订阅电台";
            auto j = api_->call(section == 2 ? "album_sublist" : "user_radio_sublist",
                                {{"offset", offset}, {"limit", 100}});
            items = parse_collections(array_at(j, {"data", "djRadios"}),
                                      section == 2 ? MediaKind::Album : MediaKind::Radio);
            has_more = items.size() == 100;
        }
        if (section == 4) {
            title = "音乐云盘";
            auto j = api_->call("user_cloud_disk", {{"offset", offset}, {"limit", 100}});
            items = parse_songs(array_at(j, {"data"}));
            has_more = items.size() == 100;
        }
        if (!current_view())
            return;
        if (more) {
            auto old = snapshot().items;
            old.insert(old.end(), items.begin(), items.end());
            items = std::move(old);
        }
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!current_view())
                return;
            state_.collection = false;
            state_.offset = offset;
        }
        assign(std::move(items), title, "你的音乐，在 Switch 上继续", has_more);
    });
}
void Model::daily() {
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.collection = false;
    }
    enqueue_view("正在获取每日推荐", [this] {
        auto j = api_->call("recommend_songs");
        auto data = object_at(j, "data");
        auto list = parse_songs(array_at(data, {"dailySongs"}));
        if (list.empty())
            list = parse_songs(array_at(j, {"recommend"}));
        assign(list, "每日推荐", "每天，为你更新");
    });
}
void Model::fm() {
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.collection = false;
    }
    enqueue_view("正在调频", [this] {
        auto j = api_->call("personal_fm");
        assign(parse_songs(array_at(j, {"data"})), "私人 FM", "听见下一首惊喜 · X 换一批");
    });
}
void Model::sync_accounts() {
    if (!accounts_)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.accounts = accounts_->list();
    state_.user_id = accounts_->active_user_id();
    state_.nickname = "未登录";
    for (const auto& entry : state_.accounts)
        if (entry.user_id == state_.user_id)
            state_.nickname = entry.nickname;
    ++state_.revision;
}
void Model::save_current_session() {
    if (!accounts_)
        throw std::runtime_error("账号存储不可用，请检查 accounts.json");
    api_->flush_cookies();
    accounts_->update_active(api_->session_cookies());
}
void Model::reset_account_state() {
    ++view_generation_;
    lyric_id_ = 0;
    library_mode_ = 0;
    likes_loaded_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.items.clear();
    state_.queue.clear();
    state_.likes.clear();
    state_.lyrics.clear();
    state_.qr_key.clear();
    state_.qr_url.clear();
    state_.qr_message.clear();
    state_.search_query.clear();
    state_.search_type = 1;
    state_.collection = false;
    state_.collection_id = 0;
    state_.subscribed = false;
    state_.more = false;
    state_.offset = 0;
    state_.error.clear();
    if (state_.page == Page::Settings) {
        state_.title = "设置";
        state_.subtitle = "账号、音质、网络与运行信息";
    } else {
        state_.page = Page::Account;
        state_.title = "我的账号";
        state_.subtitle = "扫码登录，在设置中管理多个账号";
    }
    history_.clear();
    artwork_pending_.clear();
    artwork_retry_after_.clear();
    ++queue_generation_;
    ++state_.revision;
}
void Model::set_account_change_callback(std::function<void()> callback) {
    on_account_change_ = std::move(callback);
}
void Model::cancel_qr() {
    std::lock_guard<std::mutex> qr_lock(qr_mutex_);
    ++qr_generation_;
    qr_pending_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.qr_key.clear();
    state_.qr_url.clear();
    state_.qr_message.clear();
    ++state_.revision;
}
void Model::login_qr() {
    cancel_qr();
    if (!accounts_) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.qr_message = accounts_error_;
        ++state_.revision;
        return;
    }
    std::lock_guard<std::mutex> qr_lock(qr_mutex_);
    const auto generation = qr_generation_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.qr_message = "正在生成登录二维码";
    }
    enqueue("", [this, generation] {
        try {
            ApiConfig config;
            config.proxy = snapshot().proxy;
            config.timeout_seconds = 15;
            config.persist_cookies = false;
#ifdef __SWITCH__
            config.ca_bundle = "romfs:/certs/cacert.pem";
#endif
            // No cookie filename: a cancelled scan cannot change any saved login.
            auto pending = std::make_unique<ApiClient>(config);
            auto j = pending->qr_create();
            std::lock_guard<std::mutex> qr_lock(qr_mutex_);
            if (generation != qr_generation_ || !current_view())
                return;
            auto d = object_at(j, "data");
            qr_api_ = std::move(pending);
            std::lock_guard<std::mutex> lock(mutex_);
            state_.qr_key = json_text(j, "unikey", json_text(d, "unikey"));
            state_.qr_url = json_text(j, "qr_url", "https://music.163.com/login?codekey=" + state_.qr_key);
            state_.qr_message = "打开网易云音乐，扫描二维码";
            ++state_.revision;
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> qr_lock(qr_mutex_);
            if (generation == qr_generation_) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.qr_message = error.what();
                ++state_.revision;
            }
        }
    }, false);
}
void Model::activate_qr(uint64_t generation, uint64_t account_generation,
                        uint64_t uid, std::string nickname, std::string cookies) {
    if (generation != qr_generation_ || account_generation != account_generation_ || stopping_)
        return;
    if (!uid)
        throw std::runtime_error("扫码授权未返回有效账号，请重新扫码");
    save_current_session();
    if (on_account_change_)
        on_account_change_();
    // A cancel or switch while the playback barrier was waiting must win.
    std::lock_guard<std::mutex> qr_lock(qr_mutex_);
    if (generation != qr_generation_ || account_generation != account_generation_ || stopping_)
        return;
    accounts_->save(uid, std::move(nickname), std::move(cookies));
    api_->adopt_session(accounts_->active_cookies());
    ++account_generation_;
    active_account_generation_ = account_generation_;
    ++qr_generation_;
    qr_api_.reset();
    reset_account_state();
    sync_accounts();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.qr_message = "登录成功";
        state_.notice = "账号已保存，可在设置中切换";
    }
}
void Model::poll_qr() {
    std::lock_guard<std::mutex> qr_lock(qr_mutex_);
    const auto key = snapshot().qr_key;
    const auto generation = qr_generation_.load();
    const auto account_generation = account_generation_.load();
    if (key.empty() || qr_pending_.exchange(true))
        return;
    enqueue("", [this, key, generation, account_generation] {
        try {
            if (generation != qr_generation_ || !qr_api_)
                return;
            auto j = qr_api_->qr_check(key);
            if (generation != qr_generation_ || account_generation != account_generation_)
                return;
            auto code = json_id(j, "code");
            if (code == 803) {
                auto status = qr_api_->login_status();
                auto profile = object_at(status, "profile");
                if (profile.empty())
                    profile = object_at(object_at(status, "data"), "profile");
                activate_qr(generation, account_generation, json_id(profile, "userId"),
                            json_text(profile, "nickname", "网易云用户"), qr_api_->session_cookies());
            } else {
                std::lock_guard<std::mutex> qr_lock(qr_mutex_);
                if (generation != qr_generation_ || account_generation != account_generation_)
                    return;
                std::lock_guard<std::mutex> lock(mutex_);
                state_.qr_message = code == 800 ? "二维码已过期，按 A 刷新"
                                    : code == 802 ? "已扫码，请在手机上确认" : "等待扫码";
                if (code == 800)
                    state_.qr_key.clear();
                ++state_.revision;
            }
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> qr_lock(qr_mutex_);
            if (generation == qr_generation_) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.qr_message = error.what();
                ++state_.revision;
            }
        }
        if (generation == qr_generation_ || account_generation != account_generation_)
            qr_pending_ = false;
    }, false);
}
void Model::switch_account(uint64_t uid) {
    cancel_qr();
    ++account_generation_; // Invalidate already queued operations for the old account.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        artwork_pending_.clear();
    }
    enqueue("正在切换账号", [this, uid] {
        save_current_session();
        const auto entries = accounts_->list();
        if (!uid || std::none_of(entries.begin(), entries.end(),
                                [uid](const SavedAccount& a) { return a.user_id == uid; }))
            throw std::runtime_error("保存的账号不存在");
        if (on_account_change_)
            on_account_change_();
        if (!current_view())
            return;
        accounts_->switch_to(uid);
        api_->adopt_session(accounts_->active_cookies());
        reset_account_state();
        sync_accounts();
        report("账号已切换，打开音乐库查看收藏");
    });
}
void Model::remove_account(uint64_t uid) {
    cancel_qr();
    // A server-expired session can leave user_id == 0 while its local slot is
    // still active. Invalidate old data jobs for every removal; inactive-account
    // removal still leaves the playback engine untouched.
    ++account_generation_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        artwork_pending_.clear();
    }
    enqueue("正在移除本机账号", [this, uid] {
        save_current_session();
        const bool active = accounts_->active_user_id() == uid;
        if (active && on_account_change_)
            on_account_change_();
        if (!current_view())
            return;
        accounts_->remove(uid);
        if (active) {
            api_->adopt_session(accounts_->active_cookies());
            reset_account_state();
        }
        sync_accounts();
        report(active ? "当前账号已从本机移除，请扫码登录或切换其他账号" : "账号已从本机移除");
    });
}
void Model::like(uint64_t id) {
    if (!id)
        return;
    auto s = snapshot();
    if (!s.user_id) {
        report("请先登录后再收藏歌曲");
        return;
    }
    enqueue("正在更新喜欢状态", [this, id] {
        load_likes();
        if (!current_view())
            return;
        const bool add = !snapshot().likes.count(id);
        api_->call("like_v1", {{"songid", id}, {"like", add}});
        if (!current_view())
            return;
        std::lock_guard<std::mutex> l(mutex_);
        if (add)
            state_.likes.insert(id);
        else
            state_.likes.erase(id);
        state_.notice = add ? "已添加到我喜欢的音乐" : "已取消喜欢";
    });
}
void Model::subscribe_collection() {
    auto s = snapshot();
    if (!s.collection)
        return;
    if (s.collection_kind != MediaKind::Album && s.collection_kind != MediaKind::Playlist)
        return;
    enqueue("正在更新收藏", [this, s] {
        auto latest = snapshot();
        const bool add =
            !(latest.collection_id == s.collection_id && latest.collection_kind == s.collection_kind
                  ? latest.subscribed
                  : s.subscribed);
        api_->call(s.collection_kind == MediaKind::Album ? "album_like" : "song_list_like",
                   {{"id", s.collection_id}, {"like", add}});
        if (!current_view())
            return;
        std::lock_guard<std::mutex> l(mutex_);
        if (state_.collection_id == s.collection_id && state_.collection_kind == s.collection_kind)
            state_.subscribed = add;
        state_.notice = add ? "已收藏" : "已取消收藏";
    });
}
void Model::check_in() {
    enqueue("正在签到", [this] {
        auto j = api_->call("daily_task");
        if (current_view())
            report(json_text(j, "msg", "签到完成"));
    });
}
Json Model::intelligence_arguments(uint64_t current_track_id) const {
    auto s = snapshot();
    if (!current_track_id || !s.collection || s.collection_kind != MediaKind::Playlist ||
        !s.collection_id)
        throw ApiError("请先打开歌单并播放歌曲，再使用心动模式");
    return {{"sid", current_track_id}, {"pid", s.collection_id}};
}
void Model::intelligence(uint64_t current_track_id) {
    Json arguments;
    try {
        arguments = intelligence_arguments(current_track_id);
    } catch (const std::exception &error) {
        report(error.what());
        return;
    }
    enqueue_view("正在获取心动模式", [this, arguments] {
        auto j = api_->call("playmode_intelligence_list", arguments);
        std::vector<Media> songs;
        for (const auto &i : array_at(j, {"data"}))
            if (i.contains("songInfo"))
                songs.push_back(parse_song(i["songInfo"]));
        assign(songs, "心动模式", "从喜欢出发，遇见更多");
    });
}
void Model::fm_dislike(uint64_t id) {
    enqueue("正在跳过不喜欢的歌曲", [this, id] {
        api_->call("fm_trash", {{"songid", id}});
        if (current_view())
            report("已反馈");
    });
}
void Model::set_play_callback(std::function<void(const std::vector<Media> &, size_t)> cb) {
    on_play_ = std::move(cb);
}
void Model::play(size_t index) {
    if (!accounts_) {
        report("账号存档不可用，请先恢复 accounts.json 再播放");
        return;
    }
    auto list = snapshot().items;
    if (index >= list.size() || list[index].kind != MediaKind::Song)
        return;
    // Only songs are handed across the IPC boundary. URLs are resolved by the
    // playback worker so an entire large playlist need not be fetched up front.
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.queue = list;
        ++queue_generation_;
        state_.notice = "正在准备播放";
    }
    lyric_id_ = 0; // The same ID selected in a new queue may still need its artwork.
    if (on_play_)
        on_play_(list, index);
}
void Model::play_queue(size_t index) {
    if (!accounts_) {
        report("账号存档不可用，请先恢复 accounts.json 再播放");
        return;
    }
    auto s = snapshot();
    if (index < s.queue.size() && on_play_)
        on_play_(s.queue, index);
}
void Model::lyrics_for(uint64_t id) {
    if (!id || id == lyric_id_.exchange(id))
        return;
    uint64_t metadata_generation = 0;
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.lyrics.clear();
        for (const auto &song : state_.queue)
            if (song.id == id && song.cover.empty()) {
                metadata_generation = queue_generation_;
                break;
            }
    }
    enqueue(
        "",
        [this, id, metadata_generation] {
            if (id != lyric_id_)
                return;
            if (metadata_generation) {
                try {
                    auto songs = parse_songs(array_at(api_->song_details({id}), {"songs"}));
                    for (const auto &song : songs)
                        if (song.id == id && apply_track_metadata(metadata_generation, song)) {
                            ensure_artwork({song});
                            break;
                        }
                } catch (...) { /* Saved titles remain usable when metadata is offline. */
                }
            }
            if (id != lyric_id_)
                return;
            try {
                auto j = api_->call("song_lyric", {{"music_id", id}});
                auto lines = parse_lrc(json_text(object_at(j, "lrc"), "lyric"));
                auto translated = parse_lrc(json_text(object_at(j, "tlyric"), "lyric"));
                std::map<int64_t, std::string> translations;
                for (const auto &line : translated)
                    translations[line.time_ms] = line.text;
                for (auto &line : lines) {
                    auto found = translations.find(line.time_ms);
                    if (found != translations.end() && found->second != line.text)
                        line.text += "\n" + found->second;
                }
                {
                    std::lock_guard<std::mutex> l(mutex_);
                    if (id == lyric_id_)
                        state_.lyrics = std::move(lines);
                }
            } catch (...) {
                auto expected = id;
                lyric_id_.compare_exchange_strong(expected, 0);
            }
        },
        false);
}
void Model::save_settings() {
    std::lock_guard<std::mutex> lock(settings_file_mutex_);
    auto s = snapshot();
    storage::write_file(directory_ + "/settings.json",
                        Json{{"quality", s.quality}, {"proxy", s.proxy}}.dump(2) + "\n");
}
void Model::set_quality(std::string quality) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.quality = quality;
    }
    try {
        save_settings();
        report("音质将在下一首生效");
    } catch (const std::exception &error) {
        report(std::string("音质已临时更新，但设置保存失败：") + error.what());
    }
}
void Model::set_proxy(std::string proxy) {
    enqueue("正在设置代理", [this, proxy] {
        api_->set_proxy(proxy);
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.proxy = proxy;
        }
        save_settings();
        report("网络代理已更新");
    });
}
} // namespace netease
