// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "netease/memories.hpp"
#include "netease/model.hpp"
#include "netease/player.hpp"
#include "netease/ui_layout.hpp"
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
    friend struct MusicViewTestAccess;
    void switch_page(Page);
    void back();
    void confirm_exit();
    void display_options();
    void open_memories();
    void save_preferences();
    void remember(const Snapshot &);
    void reconcile(const Snapshot &);
    void reset_content();
    void activate_header(const Snapshot &);
    void activate_transport(int);
    std::vector<std::pair<ui::Rect, std::string>> headers(const Snapshot &) const;
    int item_count(const Snapshot &) const;
    int visible_rows(const Snapshot &) const;
    float content_top(const Snapshot &) const;
    void focus_ring(NVGcontext *, ui::Rect, float radius = 10);
    void paint_hints(NVGcontext *, const Snapshot &);
    void action_x();
    void tap(float x, float y);
    void scroll_rows(int rows);
    void input_search();
    void account_tools();
    void account_actions(uint64_t user_id);
    void airplay_tools();
    void options(const std::string &, const std::vector<std::string> &, std::function<void(int)>,
                 int selected = 0);
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
    NcmAirplayStatus airplay_{};
    Snapshot display_;
    std::map<std::string, int> textures_;
    int app_icon_ = -1;
    PersonalPhotos memories_;
    std::unique_ptr<qrcodegen::QrCode> qr_;
    std::string qr_source_;
    int selection_ = 0, scroll_ = 0, nav_ = 0;
    ui::Region region_ = ui::Region::Content;
    int header_ = 0, transport_ = 0;
    bool player_open_ = false, reduced_motion_ = false;
    bool photo_background_ = true;
    float safe_scale_ = 1;
    struct Bookmark {
        int selection = 0, scroll = 0, header = 0;
        ui::Region region = ui::Region::Content;
        std::string item;
    };
    std::map<std::string, Bookmark> bookmarks_;
    std::string view_key_, anchor_, last_focus_, last_notice_, collection_cover_;
    std::string current_artwork_, previous_artwork_;
    uint64_t account_id_ = 0, last_track_ = 0;
    float focus_time_ = 1, page_time_ = 1, press_time_ = 1, notice_time_ = 10;
    float cover_time_ = 1, tonearm_ = .08f, lyric_offset_ = 0;
    int last_lyric_ = -1;
    ui::Rect focus_target_{0, 0, 0, 0};
    float angle_ = 0;
    float pan_distance_ = 0;
    std::chrono::steady_clock::time_point last_frame_, last_poll_, last_qr_, last_airplay_poll_;
};
} // namespace netease
