/* SPDX-License-Identifier: GPL-2.0-only */
#include "tesla.hpp"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../vendor/libtesla/stb_truetype.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

#ifndef CLOUDTUNE_PREVIEW_FONT
#define CLOUDTUNE_PREVIEW_FONT "resources/font/chinese_fallback.ttf"
#endif

namespace {
constexpr tsl::Color white(15, 15, 15, 15), gray(10, 10, 10, 15),
    cyan(0, 15, 13, 15);

std::vector<int> codepoints(const std::string &text) {
    std::vector<int> result;
    for (size_t i = 0; i < text.size();) {
        unsigned char first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            result.push_back(first == '\n' || first == '\r' ? ' ' : first);
            continue;
        }
        int count = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
        int value = first & (count == 3 ? 7 : count == 2 ? 15 : 31);
        bool valid = count && i + count <= text.size();
        for (int n = 0; valid && n < count; ++n) {
            unsigned char next = static_cast<unsigned char>(text[i + n]);
            valid = (next & 0xC0) == 0x80;
            value = (value << 6) | (next & 63);
        }
        if (valid) {
            i += count;
            result.push_back(value);
        } else {
            result.push_back(0xFFFD);
        }
    }
    return result;
}

struct ClipGuard {
    SDL_Renderer *renderer;
    SDL_Rect previous{};
    bool hadClip;
    ClipGuard(SDL_Renderer *renderer, SDL_Rect clip) : renderer(renderer) {
        hadClip = SDL_RenderIsClipEnabled(renderer);
        SDL_RenderGetClipRect(renderer, &previous);
        if (hadClip) {
            SDL_Rect intersection{};
            SDL_IntersectRect(&clip, &previous, &intersection);
            clip = intersection;
        }
        SDL_RenderSetClipRect(renderer, &clip);
    }
    ~ClipGuard() { SDL_RenderSetClipRect(renderer, hadClip ? &previous : nullptr); }
};

u64 keyButton(SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_a:
    case SDLK_SPACE: return HidNpadButton_A;
    case SDLK_UP: return HidNpadButton_Up;
    case SDLK_DOWN: return HidNpadButton_Down;
    case SDLK_LEFT: return HidNpadButton_Left;
    case SDLK_RIGHT: return HidNpadButton_Right;
    case SDLK_l: return HidNpadButton_L;
    case SDLK_r: return HidNpadButton_R;
    case SDLK_x: return HidNpadButton_X;
    default: return 0;
    }
}

void drawExplanation(tsl::gfx::Renderer &renderer) {
    renderer.fillRect(448, 0, 832, 720, tsl::Color(1, 1, 2, 15));
    renderer.fillRect(448, 0, 1, 720, tsl::Color(3, 4, 5, 15));
    renderer.fillRect(496, 54, 230, 34, tsl::Color(1, 4, 4, 15));
    renderer.text("DESKTOP PREVIEW", 512, 78, 21, cyan);
    renderer.text("CloudTune NX", 496, 148, 40, white);
    renderer.text("覆盖层桌面模拟", 496, 202, 34, white);
    renderer.text("左侧复用覆盖层界面，连接实际本机后台播放器。", 496, 254, 25, gray);
    renderer.text("本地合成音频 · 初始静音（音量可调）", 496, 294, 25, cyan);
    renderer.fillRect(496, 328, 730, 1, tsl::Color(3, 4, 5, 15));
    renderer.text("操作", 496, 376, 24, cyan);
    renderer.text("鼠标单击 / A / Enter", 496, 420, 25, white);
    renderer.text("执行所选按钮", 854, 420, 25, gray);
    renderer.text("滚轮 / ↑ ↓", 496, 461, 25, white);
    renderer.text("滚动列表 / 选择条目", 854, 461, 25, gray);
    renderer.text("L / R / X", 496, 502, 25, white);
    renderer.text("上一首 / 下一首 / 播放暂停", 854, 502, 23, gray, 378);
    renderer.text("← →", 496, 543, 25, white);
    renderer.text("选中音量后调节", 854, 543, 25, gray);
    renderer.text("Esc / 关闭窗口", 496, 584, 25, white);
    renderer.text("仅关闭此界面", 854, 584, 25, gray);
    renderer.fillRect(496, 628, 730, 1, tsl::Color(3, 4, 5, 15));
    renderer.text("桌面模拟不代表 Atmosphère / Tesla 实机覆盖层验证。", 496, 667, 22, gray);
}
} // namespace

namespace tsl::gfx {
struct Renderer::Impl {
    struct Glyph {
        SDL_Texture *texture = nullptr;
        int width = 0, height = 0, x = 0, y = 0;
        float advance = 0;
    };
    SDL_Renderer *renderer;
    std::vector<unsigned char> fontBytes;
    stbtt_fontinfo font{};
    std::map<std::pair<int, int>, Glyph> glyphs;
    Impl(SDL_Renderer *renderer, const std::string &path) : renderer(renderer) {
        std::ifstream file(path, std::ios::binary);
        fontBytes.assign(std::istreambuf_iterator<char>(file), {});
        if (fontBytes.empty() ||
            !stbtt_InitFont(&font, fontBytes.data(),
                           stbtt_GetFontOffsetForIndex(fontBytes.data(), 0)))
            throw std::runtime_error("Cannot load preview Chinese font: " + path);
    }
    ~Impl() {
        for (auto &[key, glyph] : glyphs)
            SDL_DestroyTexture(glyph.texture);
    }
    Glyph &glyph(int codepoint, int size) {
        auto key = std::make_pair(codepoint, size);
        auto existing = glyphs.find(key);
        if (existing != glyphs.end())
            return existing->second;
        Glyph value;
        const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &bearing);
        value.advance = advance * scale;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(
            &font, scale, scale, codepoint, &value.width, &value.height, &value.x, &value.y);
        if (bitmap && value.width > 0 && value.height > 0) {
            SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
                0, value.width, value.height, 32, SDL_PIXELFORMAT_RGBA32);
            if (!surface) {
                stbtt_FreeBitmap(bitmap, nullptr);
                throw std::runtime_error(SDL_GetError());
            }
            for (int y = 0; y < value.height; ++y) {
                auto *pixels = reinterpret_cast<Uint32 *>(
                    static_cast<Uint8 *>(surface->pixels) + y * surface->pitch);
                for (int x = 0; x < value.width; ++x)
                    pixels[x] = SDL_MapRGBA(surface->format, 255, 255, 255,
                                           bitmap[y * value.width + x]);
            }
            value.texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_FreeSurface(surface);
            if (!value.texture) {
                stbtt_FreeBitmap(bitmap, nullptr);
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(value.texture, SDL_BLENDMODE_BLEND);
        }
        stbtt_FreeBitmap(bitmap, nullptr);
        return glyphs.emplace(key, value).first->second;
    }
};

Renderer::Renderer(SDL_Renderer *renderer, const std::string &fontPath)
    : impl(std::make_unique<Impl>(renderer, fontPath)) {}
Renderer::~Renderer() = default;
SDL_Renderer *Renderer::native() const { return impl->renderer; }

void Renderer::fillRect(int x, int y, int width, int height, Color color) {
    SDL_SetRenderDrawColor(native(), color.r, color.g, color.b, color.a);
    SDL_Rect rect{x, y, width, height};
    SDL_RenderFillRect(native(), &rect);
}
void Renderer::outline(int x, int y, int width, int height, Color color, int thickness) {
    for (int offset = 0; offset < thickness; ++offset) {
        SDL_SetRenderDrawColor(native(), color.r, color.g, color.b, color.a);
        SDL_Rect rect{x + offset, y + offset, width - 2 * offset, height - 2 * offset};
        SDL_RenderDrawRect(native(), &rect);
    }
}
void Renderer::drawCircle(s32 x, s32 y, s32 radius, bool filled, Color color) {
    SDL_SetRenderDrawColor(native(), color.r, color.g, color.b, color.a);
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            int dx = static_cast<int>(std::sqrt(radius * radius - dy * dy));
            SDL_RenderDrawLine(native(), x - dx, y + dy, x + dx, y + dy);
        }
    } else {
        int dx = radius, dy = 0, error = 1 - radius;
        while (dx >= dy) {
            const SDL_Point points[] = {{x + dx, y + dy}, {x + dy, y + dx},
                                        {x - dy, y + dx}, {x - dx, y + dy},
                                        {x - dx, y - dy}, {x - dy, y - dx},
                                        {x + dy, y - dx}, {x + dx, y - dy}};
            SDL_RenderDrawPoints(native(), points, 8);
            ++dy;
            if (error < 0)
                error += 2 * dy + 1;
            else {
                --dx;
                error += 2 * (dy - dx) + 1;
            }
        }
    }
}
float Renderer::textWidth(const std::string &text, int size) {
    float width = 0;
    for (int codepoint : codepoints(text))
        width += impl->glyph(codepoint, size).advance;
    return width;
}
void Renderer::text(const std::string &text, int x, int baseline, int size, Color color,
                    int maxWidth) {
    std::unique_ptr<ClipGuard> guard;
    if (maxWidth > 0)
        guard = std::make_unique<ClipGuard>(native(), SDL_Rect{x, baseline - size * 2,
                                                             maxWidth, size * 3});
    float pen = static_cast<float>(x);
    for (int codepoint : codepoints(text)) {
        auto &glyph = impl->glyph(codepoint, size);
        if (maxWidth > 0 && pen >= x + maxWidth)
            break;
        if (glyph.texture) {
            SDL_SetTextureColorMod(glyph.texture, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(glyph.texture, color.a);
            SDL_Rect destination{static_cast<int>(std::round(pen)) + glyph.x, baseline + glyph.y,
                                 glyph.width, glyph.height};
            SDL_RenderCopy(native(), glyph.texture, nullptr, &destination);
        }
        pen += glyph.advance;
    }
}
} // namespace tsl::gfx

namespace tsl::elm {
void ListItem::draw(gfx::Renderer &renderer) {
    if (focused) {
        const float pulse = (std::sin(SDL_GetTicks64() / 350.0) + 1) / 2;
        const auto r = static_cast<u8>(2 + pulse * 6);
        const auto g = static_cast<u8>(8 + pulse * 7);
        renderer.fillRect(rect.x, rect.y, rect.w, rect.h, Color(0, 2, 2, 15));
        renderer.outline(rect.x - 4, rect.y - 4, rect.w + 8, rect.h + 8,
                         Color(r, g, 15, 15), 4);
    }
    renderer.fillRect(rect.x, rect.y + rect.h - 1, rect.w, 1, Color(4, 4, 4, 15));
    const int valueWidth = static_cast<int>(std::ceil(renderer.textWidth(value, 20)));
    const int labelWidth = std::max(20, rect.w - 40 - (value.empty() ? 0 : valueWidth + 20));
    renderer.text(label, rect.x + 20, rect.y + 45, 23, white, labelWidth);
    if (!value.empty())
        renderer.text(value, rect.x + rect.w - valueWidth - 15, rect.y + 45, 20, cyan,
                      std::max(1, valueWidth));
}
void List::addItem(Element *item, int height) {
    rows.push_back({std::unique_ptr<Element>(item), height});
    if (selected < 0 && item->focusable())
        selected = static_cast<int>(rows.size()) - 1;
}
int List::totalHeight() const {
    int total = 0;
    for (const auto &row : rows)
        total += row.height;
    return total;
}
void List::revealSelected() {
    int top = 0;
    for (int i = 0; i < selected; ++i)
        top += rows[i].height;
    targetScroll = static_cast<float>(
        std::clamp(top - 174, 0, std::max(0, totalHeight() - rect.h + 50)));
}
void List::draw(gfx::Renderer &renderer) {
    scroll += (targetScroll - scroll) * 0.18f;
    if (std::abs(targetScroll - scroll) < 0.2f)
        scroll = targetScroll;
    {
        ClipGuard clip(renderer.native(), {rect.x - 6, rect.y, rect.w + 12, rect.h});
        int y = rect.y - static_cast<int>(std::round(scroll));
        for (size_t i = 0; i < rows.size(); ++i) {
            auto &row = rows[i];
            row.element->bounds(rect.x, y, rect.w, row.height);
            row.element->focused = static_cast<int>(i) == selected;
            if (y + row.height >= rect.y && y <= rect.y + rect.h)
                row.element->draw(renderer);
            y += row.height;
        }
    }
    int total = totalHeight() + 50;
    if (total > rect.h) {
        int height = std::max(24, rect.h * rect.h / total);
        int top = rect.y + static_cast<int>((rect.h - height) * scroll / (total - rect.h));
        renderer.fillRect(rect.x + rect.w + 11, rect.y, 3, rect.h, Color(2, 2, 2, 15));
        renderer.fillRect(rect.x + rect.w + 11, top, 3, height, Color(7, 8, 8, 15));
    }
}
bool List::input(u64 buttons) {
    int direction = buttons & HidNpadButton_Down ? 1 : buttons & HidNpadButton_Up ? -1 : 0;
    if (direction) {
        int next = selected;
        while (next + direction >= 0 && next + direction < static_cast<int>(rows.size())) {
            next += direction;
            if (rows[next].element->focusable()) {
                selected = next;
                revealSelected();
                return true;
            }
        }
        return true;
    }
    return selected >= 0 && rows[selected].element->input(buttons);
}
bool List::click(int x, int y) {
    if (x < rect.x || x >= rect.x + rect.w || y < rect.y || y >= rect.y + rect.h)
        return false;
    int offset = y - rect.y + static_cast<int>(std::round(scroll));
    for (size_t i = 0; i < rows.size(); ++i) {
        if (offset < rows[i].height) {
            if (rows[i].element->focusable()) {
                selected = static_cast<int>(i);
                rows[i].element->input(HidNpadButton_A);
            }
            return true;
        }
        offset -= rows[i].height;
    }
    return false;
}
void List::wheel(int amount) {
    targetScroll = std::clamp(targetScroll - amount * 70.0f, 0.0f,
                             static_cast<float>(std::max(0, totalHeight() - rect.h + 50)));
}
void OverlayFrame::draw(gfx::Renderer &renderer) {
    renderer.fillRect(0, 0, 448, 720, Color(0, 0, 0, 13));
    renderer.text(title, 20, 50, 30, white);
    renderer.text(subtitle, 20, 70, 15, gray);
    renderer.text("桌面模拟 · 非 Switch 实机", 20, 100, 17, cyan);
    if (content) {
        content->bounds(35, 125, 363, 522);
        content->draw(renderer);
    }
    renderer.fillRect(15, 647, 418, 1, Color(7, 7, 7, 15));
    renderer.text("Esc  返回      Enter  确定", 30, 693, 23, white);
}
} // namespace tsl::elm

namespace tsl {
int runPreview(Overlay &overlay, int, char **) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "Preview SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window *window =
        SDL_CreateWindow("CloudTune NX · 覆盖层桌面模拟", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, 1280, 720,
                         SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *native = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED |
                                                                    SDL_RENDERER_PRESENTVSYNC)
                                 : nullptr;
    if (!native && window)
        native = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!window || !native) {
        std::fprintf(stderr, "Preview window creation failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(native);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(native, 1280, 720);
    SDL_SetRenderDrawBlendMode(native, SDL_BLENDMODE_BLEND);
    bool servicesStarted = false;
    int result = 0;
    try {
        gfx::Renderer renderer(native, CLOUDTUNE_PREVIEW_FONT);
        overlay.initServices();
        servicesStarted = true;
        auto gui = overlay.loadInitialGui();
        std::unique_ptr<elm::Element> frame(gui->createUI());
        bool running = true;
        while (running) {
            const Uint64 start = SDL_GetTicks64();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT)
                    running = false;
                else if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE)
                        running = false;
                    else {
                        const u64 button = keyButton(event.key.keysym.sym);
                        const bool navigation = button & (HidNpadButton_Up | HidNpadButton_Down |
                                                          HidNpadButton_Left | HidNpadButton_Right);
                        if (button && (!event.key.repeat || navigation) &&
                            !gui->handleInput(button, button, {}, {}, {}))
                            frame->input(button);
                    }
                } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    // SDL_RenderSetLogicalSize already scales mouse event coordinates.
                    frame->click(event.button.x, event.button.y);
                } else if (event.type == SDL_MOUSEWHEEL) {
                    int x, y;
                    SDL_GetMouseState(&x, &y);
                    float logicalX, logicalY;
                    SDL_RenderWindowToLogical(native, x, y, &logicalX, &logicalY);
                    if (logicalX >= 0 && logicalX < 448 && logicalY >= 125 && logicalY < 647) {
                        const int direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
                        frame->wheel(event.wheel.y * direction);
                    }
                }
            }
            if (!running)
                break;
            gui->update();
            SDL_SetRenderDrawColor(native, 24, 28, 34, 255);
            SDL_RenderClear(native);
            frame->draw(renderer);
            drawExplanation(renderer);
            SDL_RenderPresent(native);
            Uint64 elapsed = SDL_GetTicks64() - start;
            if (elapsed < 16)
                SDL_Delay(static_cast<Uint32>(16 - elapsed));
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Preview failed: %s\n", error.what());
        result = 1;
    }
    if (servicesStarted)
        overlay.exitServices();
    SDL_DestroyRenderer(native);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
} // namespace tsl
