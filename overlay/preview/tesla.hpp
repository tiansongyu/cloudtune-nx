/* SPDX-License-Identifier: GPL-2.0-only
 * Desktop-only adapter for the small Tesla API used by overlay/source/main.cpp.
 * Production Switch builds continue to include the upstream libtesla header.
 */
#pragma once

#include <switch.h>
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tsl {
struct Color {
    u8 r, g, b, a;
    constexpr Color(u8 red, u8 green, u8 blue, u8 alpha)
        : r(red * 17), g(green * 17), b(blue * 17), a(alpha * 17) {}
};

namespace gfx {
class Renderer {
  public:
    Renderer(SDL_Renderer *renderer, const std::string &fontPath);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void drawCircle(s32 x, s32 y, s32 radius, bool filled, Color color);
    void fillRect(int x, int y, int width, int height, Color color);
    void outline(int x, int y, int width, int height, Color color, int thickness = 1);
    float textWidth(const std::string &text, int size);
    void text(const std::string &text, int x, int baseline, int size, Color color,
              int maxWidth = 0);
    SDL_Renderer *native() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace gfx

namespace elm {
class Element {
  public:
    virtual ~Element() = default;
    virtual void draw(gfx::Renderer &renderer) = 0;
    virtual bool input(u64) { return false; }
    virtual bool click(int, int) { return false; }
    virtual void wheel(int) {}
    virtual bool focusable() const { return false; }
    void bounds(int x, int y, int width, int height) {
        rect = {x, y, width, height};
    }
    SDL_Rect rect{};
    bool focused = false;
};

class CustomDrawer : public Element {
  public:
    using Callback = std::function<void(gfx::Renderer *, s32, s32, s32, s32)>;
    explicit CustomDrawer(Callback callback) : callback(std::move(callback)) {}
    void draw(gfx::Renderer &renderer) override {
        callback(&renderer, rect.x, rect.y, rect.w, rect.h);
    }

  private:
    Callback callback;
};

class ListItem : public Element {
  public:
    explicit ListItem(std::string text, std::string value = "")
        : label(std::move(text)), value(std::move(value)) {}
    void setText(const std::string &text) { label = text; }
    void setValue(const std::string &text) { value = text; }
    void setClickListener(std::function<bool(u64)> handler) { listener = std::move(handler); }
    bool focusable() const override { return true; }
    bool input(u64 buttons) override { return listener && listener(buttons); }
    void draw(gfx::Renderer &renderer) override;

  private:
    std::string label, value;
    std::function<bool(u64)> listener;
};

class List : public Element {
  public:
    void addItem(Element *item, int height = 70);
    void draw(gfx::Renderer &renderer) override;
    bool input(u64 buttons) override;
    bool click(int x, int y) override;
    void wheel(int amount) override;

  private:
    struct Row {
        std::unique_ptr<Element> element;
        int height;
    };
    std::vector<Row> rows;
    int selected = -1;
    float scroll = 0, targetScroll = 0;
    int totalHeight() const;
    void revealSelected();
};

class OverlayFrame : public Element {
  public:
    OverlayFrame(std::string title, std::string subtitle)
        : title(std::move(title)), subtitle(std::move(subtitle)) {}
    void setContent(Element *element) { content.reset(element); }
    void draw(gfx::Renderer &renderer) override;
    bool input(u64 buttons) override { return content && content->input(buttons); }
    bool click(int x, int y) override { return content && content->click(x, y); }
    void wheel(int amount) override {
        if (content)
            content->wheel(amount);
    }

  private:
    std::string title, subtitle;
    std::unique_ptr<Element> content;
};
} // namespace elm

class Gui {
  public:
    virtual ~Gui() = default;
    virtual elm::Element *createUI() = 0;
    virtual void update() {}
    virtual bool handleInput(u64, u64, const HidTouchState &, HidAnalogStickState,
                             HidAnalogStickState) {
        return false;
    }
};

class Overlay {
  public:
    virtual ~Overlay() = default;
    virtual void initServices() {}
    virtual void exitServices() {}
    virtual std::unique_ptr<Gui> loadInitialGui() = 0;
    template <typename G, typename... Args> std::unique_ptr<Gui> initially(Args &&...args) {
        return std::make_unique<G>(std::forward<Args>(args)...);
    }
};

int runPreview(Overlay &overlay, int argc, char **argv);
template <typename O> int loop(int argc, char **argv) {
    O overlay;
    return runPreview(overlay, argc, argv);
}
} // namespace tsl
