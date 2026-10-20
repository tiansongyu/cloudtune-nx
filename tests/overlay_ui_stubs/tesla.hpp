/* SPDX-License-Identifier: GPL-2.0-only
 * In-memory UI adapter: executes production overlay callbacks without a window. */
#pragma once
#include <switch.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace tsl {
struct Color { Color(int, int, int, int) {} };
namespace gfx {
struct Renderer { void drawCircle(int, int, int, bool, Color) {} };
}
namespace elm {
struct Element { virtual ~Element() = default; };
struct ListItem : Element {
    std::string text, value;
    std::function<bool(u64)> callback;
    ListItem(std::string text, std::string value = "") : text(text), value(value) {}
    void setText(const std::string& next) { text = next; }
    void setValue(const std::string& next) { value = next; }
    void setClickListener(std::function<bool(u64)> next) { callback = std::move(next); }
};
struct CustomDrawer : Element {
    explicit CustomDrawer(std::function<void(gfx::Renderer*, s32, s32, s32, s32)>) {}
};
struct List : Element {
    std::vector<std::unique_ptr<Element>> rows;
    void addItem(Element* item, int = 70) { rows.emplace_back(item); }
};
struct OverlayFrame : Element {
    std::unique_ptr<Element> content;
    OverlayFrame(const std::string&, const std::string&) {}
    void setContent(Element* item) { content.reset(item); }
};
}
struct Gui {
    virtual ~Gui() = default;
    virtual elm::Element* createUI() = 0;
    virtual void update() {}
    virtual bool handleInput(u64, u64, const HidTouchState&,
                             HidAnalogStickState, HidAnalogStickState) { return false; }
};
struct Overlay {
    virtual ~Overlay() = default;
    virtual void initServices() {}
    virtual void exitServices() {}
    virtual std::unique_ptr<Gui> loadInitialGui() = 0;
    template<class T> std::unique_ptr<Gui> initially() { return std::make_unique<T>(); }
};
template<class T> int loop(int, char**) { return 0; }
}
