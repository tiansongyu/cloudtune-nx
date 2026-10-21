// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic regression tests for navigation cancellation and worker ordering.
#include "netease/model.hpp"
#include "netease/playback_protocol.h"
#include "netease/storage.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace netease {
struct ModelTestAccess {
    static void enqueue(Model &m, std::function<void()> job, bool visible = true) {
        m.enqueue("test", std::move(job), visible);
    }
    static void view(Model &m, std::function<void()> job) {
        m.enqueue_view("test view", std::move(job));
    }
    static void title(Model &m, std::string title) {
        m.assign({}, std::move(title), "");
    }
    static std::size_t pending_artwork(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.artwork_pending_.size();
    }
    static bool artwork_backoff(Model &m, const std::string &url) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.artwork_retry_after_.count(url) != 0;
    }
    static void items(Model &m, std::vector<Media> items) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.state_.items = std::move(items);
    }
    static void playlist(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.history_.clear();
        m.history_.push_back(Snapshot{});
        m.state_.collection = true;
        m.state_.collection_id = 1234;
        m.state_.collection_kind = MediaKind::Playlist;
        m.state_.title = "Fixture playlist";
        Media first;
        first.id = 111;
        Media second;
        second.id = 222;
        m.state_.queue = {first, second};
    }
    static std::size_t history_size(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.history_.size();
    }
    static Json intelligence_arguments(Model &m, uint64_t id) {
        return m.intelligence_arguments(id);
    }
    static void restore_queue(Model &m) {
        m.restore_queue();
    }
    static uint64_t queue_generation(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        return m.queue_generation_;
    }
    static bool apply_metadata(Model &m, uint64_t generation, const Media &song) {
        return m.apply_track_metadata(generation, song);
    }
    static void discard_queued(Model &m) {
        {
            std::lock_guard<std::mutex> lock(m.jobs_mutex_);
            m.jobs_.clear();
        }
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.pending_visible_ = 0;
        m.state_.busy = false;
    }
    static void private_state(Model &m) {
        playlist(m);
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.state_.likes.insert(444);
        m.state_.lyrics.push_back({0, "previous account lyric"});
        m.state_.items = m.state_.queue;
        m.state_.subscribed = true;
    }
    static std::pair<uint64_t, uint64_t> qr(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.state_.qr_key = "synthetic-test-key";
        m.state_.qr_url = "https://example.invalid/qr";
        return {m.qr_generation_, m.account_generation_};
    }
    static void authorize(Model &m, std::pair<uint64_t, uint64_t> token) {
        m.activate_qr(token.first, token.second, 33, "扫码账号",
                      "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-qr\n");
    }
    static void expired_account(Model &m) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.state_.user_id = 0;
        m.state_.nickname = "未登录";
        m.state_.likes.clear();
    }
    static bool stopping(Model &m) {
        return m.stopping_;
    }
};
} // namespace netease
using namespace netease;
using namespace std::chrono_literals;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> future = promise.get_future().share();
    bool released = false;
    void release() {
        if (!released) {
            released = true;
            promise.set_value();
        }
    }
    ~Gate() {
        release();
    }
};
static void ready(std::future<void> &future) {
    require(future.wait_for(5s) == std::future_status::ready, "Worker failed to reach checkpoint");
}
static void idle(Model &model) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (model.snapshot().busy && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    require(!model.snapshot().busy, "Busy state remained set after all visible jobs");
}
static std::string queue_bytes(uint32_t count = 2) {
    NcmQueueHeader header{{'N', 'C', 'M', 'Q', 'U', 'E', 'U', 'E'}, NCM_PROTOCOL_VERSION, count};
    std::string bytes(reinterpret_cast<const char *>(&header), sizeof(header));
    for (uint32_t i = 0; i < count; ++i) {
        NcmQueueEntry entry{};
        entry.id = 1000 + i;
        entry.duration_ms = 45000 + i;
        std::strcpy(entry.title, "Saved title");
        std::strcpy(entry.artist, "Saved artist");
        std::strcpy(entry.album, "Saved album");
        std::strcpy(entry.url, "https://example.invalid/preview.mp3");
        bytes.append(reinterpret_cast<const char *>(&entry), sizeof(entry));
    }
    return bytes;
}
static void write_bytes(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), bytes.size());
    require(bool(file), "Failed to write queue fixture");
}
static void queue_recovery_tests(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    const auto path = directory / "queue.nsq";
    const auto valid = queue_bytes();
    write_bytes(path, valid);
    {
        Model model(directory.string());
        Gate gate;
        std::promise<void> entered;
        auto future = entered.get_future();
        ModelTestAccess::enqueue(
            model,
            [&entered, f = gate.future] {
                entered.set_value();
                f.wait();
            },
            false);
        ready(future);
        int plays = 0;
        model.set_play_callback([&plays](const auto &, size_t) { ++plays; });
        model.start();
        auto state = model.snapshot();
        require(state.queue.size() == 2 && state.queue[1].id == 1001 &&
                    state.queue[1].duration_ms == 45001 && state.queue[0].title == "Saved title" &&
                    state.queue[0].artist == "Saved artist" &&
                    state.queue[0].album == "Saved album" &&
                    state.queue[0].url == "https://example.invalid/preview.mp3",
                "Start did not restore saved queue metadata");
        require(plays == 0, "Restoring a saved queue unexpectedly started playback");
        // Suppress startup network tasks while testing the actual start path.
        ModelTestAccess::discard_queued(model);
        gate.release();
        model.browse(Page::Account);
        const auto generation = ModelTestAccess::queue_generation(model);
        Media details;
        details.id = 1000;
        details.title = "Complete title";
        details.artist = "Complete artist";
        details.album = "Complete album";
        details.cover = "https://example.invalid/artwork.jpg";
        require(ModelTestAccess::apply_metadata(model, generation, details),
                "Current metadata was not applied");
        state = model.snapshot();
        require(state.queue[0].cover == details.cover && state.queue[0].duration_ms == 45000 &&
                    state.queue[0].url == "https://example.invalid/preview.mp3" &&
                    state.page == Page::Account && state.items.empty(),
                "Metadata hydration changed stream timing, URL, or the current page");
        Media replacement;
        replacement.id = 1000;
        replacement.title = "New queue title";
        ModelTestAccess::items(model, {replacement});
        model.play(model.snapshot(), 0);
        require(!ModelTestAccess::apply_metadata(model, generation, details),
                "A delayed metadata response changed a newer play queue");
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.size() == 1 &&
                    model.snapshot().queue[0].title == "New queue title",
                "Restoring a queue replaced a user-selected queue");
    }
    // Every rejected fixture is a bounded, offline parse; no playback or API calls.
    std::vector<std::string> invalid;
    invalid.push_back("");
    invalid.push_back(valid.substr(0, 15));
    auto bad = valid;
    bad[0] = 'X';
    invalid.push_back(bad);
    bad = valid;
    bad[8] = 2;
    invalid.push_back(bad);
    bad = valid;
    std::memset(bad.data() + 12, 0, 4);
    invalid.push_back(bad);
    bad = valid;
    bad[12] = 1;
    bad[13] = 2;
    invalid.push_back(bad); // 513
    invalid.push_back(valid.substr(0, valid.size() - 1));
    invalid.push_back(valid + "x");
    bad = valid;
    bad[sizeof(NcmQueueHeader) + 12] = 1;
    invalid.push_back(bad);
    bad = valid;
    std::memset(bad.data() + sizeof(NcmQueueHeader) + offsetof(NcmQueueEntry, title), 'x', 128);
    invalid.push_back(bad);
    for (const auto &bytes : invalid) {
        write_bytes(path, bytes);
        Model model(directory.string());
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.empty(), "Malformed playback queue was accepted");
    }
    write_bytes(path, queue_bytes(NCM_MAX_QUEUE));
    {
        Model model(directory.string());
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.size() == NCM_MAX_QUEUE,
                "A valid 512-entry queue was rejected");
    }
    write_bytes(path.string() + ".bak", valid);
    write_bytes(path, "invalid primary");
    {
        Model model(directory.string());
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.size() == 2,
                "Valid queue backup did not recover a corrupt primary");
    }
    std::filesystem::remove(path);
    {
        Model model(directory.string());
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.size() == 2,
                "Interrupted Horizon rename did not recover from queue backup");
    }
    write_bytes(path, queue_bytes(0));
    {
        Model model(directory.string());
        ModelTestAccess::restore_queue(model);
        require(model.snapshot().queue.empty(),
                "Empty queue tombstone restored another account's backup queue");
    }
}
static void account_model_tests(const std::filesystem::path& directory) {
    const auto jar = [](const char* value) {
        return "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\t" +
               std::string(value) + "\n";
    };
    {
        AccountStore store(directory.string());
        store.save(11, "甲", jar("synthetic-alice"));
        store.save(22, "乙", jar("synthetic-bob"), false);
    }
    Model model(directory.string());
    require(model.snapshot().user_id == 11 && model.snapshot().accounts.size() == 2,
            "Model did not restore saved active identity offline");
    ModelTestAccess::private_state(model);
    int barriers = 0;
    model.set_account_change_callback([&] { ++barriers; });
    Gate gate;
    std::promise<void> entered;
    auto future = entered.get_future();
    ModelTestAccess::enqueue(model, [&entered, f = gate.future] {
        entered.set_value();
        f.wait();
    }, false);
    ready(future);
    bool stale_ran = false;
    ModelTestAccess::enqueue(model, [&] { stale_ran = true; });
    model.switch_account(22);
    gate.release();
    idle(model);
    auto state = model.snapshot();
    require(!stale_ran && barriers == 1 && state.user_id == 22 && state.nickname == "乙",
            "Account switch executed stale requests or failed the playback barrier");
    require(state.items.empty() && state.queue.empty() && state.likes.empty() &&
                state.lyrics.empty() && !state.collection && !state.subscribed &&
                ModelTestAccess::history_size(model) == 0,
            "Account switch leaked previous account view state");
    require(storage::read_file((directory / "cookies.txt").string()).find("synthetic-bob") != std::string::npos,
            "Model did not publish the selected session");
    ModelTestAccess::enqueue(model, [&] {
        // Emulate a service response rotating its in-memory cookie, with no
        // actual HTTP request. The worker must commit it without a later switch.
        model.api().adopt_session(jar("synthetic-bob-refreshed"));
    });
    idle(model);
    require(storage::read_file((directory / "cookies.txt").string()).find("synthetic-bob-refreshed") != std::string::npos &&
                AccountStore(directory.string()).active_cookies().find("synthetic-bob-refreshed") != std::string::npos,
            "Worker completion did not persist rotated account cookies");

    model.set_account_change_callback([] { throw std::runtime_error("Synthetic playback reset failure"); });
    model.switch_account(11);
    idle(model);
    require(model.snapshot().user_id == 22 && !model.snapshot().error.empty() &&
                AccountStore(directory.string()).active_user_id() == 22,
            "Failed playback barrier changed active credentials");
    model.set_account_change_callback([&] { ++barriers; });
    model.remove_account(11);
    idle(model);
    require(model.snapshot().user_id == 22 && model.snapshot().accounts.size() == 1 && barriers == 1,
            "Removing an inactive account disrupted playback");

    // A completed authorization arriving after B/cancel must not publish its jar.
    auto token = ModelTestAccess::qr(model);
    model.cancel_qr();
    ModelTestAccess::enqueue(model, [&] { ModelTestAccess::authorize(model, token); });
    idle(model);
    require(model.snapshot().user_id == 22 && model.snapshot().qr_url.empty() &&
                model.snapshot().accounts.size() == 1,
            "Cancelled QR authorization changed the active account");

    // Cancellation while waiting for the playback reset wins over authorization.
    Gate barrier_gate;
    std::promise<void> barrier_entered;
    auto barrier_future = barrier_entered.get_future();
    model.set_account_change_callback([&] {
        barrier_entered.set_value();
        barrier_gate.future.wait();
    });
    token = ModelTestAccess::qr(model);
    ModelTestAccess::enqueue(model, [&] { ModelTestAccess::authorize(model, token); });
    ready(barrier_future);
    model.cancel_qr();
    barrier_gate.release();
    idle(model);
    require(model.snapshot().user_id == 22 && model.snapshot().accounts.size() == 1,
            "QR cancellation during playback barrier activated a stale account");
    model.set_account_change_callback([&] { ++barriers; });
    token = ModelTestAccess::qr(model);
    ModelTestAccess::enqueue(model, [&] { ModelTestAccess::authorize(model, token); });
    idle(model);
    require(model.snapshot().user_id == 33 && model.snapshot().accounts.size() == 2 &&
                model.snapshot().qr_key.empty(),
            "Successful QR authorization was not saved and activated");
    model.remove_account(33);
    idle(model);
    require(model.snapshot().user_id == 0 && model.snapshot().accounts.size() == 1 &&
                model.snapshot().queue.empty(),
            "Removing active account silently selected another identity");
    model.switch_account(22);
    idle(model);
    ModelTestAccess::expired_account(model);
    Gate expired_gate;
    std::promise<void> expired_entered;
    auto expired_future = expired_entered.get_future();
    ModelTestAccess::enqueue(model, [&expired_entered, f = expired_gate.future] {
        expired_entered.set_value();
        f.wait();
    }, false);
    ready(expired_future);
    bool expired_job_ran = false;
    ModelTestAccess::enqueue(model, [&] { expired_job_ran = true; });
    const int before_removal = barriers;
    model.remove_account(22);
    expired_gate.release();
    idle(model);
    require(!expired_job_ran && barriers == before_removal + 1 &&
                model.snapshot().user_id == 0 && model.snapshot().accounts.empty(),
            "Removing a server-expired active session retained stale account requests");
    model.stop();
}
static void damaged_accounts_tests(const std::filesystem::path& directory,
                                  const std::string& damaged) {
    std::filesystem::create_directories(directory);
    const auto registry = directory / "accounts.json";
    const auto mirror = directory / "cookies.txt";
    const std::string cookies =
        "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-mirror\n";
    write_bytes(registry, damaged);
    write_bytes(mirror, cookies);
    write_bytes(directory / "queue.nsq", queue_bytes());
    {
        Model model(directory.string());
        const auto error = model.snapshot().error;
        require(!error.empty() && error.find("synthetic-private") == std::string::npos,
                "Corrupt account registry exposed response contents");
        require(model.api().session_cookies().find("synthetic-mirror") == std::string::npos,
                "Corrupt registry silently activated its cookie mirror");
        Gate gate;
        std::promise<void> entered;
        auto future = entered.get_future();
        ModelTestAccess::enqueue(model, [&entered, f = gate.future] {
            entered.set_value();
            f.wait();
        }, false);
        ready(future);
        model.start();
        require(model.snapshot().error == error && model.snapshot().queue.empty(),
                "Startup hid the account recovery error or restored private queue data");
        model.browse(Page::Settings);
        model.browse(Page::Account);
        model.login_qr();
        require(model.snapshot().error == error && model.snapshot().qr_url.empty(),
                "Navigation or QR creation discarded the account recovery error");
        int plays = 0;
        model.set_play_callback([&plays](const auto&, size_t) { ++plays; });
        ModelTestAccess::private_state(model);
        Media public_song;
        public_song.id = 999;
        ModelTestAccess::items(model, {public_song});
        const auto queue_generation = ModelTestAccess::queue_generation(model);
        model.play(model.snapshot(), 0);
        model.play_queue(model.snapshot(), 0);
        require(plays == 0 && model.snapshot().queue.front().id == 111 &&
                    ModelTestAccess::queue_generation(model) == queue_generation,
                "Corrupt registry allowed playback to activate the old cookie mirror");
        ModelTestAccess::discard_queued(model);
        gate.release();
        model.stop();
    }
    require(storage::read_file(registry.string()) == damaged &&
                storage::read_file(mirror.string()) == cookies,
            "Corrupt registry recovery overwrote the original account archive or mirror");
}
static void stopped_account_transaction_tests(const std::filesystem::path& root) {
    const std::string alice =
        "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-alice\n";
    const std::string bob =
        "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-bob\n";
    for (const bool remove : {false, true}) {
        const auto directory = root / (remove ? "remove" : "switch");
        {
            AccountStore store(directory.string());
            store.save(11, "Alice", alice);
            store.save(22, "Bob", bob, false);
        }
        Model model(directory.string());
        Gate gate;
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        model.set_account_change_callback([&] {
            entered.set_value();
            gate.future.wait();
        });
        if (remove)
            model.remove_account(11);
        else
            model.switch_account(22);
        ready(entered_future);
        auto stopped = std::async(std::launch::async, [&] { model.stop(); });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!ModelTestAccess::stopping(model) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        const bool cancelled = ModelTestAccess::stopping(model);
        gate.release();
        require(stopped.wait_for(5s) == std::future_status::ready,
                "Account barrier prevented model shutdown");
        stopped.get();
        require(cancelled, "Shutdown failed to cancel the pending account transaction");
        const auto registry = Json::parse(storage::read_file((directory / "accounts.json").string()));
        require(registry["active_user_id"] == 11 && registry["accounts"].size() == 2 &&
                    AccountStore::playback_cookies(directory.string()).find("synthetic-alice") !=
                        std::string::npos,
                "Shutdown committed an account switch/removal after cancellation");
    }
}
int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
                               ("netease-model-test-" + std::to_string(getpid()));
        queue_recovery_tests(directory / "queue-recovery");
        account_model_tests(directory / "accounts");
        stopped_account_transaction_tests(directory / "stopped-accounts");
        damaged_accounts_tests(directory / "damaged-accounts",
                               R"({"cookies":"synthetic-private-registry"!)");
        damaged_accounts_tests(directory / "empty-accounts", "");
        {
            Model model(directory.string());
            // Gates are destroyed before model, so failures cannot strand the worker.
            Gate background_gate, first_gate;
            std::promise<void> entered, first_entered, artwork_done;
            auto entered_future = entered.get_future(), first_future = first_entered.get_future(),
                 art_future = artwork_done.get_future();
            std::vector<int> order;
            ModelTestAccess::enqueue(
                model,
                [&entered, f = background_gate.future] {
                    entered.set_value();
                    f.wait();
                },
                false);
            ready(entered_future);
            ModelTestAccess::enqueue(
                model,
                [&order, &artwork_done] {
                    order.push_back(3);
                    artwork_done.set_value();
                },
                false);
            ModelTestAccess::enqueue(model, [&order, &first_entered, f = first_gate.future] {
                order.push_back(1);
                first_entered.set_value();
                f.wait();
            });
            ModelTestAccess::enqueue(model, [&order] { order.push_back(2); });
            background_gate.release();
            ready(first_future);
            require(model.snapshot().busy, "Busy cleared while visible work remained");
            first_gate.release();
            ready(art_future);
            idle(model);
            require(order == std::vector<int>({1, 2, 3}),
                    "Visible work must stay FIFO and precede queued artwork");

            Gate old_response;
            std::promise<void> old_started;
            auto old_future = old_started.get_future();
            ModelTestAccess::view(model, [&model, &old_started, f = old_response.future] {
                old_started.set_value();
                f.wait();
                ModelTestAccess::title(model, "obsolete network result");
            });
            ready(old_future);
            model.browse(Page::Account);
            old_response.release();
            idle(model);
            require(model.snapshot().page == Page::Account && model.snapshot().title == "我的账号",
                    "A slow response replaced the newly selected page");

            Gate queued_gate;
            std::promise<void> blocked;
            auto blocked_future = blocked.get_future();
            ModelTestAccess::enqueue(
                model,
                [&blocked, f = queued_gate.future] {
                    blocked.set_value();
                    f.wait();
                },
                false);
            ready(blocked_future);
            bool obsolete_ran = false;
            ModelTestAccess::view(model, [&obsolete_ran] { obsolete_ran = true; });
            ModelTestAccess::view(model, [&model] { ModelTestAccess::title(model, "latest"); });
            queued_gate.release();
            idle(model);
            require(!obsolete_ran && model.snapshot().title == "latest",
                    "Superseded queued navigation was executed");

            Gate refresh_gate;
            std::promise<void> refresh_blocked;
            auto refresh_future = refresh_blocked.get_future();
            ModelTestAccess::enqueue(
                model,
                [&refresh_blocked, f = refresh_gate.future] {
                    refresh_blocked.set_value();
                    f.wait();
                },
                false);
            ready(refresh_future);
            ModelTestAccess::playlist(model);
            auto intelligence = ModelTestAccess::intelligence_arguments(model, 222);
            require(intelligence["sid"] == 222 && intelligence["pid"] == 1234,
                    "Intelligence mode did not use the playing track and current playlist");
            bool invalid_seed_rejected = false;
            try {
                ModelTestAccess::intelligence_arguments(model, 0);
            } catch (const ApiError &) {
                invalid_seed_rejected = true;
            }
            require(invalid_seed_rejected,
                    "Intelligence mode silently selected the first queued song");
            model.refresh();
            require(ModelTestAccess::history_size(model) == 1,
                    "Refreshing a playlist pushed duplicate history");
            model.back(); // Also cancels the queued network refresh, so this test is offline.
            require(!model.snapshot().collection && ModelTestAccess::history_size(model) == 0,
                    "Back after refresh did not return to the parent page");
            refresh_gate.release();
            idle(model);

            Gate artwork_gate;
            std::promise<void> artwork_blocked;
            auto artwork_blocked_future = artwork_blocked.get_future();
            ModelTestAccess::enqueue(
                model,
                [&artwork_blocked, f = artwork_gate.future] {
                    artwork_blocked.set_value();
                    f.wait();
                },
                false);
            ready(artwork_blocked_future);
            Media visible;
            visible.id = 1;
            visible.cover = "invalid-protocol:fixture";
            ModelTestAccess::items(model, {visible});
            for (int n = 0; n < 100; ++n)
                model.ensure_artwork({visible});
            require(ModelTestAccess::pending_artwork(model) == 1,
                    "Visible artwork was not deduplicated");
            std::promise<void> artwork_drained;
            auto artwork_drained_future = artwork_drained.get_future();
            ModelTestAccess::enqueue(
                model, [&artwork_drained] { artwork_drained.set_value(); }, false);
            artwork_gate.release();
            ready(artwork_drained_future);
            require(ModelTestAccess::pending_artwork(model) == 0 &&
                        ModelTestAccess::artwork_backoff(model, visible.cover),
                    "Failed artwork did not release pending state and apply backoff");
            model.ensure_artwork({visible});
            require(ModelTestAccess::pending_artwork(model) == 0,
                    "Failed artwork retried without the delay");

            // Emulate an SD save failure during the application's exit event.
            std::filesystem::create_directories(directory / "cookies.txt.new");
            model.stop();
            require(!model.snapshot().error.empty(),
                    "Shutdown save failure was neither handled nor reported");
        } // All destructors must tolerate that persistence failure too.
        std::filesystem::remove_all(directory);
        std::cout << "Model FIFO / navigation / queue recovery / account isolation / QR cancellation / stale metadata / shutdown tests "
                     "passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
