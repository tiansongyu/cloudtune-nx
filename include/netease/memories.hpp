// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "netease/ui_layout.hpp"
#include <array>
#include <borealis.hpp>
#include <functional>

namespace netease {
// Optional, local-only artwork. Missing files leave the standard theme intact.
class PersonalPhotos {
  public:
    PersonalPhotos();
    ~PersonalPhotos();
    bool available() const {
        return available_;
    }
    void draw(NVGcontext *, int index, ui::Rect, float alpha = 1, bool fill = false,
              float radius = 12);
    void open(bool reduced_motion, float safe_scale, std::function<void()> play_pause);

  private:
    bool available_ = false;
    std::array<int, 5> textures_{};
};
} // namespace netease
