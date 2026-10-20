/* SPDX-License-Identifier: GPL-2.0-only */
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#define main cloudtune_overlay_entry
#include "../overlay/source/main.cpp"
#undef main

namespace {
NcmPlaybackStatus response{};
bool serviceAvailable = true;
bool failStatus = false;
uint32_t seekArgument = 0;
unsigned commands = 0, launches = 0;
}
extern "C" {
Result ncmPlaybackInitialize() { return serviceAvailable ? 0 : 1; }
void ncmPlaybackExit() {}
Result ncmPlaybackGetStatus(NcmPlaybackStatus* out) {
    *out = failStatus ? NcmPlaybackStatus{} : response;
    return failStatus ? 1 : 0;
}
Result ncmPlaybackPlayPause() { ++commands; return 0; }
Result ncmPlaybackPrevious() { ++commands; return 0; }
Result ncmPlaybackNext() { ++commands; return 0; }
Result ncmPlaybackStop() { ++commands; return 0; }
Result ncmPlaybackSetVolume(uint32_t) { ++commands; return 0; }
Result ncmPlaybackSetRepeat(NcmRepeatMode) { ++commands; return 0; }
Result ncmPlaybackSetShuffle(bool) { ++commands; return 0; }
Result ncmPlaybackSeek(uint32_t value) { ++commands; seekArgument = value; return 0; }
Result ncmPlaybackLaunchBackground() { ++launches; serviceAvailable = true; return 0; }
}

int main() {
    PlayerGui gui;
    std::unique_ptr<tsl::elm::Element> element(gui.createUI());
    auto* frame = dynamic_cast<tsl::elm::OverlayFrame*>(element.get());
    auto* list = dynamic_cast<tsl::elm::List*>(frame->content.get());
    auto item = [&](size_t index) {
        auto* row = dynamic_cast<tsl::elm::ListItem*>(list->rows.at(index).get());
        assert(row);
        return row;
    };
    assert(list->rows.size() == 15);
    connected = true;
    status.position_ms = 12000;
    status.duration_ms = 0;
    assert(item(8)->callback(HidNpadButton_A) && seekArgument == 27000);
    status.duration_ms = 20000;
    item(8)->callback(HidNpadButton_A);
    assert(seekArgument == 20000);
    status.position_ms = UINT32_MAX - 3;
    status.duration_ms = 0;
    item(8)->callback(HidNpadButton_A);
    assert(seekArgument == UINT32_MAX);
    status.position_ms = 1000;
    item(7)->callback(HidNpadButton_A);
    assert(seekArgument == 0);

    std::strcpy(response.title, "Synthetic song");
    std::strcpy(response.artist, "Synthetic artist");
    response.state = NcmPlayback_Playing;
    response.position_ms = 12345;
    response.duration_ms = 60000;
    response.volume_permille = 700;
    gui.update();
    assert(item(1)->text == "Synthetic song" && item(4)->text == "暂停");
    failStatus = true;
    // A control action requests the next immediate status refresh.
    gui.handleInput(HidNpadButton_X, 0, {}, {}, {});
    gui.update();
    assert(!connected);
    assert(item(1)->text == "后台服务未连接");
    assert(item(2)->text.empty() && item(3)->text == "--:-- / --:--");
    assert(item(4)->text == "播放 / 暂停");
    assert(item(9)->value == "--" && item(10)->value == "--");
    const unsigned previousCommands = commands;
    for (size_t index = 4; index <= 12; ++index)
        item(index)->callback(HidNpadButton_A);
    gui.handleInput(HidNpadButton_X, 0, {}, {}, {});
    assert(commands == previousCommands);
    failStatus = false;
    serviceAvailable = false;
    item(13)->callback(HidNpadButton_A);
    assert(launches == 1);
    gui.update();
    assert(connected && item(1)->text == "Synthetic song" && item(9)->value == "70%");
    std::cout << "PASS overlay callbacks: unknown duration, saturated seek, disconnect and reconnect\n";
}
