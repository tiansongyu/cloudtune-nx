// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/ui_layout.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace netease::ui;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() try {
    for (auto dimensions : {Rect{0, 0, 1280, 720}, Rect{0, 0, 1920, 1080}, Rect{0, 0, 1920, 1200},
                            Rect{0, 0, 2560, 1080}}) {
        for (float safe : {1.f, .95f, .9f}) {
            const auto viewport = Viewport::fit(dimensions.w, dimensions.h, safe);
            for (auto target : {card(3, 292), setting(5), account(2), transport(5, false)}) {
                const float px = viewport.x + (target.x + target.w / 2) * viewport.scale;
                const float py = viewport.y + (target.y + target.h / 2) * viewport.scale;
                require(target.contains(viewport.logical_x(px), viewport.logical_y(py)),
                        "Scaled touch must hit the rendered control");
            }
            require(viewport.x >= 0 && viewport.y >= 0, "Contain must never crop");
        }
    }
    auto m = move(3, 0, 10, 4, 1, 2);
    require(m.index == 3, "Right edge must not wrap to next row");
    m = move(7, 1, 10, 4, 1, 1);
    require(m.index == 9 && m.scroll == 2, "Partial last row must remain reachable");
    m = move(9, 2, 10, 4, 1, 1);
    require(m.edge == Edge::Below && m.index == 9, "Last row enters playback");
    m = move(4, 1, 10, 4, 1, -2);
    require(m.edge == Edge::Left && m.index == 4, "First column enters sidebar");
    m = move(3, 0, 10, 4, 1, -1);
    require(m.edge == Edge::Above && m.index == 3, "Top row enters header actions");
    m = move(22, 18, 3, 1, 5, 0);
    require(m.index == 2 && m.scroll == 0, "Deleted content must clamp both focus and scroll");
    m = move(0, 0, 0, 4, 1, 1);
    require(m.index == 0 && m.scroll == 0, "Empty content must not create negative focus");
    require(card(0, 292).y + card(0, 292).h + 7 < MiniTop, "Card focus must clear the mini player");
    require(setting(5).y + setting(5).h + 7 < MiniTop,
            "Last settings focus must clear the mini player");
    std::cout << "UI layout and navigation boundaries passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
