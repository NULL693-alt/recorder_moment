#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string wide_to_utf8(const wchar_t *w) {
    if (!w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out((size_t)(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}

std::string config_default_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "recorder_config.ini";
    fs::path p(buf);
    p = p.parent_path() / L"recorder_config.ini";
    return wide_to_utf8(p.wstring().c_str());
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void config_load(AppConfig &cfg, const std::string &path) {
    std::ifstream in(fs::u8path(path));
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "out_dir") cfg.out_dir = val;
        else if (key == "monitor") cfg.monitor = atoi(val.c_str());
        else if (key == "fps") cfg.fps = atoi(val.c_str());
        else if (key == "kbps") cfg.kbps = atoi(val.c_str());
        else if (key == "clip_seconds") cfg.clip_seconds = atoi(val.c_str());
        else if (key == "audio_kbps") cfg.audio_kbps = atoi(val.c_str());
        else if (key == "mic_volume") cfg.mic_volume = atoi(val.c_str());
        else if (key == "out_height") cfg.out_height = atoi(val.c_str());
        else if (key == "video_codec") cfg.video_codec = (val == "av1") ? "av1" : "h264";
        else if (key == "audio_enabled") cfg.audio_enabled = (val == "1" || val == "true");
        else if (key == "mic_enabled") cfg.mic_enabled = (val == "1" || val == "true");
        else if (key == "output_device") cfg.output_device = val;
        else if (key == "mic_device") cfg.mic_device = val;
        else if (key == "minimize_to_tray") cfg.minimize_to_tray = (val == "1" || val == "true");
        else if (key == "clip_sound") cfg.clip_sound = (val == "1" || val == "true");
        else if (key == "record_cursor") cfg.record_cursor = (val == "1" || val == "true");
    }

    // Valores sanitizados por si el archivo quedo a mano rara.
    if (cfg.fps <= 0) cfg.fps = 60;
    if (cfg.kbps <= 0) cfg.kbps = 12000;
    if (cfg.clip_seconds <= 0) cfg.clip_seconds = 120;
    if (cfg.audio_kbps <= 0) cfg.audio_kbps = 160;
    if (cfg.mic_volume < 0) cfg.mic_volume = 0;
    if (cfg.mic_volume > 200) cfg.mic_volume = 200;
    if (cfg.out_height < 0) cfg.out_height = 0;
    if (cfg.monitor < 0) cfg.monitor = 0;
    if (cfg.out_dir.empty()) cfg.out_dir = ".";
}

void config_save(const AppConfig &cfg, const std::string &path) {
    std::ofstream out(fs::u8path(path), std::ios::trunc);
    if (!out) return;

    out << "# recorder_moment — ajustes\n";
    out << "out_dir=" << cfg.out_dir << "\n";
    out << "monitor=" << cfg.monitor << "\n";
    out << "fps=" << cfg.fps << "\n";
    out << "kbps=" << cfg.kbps << "\n";
    out << "clip_seconds=" << cfg.clip_seconds << "\n";
    out << "audio_kbps=" << cfg.audio_kbps << "\n";
    out << "mic_volume=" << cfg.mic_volume << "\n";
    out << "out_height=" << cfg.out_height << "\n";
    out << "video_codec=" << cfg.video_codec << "\n";
    out << "audio_enabled=" << (cfg.audio_enabled ? 1 : 0) << "\n";
    out << "mic_enabled=" << (cfg.mic_enabled ? 1 : 0) << "\n";
    out << "output_device=" << cfg.output_device << "\n";
    out << "mic_device=" << cfg.mic_device << "\n";
    out << "minimize_to_tray=" << (cfg.minimize_to_tray ? 1 : 0) << "\n";
    out << "clip_sound=" << (cfg.clip_sound ? 1 : 0) << "\n";
    out << "record_cursor=" << (cfg.record_cursor ? 1 : 0) << "\n";
}
