// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "netease/model.hpp"
#include "netease/player.hpp"
#include "qrcodegen.hpp"
#include <borealis.hpp>
#include <chrono>
#include <map>

namespace netease {
class MusicView : public brls::View {
  public:
    MusicView(Model &model, Player &player);
    ~MusicView() override;
    void draw(NVGcontext *, float, float, float, float, brls::Style, brls::FrameContext *) override;
    void navigate(int direction);
    void activate();

  private:
    void switch_page(Page);
    void action_x();
    void tap(float x, float y);
    void scroll_rows(int rows);
    void input_search();
    void phone_login();
    void options(const std::string &, const std::vector<std::string> &, std::function<void(int)>);
    void paint_sidebar(NVGcontext *, const Snapshot &);
    void paint_list(NVGcontext *, const Snapshot &);
    void paint_player(NVGcontext *, const Snapshot &, bool full);
    void paint_account(NVGcontext *, const Snapshot &);
    void paint_settings(NVGcontext *, const Snapshot &);
    void cover(NVGcontext *, const Snapshot &, const std::string &, float, float, float,
               float radius = 12);
    void vinyl(NVGcontext *, const Snapshot &, float, float, float);
    void text(NVGcontext *, float, float, float, NVGcolor, const std::string &,
              float max_width = 1000);
    void button(NVGcontext *, float, float, float, const std::string &, bool selected = false);
    Model &model_;
    Player &player_;
    NcmPlaybackStatus playback_{};
    Snapshot display_;
    std::map<std::string, int> textures_;
    std::unique_ptr<qrcodegen::QrCode> qr_;
    std::string qr_source_;
    int selection_ = 0, scroll_ = 0, nav_ = 0, revision_ = -1;
    bool sidebar_ = false;
    float angle_ = 0;
    float pan_distance_ = 0;
    std::chrono::steady_clock::time_point last_frame_, last_poll_, last_qr_;
};
} // namespace netease
