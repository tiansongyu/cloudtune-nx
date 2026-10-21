// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace netease::ui {
// Every painted control and touch target uses this same logical canvas.
constexpr float Width = 1280, Height = 720, MainLeft = 248, MainRight = 1240;
constexpr float MiniTop = 580, HintsTop = 664;
struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};
struct Viewport {
    float x, y, scale;
    static Viewport fit(float w, float h, float safe = 1) {
        const float scale = std::min(w / Width, h / Height) * safe;
        return {(w - Width * scale) / 2, (h - Height * scale) / 2, scale};
    }
    float logical_x(float px) const {
        return (px - x) / scale;
    }
    float logical_y(float py) const {
        return (py - y) / scale;
    }
};
enum class Region { Sidebar, Header, Content, Playback };
enum class Edge { None, Left, Above, Below };
struct Move {
    int index, scroll;
    Edge edge = Edge::None;
};
inline int clamp_scroll(int scroll, int count, int columns, int visible) {
    return std::clamp(scroll, 0, std::max(0, (count + columns - 1) / columns - visible));
}
inline Move move(int index, int scroll, int count, int columns, int visible, int direction) {
    Move result{std::clamp(index, 0, std::max(0, count - 1)),
                clamp_scroll(scroll, count, columns, visible)};
    if (direction == -2) {
        if (result.index % columns == 0)
            result.edge = Edge::Left;
        else
            --result.index;
    } else if (direction == 2) {
        if (result.index % columns < columns - 1 && result.index + 1 < count)
            ++result.index;
    } else if (direction == -1) {
        if (result.index < columns)
            result.edge = Edge::Above;
        else
            result.index -= columns;
    } else if (direction == 1) {
        if (result.index / columns >= (std::max(1, count) - 1) / columns)
            result.edge = Edge::Below;
        else
            result.index = std::min(count - 1, result.index + columns);
    }
    const int row = result.index / columns;
    if (row < result.scroll)
        result.scroll = row;
    if (row >= result.scroll + visible)
        result.scroll = row - visible + 1;
    return result;
}
inline Rect card(int column, float top) {
    return {MainLeft + column * 258.f, top, 218, 273};
}
inline Rect row(int row, float top) {
    return {MainLeft, top + row * 58.f, 992, 50};
}
inline Rect setting(int row) {
    return {MainLeft, 112.f + row * 73, 992, 65};
}
inline Rect account(int row) {
    return {660, 260.f + row * 56, 540, 44};
}
inline Rect transport(int index, bool full) {
    if (full) {
        const Rect controls[] = {{80, 576, 160, 52}, {259, 576, 52, 52}, {323, 576, 52, 52},
                                 {387, 576, 52, 52}, {458, 576, 52, 52}, {1120, 576, 80, 52}};
        return controls[std::clamp(index, 0, 5)];
    }
    const Rect controls[] = {{24, 594, 368, 56}, {426, 596, 44, 44},  {478, 596, 44, 44},
                             {530, 596, 44, 44}, {1000, 596, 60, 44}, {1072, 596, 168, 44}};
    return controls[std::clamp(index, 0, 5)];
}
} // namespace netease::ui
