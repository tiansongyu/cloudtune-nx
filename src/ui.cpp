// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/ui.hpp"
#include "netease/storage.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace netease {
namespace {
const NVGcolor bg = nvgRGB(16, 17, 20), panel = nvgRGB(27, 29, 34), raised = nvgRGB(41, 43, 50);
const NVGcolor white = nvgRGB(245, 243, 239), muted = nvgRGB(172, 174, 183),
               red = nvgRGB(255, 75, 87), selected_text = nvgRGB(255, 136, 147),
               selected_bg = nvgRGB(53, 33, 39);
const char *labels[] = {"发现音乐", "排行榜",   "搜索",     "我的音乐",
                        "正在播放", "播放队列", "我的账号", "设置"};
enum SettingRow {
    AccountSetting,
    QualitySetting,
    ProxySetting,
    AirplaySetting,
    StopSetting,
    ExitSetting,
    SettingsCount
};
constexpr int NavOrder[] = {0, 1, 2, 3, 4, 5, 7, 6};
constexpr float NavTop[] = {131, 179, 227, 299, 347, 395, 473, 525};
ui::Rect nav_rect(int index) {
    return {24, NavTop[index], 160, 40};
}
std::string media_key(const Media &m) {
    return std::to_string(static_cast<int>(m.kind)) + ":" + std::to_string(m.id);
}
std::string view_key(const Snapshot &s) {
    return std::to_string(static_cast<int>(s.page)) + ":" +
           (s.collection ? std::to_string(s.collection_id) : "root") + ":" +
           std::to_string(static_cast<int>(s.collection_kind)) + ":" +
           std::to_string(s.search_type) + ":" + s.search_query + ":" + s.title;
}
float ease(float time, float duration) {
    const float t = std::clamp(time / duration, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
int account_rows(const Snapshot &s) {
    return s.user_id ? 3 : 2;
}
std::string quality_name(const std::string &quality) {
    if (quality == "higher")
        return "较高 · 192 kbps";
    if (quality == "exhigh" || quality == "extreme")
        return "极高 · 320 kbps";
    if (quality == "lossless")
        return "无损 · FLAC";
    if (quality == "hires")
        return "Hi-Res · FLAC";
    if (quality == "jyeffect")
        return "高清环绕";
    if (quality == "sky")
        return "沉浸环绕";
    if (quality == "jymaster")
        return "超清母带";
    return "标准 · 128 kbps";
}
const char *airplay_state(const NcmAirplayStatus &status) {
    if (status.version != NCM_AIRPLAY_VERSION)
        return "请更新后台服务";
    switch (status.state) {
    case NcmAirplay_Disabled:
        return "已关闭";
    case NcmAirplay_Starting:
        return "正在启动";
    case NcmAirplay_Ready:
        return "等待连接";
    case NcmAirplay_Connected:
        return "设备已连接";
    case NcmAirplay_Playing:
        return "正在接收音频";
    case NcmAirplay_Error:
        return "服务异常";
    default:
        return "状态未知";
    }
}
template <size_t N> std::string status_text(const char (&value)[N]) {
    const char *end = static_cast<const char *>(std::memchr(value, '\0', N));
    return std::string(value, end ? static_cast<size_t>(end - value) : N);
}
bool airplay_source(const NcmPlaybackStatus &status) {
    // The music ABI remains unchanged. The engine marks every external-source
    // state, including local pause and output failure, with this prefix.
    // Receiver Playing alone cannot tell whether local audio is paused.
    return status.track_id == 0 && status.state != NcmPlayback_Stopped &&
           status_text(status.message).rfind("AirPlay ", 0) == 0;
}
void rect(NVGcontext *v, float x, float y, float w, float h, NVGcolor color, float r = 0) {
    nvgBeginPath(v);
    if (r)
        nvgRoundedRect(v, x, y, w, h, r);
    else
        nvgRect(v, x, y, w, h);
    nvgFillColor(v, color);
    nvgFill(v);
}
void circle(NVGcontext *v, float x, float y, float r, NVGcolor color) {
    nvgBeginPath(v);
    nvgCircle(v, x, y, r);
    nvgFillColor(v, color);
    nvgFill(v);
}
void line(NVGcontext *v, float x, float y, float x2, float y2, NVGcolor color, float width = 1) {
    nvgBeginPath(v);
    nvgMoveTo(v, x, y);
    nvgLineTo(v, x2, y2);
    nvgStrokeColor(v, color);
    nvgStrokeWidth(v, width);
    nvgStroke(v);
}
void play_icon(NVGcontext *v, float x, float y, bool paused, float size = 15,
               NVGcolor color = white) {
    if (!paused) {
        rect(v, x - size / 2, y - size / 2, 4, size, color, 1);
        rect(v, x + size / 2 - 4, y - size / 2, 4, size, color, 1);
    } else {
        nvgBeginPath(v);
        nvgMoveTo(v, x - size / 3, y - size / 2);
        nvgLineTo(v, x + size / 2, y);
        nvgLineTo(v, x - size / 3, y + size / 2);
        nvgClosePath(v);
        nvgFillColor(v, color);
        nvgFill(v);
    }
}
bool grid(const Snapshot &s) {
    return !s.items.empty() && s.items.front().kind != MediaKind::Song;
}
void skip_icon(NVGcontext *v, float x, float y, bool next) {
    nvgSave(v);
    nvgTranslate(v, x, y);
    if (!next)
        nvgScale(v, -1, 1);
    play_icon(v, -2, 0, true, 16);
    rect(v, 8, -8, 2, 16, white, 1);
    nvgRestore(v);
}
void nav_icon(NVGcontext *v, int page, float x, float y, NVGcolor color) {
    nvgSave(v);
    nvgTranslate(v, x, y);
    nvgStrokeColor(v, color);
    nvgStrokeWidth(v, 1.6f);
    nvgBeginPath(v);
    switch (page) {
    case 0:
        nvgCircle(v, 0, 0, 8);
        nvgStroke(v);
        line(v, -3, 3, 3, -3, color, 2);
        break;
    case 1:
        for (int i = 0; i < 3; ++i)
            rect(v, -8 + i * 6, 6 - (i + 1) * 5, 3, (i + 1) * 5, color, 1);
        break;
    case 2:
        nvgCircle(v, -2, -2, 6);
        nvgStroke(v);
        line(v, 3, 3, 9, 9, color, 2);
        break;
    case 3:
        nvgRoundedRect(v, -8, -8, 11, 16, 2);
        nvgStroke(v);
        line(v, 7, -7, 10, 7, color, 2);
        break;
    case 4:
        nvgCircle(v, 0, 0, 9);
        nvgStroke(v);
        circle(v, 0, 0, 3, color);
        break;
    case 5:
        for (int i = 0; i < 3; ++i)
            line(v, -8, -6 + i * 6, 6, -6 + i * 6, color, 1.6f);
        break;
    case 6:
        nvgCircle(v, 0, -4, 4);
        nvgStroke(v);
        nvgBeginPath(v);
        nvgArc(v, 0, 9, 7, 3.14159f, 6.28318f, NVG_CW);
        nvgStroke(v);
        break;
    default:
        nvgCircle(v, 0, 0, 8);
        nvgStroke(v);
        nvgBeginPath(v);
        nvgCircle(v, 0, 0, 3);
        nvgStroke(v);
    }
    nvgRestore(v);
}
class MusicDropdown : public brls::Dropdown {
    bool reduced_;

  public:
    MusicDropdown(const std::string &title, const std::vector<std::string> &values, int selected,
                  std::function<void(int)> dismiss, bool reduced)
        : Dropdown(title, values, [](int) {}, selected, std::move(dismiss)), reduced_(reduced) {
        // Keep Borealis' recycler, modal input trap and dismissal ordering.
        auto *content = dynamic_cast<brls::Box *>(getView("brls/dropdown/content"));
        auto *header = dynamic_cast<brls::Box *>(getView("brls/dropdown/header"));
        auto *recycler = dynamic_cast<brls::RecyclerFrame *>(getView("brls/dropdown/recycler"));
        content->getParent()->setJustifyContent(brls::JustifyContent::CENTER);
        content->getParent()->setAlignItems(brls::AlignItems::CENTER);
        content->setWidth(720);
        content->setHeight(std::min(572.f, 180.f + values.size() * 60.f));
        content->setCornerRadius(17);
        header->setHeight(68);
        header->setMargins(0, 0, 0, 0);
        header->setPadding(16, 28, 16, 28);
        header->setLineColor(raised);
        recycler->setPadding(12, 28, 12, 28);
        for (auto *child : getChildren())
            if (dynamic_cast<brls::BottomBar *>(child))
                child->setVisibility(brls::Visibility::GONE);
        auto *hint = new brls::Label();
        hint->setText("B 返回    A 选择");
        hint->setFontSize(14);
        hint->setTextColor(muted);
        hint->setHeight(44);
        hint->setMarginLeft(28);
        content->addView(hint);
    }
    void show(std::function<void()> cb, bool animate, float) override {
        Dropdown::show(std::move(cb), animate && !reduced_, 220);
    }
    void hide(std::function<void()> cb, bool animate, float) override {
        Dropdown::hide(std::move(cb), animate && !reduced_, 160);
    }
};
class MusicDialog : public brls::Dialog {
    bool reduced_;

  public:
    MusicDialog(const std::string &message, bool reduced) : Dialog(message), reduced_(reduced) {}
    void show(std::function<void()> cb, bool animate, float) override {
        Dialog::show(std::move(cb), animate && !reduced_, 220);
    }
    void hide(std::function<void()> cb, bool animate, float) override {
        Dialog::hide(std::move(cb), animate && !reduced_, 160);
    }
};
} // namespace
MusicView::MusicView(Model &m, Player &p) : model_(m), player_(p) {
    setFocusable(true);
    // Focus is painted at the selected card/row, never around the entire canvas.
    setHideHighlight(true);
    setHideClickAnimation(true);
    try {
        const auto prefs = Json::parse(storage::read_file(model_.directory() + "/ui.json"));
        reduced_motion_ = prefs.value("reduced_motion", false);
        photo_background_ = prefs.value("photo_background", true);
        safe_scale_ = std::clamp(prefs.value("safe_scale", 1.f), .9f, 1.f);
    } catch (...) {
        // Optional visual preferences never prevent starting the player.
    }
    auto &theme = brls::Theme::getDarkTheme();
    theme.addColor("brls/background", bg);
    theme.addColor("brls/text", white);
    theme.addColor("brls/highlight/color1", white);
    theme.addColor("brls/highlight/color2", white);
    theme.addColor("brls/accent", selected_text);
    theme.addColor("brls/list/listItem_value_color", selected_text);
    theme.addColor("brls/button/primary_enabled_background", red);
    theme.addColor("brls/button/primary_enabled_text", bg);
    theme.addColor("brls/button/highlight_enabled_text", selected_text);
    last_frame_ = last_poll_ = last_qr_ = std::chrono::steady_clock::now();
    last_poll_ -= std::chrono::seconds(1);
    last_airplay_poll_ = last_poll_;
    auto bind = [this](brls::ControllerButton b, std::function<void()> f, bool repeat = false) {
        registerAction(
            "", b,
            [f](brls::View *) {
                f();
                return true;
            },
            true, repeat);
    };
    bind(brls::BUTTON_A, [this] { activate(); });
    bind(brls::BUTTON_B, [this] { back(); });
    bind(brls::BUTTON_X, [this] { action_x(); });
    bind(brls::BUTTON_Y, [this] { player_.command(NcmCommand_PlayPause); });
    bind(brls::BUTTON_NAV_UP, [this] { navigate(-1); }, true);
    bind(brls::BUTTON_NAV_DOWN, [this] { navigate(1); }, true);
    bind(brls::BUTTON_NAV_LEFT, [this] { navigate(-2); }, true);
    bind(brls::BUTTON_NAV_RIGHT, [this] { navigate(2); }, true);
    bind(brls::BUTTON_LB, [this] { player_.command(NcmCommand_Previous); });
    bind(brls::BUTTON_RB, [this] { player_.command(NcmCommand_Next); });
    bind(
        brls::BUTTON_LT,
        [this] {
            player_.command(NcmCommand_Volume,
                            playback_.volume_permille > 50 ? playback_.volume_permille - 50 : 0);
        },
        true);
    bind(
        brls::BUTTON_RT,
        [this] {
            player_.command(NcmCommand_Volume, std::min(1000u, playback_.volume_permille + 50));
        },
        true);
    bind(brls::BUTTON_START, [this] { switch_page(Page::Player); });
    bind(brls::BUTTON_BACK, [this] { confirm_exit(); });
    registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_SPACE), [this](brls::View *) {
        player_.command(NcmCommand_PlayPause);
        return true;
    });
    registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_X), [this](brls::View *) {
        action_x();
        return true;
    });
    registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_P), [this](brls::View *) {
        switch_page(Page::Player);
        return true;
    });
    registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_TAB), [this](brls::View *) {
        if (player_open_)
            back();
        else
            region_ = region_ == ui::Region::Sidebar ? ui::Region::Content : ui::Region::Sidebar;
        return true;
    });
    registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F), [this](brls::View *) {
        input_search();
        return true;
    });
    addGestureRecognizer(
        new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound *) {
            if (status.state == brls::GestureState::END) {
                auto r = getFrame();
                const auto viewport = ui::Viewport::fit(r.getWidth(), r.getHeight(), safe_scale_);
                tap(viewport.logical_x(status.position.x - r.getMinX()),
                    viewport.logical_y(status.position.y - r.getMinY()));
            }
        }));
    addGestureRecognizer(new brls::ScrollGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound *) {
            if (status.state == brls::GestureState::START)
                pan_distance_ = 0;
            const auto frame = getFrame();
            const auto viewport =
                ui::Viewport::fit(frame.getWidth(), frame.getHeight(), safe_scale_);
            const float x = viewport.logical_x(status.startPosition.x - frame.getMinX());
            const float y = viewport.logical_y(status.startPosition.y - frame.getMinY());
            if (player_open_ ||
                (!status.deltaOnly && (x < ui::MainLeft || y < content_top(display_) || y > 567)))
                return;
            pan_distance_ += (status.deltaOnly ? -status.delta.y : status.delta.y) / viewport.scale;
            const float threshold = grid(display_) ? 110.f : 45.f;
            while (std::abs(pan_distance_) >= threshold) {
                const int direction = pan_distance_ > 0 ? 1 : -1;
                scroll_rows(direction);
                pan_distance_ -= direction * threshold;
            }
        },
        brls::PanAxis::VERTICAL));
}
MusicView::~MusicView() {
    auto vg = brls::Application::getNVGContext();
    if (vg && app_icon_ > 0)
        nvgDeleteImage(vg, app_icon_);
    if (vg)
        for (auto &[path, id] : textures_)
            if (id > 0)
                nvgDeleteImage(vg, id);
}
void MusicView::text(NVGcontext *v, float x, float y, float size, NVGcolor color,
                     const std::string &s, float w) {
    nvgSave(v);
    nvgIntersectScissor(v, x, y - size, w, size * 1.7f);
    nvgFontFaceId(v, brls::Application::getFont(brls::FONT_CHINESE_SIMPLIFIED));
    nvgFontSize(v, size);
    nvgTextAlign(v, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    nvgFillColor(v, color);
    std::string display = s;
    if (nvgTextBounds(v, 0, 0, display.c_str(), nullptr, nullptr) > w) {
        std::vector<size_t> boundaries{0};
        for (size_t i = 0; i < s.size(); ++i)
            if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80 && i)
                boundaries.push_back(i);
        boundaries.push_back(s.size());
        size_t lo = 0, hi = boundaries.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            auto trial = s.substr(0, boundaries[mid]) + "…";
            if (nvgTextBounds(v, 0, 0, trial.c_str(), nullptr, nullptr) <= w)
                lo = mid;
            else
                hi = mid - 1;
        }
        display = s.substr(0, boundaries[lo]) + "…";
    }
    nvgText(v, x, y, display.c_str(), nullptr);
    nvgRestore(v);
}
void MusicView::button(NVGcontext *v, float x, float y, float w, const std::string &title,
                       bool selected) {
    rect(v, x, y, w, 44, selected ? selected_bg : raised, 10);
    text(v, x + 16, y + 29, 16, selected ? selected_text : white, title, w - 32);
}
void MusicView::options(const std::string &title, const std::vector<std::string> &values,
                        std::function<void(int)> f, int selected) {
    brls::Application::pushActivity(new brls::Activity(new MusicDropdown(
        title, values, selected,
        [f](int i) {
            // Open child dialogs only after the dropdown has been dismissed.
            // Otherwise its popActivity() would immediately pop the child.
            if (i >= 0)
                f(i);
        },
        reduced_motion_)));
}
void MusicView::switch_page(Page p) {
    const auto s = model_.snapshot();
    if (p == Page::Player) {
        player_open_ = !player_open_;
        page_time_ = 0;
        if (player_open_ && s.page == Page::Account)
            model_.cancel_qr();
        return;
    }
    const bool leaving_account = s.page == Page::Account && p != Page::Account;
    if (leaving_account)
        model_.cancel_qr();
    if (s.busy && !leaving_account)
        return;
    remember(s);
    player_open_ = false;
    nav_ =
        static_cast<int>(std::find(std::begin(NavOrder), std::end(NavOrder), static_cast<int>(p)) -
                         std::begin(NavOrder));
    region_ = ui::Region::Content;
    if (s.page == p && !s.collection)
        return;
    reset_content();
    model_.browse(p);
    page_time_ = 0;
}
void MusicView::back() {
    if (player_open_) {
        player_open_ = false;
        page_time_ = 0;
        return;
    }
    auto s = model_.snapshot();
    if (s.page == Page::Account &&
        (!s.qr_key.empty() || !s.qr_url.empty() || !s.qr_message.empty())) {
        model_.cancel_qr();
    } else if (s.collection) {
        remember(s);
        model_.back();
        reconcile(model_.snapshot());
        page_time_ = 0;
    } else {
        region_ = region_ == ui::Region::Sidebar ? ui::Region::Content : ui::Region::Sidebar;
    }
}
void MusicView::confirm_exit() {
    options("退出 CloudTune NX",
            {"继续听歌",
             player_.background() ? "退出应用（后台服务保持运行）" : "退出应用（停止前台播放）"},
            [](int i) {
                if (i == 1)
                    brls::Application::quit();
            });
}
void MusicView::save_preferences() {
    try {
        storage::write_file(model_.directory() + "/ui.json",
                            Json{{"reduced_motion", reduced_motion_},
                                 {"safe_scale", safe_scale_},
                                 {"photo_background", photo_background_}}
                                .dump(2));
        model_.report("界面偏好已保存");
    } catch (...) {
        model_.report("界面偏好保存失败，本次仍然生效");
    }
}
void MusicView::display_options() {
    std::vector<std::string> values{
        reduced_motion_ ? "减少动态效果：已开启" : "减少动态效果：已关闭", "画面范围：100%",
        "画面范围：95%", "画面范围：90%", "关于 CloudTune NX"};
    if (memories_.available()) {
        values.push_back(photo_background_ ? "照片氛围背景：已开启" : "照片氛围背景：已关闭");
        values.push_back("查看我们的时光");
    }
    options("界面与动态效果", values, [this](int i) {
        if (i == 6) {
            open_memories();
            return;
        }
        if (i == 5) {
            photo_background_ = !photo_background_;
            save_preferences();
            return;
        }
        if (i == 4) {
            auto *dialog =
                new MusicDialog("CloudTune NX " CLOUDTUNE_VERSION
                                "\nBorealis / libnx · GPL-3.0-or-later\nMP3 / AAC / FLAC 自动识别"
                                "\n游戏内播放需要后台服务与 Tesla / Ultrahand 覆盖层。",
                                reduced_motion_);
            dialog->addButton("返回", [] {});
            dialog->open();
            return;
        }
        if (i == 0)
            reduced_motion_ = !reduced_motion_;
        else
            safe_scale_ = 1.f - (i - 1) * .05f;
        save_preferences();
    });
}
void MusicView::open_memories() {
    memories_.open(reduced_motion_, safe_scale_, [this] { player_.command(NcmCommand_PlayPause); });
}
int MusicView::item_count(const Snapshot &s) const {
    if (s.page == Page::Settings)
        return SettingsCount;
    if (s.page == Page::Account)
        return account_rows(s);
    return std::max(1, static_cast<int>(s.items.size()) + (s.more ? 1 : 0));
}
int MusicView::visible_rows(const Snapshot &s) const {
    if (s.page == Page::Settings)
        return SettingsCount;
    if (s.page == Page::Account)
        return account_rows(s);
    return grid(s) ? 1 : ui::list_rows(content_top(s));
}
float MusicView::content_top(const Snapshot &s) const {
    if (s.collection)
        return 270;
    // Daily recommendations and FM replace cards with songs, but retain the
    // discovery banner and section heading. Both content types must clear them.
    if (s.page == Page::Discover)
        return 292;
    return grid(s) ? 224 : 210;
}
void MusicView::reset_content() {
    selection_ = scroll_ = header_ = 0;
    anchor_.clear();
    view_key_.clear();
    region_ = ui::Region::Content;
}
void MusicView::remember(const Snapshot &s) {
    if (s.busy)
        return;
    Bookmark b{selection_, scroll_, header_, region_, ""};
    if (selection_ >= 0 && selection_ < static_cast<int>(s.items.size()))
        b.item = media_key(s.items[selection_]);
    // Restoring a content page always returns to content, not its old sidebar.
    if (b.region == ui::Region::Sidebar)
        b.region = ui::Region::Content;
    if (bookmarks_.size() >= 64 && !bookmarks_.count(view_key(s)))
        bookmarks_.erase(bookmarks_.begin());
    bookmarks_[view_key(s)] = std::move(b);
}
void MusicView::reconcile(const Snapshot &s) {
    if (account_id_ != s.user_id) {
        account_id_ = s.user_id;
        bookmarks_.clear();
        view_key_.clear();
        anchor_.clear();
        selection_ = scroll_ = 0;
    }
    if (s.busy)
        return;
    const auto key = view_key(s);
    if (view_key_ != key) {
        const auto saved = bookmarks_.find(key);
        selection_ = scroll_ = header_ = 0;
        anchor_.clear();
        if (saved != bookmarks_.end()) {
            selection_ = saved->second.selection;
            scroll_ = saved->second.scroll;
            header_ = saved->second.header;
            anchor_ = saved->second.item;
            if (region_ != ui::Region::Sidebar)
                region_ = saved->second.region;
        } else if (s.page == Page::Queue) {
            for (size_t i = 0; i < s.items.size(); ++i)
                if (s.items[i].id == playback_.track_id)
                    selection_ = static_cast<int>(i);
        }
        page_time_ = 0;
        view_key_ = key;
    }
    if (!anchor_.empty()) {
        auto it = std::find_if(s.items.begin(), s.items.end(),
                               [this](const Media &m) { return media_key(m) == anchor_; });
        if (it != s.items.end())
            selection_ = static_cast<int>(it - s.items.begin());
    }
    selection_ = std::clamp(selection_, 0, item_count(s) - 1);
    const auto result =
        ui::move(selection_, scroll_, item_count(s), grid(s) ? 4 : 1, visible_rows(s), 0);
    scroll_ = result.scroll;
    header_ = std::clamp(header_, 0, std::max(0, static_cast<int>(headers(s).size()) - 1));
    anchor_ = selection_ < static_cast<int>(s.items.size()) ? media_key(s.items[selection_]) : "";
}
void MusicView::navigate(int d) {
    auto s = model_.snapshot();
    if (player_open_) {
        if (d == -2 || d == 2) {
            if (!airplay_source(playback_) && playback_.duration_ms) {
                const auto t =
                    static_cast<int64_t>(playback_.position_ms) + (d > 0 ? 10000 : -10000);
                player_.command(NcmCommand_Seek, std::clamp<int64_t>(t, 0, playback_.duration_ms));
            }
        } else
            player_.command(
                NcmCommand_Volume,
                std::clamp(static_cast<int>(playback_.volume_permille) - d * 50, 0, 1000));
        return;
    }
    if (region_ == ui::Region::Sidebar) {
        if (d == -1 || d == 1)
            nav_ = std::clamp(nav_ + d, 0, 7);
        if (d == 2) {
            // Previewing another sidebar item does not navigate until A is pressed.
            region_ = ui::Region::Content;
        }
        return;
    }
    if (region_ == ui::Region::Playback) {
        if (d == -1)
            region_ = ui::Region::Content;
        if (d == -2)
            transport_ = std::max(0, transport_ - 1);
        if (d == 2)
            transport_ = std::min(5, transport_ + 1);
        return;
    }
    const auto controls = headers(s);
    if (region_ == ui::Region::Header) {
        if (d == 1)
            region_ = ui::Region::Content;
        else if (d == -2 && header_ == 0)
            region_ = ui::Region::Sidebar;
        else if (d == -2)
            --header_;
        else if (d == 2)
            header_ = std::min(header_ + 1, std::max(0, static_cast<int>(controls.size()) - 1));
        return;
    }
    auto result = ui::move(selection_, scroll_, item_count(s), grid(s) ? 4 : 1, visible_rows(s), d);
    selection_ = result.index;
    scroll_ = result.scroll;
    if (result.edge == ui::Edge::Left)
        region_ = ui::Region::Sidebar;
    if (result.edge == ui::Edge::Above && !controls.empty())
        region_ = ui::Region::Header;
    if (result.edge == ui::Edge::Below) {
        region_ = ui::Region::Playback;
        transport_ = 2;
    }
    anchor_ = selection_ < static_cast<int>(s.items.size()) ? media_key(s.items[selection_]) : "";
}
void MusicView::scroll_rows(int rows) {
    const auto s = model_.snapshot();
    if (s.page == Page::Account || s.page == Page::Settings || player_open_)
        return;
    const int columns = grid(s) ? 4 : 1;
    const int visible = visible_rows(s);
    const int count = static_cast<int>(s.items.size()) + (s.more ? 1 : 0);
    const int total_rows = (count + columns - 1) / columns;
    scroll_ = std::clamp(scroll_ + rows, 0, std::max(0, total_rows - visible));
    selection_ = std::clamp(
        selection_, scroll_ * columns,
        std::max(scroll_ * columns, std::min(count - 1, (scroll_ + visible) * columns - 1)));
    region_ = ui::Region::Content;
    anchor_ = selection_ < static_cast<int>(s.items.size()) ? media_key(s.items[selection_]) : "";
}
void MusicView::input_search() {
    if (model_.snapshot().busy)
        return;
    brls::Application::getImeManager()->openForText(
        [this](std::string s) {
            if (!s.empty()) {
                remember(model_.snapshot());
                reset_content();
                player_open_ = false;
                nav_ = 2;
                model_.search(s);
            }
        },
        "搜索网易云音乐", "歌曲、歌单、专辑、歌手", 80, model_.snapshot().search_query);
}
void MusicView::account_tools() {
    const auto s = model_.snapshot();
    std::vector<std::string> names;
    int selected = 0;
    for (const auto &account : s.accounts) {
        if (account.user_id == s.user_id)
            selected = static_cast<int>(names.size());
        names.push_back((account.user_id == s.user_id ? "当前 · " : "") + account.nickname +
                        "  (ID " + std::to_string(account.user_id) + ")");
    }
    names.push_back("＋ 扫码添加账号");
    options(
        "账号管理 · 已保存 " + std::to_string(s.accounts.size()) + " 个账号", names,
        [this, accounts = s.accounts](int i) {
            if (i < static_cast<int>(accounts.size())) {
                account_actions(accounts[i].user_id);
                return;
            }
            switch_page(Page::Account);
            model_.login_qr();
        },
        selected);
}
void MusicView::account_actions(uint64_t user_id) {
    const auto s = model_.snapshot();
    const auto account = std::find_if(s.accounts.begin(), s.accounts.end(),
                                      [user_id](const auto &a) { return a.user_id == user_id; });
    if (account == s.accounts.end())
        return;
    const auto nickname = account->nickname;
    auto *dialog = new MusicDialog(
        nickname + "\nID " + std::to_string(user_id) +
            (s.user_id == user_id ? "\n当前正在使用此账号" : "\n切换后使用该账号的音乐收藏"),
        reduced_motion_);
    if (s.user_id != user_id)
        dialog->addButton("切换账号", [this, user_id] {
            model_.switch_account(user_id);
            selection_ = scroll_ = 0;
        });
    dialog->addButton("移除此账号", [this, user_id, nickname] {
        auto *confirm = new MusicDialog("从此设备移除「" + nickname +
                                            "」的登录信息？\n再次使用时需要重新扫码登录。",
                                        reduced_motion_);
        confirm->addButton("取消", [] {});
        confirm->addButton("确认移除", [this, user_id] {
            model_.remove_account(user_id);
            selection_ = scroll_ = 0;
        });
        confirm->open();
    });
    dialog->addButton("返回", [] {});
    dialog->open();
}
void MusicView::activate() {
    press_time_ = 0;
    if (player_open_) {
        player_.command(NcmCommand_PlayPause);
        return;
    }
    if (region_ == ui::Region::Sidebar) {
        switch_page(static_cast<Page>(NavOrder[nav_]));
        return;
    }
    if (region_ == ui::Region::Playback) {
        activate_transport(transport_);
        return;
    }
    auto s = model_.snapshot();
    // Local photos remain available while music recommendations are loading.
    if (region_ == ui::Region::Header && s.page == Page::Discover && !s.collection &&
        memories_.available() && header_ == 1) {
        open_memories();
        return;
    }
    if (s.busy)
        return;
    reconcile(s);
    if (region_ == ui::Region::Header) {
        activate_header(s);
        return;
    }
    if (s.page == Page::Account) {
        if (selection_ == 0)
            model_.login_qr();
        if (selection_ == 1)
            account_tools();
        if (selection_ == 2 && s.user_id)
            model_.check_in();
        return;
    }
    if (s.page == Page::Settings) {
        if (selection_ == AccountSetting)
            account_tools();
        if (selection_ == QualitySetting) {
            const std::vector<std::string> levels{"standard", "higher",   "exhigh", "lossless",
                                                  "hires",    "jyeffect", "sky",    "jymaster"};
            const auto current = std::find(levels.begin(), levels.end(),
                                           s.quality == "extreme" ? "exhigh" : s.quality);
            options(
                "播放音质 · 取决于账号与曲目权益",
                {"标准 · 128 kbps", "较高 · 192 kbps", "极高 · 320 kbps", "无损 · FLAC",
                 "Hi-Res · FLAC", "高清环绕", "沉浸环绕", "超清母带"},
                [this, levels](int i) { model_.set_quality(levels[i]); },
                current == levels.end() ? 0 : static_cast<int>(current - levels.begin()));
        }
        if (selection_ == ProxySetting)
            brls::Application::getImeManager()->openForText(
                [this](std::string proxy) { model_.set_proxy(proxy); }, "HTTP / SOCKS5 代理",
                "留空使用直连", 200, s.proxy);
        if (selection_ == AirplaySetting)
            airplay_tools();
        if (selection_ == StopSetting)
            options("播放控制", {"取消", "停止播放并清空当前音频"}, [this](int i) {
                if (i == 1)
                    player_.command(NcmCommand_Stop);
            });
        if (selection_ == ExitSetting)
            confirm_exit();
        return;
    }
    if (s.items.empty()) {
        if (s.page == Page::Search)
            input_search();
        else if (s.page == Page::Library && !s.user_id)
            switch_page(Page::Account);
        else
            model_.refresh();
        return;
    }
    if (selection_ == static_cast<int>(s.items.size()) && s.more) {
        model_.load_more();
        return;
    }
    if (selection_ < 0 || selection_ >= static_cast<int>(s.items.size()))
        return;
    if (s.items[selection_].kind == MediaKind::Song) {
        if (s.page == Page::Queue)
            model_.play_queue(selection_);
        else
            model_.play(selection_);
    } else {
        remember(s);
        collection_cover_ = s.items[selection_].cover;
        model_.open(s.items[selection_]);
        reset_content();
    }
}
void MusicView::airplay_tools() {
    airplay_ = player_.airplay_status();
    std::string description = "AirPlay 1 · 独立工具\n";
    description += "状态：" + std::string(airplay_state(airplay_));
    if (airplay_.version == NCM_AIRPLAY_VERSION) {
        auto name = status_text(airplay_.name);
        auto address = status_text(airplay_.address);
        description += "\n设备：" + (name.empty() ? std::string("尚未就绪") : name);
        description += "\n地址：" + (address.empty() ? std::string("尚未获取局域网 IP") : address);
        if (!address.empty() && airplay_.port)
            description += ":" + std::to_string(airplay_.port);
        if (airplay_.state == NcmAirplay_Error && airplay_.message[0])
            description += "\n" + status_text(airplay_.message);
        description += player_.background() ? "\n退出应用后，后台接收保持运行"
                                            : "\n退出应用后，本地接收会停止";
    } else {
        description += "\n音乐播放仍可使用；更新后台服务后再开启接收。";
    }
    auto *dialog = new MusicDialog(description, reduced_motion_);
    if (airplay_.version == NCM_AIRPLAY_VERSION) {
        const bool enable = !airplay_.enabled;
        dialog->addButton(enable ? "开启接收" : "关闭接收", [this, enable] {
            if (!player_.set_airplay_enabled(enable))
                model_.report(player_.error());
            else
                model_.report(enable ? "AirPlay 开启偏好已保存，请查看实际接收状态"
                                     : "AirPlay 已关闭，下次启动保持关闭");
            airplay_ = player_.airplay_status();
        });
        dialog->addButton("断开当前会话", [this] {
            if (!player_.disconnect_airplay())
                model_.report(player_.error());
            else
                model_.report("当前 AirPlay 会话已断开，接收开关保持原设置");
            airplay_ = player_.airplay_status();
        });
    }
    dialog->addButton("返回", [] {});
    dialog->open();
}
void MusicView::action_x() {
    auto s = model_.snapshot();
    if (player_open_) {
        s.page = Page::Player;
        s.collection = false;
    }
    if ((s.page == Page::Player || s.page == Page::Queue) && airplay_source(playback_)) {
        airplay_tools();
        return;
    }
    if (s.busy && !player_open_)
        return;
    if (s.page == Page::Account) {
        account_tools();
        return;
    }
    if (s.page == Page::Settings) {
        display_options();
        return;
    }
    if (s.collection) {
        options("收藏与播放",
                {"收藏 / 取消收藏此歌单或专辑", "喜欢 / 取消喜欢选中歌曲", "心动模式", "刷新"},
                [this, s](int i) {
                    if (i == 0)
                        model_.subscribe_collection();
                    if (i == 1 && selection_ < static_cast<int>(s.items.size()))
                        model_.like(s.items[selection_].id);
                    if (i == 2)
                        model_.intelligence(playback_.track_id);
                    if (i == 3)
                        model_.refresh();
                });
        return;
    }
    if (s.page == Page::Discover) {
        options("发现更多",
                {"热门歌单", "精品歌单", "每日推荐歌曲", "私人 FM", "新碟上架", "华语", "电子",
                 "轻音乐", "ACG", "游戏"},
                [this](int i) {
                    reset_content();
                    if (i == 0)
                        model_.category("全部");
                    if (i == 1)
                        model_.category("全部", true);
                    if (i == 2)
                        model_.daily();
                    if (i == 3)
                        model_.fm();
                    if (i == 4)
                        model_.new_albums();
                    if (i >= 5)
                        model_.category(std::vector<std::string>{"华语", "电子", "轻音乐", "ACG",
                                                                 "游戏"}[i - 5]);
                });
        return;
    }
    if (s.page == Page::Library) {
        options("我的音乐", {"我的歌单", "我喜欢的音乐", "收藏专辑", "订阅电台", "音乐云盘"},
                [this](int i) {
                    reset_content();
                    model_.library_section(i);
                });
        return;
    }
    if (s.page == Page::Search) {
        options("搜索与操作",
                {"输入关键词", "单曲", "歌单", "专辑", "歌手", "歌词", "喜欢选中歌曲", "刷新"},
                [this, s](int i) {
                    if (i == 0)
                        input_search();
                    else if (i <= 5) {
                        reset_content();
                        model_.search(s.search_query,
                                      std::vector<int>{1, 1000, 10, 100, 1006}[i - 1]);
                    } else if (i == 6 && selection_ < static_cast<int>(s.items.size()) &&
                               s.items[selection_].kind == MediaKind::Song)
                        model_.like(s.items[selection_].id);
                    else if (i == 7)
                        model_.refresh();
                });
        return;
    }
    if (s.page == Page::Player || s.page == Page::Queue) {
        options("播放选项",
                {"喜欢 / 取消喜欢当前歌曲", "随机播放 开/关", "顺序播放", "列表循环", "单曲循环",
                 "私人 FM 不喜欢", "停止播放", "查看播放队列"},
                [this](int i) {
                    // The source may change while this menu is open.
                    if (airplay_source(playback_)) {
                        model_.report("AirPlay 曲目由发送端控制；X 打开接收设置");
                        return;
                    }
                    if (i == 0)
                        model_.like(playback_.track_id);
                    if (i == 1)
                        player_.command(NcmCommand_Shuffle, !playback_.shuffle);
                    if (i == 2)
                        player_.command(NcmCommand_Repeat, NcmRepeat_Off);
                    if (i == 3)
                        player_.command(NcmCommand_Repeat, NcmRepeat_All);
                    if (i == 4)
                        player_.command(NcmCommand_Repeat, NcmRepeat_One);
                    if (i == 5) {
                        model_.fm_dislike(playback_.track_id);
                        player_.command(NcmCommand_Next);
                    }
                    if (i == 6)
                        player_.command(NcmCommand_Stop);
                    if (i == 7)
                        switch_page(Page::Queue);
                });
        return;
    }
    model_.refresh();
}
std::vector<std::pair<ui::Rect, std::string>> MusicView::headers(const Snapshot &s) const {
    if (s.collection)
        return {{{420, 178, 156, 44}, "播放全部"},
                {{588, 178, 156, 44}, s.subscribed ? "已收藏" : "收藏"},
                {{756, 178, 140, 44}, "更多选项"}};
    if (s.page == Page::Discover && memories_.available())
        return {{{248, 112, 416, 112}, "开启每日推荐"},
                {{680, 112, 560, 112}, "我们的时光"},
                {{946, 236, 90, 44}, "私人 FM"},
                {{1040, 236, 90, 44}, "新碟上架"},
                {{1134, 236, 106, 44}, "发现更多"}};
    if (s.page == Page::Discover)
        return {{{248, 112, 992, 112}, "开启每日推荐"},
                {{946, 236, 90, 44}, "私人 FM"},
                {{1040, 236, 90, 44}, "新碟上架"},
                {{1134, 236, 106, 44}, "发现更多"}};
    if (s.page == Page::Library)
        return {{{248, 112, 166, 44}, "我的歌单"},
                {{428, 112, 192, 44}, "我喜欢的音乐"},
                {{634, 112, 166, 44}, "收藏专辑"},
                {{814, 112, 166, 44}, "订阅电台"},
                {{994, 112, 166, 44}, "音乐云盘"}};
    if (s.page == Page::Search)
        return {{{248, 112, 650, 48},
                 s.search_query.empty() ? "搜索歌曲、歌单、歌手…" : s.search_query},
                {{914, 112, 154, 44}, "分类与操作"},
                {{1084, 112, 156, 44}, "重新加载"}};
    if (s.page == Page::Charts || s.page == Page::Queue)
        return {{{1084, 112, 156, 44}, s.page == Page::Queue ? "播放选项" : "重新加载"}};
    return {};
}
void MusicView::activate_header(const Snapshot &s) {
    if (s.page == Page::Discover && !s.collection && memories_.available() && header_ == 1) {
        open_memories();
        return;
    }
    if (s.busy)
        return;
    if (s.collection) {
        if (header_ == 0 && !s.items.empty())
            model_.play(0);
        if (header_ == 1)
            model_.subscribe_collection();
        if (header_ == 2)
            action_x();
    } else if (s.page == Page::Discover) {
        const int action = header_ - (memories_.available() && header_ > 1 ? 1 : 0);
        if (action == 3) {
            action_x();
            return;
        }
        reset_content();
        if (action == 0)
            model_.daily();
        else if (action == 1)
            model_.fm();
        else
            model_.new_albums();
    } else if (s.page == Page::Library) {
        const int section = header_;
        reset_content();
        model_.library_section(section);
    } else if (s.page == Page::Search) {
        if (header_ == 0)
            input_search();
        else if (header_ == 1)
            action_x();
        else
            model_.refresh();
    } else if (s.page == Page::Queue)
        action_x();
    else
        model_.refresh();
}
void MusicView::activate_transport(int index) {
    press_time_ = 0;
    if (index == 0 && !player_open_) {
        switch_page(Page::Player);
        return;
    }
    if (index == 1)
        player_.command(NcmCommand_Previous);
    if (index == 2)
        player_.command(NcmCommand_PlayPause);
    if (index == 3)
        player_.command(NcmCommand_Next);
    if (index == 4) {
        if (player_open_) {
            if (!airplay_source(playback_) && playback_.track_id)
                model_.like(playback_.track_id);
        } else
            switch_page(Page::Queue);
    }
    if (index == 5 && player_open_)
        switch_page(Page::Queue);
    else if ((index == 5 && !player_open_) || (index == 0 && player_open_)) {
        options(
            "音量", {"静音", "25%", "50%", "75%", "100%"},
            [this](int i) { player_.command(NcmCommand_Volume, i * 250); },
            std::clamp(static_cast<int>((playback_.volume_permille + 125) / 250), 0, 4));
    }
}
void MusicView::tap(float x, float y) {
    if (x < 0 || x >= ui::Width || y < 0 || y >= ui::Height)
        return;
    // Visible and interactive bounds share ui_layout.hpp, including safe-area scaling.
    if ((player_open_ && y >= 566 && y < 641) ||
        (!player_open_ && y >= ui::MiniTop && y < ui::HintsTop)) {
        for (int i = 0; i < 6; ++i)
            if (ui::transport(i, player_open_).contains(x, y)) {
                transport_ = i;
                if (!player_open_)
                    region_ = ui::Region::Playback;
                activate_transport(i);
                return;
            }
        const ui::Rect progress =
            player_open_ ? ui::Rect{540, 580, 550, 44} : ui::Rect{620, 612, 344, 44};
        if (progress.contains(x, y) && !airplay_source(playback_) && playback_.duration_ms)
            player_.command(
                NcmCommand_Seek,
                static_cast<uint32_t>(playback_.duration_ms *
                                      std::clamp((x - progress.x) / progress.w, 0.f, 1.f)));
        return;
    }
    if (player_open_) {
        if (ui::Rect{48, 28, 140, 44}.contains(x, y))
            back();
        else if (ui::Rect{641, 484, 310, 44}.contains(x, y))
            action_x();
        else if (ui::Rect{82, 129, 414, 414}.contains(x, y))
            activate();
        return;
    }
    for (int i = 0; i < 8; ++i)
        if (nav_rect(i).contains(x, y)) {
            nav_ = i;
            switch_page(static_cast<Page>(NavOrder[i]));
            return;
        }
    auto s = model_.snapshot();
    const auto controls = headers(s);
    for (size_t i = 0; i < controls.size(); ++i)
        if (controls[i].first.contains(x, y)) {
            region_ = ui::Region::Header;
            header_ = static_cast<int>(i);
            activate();
            return;
        }
    if (s.page == Page::Account || s.page == Page::Settings) {
        for (int i = 0; i < item_count(s); ++i)
            if ((s.page == Page::Account ? ui::account(i) : ui::setting(i)).contains(x, y)) {
                selection_ = i;
                region_ = ui::Region::Content;
                activate();
                return;
            }
    } else if (s.items.empty()) {
        if (ui::Rect{632, 440, 224, 44}.contains(x, y)) {
            region_ = ui::Region::Content;
            activate();
        }
    } else {
        const int columns = grid(s) ? 4 : 1;
        for (int k = 0; k < columns * visible_rows(s); ++k) {
            const auto bounds = grid(s) ? ui::card(k, content_top(s)) : ui::row(k, content_top(s));
            const int index = scroll_ * columns + k;
            if (index < item_count(s) && bounds.contains(x, y)) {
                selection_ = index;
                anchor_ = index < static_cast<int>(s.items.size()) ? media_key(s.items[index]) : "";
                region_ = ui::Region::Content;
                activate();
                return;
            }
        }
    }
}
void MusicView::cover(NVGcontext *v, const Snapshot &s, const std::string &url, float x, float y,
                      float size, float radius) {
    auto entry = s.images.find(url);
    int texture = 0;
    if (entry != s.images.end()) {
        auto t = textures_.find(entry->second);
        if (t == textures_.end()) {
            if (textures_.size() >= 48) {
                auto first = textures_.begin();
                if (first->second > 0)
                    nvgDeleteImage(v, first->second);
                textures_.erase(first);
            }
            texture = nvgCreateImage(v, entry->second.c_str(), 0);
            textures_[entry->second] = texture;
        } else
            texture = t->second;
    }
    if (texture > 0) {
        nvgBeginPath(v);
        nvgRoundedRect(v, x, y, size, size, radius);
        nvgFillPaint(v, nvgImagePattern(v, x, y, size, size, 0, texture, 1));
        nvgFill(v);
    } else {
        rect(v, x, y, size, size, nvgRGB(49, 40, 46), radius);
        circle(v, x + size * .5f, y + size * .5f, size * .3f, nvgRGB(77, 45, 54));
        circle(v, x + size * .5f, y + size * .5f, size * .13f, red);
        circle(v, x + size * .5f, y + size * .5f, size * .025f, panel);
    }
}
void MusicView::vinyl(NVGcontext *v, const Snapshot &s, float x, float y, float radius) {
    nvgBeginPath(v);
    nvgCircle(v, x, y + 12, radius + 12);
    nvgFillPaint(v, nvgRadialGradient(v, x, y + 12, radius * .7f, radius + 20,
                                      nvgRGBA(0, 0, 0, 160), nvgRGBA(0, 0, 0, 0)));
    nvgFill(v);
    circle(v, x, y, radius, nvgRGB(7, 8, 10));
    for (float r = radius * .42f; r < radius - 6; r += 3.8f) {
        nvgBeginPath(v);
        nvgCircle(v, x, y, r);
        nvgStrokeWidth(v, .7f);
        nvgStrokeColor(v, nvgRGBA(132, 133, 144, 32));
        nvgStroke(v);
    }
    nvgSave(v);
    nvgTranslate(v, x, y);
    nvgRotate(v, angle_);
    // Visible specular sectors and label rotate with the record, at 33 1/3 rpm.
    for (int i = 0; i < 2; ++i) {
        nvgBeginPath(v);
        nvgMoveTo(v, 0, 0);
        nvgArc(v, 0, 0, radius - 3, i * 3.14159f + .1f, i * 3.14159f + .75f, NVG_CW);
        nvgClosePath(v);
        nvgFillPaint(v, nvgLinearGradient(v, -radius, 0, radius, 0, nvgRGBA(230, 228, 244, 1),
                                          nvgRGBA(230, 228, 244, 25)));
        nvgFill(v);
    }
    std::string artwork;
    if (!airplay_source(playback_))
        for (const auto &m : s.queue)
            if (m.id == playback_.track_id) {
                artwork = m.cover;
                break;
            }
    const float fade = reduced_motion_ ? 1 : ease(cover_time_, .22f);
    if (fade < 1)
        cover(v, s, previous_artwork_, -radius * .37f, -radius * .37f, radius * .74f,
              radius * .37f);
    nvgSave(v);
    nvgGlobalAlpha(v, fade);
    cover(v, s, artwork, -radius * .37f, -radius * .37f, radius * .74f, radius * .37f);
    nvgRestore(v);
    circle(v, 0, 0, 9, nvgRGB(195, 193, 192));
    circle(v, 0, 0, 4, bg);
    nvgRestore(v);
    // Tonearm lifts away when paused. It is not part of the record transform.
    const float pivot_x = x + radius * .91f, pivot_y = y - radius * .94f;
    nvgSave(v);
    nvgTranslate(v, pivot_x, pivot_y);
    nvgRotate(v, tonearm_);
    line(v, 0, 0, -30, 75, nvgRGB(112, 114, 120), 8);
    line(v, -30, 75, -46, 154, nvgRGB(174, 175, 181), 5);
    rect(v, -56, 145, 18, 35, nvgRGB(205, 204, 207), 4);
    circle(v, 0, 0, 15, raised);
    circle(v, 0, 0, 7, muted);
    nvgRestore(v);
}
void MusicView::focus_ring(NVGcontext *v, ui::Rect r, float radius) {
    focus_target_ = r;
    nvgBeginPath(v);
    nvgRoundedRect(v, r.x - 4, r.y - 4, r.w + 8, r.h + 8, radius + 4);
    auto color = white;
    color.a = reduced_motion_ ? 1 : .6f + .4f * ease(focus_time_, .14f);
    nvgStrokeColor(v, color);
    nvgStrokeWidth(v, 3);
    nvgStroke(v);
    if (press_time_ < .09f)
        rect(v, r.x, r.y, r.w, r.h, nvgRGBA(250, 248, 243, 26), radius);
}
void MusicView::paint_sidebar(NVGcontext *v, const Snapshot &s) {
    rect(v, 0, 0, 208, ui::MiniTop, nvgRGB(23, 24, 28));
    line(v, 208, 0, 208, ui::MiniTop, nvgRGB(44, 45, 51));
    if (app_icon_ < 0)
        app_icon_ = nvgCreateImage(v, BRLS_ASSET("icon/icon.png"), NVG_IMAGE_GENERATE_MIPMAPS);
    if (app_icon_ > 0) {
        nvgBeginPath(v);
        nvgRoundedRect(v, 24, 39, 35, 35, 9);
        nvgFillPaint(v, nvgImagePattern(v, 24, 39, 35, 35, 0, app_icon_, 1));
        nvgFill(v);
    }
    text(v, 69, 59, 21, white, "CloudTune", 133);
    text(v, 70, 78, 10, muted, "NX  ·  MUSIC PLAYER", 130);
    text(v, 33, 118, 12, muted, "探索音乐", 145);
    text(v, 33, 285, 12, muted, "音乐资料库", 145);
    for (int i = 0; i < 8; ++i) {
        const auto r = nav_rect(i);
        const bool active = NavOrder[i] == static_cast<int>(s.page);
        const bool focused = region_ == ui::Region::Sidebar && i == nav_;
        if (active || focused)
            rect(v, r.x, r.y, r.w, r.h, active ? selected_bg : raised, 10);
        if (active)
            rect(v, r.x, r.y + 13, 3, 14, red, 1);
        const auto color = active ? selected_text : focused ? white : muted;
        nav_icon(v, NavOrder[i], 46, r.y + 20, color);
        text(v, 68, r.y + 27, 16, color,
             NavOrder[i] == static_cast<int>(Page::Account) && s.user_id ? s.nickname
                                                                         : labels[NavOrder[i]],
             109);
        if (focused)
            focus_ring(v, r);
    }
}
void MusicView::paint_list(NVGcontext *v, const Snapshot &s) {
    const auto controls = headers(s);
    if (s.collection) {
        cover(v, s,
              collection_cover_.empty() && !s.items.empty() ? s.items.front().cover
                                                            : collection_cover_,
              248, 70, 148, 12);
        text(v, 420, 88, 12, selected_text, s.collection_kind == MediaKind::Album ? "专辑" : "歌单",
             700);
        text(v, 420, 128, 30, white, s.title, 808);
        text(v, 420, 155, 14, muted, s.subtitle, 808);
        text(v, 248, 251, 14, muted,
             "歌曲  ·  " + std::to_string(s.items.size()) + (s.more ? "+" : ""), 600);
        text(v, 1160, 251, 12, muted, "时长", 70);
    } else if (s.page == Page::Discover) {
        // Quiet sleeve-like artwork is vector drawn; no new network or GPU blur.
        rect(v, 248, 112, 992, 112, nvgRGB(28, 50, 51), 16);
        nvgSave(v);
        nvgIntersectScissor(v, 248, 112, 992, 112);
        nvgBeginPath(v);
        nvgRoundedRect(v, 248, 112, 992, 112, 16);
        nvgFillPaint(v, nvgLinearGradient(v, 600, 112, 1240, 224, nvgRGBA(31, 53, 55, 0),
                                          nvgRGBA(116, 153, 143, 125)));
        nvgFill(v);
        circle(v, 1065, 151, 78, nvgRGB(181, 169, 124));
        nvgBeginPath(v);
        nvgMoveTo(v, 790, 224);
        nvgBezierTo(v, 920, 176, 1080, 272, 1240, 183);
        nvgLineTo(v, 1240, 208);
        nvgQuadTo(v, 1240, 224, 1224, 224);
        nvgLineTo(v, 790, 224);
        nvgClosePath(v);
        nvgFillColor(v, nvgRGB(70, 111, 108));
        nvgFill(v);
        nvgRestore(v);
        if (memories_.available()) {
            rect(v, 248, 112, 992, 112, nvgRGB(38, 31, 32), 16);
            memories_.draw(v, 0, {680, 112, 560, 112}, .92f, true, 16);
            nvgBeginPath(v);
            nvgRoundedRect(v, 668, 112, 572, 112, 16);
            nvgFillPaint(v, nvgLinearGradient(v, 668, 112, 808, 112, nvgRGBA(38, 31, 32, 255),
                                              nvgRGBA(38, 31, 32, 0)));
            nvgFill(v);
            rect(v, 1094, 187, 132, 28, nvgRGBA(16, 17, 20, 205), 14);
            text(v, 1107, 206, 13, white, "我们的时光  ›", 112);
        }
        text(v, 276, 153, 27, white,
             memories_.available() ? "每一首歌，都有你" : "今天，从一首好歌开始", 640);
        rect(v, 276, 168, 160, 44, white, 22);
        play_icon(v, 298, 190, true, 12, bg);
        text(v, 316, 196, 16, bg, "开启每日推荐", 110);
        text(v, 248, 269, 21, white, s.title.empty() ? "值得慢慢听" : s.title, 620);
    } else {
        const std::string heading = s.page == Page::Queue    ? "接下来播放"
                                    : s.page == Page::Search ? "搜索结果"
                                    : s.page == Page::Charts ? "发现此刻的热门音乐"
                                                             : "收藏，随时再听";
        text(v, 248, 198, 21, white, heading, 740);
        if (!s.items.empty())
            text(v, 1104, 198, 12, muted,
                 std::to_string(s.items.size()) + (s.more ? "+ 项" : " 项"), 136);
    }
    for (size_t i = 0; i < controls.size(); ++i) {
        const auto &[r, label] = controls[i];
        if (!(s.page == Page::Discover && !s.collection)) {
            button(v, r.x, r.y, r.w, label, s.collection && i == 0);
        } else if (i > 0 && !(memories_.available() && i == 1))
            text(v, r.x + 4, r.y + 28, 14, white, label, r.w - 8);
        if (region_ == ui::Region::Header && header_ == static_cast<int>(i))
            focus_ring(v, r, 12);
    }
    if (s.items.empty()) {
        if (s.busy) {
            for (int i = 0; i < 4; ++i) {
                const float left = 248 + i * 258;
                rect(v, left, 296, 218, 114, raised, 10);
                rect(v, left, 426, 170, 12, raised, 4);
                rect(v, left, 450, 116, 10, panel, 4);
            }
            if (region_ == ui::Region::Content)
                focus_ring(v, {248, 296, 218, 164});
            text(v, 631, 512, 17, muted, "正在加载音乐…", 400);
        } else {
            circle(v, 744, 320, 43, raised);
            circle(v, 744, 320, 24, selected_bg);
            circle(v, 744, 320, 6, red);
            const std::string title =
                !s.error.empty() ? "暂时无法加载音乐"
                : s.page == Page::Search
                    ? (s.search_query.empty() ? "下一首喜欢的歌，从这里开始" : "没有找到相关音乐")
                : s.page == Page::Queue ? "播放队列还是空的"
                                        : "你的音乐，等你来收藏";
            text(v, 480, 394, 23, white, title, 680);
            text(v, 480, 423, 14, muted,
                 !s.error.empty() ? s.error
                 : s.page == Page::Search ? "输入歌名、歌手或专辑，按 X 切换搜索分类"
                 : s.page == Page::Library && !s.user_id ? "登录后同步歌单、收藏与我喜欢的音乐"
                                                         : "前往发现音乐，选择一张歌单开始播放",
                 700);
            const std::string action = s.page == Page::Search                  ? "输入关键词"
                                       : s.page == Page::Library && !s.user_id ? "前往登录"
                                                                               : "重新加载";
            button(v, 632, 440, 224, action);
            if (region_ == ui::Region::Content)
                focus_ring(v, {632, 440, 224, 44});
        }
        return;
    }
    const float top = content_top(s);
    const int columns = grid(s) ? 4 : 1;
    const int visible = visible_rows(s) * columns;
    for (int k = 0; k < visible; ++k) {
        const int i = scroll_ * columns + k;
        if (i >= static_cast<int>(s.items.size()) + (s.more ? 1 : 0))
            break;
        const bool focused = region_ == ui::Region::Content && i == selection_;
        const bool more = i == static_cast<int>(s.items.size());
        if (grid(s)) {
            const auto r = ui::card(k, top);
            if (more) {
                rect(v, r.x, r.y, 218, 218, raised, 10);
                text(v, r.x + 85, r.y + 128, 44, white, "+", 60);
                text(v, r.x, r.y + 244, 17, white, "加载更多音乐", 218);
                text(v, r.x, r.y + 268, 14, muted, "A 继续浏览", 218);
            } else {
                const auto &m = s.items[i];
                const float grow =
                    focused && !reduced_motion_ ? 2.725f * ease(focus_time_, .16f) : 0;
                cover(v, s, m.cover, r.x - grow, r.y - grow, 218 + 2 * grow, 10);
                if (focused) {
                    circle(v, r.x + 188, r.y + 188, 19, white);
                    play_icon(v, r.x + 188, r.y + 188, true, 13, bg);
                }
                text(v, r.x, r.y + 244, 17, white, m.title, 218);
                text(v, r.x, r.y + 268, 14, muted,
                     m.artist.empty() ? std::to_string(m.count) + " 首音乐" : m.artist, 218);
            }
            if (focused)
                focus_ring(v, r, 11);
        } else {
            const auto r = ui::row(k, top);
            if (focused)
                rect(v, r.x, r.y, r.w, r.h, raised, 9);
            if (more) {
                text(v, r.x + 26, r.y + 32, 17, white, "加载更多结果…", 930);
            } else {
                const auto &m = s.items[i];
                const bool playing = !airplay_source(playback_) && m.id == playback_.track_id;
                if (playing) {
                    for (int j = 0; j < 3; ++j) {
                        const float height = 8 + (j == 1 ? 9 : 4);
                        rect(v, 262 + j * 5, r.y + 32 - height, 3, height, selected_text, 1);
                    }
                } else
                    text(v, 261, r.y + 31, 12, muted, std::to_string(i + 1), 34);
                cover(v, s, m.cover, 300, r.y + 4, 42, 6);
                text(v, 358, r.y + 23, 17, playing ? selected_text : white, m.title, 640);
                text(v, 358, r.y + 44, 13, muted,
                     m.artist + (m.album.empty() ? "" : " · " + m.album), 640);
                if (s.likes.count(m.id))
                    text(v, 1078, r.y + 32, 18, selected_text, "♥", 36);
                text(v, 1154, r.y + 31, 14, muted, duration_text(m.duration_ms), 72);
            }
            if (focused)
                focus_ring(v, r, 9);
        }
    }
    const int total = static_cast<int>(s.items.size()) + (s.more ? 1 : 0);
    if (total > visible) {
        const float height = grid(s) ? 266 : ui::RowPitch * (visible_rows(s) - 1) + ui::RowHeight;
        const float thumb = std::max(22.f, height * visible / total);
        rect(v, 1256, top, 3, height, raised, 1);
        rect(v, 1256,
             top + (height - thumb) *
                       std::min(1.f, float(scroll_ * columns) / std::max(1, total - visible)),
             3, thumb, muted, 1);
    }
}
void MusicView::paint_player(NVGcontext *v, const Snapshot &s, bool full) {
    const bool incoming = airplay_source(playback_);
    const bool paused = playback_.state != NcmPlayback_Playing;
    const bool has_source = playback_.track_id || incoming;
    std::string title = has_source ? status_text(playback_.title) : "还没有开始播放";
    std::string artist = has_source ? status_text(playback_.artist) : "发现一首好歌，让游戏更动听";
    if (incoming && title.empty())
        title = "AirPlay";
    if (incoming && artist.empty())
        artist = "局域网音频接收";
    std::string artwork;
    if (!incoming)
        for (const auto &m : s.queue)
            if (m.id == playback_.track_id) {
                artwork = m.cover;
                break;
            }
    if (full) {
        if (photo_background_ && memories_.available()) {
            memories_.draw(v, 0, {0, 0, 1280, ui::HintsTop}, .24f, false, 0);
            nvgBeginPath(v);
            nvgRect(v, 0, 0, 1280, ui::HintsTop);
            nvgFillPaint(v, nvgLinearGradient(v, 400, 0, 1040, 0, nvgRGBA(16, 17, 20, 45),
                                              nvgRGBA(16, 17, 20, 220)));
            nvgFill(v);
            nvgBeginPath(v);
            nvgRect(v, 0, 0, 1280, ui::HintsTop);
            nvgFillPaint(v, nvgLinearGradient(v, 0, 430, 0, ui::HintsTop, nvgRGBA(16, 17, 20, 0),
                                              nvgRGBA(16, 17, 20, 255)));
            nvgFill(v);
        }
        // A single radial wash stays inexpensive on Switch. No full-screen blur.
        nvgBeginPath(v);
        nvgRect(v, 0, 0, 1280, ui::HintsTop);
        nvgFillPaint(v, nvgRadialGradient(v, 289, 324, 70, 570, nvgRGBA(80, 58, 64, 100),
                                          nvgRGBA(16, 17, 20, 0)));
        nvgFill(v);
        text(v, 52, 58, 17, muted, "‹  返回音乐", 170);
        text(v, 1026, 58, 14, muted,
             incoming                            ? "AirPlay · 实时音频"
             : playback_.repeat == NcmRepeat_One ? "单曲循环"
             : playback_.shuffle                 ? "随机播放"
             : playback_.repeat == NcmRepeat_All ? "列表循环"
                                                 : "顺序播放",
             214);
        vinyl(v, s, 289, 336, 207);
        text(v, 641, 120, 12, selected_text, incoming ? "AIRPLAY" : "正在播放", 540);
        text(v, 641, 170, 39, white, title, 559);
        text(v, 643, 204, 17, muted, artist, 553);
        if (incoming) {
            text(v, 643, 310, 27, white, "音乐从你的设备而来", 553);
            text(v, 643, 356, 18, muted,
                 playback_.state == NcmPlayback_Error ? "音频输出异常，请查看接收设置"
                 : paused                             ? "本地音频已暂停"
                                                      : "正在接收局域网音频",
                 553);
            text(v, 643, 398, 16, muted, "L / R 结束投送并切回网易云", 553);
        } else if (s.lyrics.empty()) {
            text(v, 643, 328, 24, white, playback_.track_id ? "让音乐自己说话" : "在这里，听见热爱",
                 553);
            text(v, 643, 370, 17, muted,
                 playback_.track_id ? "这首歌暂无滚动歌词" : "选择一首音乐，开始你的聆听时光", 553);
        } else {
            const int active = lyric_index(s.lyrics, playback_.position_ms);
            const int first = std::max(0, active - 1);
            nvgSave(v);
            nvgIntersectScissor(v, 638, 237, 570, 230);
            for (int i = std::max(0, first - 1); i < std::min<int>(s.lyrics.size(), first + 5);
                 ++i) {
                const auto &lyric = s.lyrics[i].text;
                const auto separator = lyric.find('\n');
                const float baseline = 266 + (i - first) * 66 + lyric_offset_;
                text(v, 643, baseline, i == active ? 27 : 21, i == active ? white : muted,
                     lyric.substr(0, separator), 553);
                if (separator != std::string::npos)
                    text(v, 644, baseline + 24, 14, i == active ? selected_text : muted,
                         lyric.substr(separator + 1), 552);
            }
            nvgRestore(v);
        }
        button(v, 641, 484, 310,
               incoming                            ? "X  AirPlay 接收设置"
               : s.likes.count(playback_.track_id) ? "X  已喜欢 · 播放选项"
                                                   : "X  喜欢与播放选项");
        text(v, 974, 512, 14, muted, "↑ ↓ 调节音量", 226);
        line(v, 80, 558, 1200, 558, raised);
    } else {
        rect(v, 0, ui::MiniTop, 1280, 84, nvgRGB(27, 29, 34));
        line(v, 0, ui::MiniTop, 1280, ui::MiniTop, nvgRGB(49, 51, 57));
        const float fade = reduced_motion_ ? 1 : ease(cover_time_, .22f);
        if (fade < 1)
            cover(v, s, previous_artwork_, 32, 595, 52, 7);
        nvgSave(v);
        nvgGlobalAlpha(v, fade);
        cover(v, s, artwork, 32, 595, 52, 7);
        nvgRestore(v);
        text(v, 101, 617, 17, white, title, 295);
        text(v, 101, 640, 14, muted, artist, 295);
    }
    const auto prev = ui::transport(1, full), play = ui::transport(2, full),
               next = ui::transport(3, full);
    skip_icon(v, prev.x + prev.w / 2, prev.y + prev.h / 2, false);
    circle(v, play.x + play.w / 2, play.y + play.h / 2, full ? 26 : 22, white);
    play_icon(v, play.x + play.w / 2, play.y + play.h / 2, paused, 17, bg);
    skip_icon(v, next.x + next.w / 2, next.y + next.h / 2, true);
    if (full) {
        text(v, 94, 609, 14, muted, "音量 " + std::to_string(playback_.volume_permille / 10) + "%",
             140);
        const auto like = ui::transport(4, true);
        text(v, like.x + 15, like.y + 35, 24,
             incoming                            ? muted
             : s.likes.count(playback_.track_id) ? selected_text
                                                 : white,
             incoming ? "—" : "♥", 40);
        text(v, 1132, 609, 16, white, "队列", 64);
    } else {
        nav_icon(v, 5, 1030, 618, white);
        text(v, 1082, 615, 12, player_.background() ? nvgRGB(169, 216, 196) : muted,
             player_.background() ? "BGM · 后台服务" : "前台播放", 150);
        text(v, 1082, 638, 14, muted,
             "音量 " + std::to_string(playback_.volume_permille / 10) + "%", 150);
    }
    const float progress_x = full ? 540 : 620, progress_w = full ? 550 : 344;
    const float progress_y = full ? 611 : 638;
    text(v, progress_x, progress_y - 14, 12, muted, duration_text(playback_.position_ms), 80);
    text(v, progress_x + progress_w - 64, progress_y - 14, 12, muted,
         incoming ? "实时音频" : duration_text(playback_.duration_ms), 68);
    rect(v, progress_x, progress_y, progress_w, 4, nvgRGB(66, 68, 77), 2);
    if (incoming) {
        rect(v, progress_x, progress_y, progress_w, 4, selected_bg, 2);
    } else if (playback_.duration_ms) {
        const float amount =
            std::clamp(float(playback_.position_ms) / playback_.duration_ms, 0.f, 1.f);
        rect(v, progress_x, progress_y, progress_w * amount, 4, red, 2);
    }
    if (full)
        focus_ring(v, play, 26);
    else if (region_ == ui::Region::Playback)
        focus_ring(v, ui::transport(transport_, false));
    const auto message = status_text(playback_.message);
    const std::string state =
        playback_.state == NcmPlayback_Buffering ? "正在缓冲…"
        : playback_.state == NcmPlayback_Error   ? message
        : playback_.state == NcmPlayback_Playing && message.find("preview") != std::string::npos
            ? "试听 · 完整播放取决于账号与曲目权益"
            : "";
    if (!state.empty()) {
        // Status occupies a reserved line, never a song row or a focus target.
        rect(v, full ? 540 : 620, full ? 628 : 643, full ? 550 : 344, 18, panel, 2);
        text(v, full ? 540 : 620, full ? 642 : 657, 12,
             playback_.state == NcmPlayback_Error ? selected_text : muted, state, full ? 550 : 344);
    }
}
void MusicView::paint_account(NVGcontext *v, const Snapshot &s) {
    rect(v, 248, 112, 992, 372, panel, 16);
    if (s.qr_url != qr_source_) {
        qr_source_ = s.qr_url;
        qr_.reset();
        if (!qr_source_.empty())
            try {
                qr_ = std::make_unique<qrcodegen::QrCode>(qrcodegen::QrCode::encodeText(
                    qr_source_.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
            } catch (...) {
            }
    }
    if (qr_) {
        const int n = qr_->getSize();
        const float scale = 230.f / (n + 8);
        rect(v, 302, 157, 230, 230, nvgRGB(255, 255, 255), 8);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qr_->getModule(x, y))
                    rect(v, 302 + (x + 4) * scale, 157 + (y + 4) * scale, scale + .1f, scale + .1f,
                         nvgRGB(0, 0, 0));
    } else {
        rect(v, 302, 157, 230, 230, raised, 12);
        circle(v, 417, 266, 63, selected_bg);
        circle(v, 417, 266, 35, red);
        circle(v, 417, 266, 9, panel);
        text(v, 327, 367, 16, white, s.user_id ? "已登录网易云音乐" : "生成二维码后扫码", 200);
    }
    text(v, 302, 428, 14, muted, s.qr_message.empty() ? "登录信息仅保存在此设备" : s.qr_message,
         315);
    text(v, 660, 162, 27, white, s.user_id ? "你好，" + s.nickname : "让喜欢的音乐，一路相随", 540);
    text(v, 660, 199, 16, muted, "打开网易云音乐 App「扫一扫」", 540);
    text(v, 660, 226, 16, muted, "扫码后在手机上确认登录", 540);
    const std::string names[] = {
        s.qr_url.empty() ? (s.user_id ? "扫码添加账号" : "生成登录二维码") : "刷新登录二维码",
        "管理已保存账号（" + std::to_string(s.accounts.size()) + "）", "每日签到"};
    for (int i = 0; i < account_rows(s); ++i) {
        const auto r = ui::account(i);
        const bool selected = region_ == ui::Region::Content && selection_ == i;
        button(v, r.x, r.y, r.w, names[i], i == 0);
        if (selected)
            focus_ring(v, r);
    }
    text(v, 660, 457, 14, muted,
         s.qr_url.empty() ? "账号可在此设备上保存与切换" : "B 取消扫码，当前账号保持登录", 540);
    text(v, 248, 532, 15, muted,
         s.user_id ? "歌单、收藏与喜欢的音乐，会随当前账号同步。"
                   : "登录后即可浏览你的歌单、收藏和喜欢的音乐。",
         992);
}
void MusicView::paint_settings(NVGcontext *v, const Snapshot &s) {
    const char *names[SettingsCount] = {"账号管理",       "音质",     "网络代理",
                                        "AirPlay 1 接收", "停止播放", "退出应用"};
    std::string tool = airplay_state(airplay_);
    const auto name = status_text(airplay_.name);
    if (!name.empty())
        tool += " · " + name;
    const std::vector<std::string> desc = {
        s.nickname + " · 已保存 " + std::to_string(s.accounts.size()) + " 个账号",
        quality_name(s.quality),
        s.proxy.empty() ? "直连" : s.proxy,
        tool,
        player_.background() ? "独立后台服务" : "本应用音频引擎",
        player_.background() ? "退出后，后台服务继续播放" : "退出后，前台音频将停止"};
    for (int i = 0; i < SettingsCount; ++i) {
        const auto r = ui::setting(i);
        const bool focused = region_ == ui::Region::Content && selection_ == i;
        rect(v, r.x, r.y, r.w, r.h, focused ? raised : panel, 10);
        text(v, 271, r.y + 40, 18, white, names[i], 260);
        text(v, 565, r.y + 40, 16, muted, desc[i], 616);
        text(v, 1202, r.y + 41, 24, muted, "›", 22);
        if (focused)
            focus_ring(v, r);
    }
}
void MusicView::paint_hints(NVGcontext *v, const Snapshot &s) {
    rect(v, 0, ui::HintsTop, 1280, 56, nvgRGB(19, 20, 24));
    const auto hint = [&](float x, const std::string &key, const std::string &label, float width) {
        nvgBeginPath(v);
        if (key.size() == 1)
            nvgCircle(v, x + 10, 688, 10);
        else
            nvgRoundedRect(v, x, 678, key.size() > 3 ? 48 : 34, 20, 4);
        nvgStrokeColor(v, muted);
        nvgStrokeWidth(v, 1);
        nvgStroke(v);
        text(v, x + (key.size() == 1 ? 5 : 4), 693, 12, white, key, 48);
        text(v,
             x + (key.size() == 1  ? 27
                  : key.size() > 3 ? 55
                                   : 41),
             694, 14, muted, label, width);
    };
    hint(32, "A",
         player_open_ ? (playback_.state == NcmPlayback_Playing ? "暂停" : "播放") : "选择", 50);
    hint(107, "B", player_open_ || s.collection ? "返回" : "导航", 50);
    hint(182, "X", s.page == Page::Settings && !player_open_ ? "界面" : "更多", 50);
    hint(257, "Y", playback_.state == NcmPlayback_Playing ? "暂停" : "播放", 50);
    if (player_open_) {
        text(v, 366, 694, 14, muted,
             airplay_source(playback_) ? "↑ ↓ 音量" : "← → 快进 10 秒   ↑ ↓ 音量", 282);
    } else if (s.page == Page::Settings) {
        text(v, 366, 694, 12, muted, "X 动态效果 / 画面范围", 280);
    }
    hint(696, "L/R", airplay_source(playback_) ? "切回云音乐" : "切歌", 106);
    hint(864, "ZL/ZR", "音量", 60);
    hint(1008, "+", player_open_ ? "收起播放" : "正在播放", 100);
    hint(1160, "−", "退出", 60);
}
void MusicView::draw(NVGcontext *v, float x, float y, float w, float h, brls::Style,
                     brls::FrameContext *) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last_frame_).count(), 0.f, .1f);
    last_frame_ = now;
    focus_time_ += dt;
    page_time_ += dt;
    press_time_ += dt;
    notice_time_ += dt;
    cover_time_ += dt;
    if (now - last_poll_ > std::chrono::milliseconds(200)) {
        playback_ = player_.status();
        last_poll_ = now;
        if (!airplay_source(playback_))
            model_.lyrics_for(playback_.track_id);
        display_ = model_.snapshot();
        reconcile(display_);
        std::vector<Media> visible;
        const auto first = static_cast<size_t>(std::max(0, grid(display_) ? scroll_ * 4 : scroll_));
        const auto end = std::min(display_.items.size(), first + (grid(display_) ? 4 : 6));
        if (!player_open_)
            for (size_t i = first; i < end; ++i)
                visible.push_back(display_.items[i]);
        for (const auto &track : display_.queue)
            if (track.id == playback_.track_id) {
                visible.push_back(track);
                break;
            }
        model_.ensure_artwork(visible);
    }
    if (now - last_airplay_poll_ > std::chrono::milliseconds(500)) {
        airplay_ = player_.airplay_status();
        last_airplay_poll_ = now;
    }
    if (!reduced_motion_ && playback_.state == NcmPlayback_Playing)
        angle_ = std::fmod(angle_ + dt * 3.4906585f, 6.2831853f);
    const float target_arm = playback_.state == NcmPlayback_Playing ? .4f : .08f;
    tonearm_ += (target_arm - tonearm_) * (reduced_motion_ ? 1 : std::min(1.f, dt * 12));
    const auto &s = display_;
    if (!player_open_ && s.page == Page::Account && now - last_qr_ > std::chrono::seconds(3)) {
        model_.poll_qr();
        last_qr_ = now;
    }
    if (playback_.track_id != last_track_) {
        last_track_ = playback_.track_id;
        last_lyric_ = -1;
        lyric_offset_ = 0;
        cover_time_ = 0;
    }
    std::string artwork;
    if (!airplay_source(playback_))
        for (const auto &track : s.queue)
            if (track.id == playback_.track_id) {
                artwork = track.cover;
                break;
            }
    if (artwork != current_artwork_) {
        previous_artwork_ = current_artwork_;
        current_artwork_ = artwork;
        cover_time_ = 0;
    }
    const int lyric = airplay_source(playback_) ? -1 : lyric_index(s.lyrics, playback_.position_ms);
    if (lyric != last_lyric_) {
        if (last_lyric_ >= 1 && lyric == last_lyric_ + 1 && !reduced_motion_)
            lyric_offset_ = 66;
        else
            lyric_offset_ = 0;
        last_lyric_ = lyric;
    }
    lyric_offset_ *= reduced_motion_ ? 0 : std::max(0.f, 1 - dt * 15);
    const auto focus = std::to_string(static_cast<int>(region_)) + ":" +
                       std::to_string(selection_) + ":" + std::to_string(nav_) + ":" +
                       std::to_string(header_) + ":" + std::to_string(transport_) + ":" +
                       (player_open_ ? "player" : view_key_);
    if (last_focus_ != focus) {
        last_focus_ = focus;
        focus_time_ = 0;
    }
    const std::string notice = !s.error.empty() ? s.error : s.notice;
    if (notice != last_notice_) {
        last_notice_ = notice;
        notice_time_ = 0;
    }
    nvgSave(v);
    rect(v, x, y, w, h, bg);
    const auto viewport = ui::Viewport::fit(w, h, safe_scale_);
    nvgTranslate(v, x + viewport.x, y + viewport.y);
    nvgScale(v, viewport.scale, viewport.scale);
    nvgIntersectScissor(v, 0, 0, 1280, 720);
    rect(v, 0, 0, 1280, 720, bg);
    focus_target_ = {0, 0, 0, 0};
    if (!player_open_)
        paint_sidebar(v, s);
    nvgSave(v);
    const float enter = reduced_motion_ ? 1 : ease(page_time_, player_open_ ? .26f : .22f);
    nvgGlobalAlpha(v, .65f + .35f * enter);
    nvgTranslate(v, 0, (1 - enter) * 8);
    if (player_open_)
        paint_player(v, s, true);
    else {
        if (!s.collection) {
            text(v, 248, 66, 30, white,
                 s.page == Page::Discover ? "让音乐，陪你去远方。"
                                          : labels[static_cast<int>(s.page)],
                 760);
            text(v, 248, 90, 14, muted,
                 s.page == Page::Discover ? "在掌心，听见热爱。" : s.subtitle, 900);
        }
        if (s.page == Page::Account)
            paint_account(v, s);
        else if (s.page == Page::Settings)
            paint_settings(v, s);
        else
            paint_list(v, s);
    }
    nvgRestore(v);
    if (!player_open_)
        paint_player(v, s, false);
    if (!notice.empty() && notice_time_ < (s.error.empty() ? 2.6f : 6.f) && !s.busy) {
        nvgSave(v);
        const float alpha =
            reduced_motion_
                ? 1
                : std::min(
                      ease(notice_time_, .14f),
                      std::clamp(((s.error.empty() ? 2.6f : 6.f) - notice_time_) / .16f, 0.f, 1.f));
        nvgGlobalAlpha(v, alpha);
        rect(v, 762, 26, 478, 54, s.error.empty() ? raised : nvgRGB(52, 34, 41), 12);
        text(v, 780, 59, 14, s.error.empty() ? white : nvgRGB(240, 200, 205), notice, 442);
        nvgRestore(v);
    } else if (s.busy && !player_open_) {
        circle(v, 1134, 54, 4, red);
        text(v, 1150, 60, 14, muted, "加载中", 90);
    }
    paint_hints(v, s);
    nvgRestore(v);
}
} // namespace netease
