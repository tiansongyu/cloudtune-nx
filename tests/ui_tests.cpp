// SPDX-License-Identifier: GPL-3.0-or-later
// Uses the production Borealis view. Synthetic data lives only in this test binary.
#include "netease/ui.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace netease {
struct ModelTestAccess {
    static void set(Model &m, const Snapshot &s) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.state_ = s;
    }
    static void push_parent(Model &m, const Snapshot &s) {
        std::lock_guard<std::mutex> lock(m.mutex_);
        m.history_.push_back(s);
    }
    static size_t depth(Model &m) {
        return m.history_.size();
    }
};
struct MusicViewTestAccess {
    static void freeze(MusicView &v, const Snapshot &s, bool full = false) {
        v.display_ = s;
        v.reconcile(s);
        v.last_poll_ = v.last_airplay_poll_ = v.last_qr_ =
            std::chrono::steady_clock::now() + std::chrono::hours(1);
        v.player_open_ = full;
        v.playback_.state = NcmPlayback_Playing;
        v.playback_.track_id = 101;
        v.playback_.position_ms = 98000;
        v.playback_.duration_ms = 272000;
        v.playback_.volume_permille = 700;
        std::snprintf(v.playback_.title, sizeof(v.playback_.title), "%s", "晚风经过海岸");
        std::snprintf(v.playback_.artist, sizeof(v.playback_.artist), "%s", "CloudTune Sessions");
        v.airplay_.version = NCM_AIRPLAY_VERSION;
        v.airplay_.state = NcmAirplay_Ready;
        v.airplay_.enabled = 1;
    }
    static void airplay(MusicView &v) {
        v.playback_.track_id = 0;
        std::snprintf(v.playback_.message, sizeof(v.playback_.message), "%s", "AirPlay playing");
    }
    static void set_focus(MusicView &v, int index, int scroll = 0) {
        v.selection_ = index;
        v.scroll_ = scroll;
        v.anchor_.clear();
        v.region_ = ui::Region::Content;
    }
    static void sync(MusicView &v, const Snapshot &s) {
        v.reconcile(s);
    }
    static void save(MusicView &v, const Snapshot &s) {
        v.remember(s);
    }
    static void page(MusicView &v, Page p) {
        v.switch_page(p);
    }
    static void back(MusicView &v) {
        v.back();
    }
    static void tap(MusicView &v, float x, float y) {
        v.tap(x, y);
    }
    static int selected(const MusicView &v) {
        return v.selection_;
    }
    static int scroll(const MusicView &v) {
        return v.scroll_;
    }
    static float top(const MusicView &v, const Snapshot &s) {
        return v.content_top(s);
    }
    static int rows(const MusicView &v, const Snapshot &s) {
        return v.visible_rows(s);
    }
    static std::vector<ui::Rect> header_bounds(const MusicView &v, const Snapshot &s) {
        std::vector<ui::Rect> bounds;
        for (const auto &control : v.headers(s))
            bounds.push_back(control.first);
        return bounds;
    }
    static ui::Region region(const MusicView &v) {
        return v.region_;
    }
    static bool full(const MusicView &v) {
        return v.player_open_;
    }
    static void reset(MusicView &v) {
        v.reset_content();
        v.bookmarks_.clear();
    }
    static void header(MusicView &v, int index) {
        v.region_ = ui::Region::Header;
        v.header_ = index;
    }
    static void modal(MusicView &v) {
        v.options("播放选项",
                  {"喜欢当前歌曲", "随机播放", "顺序播放", "列表循环", "单曲循环", "停止播放"},
                  [](int) {});
    }
    static bool photos(const MusicView &v) {
        return v.memories_.available();
    }
    static void memories(MusicView &v) {
        v.open_memories();
    }
    static void reduced(MusicView &v, bool value) {
        v.reduced_motion_ = value;
    }
};
} // namespace netease
using namespace netease;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static Snapshot fixture(const std::string &scene, const std::string &assets) {
    Snapshot s;
    s.page = Page::Discover;
    s.title = "值得慢慢听";
    s.nickname = "演示账号";
    s.quality = "lossless";
    const char *titles[] = {"海岸线的慢时光", "微醺爵士，夜色正好", "把心事交给森林",
                            "下一站，另一个世界"};
    const char *artists[] = {"电子 · 氛围 · 放松", "爵士 · 复古 · 夜晚", "自然 · 轻音乐",
                             "游戏 · 原声 · 探索"};
    for (int i = 0; i < 12; ++i) {
        Media m;
        m.id = 1001 + i;
        m.kind = MediaKind::Playlist;
        m.title = titles[i % 4];
        m.artist = artists[i % 4];
        m.cover = "fixture://cover/" + std::to_string(i % 4);
        m.count = 24 + i;
        s.images[m.cover] = assets + "/cover-" + std::to_string(i % 4) + ".png";
        s.items.push_back(m);
    }
    const char *songs[] = {"晚风经过海岸",   "慢一点，也没关系", "月光落在你的肩上",
                           "留一盏灯给夜晚", "当世界安静下来",   "下一次日落",
                           "未经命名的旅程", "山海之间"};
    for (int i = 0; i < 8; ++i) {
        Media m;
        m.id = 101 + i;
        m.kind = MediaKind::Song;
        m.title = songs[i];
        m.artist = "CloudTune Sessions";
        m.album = "Slow Tides";
        m.cover = "fixture://cover/" + std::to_string(i % 4);
        m.duration_ms = 272000 + i * 12000;
        s.queue.push_back(m);
    }
    s.lyrics = {{0, "放慢脚步，等一阵晚风"},
                {40000, "让漫长的白昼，轻轻靠岸"},
                {90000, "把今天的心事，交给海浪"},
                {130000, "远处的灯火，像未说完的愿望"},
                {170000, "我们还有很长的路要走"}};
    s.likes.insert(101);
    if (scene == "library") {
        s.page = Page::Library;
        s.title = "我的歌单";
    }
    if (scene == "charts") {
        s.page = Page::Charts;
        s.title = "排行榜";
    }
    if (scene == "collection") {
        s.collection = true;
        s.collection_id = 1001;
        s.title = "海岸线的慢时光";
        s.subtitle = "CloudTune Sessions · 8 首音乐 · 36 分钟";
        s.items = s.queue;
    }
    if (scene == "daily" || scene == "fm") {
        s.title = scene == "daily" ? "每日推荐" : "私人 FM";
        s.items = s.queue;
    }
    if (scene == "search") {
        s.page = Page::Search;
        s.search_query = "晚风";
        s.title = "搜索";
        s.items = s.queue;
    }
    if (scene == "queue") {
        s.page = Page::Queue;
        s.title = "播放队列";
        s.items = s.queue;
    }
    if (scene == "settings") {
        s.page = Page::Settings;
        s.items.clear();
    }
    if (scene == "account") {
        s.page = Page::Account;
        s.items.clear();
        s.qr_url = "https://example.invalid/cloudtune-ui-test";
        s.qr_message = "测试二维码 · 不用于登录";
    }
    if (scene == "loading" || scene == "empty" || scene == "error") {
        s.items.clear();
        s.busy = scene == "loading";
        if (scene == "error")
            s.error = "连接暂时中断，请检查网络后重试";
    }
    return s;
}
static void controller_checks(MusicView &v, Model &m, const std::string &assets) {
    auto root = fixture("discover", assets);
    ModelTestAccess::set(m, root);
    MusicViewTestAccess::freeze(v, root);
    v.navigate(-2);
    v.navigate(1);
    v.navigate(2);
    require(m.snapshot().page == Page::Discover &&
                MusicViewTestAccess::region(v) == ui::Region::Content,
            "Sidebar Right must return to current content without activating the hovered page");
    MusicViewTestAccess::set_focus(v, 3);
    v.navigate(2);
    require(MusicViewTestAccess::selected(v) == 3, "Right must not wrap grid rows");
    v.navigate(1);
    require(MusicViewTestAccess::selected(v) == 7 && MusicViewTestAccess::scroll(v) == 1,
            "D-pad must reveal the next large-cover row");
    v.navigate(-1);
    v.navigate(-1);
    require(MusicViewTestAccess::region(v) == ui::Region::Header,
            "Header actions must be reachable");
    v.navigate(1);
    require(MusicViewTestAccess::region(v) == ui::Region::Content,
            "Header down must return to content");
    MusicViewTestAccess::set_focus(v, 7, 1);
    MusicViewTestAccess::sync(v, root);
    MusicViewTestAccess::save(v, root);
    auto detail = fixture("collection", assets);
    ModelTestAccess::push_parent(m, root);
    ModelTestAccess::set(m, detail);
    MusicViewTestAccess::sync(v, detail);
    MusicViewTestAccess::set_focus(v, 6, 2);
    MusicViewTestAccess::page(v, Page::Player);
    require(MusicViewTestAccess::full(v) && m.snapshot().collection &&
                ModelTestAccess::depth(m) == 1,
            "Opening player must preserve model collection and navigation history");
    MusicViewTestAccess::back(v);
    require(!MusicViewTestAccess::full(v) && MusicViewTestAccess::selected(v) == 6 &&
                MusicViewTestAccess::scroll(v) == 2,
            "Closing player must restore exact focus and scroll");
    MusicViewTestAccess::back(v);
    require(!m.snapshot().collection && MusicViewTestAccess::selected(v) == 7 &&
                MusicViewTestAccess::scroll(v) == 1,
            "B from collection must restore its parent card");
    auto reordered = root;
    std::swap(reordered.items[0], reordered.items[7]);
    MusicViewTestAccess::sync(v, reordered);
    require(MusicViewTestAccess::selected(v) == 0,
            "Metadata refresh must preserve stable media focus");
    auto queue = fixture("queue", assets);
    queue.queue[2].title = "Canonical queue entry";
    ModelTestAccess::set(m, queue);
    MusicViewTestAccess::sync(v, queue);
    size_t played = 99;
    std::string played_title;
    m.set_play_callback([&](const std::vector<Media> &items, size_t index) {
        played = index;
        played_title = items[index].title;
    });
    MusicViewTestAccess::set_focus(v, 2);
    v.activate();
    require(played == 2 && played_title == "Canonical queue entry",
            "Queue activation must use canonical playback queue");
    ModelTestAccess::set(m, detail);
    MusicViewTestAccess::sync(v, detail);
    MusicViewTestAccess::set_focus(v, 4);
    v.activate();
    require(played == 4 && played_title == detail.items[4].title,
            "Collection activation must hand the selected song to the existing model");
    for (const auto *scene : {"daily", "fm"}) {
        auto songs = fixture(scene, assets);
        ModelTestAccess::set(m, songs);
        MusicViewTestAccess::sync(v, songs);
        MusicViewTestAccess::set_focus(v, 0);
        const float top = MusicViewTestAccess::top(v, songs);
        const int rows = MusicViewTestAccess::rows(v, songs);
        const auto first = ui::row(0, top), last = ui::row(rows - 1, top);
        for (const auto &header : MusicViewTestAccess::header_bounds(v, songs))
            require(first.y - 7 > header.y + header.h,
                    "Discovery song rows must clear the banner and header actions");
        require(first.y - 7 > 276 && last.y + last.h + 7 < ui::MiniTop,
                "Song focus must clear the section title and mini player");
        for (int i = 0; i < static_cast<int>(songs.items.size()) - 1; ++i)
            v.navigate(1);
        require(MusicViewTestAccess::selected(v) == 7 &&
                    MusicViewTestAccess::scroll(v) == 8 - rows &&
                    MusicViewTestAccess::region(v) == ui::Region::Content,
                "Every discovery song must remain reachable within the visible rows");
        played = 99;
        MusicViewTestAccess::tap(v, last.x + 140, last.y + last.h / 2);
        require(played == 7 && played_title == songs.items[7].title,
                "Touch must activate the visible song after scrolling");
        MusicViewTestAccess::set_focus(v, 0);
        v.navigate(-1);
        require(MusicViewTestAccess::region(v) == ui::Region::Header,
                "Discovery songs must retain header navigation");
        v.navigate(1);
        played = 99;
        MusicViewTestAccess::tap(v, first.x + 140, first.y + first.h / 2);
        require(played == 0 && played_title == songs.items[0].title,
                "First song touch must not activate the daily banner");
    }
    auto settings = fixture("settings", assets);
    ModelTestAccess::set(m, settings);
    MusicViewTestAccess::sync(v, settings);
    MusicViewTestAccess::set_focus(v, 5);
    v.navigate(1);
    require(MusicViewTestAccess::region(v) == ui::Region::Playback,
            "Bottom settings must reach transport");
    v.navigate(-1);
    require(MusicViewTestAccess::selected(v) == 5, "Transport up must restore selected setting");
    MusicViewTestAccess::tap(v, 1100, 622); // Volume is a native modal.
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
    ModelTestAccess::set(m, root);
    MusicViewTestAccess::sync(v, root);
    if (MusicViewTestAccess::photos(v)) {
        // Photo browsing must be a separate activity, even while music is loading.
        auto loading = root;
        loading.busy = true;
        ModelTestAccess::set(m, loading);
        MusicViewTestAccess::reduced(v, true);
        MusicViewTestAccess::header(v, 1);
        const auto depth = ModelTestAccess::depth(m);
        v.activate();
        require(brls::Application::getCurrentFocus() != &v,
                "Local gallery must open while network data is busy");
        require(m.snapshot().page == Page::Discover && ModelTestAccess::depth(m) == depth,
                "Opening photos must preserve music navigation and history");
        brls::Application::popActivity(brls::TransitionAnimation::NONE);
        require(brls::Application::getCurrentFocus() == &v &&
                    MusicViewTestAccess::region(v) == ui::Region::Header,
                "Closing photos must restore the music header focus");
        MusicViewTestAccess::reduced(v, false);
        ModelTestAccess::set(m, root);
    }
    MusicViewTestAccess::header(v, MusicViewTestAccess::photos(v) ? 2 : 1);
    v.activate();
    require(
        m.snapshot().notice == "正在调频",
        "Resetting content must not change the selected header action to daily recommendations");
    MusicViewTestAccess::reset(v);
    m.set_play_callback({});
}
int main(int argc, char **argv) try {
    bool check = false;
    int frame_limit = 0;
    std::string scene = "discover", assets = "build/validation/ui-redesign/assets";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--check")
            check = true;
        else if (std::string(argv[i]) == "--frames" && i + 1 < argc)
            frame_limit = std::stoi(argv[++i]);
        else if (std::string(argv[i]) == "--scene" && i + 1 < argc)
            scene = argv[++i];
        else if (std::string(argv[i]) == "--assets" && i + 1 < argc)
            assets = argv[++i];
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_ZH_HANS;
    if (!brls::Application::init())
        return 1;
    brls::Application::createWindow("CloudTune NX · UI review fixture");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    brls::Application::setLimitedFPS(60);
    char temp[] = "/tmp/cloudtune-ui-test-XXXXXX";
    const auto directory = mkdtemp(temp);
    if (!directory)
        throw std::runtime_error("Cannot create isolated test directory");
    {
        Player player(directory);
        Model model(directory);
        auto *view = new MusicView(model, player);
        brls::Application::pushActivity(new brls::Activity(view));
        if (check)
            controller_checks(*view, model, assets);
        const auto s = fixture(scene, assets);
        ModelTestAccess::set(model, s);
        MusicViewTestAccess::freeze(*view, s,
                                    scene == "player" || scene == "airplay" || scene == "modal");
        if (scene == "airplay")
            MusicViewTestAccess::airplay(*view);
        int frames = 0;
        while (brls::Application::mainLoop()) {
            if (scene == "modal" && frames == 30)
                MusicViewTestAccess::modal(*view);
            if (scene == "memories" && frames == 30)
                MusicViewTestAccess::memories(*view);
            if (check && frames > 12)
                brls::Application::quit();
            if (frame_limit > 0 && frames >= frame_limit)
                brls::Application::quit();
            ++frames;
        }
        if (check)
            require(frames > 12, "Navigation or modal checks must not exit the application");
        model.stop();
        player.shutdown();
    }
    std::filesystem::remove_all(directory);
    curl_global_cleanup();
    if (check)
        std::cout << "Native UI focus, player return, queue and modal checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
