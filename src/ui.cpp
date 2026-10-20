// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/ui.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace netease {
namespace {
const NVGcolor bg = nvgRGB(17, 18, 22), panel = nvgRGB(24, 25, 30), raised = nvgRGB(35, 36, 43);
const NVGcolor white = nvgRGB(245, 243, 240), muted = nvgRGB(150, 151, 161),
               red = nvgRGB(244, 68, 79);
const char *labels[] = {"发现音乐", "排行榜",   "搜索",     "我的音乐",
                        "正在播放", "播放队列", "我的账号", "设置"};
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
void play_icon(NVGcontext *v, float x, float y, bool paused, float size = 15) {
    if (!paused) {
        rect(v, x - size / 2, y - size / 2, 4, size, white, 1);
        rect(v, x + size / 2 - 4, y - size / 2, 4, size, white, 1);
    } else {
        nvgBeginPath(v);
        nvgMoveTo(v, x - size / 3, y - size / 2);
        nvgLineTo(v, x + size / 2, y);
        nvgLineTo(v, x - size / 3, y + size / 2);
        nvgClosePath(v);
        nvgFillColor(v, white);
        nvgFill(v);
    }
}
bool grid(const Snapshot &s) {
    return !s.items.empty() && s.items.front().kind != MediaKind::Song;
}
} // namespace
MusicView::MusicView(Model &m, Player &p) : model_(m), player_(p) {
    setFocusable(true);
    // Focus is painted at the selected card/row, never around the entire canvas.
    setHideHighlight(true);
    setHideClickAnimation(true);
    last_frame_ = last_poll_ = last_qr_ = std::chrono::steady_clock::now();
    last_poll_ -= std::chrono::seconds(1);
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
    bind(brls::BUTTON_B, [this] {
        auto s = model_.snapshot();
        if (s.collection) {
            model_.back();
            selection_ = scroll_ = 0;
        } else
            sidebar_ = !sidebar_;
    });
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
    bind(brls::BUTTON_BACK, [this] {
        options("退出应用",
                {"继续听歌", player_.background() ? "退出应用（后台服务保持运行）"
                                                  : "退出应用（停止前台播放）"},
                [](int i) {
                    if (i == 1)
                        brls::Application::quit();
                });
    });
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
        sidebar_ = !sidebar_;
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
                tap((status.position.x - r.getMinX()) * 1280 / r.getWidth(),
                    (status.position.y - r.getMinY()) * 720 / r.getHeight());
            }
        }));
    addGestureRecognizer(new brls::ScrollGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound *) {
            if (status.state == brls::GestureState::START)
                pan_distance_ = 0;
            const auto frame = getFrame();
            const float x = (status.startPosition.x - frame.getMinX()) * 1280 / frame.getWidth();
            const float y = (status.startPosition.y - frame.getMinY()) * 720 / frame.getHeight();
            if (!status.deltaOnly && (x < 250 || y < 190 || y > 590))
                return;
            pan_distance_ +=
                (status.deltaOnly ? -status.delta.y : status.delta.y) * 720 / frame.getHeight();
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
    rect(v, x, y, w, 39, selected ? red : raised, 19);
    text(v, x + 16, y + 26, 16, white, title, w - 26);
}
void MusicView::options(const std::string &title, const std::vector<std::string> &values,
                        std::function<void(int)> f) {
    brls::Application::pushActivity(
        new brls::Activity(new brls::Dropdown(title, values, [f](int i) {
            if (i >= 0)
                f(i);
        })));
}
void MusicView::switch_page(Page p) {
    if (model_.snapshot().busy)
        return;
    nav_ = static_cast<int>(p);
    selection_ = scroll_ = 0;
    sidebar_ = false;
    model_.browse(p);
}
void MusicView::navigate(int d) {
    auto s = model_.snapshot();
    if (sidebar_) {
        if (d == -1 || d == 1)
            nav_ = std::clamp(nav_ + d, 0, 7);
        if (d == 2) {
            switch_page(static_cast<Page>(nav_));
            sidebar_ = false;
        }
        return;
    }
    if (s.page == Page::Player) {
        if (d == -2 || d == 2) {
            auto t = static_cast<int64_t>(playback_.position_ms) + (d > 0 ? 10000 : -10000);
            player_.command(NcmCommand_Seek, static_cast<uint32_t>(std::max<int64_t>(
                                                 0, std::min<int64_t>(t, playback_.duration_ms))));
        } else
            player_.command(
                NcmCommand_Volume,
                std::clamp(static_cast<int>(playback_.volume_permille) - d * 50, 0, 1000));
        return;
    }
    if (d == -2) {
        if (!grid(s) || selection_ % 4 == 0) {
            sidebar_ = true;
            return;
        }
        --selection_;
    } else if (d == 2) {
        if (grid(s))
            ++selection_;
    } else
        selection_ += grid(s) ? d * 4 : d;
    int count = static_cast<int>(s.items.size()) + (s.more ? 1 : 0);
    if (s.page == Page::Account)
        count = 4;
    if (s.page == Page::Settings)
        count = 4;
    selection_ = std::clamp(selection_, 0, std::max(0, count - 1));
    int rows = grid(s) ? 2 : 6, index = grid(s) ? selection_ / 4 : selection_;
    if (index < scroll_)
        scroll_ = index;
    if (index >= scroll_ + rows)
        scroll_ = index - rows + 1;
}
void MusicView::scroll_rows(int rows) {
    const auto s = model_.snapshot();
    if (s.page == Page::Account || s.page == Page::Settings || s.page == Page::Player)
        return;
    const int columns = grid(s) ? 4 : 1;
    const int visible = grid(s) ? 2 : 6;
    const int count = static_cast<int>(s.items.size()) + (s.more ? 1 : 0);
    const int total_rows = (count + columns - 1) / columns;
    scroll_ = std::clamp(scroll_ + rows, 0, std::max(0, total_rows - visible));
    selection_ = std::clamp(
        selection_, scroll_ * columns,
        std::max(scroll_ * columns, std::min(count - 1, (scroll_ + visible) * columns - 1)));
    sidebar_ = false;
}
void MusicView::input_search() {
    if (model_.snapshot().busy)
        return;
    brls::Application::getImeManager()->openForText(
        [this](std::string s) {
            if (!s.empty()) {
                selection_ = scroll_ = 0;
                nav_ = 2;
                model_.search(s);
            }
        },
        "搜索网易云音乐", "歌曲、歌单、专辑、歌手", 80, model_.snapshot().search_query);
}
void MusicView::phone_login() {
    brls::Application::getImeManager()->openForText(
        [this](std::string phone) {
            if (phone.empty())
                return;
            brls::Application::getImeManager()->openForText(
                [this, phone](std::string country) {
                    if (country.empty())
                        country = "86";
                    options("手机号登录", {"发送验证码（每次只发送一次）", "输入已收到的验证码"},
                            [this, phone, country](int i) {
                                if (i == 0)
                                    model_.send_captcha(phone, country);
                                else
                                    brls::Application::getImeManager()->openForText(
                                        [this, phone, country](std::string code) {
                                            if (!code.empty())
                                                model_.login_phone(phone, country, code);
                                        },
                                        "输入短信验证码", "验证码不会写入日志", 8);
                            });
                },
                "国家/地区代码", "中国大陆为 86", 6, "86");
        },
        "手机号", "仅用于网易云音乐验证码登录", 20);
}
void MusicView::activate() {
    if (sidebar_) {
        switch_page(static_cast<Page>(nav_));
        return;
    }
    auto s = model_.snapshot();
    if (s.busy)
        return;
    if (s.page == Page::Player) {
        player_.command(NcmCommand_PlayPause);
        return;
    }
    if (s.page == Page::Account) {
        if (selection_ == 0)
            model_.login_qr();
        if (selection_ == 1)
            phone_login();
        if (selection_ == 2)
            options("退出网易云账号", {"取消", "退出并清除本地登录会话"}, [this](int i) {
                if (i == 1)
                    model_.logout();
            });
        if (selection_ == 3)
            model_.check_in();
        return;
    }
    if (s.page == Page::Settings) {
        if (selection_ == 0)
            options("播放音质", {"标准 · 128 kbps", "较高 · 192 kbps", "极高 · 320 kbps"},
                    [this](int i) {
                        model_.set_quality(
                            std::vector<std::string>{"standard", "higher", "exhigh"}[i]);
                    });
        if (selection_ == 1)
            brls::Application::getImeManager()->openForText(
                [this](std::string proxy) { model_.set_proxy(proxy); }, "HTTP / SOCKS5 代理",
                "留空使用直连", 200, s.proxy);
        if (selection_ == 2)
            options("播放控制", {"取消", "停止播放并清空当前音频"}, [this](int i) {
                if (i == 1)
                    player_.command(NcmCommand_Stop);
            });
        if (selection_ == 3)
            brls::Application::quit();
        return;
    }
    if (s.page == Page::Search && s.items.empty()) {
        input_search();
        return;
    }
    if (selection_ == static_cast<int>(s.items.size()) && s.more) {
        model_.load_more();
        return;
    }
    if (selection_ < 0 || selection_ >= static_cast<int>(s.items.size()))
        return;
    if (s.items[selection_].kind == MediaKind::Song)
        model_.play(selection_);
    else {
        model_.open(s.items[selection_]);
        selection_ = scroll_ = 0;
    }
}
void MusicView::action_x() {
    auto s = model_.snapshot();
    if (s.busy)
        return;
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
                    selection_ = scroll_ = 0;
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
                    selection_ = scroll_ = 0;
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
                        selection_ = scroll_ = 0;
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
                 "私人 FM 不喜欢", "停止播放"},
                [this](int i) {
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
                });
        return;
    }
    model_.refresh();
}
void MusicView::tap(float x, float y) {
    if (x < 220 && y >= 158 && y < 158 + 8 * 47) {
        switch_page(static_cast<Page>(int((y - 158) / 47)));
        return;
    }
    if (y >= 621) {
        if (y > 680 && x >= 490 && x < 974) {
            player_.command(NcmCommand_Seek, playback_.duration_ms * (x - 490) / 484);
            return;
        }
        if (x < 490) {
            switch_page(Page::Player);
            return;
        }
        if (x >= 1111) {
            player_.command(NcmCommand_Volume, std::clamp(int((x - 1111) / 130 * 1000), 0, 1000));
            return;
        }
        if (y > 680)
            return;
        if (x >= 617 && x < 665)
            player_.command(NcmCommand_Previous);
        if (x >= 665 && x < 730)
            player_.command(NcmCommand_PlayPause);
        if (x >= 730 && x < 790)
            player_.command(NcmCommand_Next);
        return;
    }
    auto s = model_.snapshot();
    if (s.busy)
        return;
    if (x > 250 && y > 121 && y < 169) {
        if (s.collection) {
            if (x < 436)
                action_x();
        } else if (s.page == Page::Discover) {
            selection_ = scroll_ = 0;
            if (x < 397)
                model_.daily();
            else if (x < 542)
                model_.fm();
            else if (x < 687)
                model_.new_albums();
            else
                action_x();
        } else if (s.page == Page::Library)
            action_x();
        else if (s.page == Page::Search) {
            if (x < 430)
                input_search();
            else
                action_x();
        }
        return;
    }
    if (s.page == Page::Account && x > 710 && y > 220 && y < 440) {
        selection_ = int((y - 220) / 55);
        activate();
        return;
    }
    if (s.page == Page::Settings && y >= 200 && y < 480) {
        selection_ = int((y - 200) / 70);
        activate();
        return;
    }
    if (s.page == Page::Player) {
        if (x < 760)
            player_.command(NcmCommand_PlayPause);
        else if (y > 500)
            action_x();
        return;
    }
    if (x > 250 && y > 190 && y < 590) {
        if (grid(s))
            selection_ =
                (scroll_ + int((y - 190) / 203)) * 4 + std::clamp(int((x - 256) / 244), 0, 3);
        else
            selection_ = scroll_ + int((y - 190) / 64);
        sidebar_ = false;
        activate();
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
    for (const auto &m : s.queue)
        if (m.id == playback_.track_id) {
            artwork = m.cover;
            break;
        }
    cover(v, s, artwork, -radius * .37f, -radius * .37f, radius * .74f, radius * .37f);
    circle(v, 0, 0, 9, nvgRGB(195, 193, 192));
    circle(v, 0, 0, 4, bg);
    nvgRestore(v);
    // Tonearm lifts away when paused. It is not part of the record transform.
    const float pivot_x = x + radius * .91f, pivot_y = y - radius * .94f;
    nvgSave(v);
    nvgTranslate(v, pivot_x, pivot_y);
    nvgRotate(v, playback_.state == NcmPlayback_Playing ? .4f : .08f);
    line(v, 0, 0, -30, 75, nvgRGB(112, 114, 120), 8);
    line(v, -30, 75, -46, 154, nvgRGB(174, 175, 181), 5);
    rect(v, -56, 145, 18, 35, nvgRGB(205, 204, 207), 4);
    circle(v, 0, 0, 15, raised);
    circle(v, 0, 0, 7, muted);
    nvgRestore(v);
}
void MusicView::paint_sidebar(NVGcontext *v, const Snapshot &s) {
    rect(v, 0, 0, 220, 620, panel);
    line(v, 220, 0, 220, 620, nvgRGB(43, 44, 49));
    circle(v, 44, 53, 20, red);
    circle(v, 44, 53, 11, panel);
    circle(v, 44, 53, 4, red);
    text(v, 76, 50, 20, white, "云音乐");
    text(v, 77, 72, 10, muted, "NETEASE · SWITCH");
    text(v, 28, 125, 10, muted, "M U S I C   E V E R Y W H E R E");
    for (int i = 0; i < 8; ++i) {
        float y = 158 + i * 47;
        bool active = i == static_cast<int>(s.page), focused = sidebar_ && i == nav_;
        if (active || focused)
            rect(v, 14, y, 192, 39, focused ? nvgRGB(91, 40, 48) : nvgRGB(47, 31, 37), 8);
        if (active)
            rect(v, 14, y + 10, 3, 19, red, 1);
        text(v, 31, y + 26, 16, active || focused ? white : muted, labels[i], 163);
        if (active)
            circle(v, 188, y + 20, 3, red);
    }
    circle(v, 36, 578, 12, raised);
    text(v, 59, 584, 13, muted, s.nickname, 144);
}
void MusicView::paint_list(NVGcontext *v, const Snapshot &s) {
    if (!s.collection) {
        if (s.page == Page::Discover) {
            button(v, 256, 125, 132, "每日推荐");
            button(v, 402, 125, 132, "私人 FM");
            button(v, 548, 125, 132, "新碟上架");
            button(v, 694, 125, 146, "X 发现更多");
        } else if (s.page == Page::Library)
            button(v, 256, 125, 210, "X 切换音乐分类");
        else if (s.page == Page::Search) {
            button(v, 256, 125, 165, "A 输入关键词");
            button(v, 437, 125, 220, "X 分类 / 收藏");
        } else
            text(v, 258, 155, 14, muted, "为每一段旅程，收藏一首歌。");
    } else {
        button(v, 256, 125, 180, s.subscribed ? "X 已收藏" : "X 收藏与播放");
        text(v, 457, 151, 14, muted, std::to_string(s.items.size()) + " 首歌曲");
    }
    if (s.items.empty()) {
        circle(v, 750, 331, 53, raised);
        circle(v, 750, 331, 28, nvgRGB(65, 39, 46));
        circle(v, 750, 331, 7, red);
        text(v, 566, 444, 21, white, s.busy ? "正在加载音乐…" : "这里等待你的第一首歌", 540);
        text(v, 522, 481, 15, muted,
             s.busy ? "正在连接网易云音乐" : "X 打开更多功能，或前往我的账号登录", 620);
        return;
    }
    if (grid(s)) {
        for (int k = 0; k < 8; ++k) {
            int i = scroll_ * 4 + k;
            if (i > static_cast<int>(s.items.size()) ||
                (i == static_cast<int>(s.items.size()) && !s.more))
                break;
            float x = 256 + (k % 4) * 244, y = 190 + (k / 4) * 203;
            bool selected = !sidebar_ && i == selection_;
            if (selected) {
                rect(v, x - 6, y - 6, 226, 198, nvgRGB(66, 37, 45), 13);
            }
            if (i == static_cast<int>(s.items.size())) {
                rect(v, x, y, 136, 136, raised, 12);
                text(v, x + 53, y + 81, 36, red, "+", 50);
                text(v, x, y + 161, 16, white, "加载更多…", 216);
                break;
            }
            const auto &m = s.items[i];
            cover(v, s, m.cover, x, y, 136);
            if (selected) {
                circle(v, x + 112, y + 112, 17, red);
                play_icon(v, x + 112, y + 112, true, 14);
            }
            text(v, x, y + 161, 16, white, m.title, 216);
            text(v, x, y + 183, 12, muted,
                 m.artist.empty() ? std::to_string(m.count) + " 首音乐" : m.artist, 216);
        }
    } else {
        for (int k = 0; k < 6; ++k) {
            int i = scroll_ + k;
            if (i > static_cast<int>(s.items.size()))
                break;
            float y = 190 + k * 64;
            bool selected = !sidebar_ && i == selection_;
            if (selected)
                rect(v, 250, y - 2, 988, 61, raised, 9);
            if (i == static_cast<int>(s.items.size())) {
                if (s.more)
                    text(v, 284, y + 36, 17, red, "加载更多结果…");
                break;
            }
            const auto &m = s.items[i];
            bool playing = m.id == playback_.track_id;
            text(v, 268, y + 34, 12, playing ? red : muted, playing ? "▶" : std::to_string(i + 1),
                 45);
            cover(v, s, m.cover, 311, y + 4, 47, 6);
            text(v, 374, y + 25, 17, playing ? red : white, m.title, 575);
            text(v, 374, y + 47, 12, muted, m.artist + (m.album.empty() ? "" : " · " + m.album),
                 620);
            if (s.likes.count(m.id))
                text(v, 1098, y + 32, 17, red, "♥", 35);
            text(v, 1150, y + 33, 13, muted, duration_text(m.duration_ms), 75);
        }
    }
    const float total = std::max<size_t>(1, s.items.size()), visible = grid(s) ? 8 : 6;
    if (total > visible) {
        rect(v, 1250, 190, 3, 386, raised, 1);
        float pos = (grid(s) ? scroll_ * 4 : scroll_) / total;
        rect(v, 1250, 190 + 386 * pos, 3, std::max(18.f, 386 * visible / total), red, 1);
    }
}
void MusicView::paint_player(NVGcontext *v, const Snapshot &s, bool full) {
    bool paused = playback_.state != NcmPlayback_Playing;
    std::string title = playback_.track_id ? playback_.title : "还没有开始播放";
    std::string artist = playback_.track_id ? playback_.artist : "发现一首好歌，让游戏更动听";
    if (full) {
        text(v, 262, 148, 11, red, player_.background() ? "BACKGROUND AUDIO" : "NOW PLAYING");
        vinyl(v, s, 494, 365, 172);
        text(v, 288, 577, 19, white, title, 410);
        text(v, 744, 168, 27, white, title, 483);
        text(v, 746, 204, 16, muted, artist, 475);
        const int active = lyric_index(s.lyrics, playback_.position_ms);
        if (s.lyrics.empty()) {
            text(v, 747, 354, 18, muted,
                 playback_.track_id ? "暂无滚动歌词" : "播放音乐后，黑胶唱片将随音乐旋转", 478);
        } else {
            int start = std::max(0, active - 1);
            for (int i = start; i < std::min<int>(s.lyrics.size(), start + 4); ++i) {
                const auto &lyric = s.lyrics[i].text;
                const auto separator = lyric.find('\n');
                text(v, 747, 274 + (i - start) * 62, i == active ? 20 : 16,
                     i == active ? white : muted, lyric.substr(0, separator), 470);
                if (separator != std::string::npos)
                    text(v, 747, 299 + (i - start) * 62, 13,
                         i == active ? nvgRGB(211, 168, 171) : muted, lyric.substr(separator + 1),
                         470);
            }
        }
        button(v, 746, 548, 224,
               s.likes.count(playback_.track_id) ? "X 已喜欢 · 播放选项" : "X 喜欢 · 播放选项");
        text(v, 995, 575, 12, muted, "← / → 快进 10 秒", 235);
    }
    rect(v, 0, 620, 1280, 100, nvgRGB(25, 26, 31));
    line(v, 0, 620, 1280, 620, nvgRGB(54, 42, 47));
    std::string artwork;
    for (const auto &m : s.queue)
        if (m.id == playback_.track_id) {
            artwork = m.cover;
            break;
        }
    cover(v, s, artwork, 22, 640, 58, 10);
    text(v, 96, 661, 16, white, title, 337);
    text(v, 96, 686, 12, muted, artist, 337);
    text(v, 610, 664, 24, white, "‹", 30);
    circle(v, 700, 657, 24, red);
    play_icon(v, 700, 657, paused, 18);
    text(v, 757, 664, 24, white, "›", 30);
    text(v, 494, 671, 10, muted, duration_text(playback_.position_ms), 70);
    text(v, 921, 671, 10, muted, duration_text(playback_.duration_ms), 70);
    rect(v, 491, 687, 484, 3, nvgRGB(68, 67, 73), 1);
    if (playback_.duration_ms)
        rect(v, 491, 687, 484 * std::min(1.f, float(playback_.position_ms) / playback_.duration_ms),
             3, red, 1);
    text(v, 1036, 650, 10, player_.background() ? nvgRGB(110, 206, 158) : muted,
         player_.background() ? "BGM · 后台服务" : "前台播放");
    text(v, 1036, 679, 11, muted, "音量 " + std::to_string(playback_.volume_permille / 10) + "%");
    rect(v, 1111, 675, 130, 3, raised, 1);
    rect(v, 1111, 675, 130 * playback_.volume_permille / 1000.f, 3, muted, 1);
    if (playback_.state == NcmPlayback_Buffering)
        text(v, 620, 715, 10, red, "正在缓冲…", 350);
    if (playback_.state == NcmPlayback_Error)
        text(v, 495, 715, 10, red, playback_.message, 740);
    if (playback_.state == NcmPlayback_Playing && std::strstr(playback_.message, "preview"))
        text(v, 494, 715, 10, muted, "试听 · 完整播放取决于当前账号和曲目权益", 490);
}
void MusicView::paint_account(NVGcontext *v, const Snapshot &s) {
    rect(v, 288, 187, 350, 350, raised, 20);
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
        int n = qr_->getSize();
        float scale = 280.f / (n + 8);
        rect(v, 323, 222, 280, 280, nvgRGB(255, 255, 255), 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qr_->getModule(x, y))
                    rect(v, 323 + (x + 4) * scale, 222 + (y + 4) * scale, scale + .15f,
                         scale + .15f, nvgRGB(0, 0, 0));
    } else {
        circle(v, 463, 349, 75, nvgRGB(63, 36, 43));
        circle(v, 463, 349, 43, red);
        circle(v, 463, 349, 12, panel);
        text(v, 351, 466, 16, white, s.user_id ? "已登录网易云音乐" : "A 生成登录二维码", 245);
    }
    text(v, 288, 573, 14, muted, s.qr_message.empty() ? "登录状态仅保存于本机 SD 卡" : s.qr_message,
         810);
    const char *options[] = {"扫码登录 / 刷新二维码", "手机号验证码登录", "退出账号", "每日签到"};
    for (int i = 0; i < 4; ++i) {
        bool selected = !sidebar_ && selection_ == i;
        rect(v, 711, 220 + i * 55, 475, 44, selected ? nvgRGB(78, 37, 46) : raised, 8);
        text(v, 734, 249 + i * 55, 17, white, options[i], 430);
    }
    text(v, 714, 481, 14, muted, "登录后同步喜欢的音乐、收藏歌单和专辑。", 480);
    text(v, 714, 509, 13, muted, "扫码需要手机端确认，验证码不会写入日志。", 480);
}
void MusicView::paint_settings(NVGcontext *v, const Snapshot &s) {
    std::vector<std::string> names = {"音质", "网络代理", "停止播放", "退出应用"};
    std::vector<std::string> desc = {s.quality, s.proxy.empty() ? "直连" : s.proxy,
                                     player_.background() ? "独立后台服务" : "本应用音频引擎",
                                     player_.background() ? "退出应用后，后台服务继续播放"
                                                          : "退出应用后，前台音频将停止"};
    for (int i = 0; i < 4; ++i) {
        rect(v, 256, 200 + i * 70, 974, 61, !sidebar_ && selection_ == i ? raised : panel, 9);
        text(v, 280, 235 + i * 70, 18, white, names[i], 270);
        text(v, 573, 235 + i * 70, 14, muted, desc[i], 625);
    }
    text(v, 260, 532, 14, muted, "Netease Switch 0.1.0 · Borealis / libnx · GPL-3.0-or-later", 970);
    text(v, 260, 561, 13, muted, "游戏内播放需要 SD 卡上的后台服务与 Tesla / Ultrahand 覆盖层。",
         970);
}
void MusicView::draw(NVGcontext *v, float x, float y, float w, float h, brls::Style,
                     brls::FrameContext *) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last_frame_).count();
    last_frame_ = now;
    if (now - last_poll_ > std::chrono::milliseconds(200)) {
        playback_ = player_.status();
        last_poll_ = now;
        model_.lyrics_for(playback_.track_id);
        display_ = model_.snapshot();
        std::vector<Media> visible;
        const auto first = static_cast<size_t>(std::max(0, grid(display_) ? scroll_ * 4 : scroll_));
        const auto end = std::min(display_.items.size(), first + (grid(display_) ? 8 : 6));
        for (size_t i = first; i < end; ++i)
            visible.push_back(display_.items[i]);
        for (const auto &track : display_.queue)
            if (track.id == playback_.track_id) {
                visible.push_back(track);
                break;
            }
        model_.ensure_artwork(visible);
    }
    if (playback_.state == NcmPlayback_Playing)
        angle_ = std::fmod(angle_ + std::min(dt, .1f) * 3.4906585f, 6.2831853f);
    const auto &s = display_;
    if (s.page == Page::Account && now - last_qr_ > std::chrono::seconds(3)) {
        model_.poll_qr();
        last_qr_ = now;
    }
    if (revision_ != s.revision) {
        revision_ = s.revision;
        if (s.page != Page::Account && s.page != Page::Settings) {
            selection_ =
                std::clamp(selection_, 0, std::max(0, int(s.items.size()) - (s.more ? 0 : 1)));
            const int columns = grid(s) ? 4 : 1;
            const int rows =
                (static_cast<int>(s.items.size()) + (s.more ? 1 : 0) + columns - 1) / columns;
            scroll_ = std::clamp(scroll_, 0, std::max(0, rows - (grid(s) ? 2 : 6)));
        }
    }
    nvgSave(v);
    nvgTranslate(v, x, y);
    nvgScale(v, w / 1280.f, h / 720.f);
    rect(v, 0, 0, 1280, 720, bg);
    paint_sidebar(v, s);
    text(v, 256, 58, 29, white, s.title, 800);
    text(v, 258, 91, 14, muted, s.page == Page::Player ? "让音乐，陪伴每一次冒险。" : s.subtitle,
         900);
    if (s.busy) {
        circle(v, 1224, 51, 5, red);
        text(v, 1073, 57, 12, muted, "正在连接…", 140);
    }
    if (s.page == Page::Player)
        paint_player(v, s, true);
    else {
        if (s.page == Page::Account)
            paint_account(v, s);
        else if (s.page == Page::Settings)
            paint_settings(v, s);
        else
            paint_list(v, s);
        paint_player(v, s, false);
    }
    if (!s.error.empty()) {
        rect(v, 249, 545, 990, 47, nvgRGB(69, 35, 40), 9);
        text(v, 266, 575, 13, nvgRGB(255, 193, 197), s.error, 955);
    } else if (!s.notice.empty())
        text(v, 257, 607, 12, s.busy ? red : muted, s.notice, 700);
    text(v, 939, 607, 10, muted, "A 选择   B 导航   X 更多   Y 播放", 300);
    nvgRestore(v);
}
} // namespace netease
