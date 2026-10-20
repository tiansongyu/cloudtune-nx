/* SPDX-License-Identifier: GPL-2.0-only
 * Standalone Tesla UI. The playback service remains independent of this UI. */
#define TESLA_INIT_IMPL
#include "netease/playback_client.h"
#include <cmath>
#include <cstdio>
#include <tesla.hpp>

namespace {
NcmPlaybackStatus status{};
bool connected = false;
std::string clockText(uint32_t ms) {
    char text[24];
    std::snprintf(text, sizeof(text), "%u:%02u", ms / 60000, (ms / 1000) % 60);
    return text;
}
class PlayerGui : public tsl::Gui {
    tsl::elm::ListItem *title = nullptr, *artist = nullptr, *play = nullptr, *time = nullptr,
                       *volume = nullptr, *mode = nullptr, *information = nullptr;
    std::chrono::steady_clock::time_point refresh{};
    double angle = 0;
    std::chrono::steady_clock::time_point animation_time = std::chrono::steady_clock::now();
    void control(Result result) {
        if (R_FAILED(result)) {
            ncmPlaybackExit();
            connected = false;
        }
        refresh = {};
    }
    void button(tsl::elm::List *list, const char *label, std::function<Result()> action) {
        auto *item = new tsl::elm::ListItem(label);
        item->setClickListener([this, action](u64 keys) {
            if (!(keys & HidNpadButton_A))
                return false;
            control(action());
            return true;
        });
        list->addItem(item);
    }

  public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("网易云 · Switch", "后台播放器 / v0.1.0");
        auto *list = new tsl::elm::List();
        auto *disc =
            new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32) {
                int cx = x + w / 2, cy = y + 62;
                r->drawCircle(cx, cy, 54, true, tsl::Color(1, 1, 1, 15));
                for (int radius = 25; radius < 53; radius += 5)
                    r->drawCircle(cx, cy, radius, false, tsl::Color(3, 3, 3, 15));
                r->drawCircle(cx, cy, 20, true, tsl::Color(13, 2, 3, 15));
                r->drawCircle(cx + static_cast<int>(std::cos(angle) * 13),
                              cy + static_cast<int>(std::sin(angle) * 13), 3, true,
                              tsl::Color(15, 14, 13, 15));
                r->drawCircle(cx, cy, 3, true, tsl::Color(1, 1, 1, 15));
            });
        list->addItem(disc, 125);
        title = new tsl::elm::ListItem("请在主应用中选择歌曲");
        list->addItem(title);
        artist = new tsl::elm::ListItem("");
        list->addItem(artist);
        time = new tsl::elm::ListItem("0:00 / 0:00");
        list->addItem(time);
        play = new tsl::elm::ListItem("播放 / 暂停");
        play->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A))
                return false;
            control(ncmPlaybackPlayPause());
            return true;
        });
        list->addItem(play);
        button(list, "上一首  /  L", [] { return ncmPlaybackPrevious(); });
        button(list, "下一首  /  R", [] { return ncmPlaybackNext(); });
        button(list, "后退 15 秒", [] {
            return ncmPlaybackSeek(status.position_ms > 15000 ? status.position_ms - 15000 : 0);
        });
        button(list, "前进 15 秒", [] {
            return ncmPlaybackSeek(std::min(status.position_ms + 15000, status.duration_ms));
        });
        volume = new tsl::elm::ListItem("音量", "70%");
        volume->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_Left) {
                control(ncmPlaybackSetVolume(
                    status.volume_permille > 50 ? status.volume_permille - 50 : 0));
                return true;
            }
            if (keys & (HidNpadButton_A | HidNpadButton_Right)) {
                control(ncmPlaybackSetVolume(std::min(status.volume_permille + 50, 1000u)));
                return true;
            }
            return false;
        });
        list->addItem(volume);
        mode = new tsl::elm::ListItem("循环模式", "顺序");
        mode->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A))
                return false;
            control(ncmPlaybackSetRepeat(static_cast<NcmRepeatMode>((status.repeat + 1) % 3)));
            return true;
        });
        list->addItem(mode);
        button(list, "开启 / 关闭随机播放", [] { return ncmPlaybackSetShuffle(!status.shuffle); });
        button(list, "停止播放", [] { return ncmPlaybackStop(); });
        button(list, "启动 / 重连后台服务", [] {
            ncmPlaybackLaunchBackground();
            return ncmPlaybackInitialize();
        });
        information = new tsl::elm::ListItem("正在连接…");
        list->addItem(information);
        frame->setContent(list);
        return frame;
    }
    void update() override {
        auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - animation_time).count();
        animation_time = now;
        if (connected && status.state == NcmPlayback_Playing)
            angle = std::fmod(angle + std::min(dt, 0.1) * 3.4906585, 6.2831853);
        if (now < refresh)
            return;
        refresh = now + std::chrono::milliseconds(500);
        if (!connected)
            connected = R_SUCCEEDED(ncmPlaybackInitialize());
        if (connected && R_FAILED(ncmPlaybackGetStatus(&status))) {
            ncmPlaybackExit();
            connected = false;
        }
        if (!connected) {
            information->setText("请安装或启动后台服务");
            return;
        }
        title->setText(status.title[0] ? status.title : "请在主应用中选择歌曲");
        artist->setText(status.artist);
        time->setText(clockText(status.position_ms) + " / " + clockText(status.duration_ms));
        play->setText(status.state == NcmPlayback_Playing ? "暂停" : "播放");
        volume->setValue(std::to_string(status.volume_permille / 10) + "%");
        mode->setValue(status.repeat == NcmRepeat_One   ? "单曲"
                       : status.repeat == NcmRepeat_All ? "列表"
                                                        : "顺序");
        information->setText(status.message);
    }
    bool handleInput(u64 keys, u64, const HidTouchState &, HidAnalogStickState,
                     HidAnalogStickState) override {
        if (keys & HidNpadButton_L) {
            control(ncmPlaybackPrevious());
            return true;
        }
        if (keys & HidNpadButton_R) {
            control(ncmPlaybackNext());
            return true;
        }
        if (keys & HidNpadButton_X) {
            control(ncmPlaybackPlayPause());
            return true;
        }
        return false;
    }
};
class MusicOverlay : public tsl::Overlay {
  public:
    void initServices() override {
        // Tesla's initialization wrapper releases its temporary SM reference.
        // Retain one for reconnecting or launching the service from update().
        smInitialize();
        connected = R_SUCCEEDED(ncmPlaybackInitialize());
    }
    void exitServices() override {
        ncmPlaybackExit();
        smExit();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<PlayerGui>();
    }
};
} // namespace
int main(int argc, char **argv) {
    return tsl::loop<MusicOverlay>(argc, argv);
}
