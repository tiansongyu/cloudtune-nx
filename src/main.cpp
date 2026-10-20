// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/ui.hpp"
#include <borealis.hpp>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return 1;
    if (!brls::Application::init())
        return 1;
    brls::Application::createWindow("Netease Switch");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    brls::Application::setLimitedFPS(60);
    // Bundle an OFL CJK font, including on emulator systems without shared fonts.
    int font = nvgCreateFont(brls::Application::getNVGContext(), "ncm",
                             BRLS_ASSET("font/chinese_fallback.ttf"));
    if (font < 0) {
        std::cerr << "CJK font failed to load\n";
        return 1;
    }
#ifdef __SWITCH__
    std::string directory = "sdmc:/config/netease-switch";
#else
    const char *custom = std::getenv("NETEASE_SWITCH_DATA");
    std::string directory = custom ? custom
                                   : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                                         "/.local/share/netease-switch";
#endif
    std::filesystem::create_directories(directory);
    netease::Model model(directory);
    netease::Player player(directory);
    model.set_play_callback([&](const std::vector<netease::Media> &list, size_t index) {
        if (!player.play(list, index))
            model.report(player.error());
        else
            model.report(player.background() ? "已交由后台播放，可返回 HOME 开始游戏"
                                             : "前台播放：游戏内 BGM 需要安装后台服务");
    });
    // Stop network/audio workers while libnx, ROMFS and graphics still exist.
    auto shutdown = [&] {
        // Stop local audio first; the remote service deliberately survives.
        player.shutdown();
        model.stop();
    };
    brls::Application::getExitEvent()->subscribe(shutdown);
    brls::Application::pushActivity(new brls::Activity(new netease::MusicView(model, player)));
    model.start();
    int frame_limit = 0;
#ifndef __SWITCH__
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--frames")
            frame_limit = std::atoi(argv[i + 1]);
#endif
    int frames = 0;
    while (brls::Application::mainLoop()) {
        if (frame_limit > 0 && ++frames >= frame_limit)
            brls::Application::quit();
    }
    shutdown();
    curl_global_cleanup();
    return 0;
}
