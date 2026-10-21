// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/memories.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>

namespace netease {
namespace {
const char *paths[] = {BRLS_ASSET("personal/panorama.png"), BRLS_ASSET("personal/1.jpg"),
                       BRLS_ASSET("personal/2.jpg"), BRLS_ASSET("personal/3.jpg"),
                       BRLS_ASSET("personal/4.jpg")};
const char *captions[] = {"和你一起的日常", "在一起的时光", "一起走过的夜晚", "晚风里的你"};
const auto canvas = nvgRGB(16, 17, 20), ink = nvgRGB(245, 243, 239), muted = nvgRGB(172, 174, 183);
void box(NVGcontext *v, ui::Rect r, NVGcolor color, float radius = 0) {
    nvgBeginPath(v);
    nvgRoundedRect(v, r.x, r.y, r.w, r.h, radius);
    nvgFillColor(v, color);
    nvgFill(v);
}
void label(NVGcontext *v, float x, float y, float size, NVGcolor color, const std::string &s) {
    nvgFontFaceId(v, brls::Application::getFont(brls::FONT_CHINESE_SIMPLIFIED));
    nvgFontSize(v, size);
    nvgTextAlign(v, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    nvgFillColor(v, color);
    nvgText(v, x, y, s.c_str(), nullptr);
}
ui::Rect thumbnail(int index) {
    return {932.f + (index % 2) * 144, 266.f + (index / 2) * 142, 128, 118};
}

class MemoryGallery : public brls::View {
    PersonalPhotos &photos_;
    bool reduced_;
    float safe_scale_;
    int selected_ = 0, previous_ = 0;
    std::chrono::steady_clock::time_point changed_ = std::chrono::steady_clock::now();
    void choose(int index) {
        index = std::clamp(index, 0, 3);
        if (index != selected_) {
            previous_ = selected_;
            selected_ = index;
            changed_ = std::chrono::steady_clock::now();
        }
    }
    void close() {
        brls::Application::popActivity(reduced_ ? brls::TransitionAnimation::NONE
                                                : brls::TransitionAnimation::FADE);
    }

  public:
    MemoryGallery(PersonalPhotos &photos, bool reduced, float scale, std::function<void()> play)
        : photos_(photos), reduced_(reduced), safe_scale_(scale) {
        setFocusable(true);
        setHideHighlight(true);
        setHideClickAnimation(true);
        const auto bind = [this](brls::ControllerButton key, std::function<void()> action) {
            registerAction(
                "", key,
                [action](brls::View *) {
                    action();
                    return true;
                },
                true);
        };
        bind(brls::BUTTON_B, [this] { close(); });
        bind(brls::BUTTON_NAV_LEFT, [this] { choose(ui::move(selected_, 0, 4, 2, 2, -2).index); });
        bind(brls::BUTTON_NAV_RIGHT, [this] { choose(ui::move(selected_, 0, 4, 2, 2, 2).index); });
        bind(brls::BUTTON_NAV_UP, [this] { choose(ui::move(selected_, 0, 4, 2, 2, -1).index); });
        bind(brls::BUTTON_NAV_DOWN, [this] { choose(ui::move(selected_, 0, 4, 2, 2, 1).index); });
        bind(brls::BUTTON_LB, [this] { choose(selected_ - 1); });
        bind(brls::BUTTON_RB, [this] { choose(selected_ + 1); });
        bind(brls::BUTTON_Y, play);
        registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_SPACE), [play](brls::View *) {
            play();
            return true;
        });
        addGestureRecognizer(
            new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound *) {
                if (status.state != brls::GestureState::END)
                    return;
                const auto frame = getFrame();
                const auto vp = ui::Viewport::fit(frame.getWidth(), frame.getHeight(), safe_scale_);
                const float x = vp.logical_x(status.position.x - frame.getMinX());
                const float y = vp.logical_y(status.position.y - frame.getMinY());
                if (ui::Rect{48, 26, 200, 48}.contains(x, y))
                    close();
                else
                    for (int i = 0; i < 4; ++i)
                        if (thumbnail(i).contains(x, y))
                            choose(i);
            }));
    }
    void show(std::function<void()> cb, bool animate, float) override {
        View::show(std::move(cb), animate && !reduced_, 220);
    }
    void hide(std::function<void()> cb, bool animate, float) override {
        View::hide(std::move(cb), animate && !reduced_, 160);
    }
    void draw(NVGcontext *v, float x, float y, float w, float h, brls::Style,
              brls::FrameContext *) override {
        nvgSave(v);
        box(v, {x, y, w, h}, canvas);
        const auto vp = ui::Viewport::fit(w, h, safe_scale_);
        nvgTranslate(v, x + vp.x, y + vp.y);
        nvgScale(v, vp.scale, vp.scale);
        nvgIntersectScissor(v, 0, 0, 1280, 720);
        photos_.draw(v, 0, {0, 0, 1280, 664}, .12f, false, 0);
        box(v, {0, 0, 1280, 664}, nvgRGBA(16, 17, 20, 115));
        label(v, 52, 58, 17, muted, "‹  返回音乐");
        label(v, 1163, 58, 14, muted, "0" + std::to_string(selected_ + 1) + " / 04");
        // Contain the original photograph: no face crops, recoloring or AI reconstruction.
        box(v, {64, 94, 808, 508}, nvgRGBA(8, 9, 12, 210), 16);
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - changed_).count();
        const float blend = reduced_ ? 1.f : std::clamp(elapsed / .22f, 0.f, 1.f);
        if (blend < 1 && selected_ != previous_)
            photos_.draw(v, previous_ + 1, {78, 108, 780, 480}, 1 - blend);
        photos_.draw(v, selected_ + 1, {78, 108, 780, 480}, blend);
        label(v, 932, 134, 12, nvgRGB(255, 136, 147), "OUR MOMENTS");
        label(v, 932, 181, 34, ink, "我们的时光");
        label(v, 933, 217, 16, muted, "你在身边，音乐也在。");
        for (int i = 0; i < 4; ++i) {
            const auto r = thumbnail(i);
            box(v, r, nvgRGB(32, 31, 33), 10);
            photos_.draw(v, i + 1, {r.x + 6, r.y + 6, r.w - 12, r.h - 12}, 1, false, 6);
            if (selected_ == i) {
                nvgBeginPath(v);
                nvgRoundedRect(v, r.x - 4, r.y - 4, r.w + 8, r.h + 8, 13);
                nvgStrokeColor(v, ink);
                nvgStrokeWidth(v, 3);
                nvgStroke(v);
            }
        }
        label(v, 932, 575, 18, ink, captions[selected_]);
        label(v, 64, 634, 14, muted, "把喜欢的歌，留给一起走过的时光。");
        box(v, {0, 664, 1280, 56}, nvgRGB(19, 20, 24));
        label(v, 52, 697, 16, ink, "B  返回");
        label(v, 202, 697, 16, muted, "方向键  选择照片");
        label(v, 442, 697, 16, muted, "L / R  上一张 / 下一张");
        label(v, 980, 697, 16, muted, "Y  播放 / 暂停音乐");
        nvgRestore(v);
    }
};
} // namespace

PersonalPhotos::PersonalPhotos() {
    available_ = std::all_of(std::begin(paths), std::end(paths),
                             [](const char *path) { return std::ifstream(path).good(); });
}
PersonalPhotos::~PersonalPhotos() {
    if (auto *v = brls::Application::getNVGContext())
        for (int texture : textures_)
            if (texture > 0)
                nvgDeleteImage(v, texture);
}
void PersonalPhotos::draw(NVGcontext *v, int index, ui::Rect r, float alpha, bool fill,
                          float radius) {
    if (!available_ || index < 0 || index >= static_cast<int>(textures_.size()))
        return;
    auto &texture = textures_[index];
    if (!texture)
        texture = nvgCreateImage(v, paths[index], 0);
    if (texture <= 0) {
        texture = -1;
        return;
    }
    int width = 0, height = 0;
    nvgImageSize(v, texture, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    const float scale =
        fill ? std::max(r.w / width, r.h / height) : std::min(r.w / width, r.h / height);
    const float iw = width * scale, ih = height * scale;
    const float ix = r.x + (r.w - iw) / 2, iy = r.y + (r.h - ih) / 2;
    nvgBeginPath(v);
    if (fill)
        nvgRoundedRect(v, r.x, r.y, r.w, r.h, radius);
    else
        nvgRoundedRect(v, ix, iy, iw, ih, radius);
    nvgFillPaint(v, nvgImagePattern(v, ix, iy, iw, ih, 0, texture, alpha));
    nvgFill(v);
}
void PersonalPhotos::open(bool reduced_motion, float safe_scale, std::function<void()> play_pause) {
    if (available_) {
        // Load the bounded set once on entry; switching photographs then only
        // draws cached textures and does not decode files during the transition.
        if (auto *v = brls::Application::getNVGContext())
            for (size_t i = 1; i < textures_.size(); ++i)
                if (!textures_[i]) {
                    textures_[i] = nvgCreateImage(v, paths[i], 0);
                    if (!textures_[i])
                        textures_[i] = -1;
                }
        brls::Application::pushActivity(
            new brls::Activity(
                new MemoryGallery(*this, reduced_motion, safe_scale, std::move(play_pause))),
            reduced_motion ? brls::TransitionAnimation::NONE : brls::TransitionAnimation::FADE);
    }
}
} // namespace netease
