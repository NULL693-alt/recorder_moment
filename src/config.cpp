#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
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

// Carpeta conocida de Windows (AppData, Videos...), o vacio si no se pudo.
static fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::string config_default_path() {
    fs::path dir = known_folder(FOLDERID_RoamingAppData);
    if (dir.empty()) return config_legacy_path();
    return wide_to_utf8((dir / L"recorder_moment" / L"recorder_config.ini").wstring().c_str());
}

std::string config_legacy_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "recorder_config.ini";
    fs::path p(buf);
    p = p.parent_path() / L"recorder_config.ini";
    return wide_to_utf8(p.wstring().c_str());
}

std::string config_default_clips_dir() {
    fs::path videos = known_folder(FOLDERID_Videos);
    if (videos.empty()) return ".";
    return wide_to_utf8((videos / L"recorder_moment").wstring().c_str());
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool config_load(AppConfig &cfg, const std::string &path) {
    std::ifstream in(fs::u8path(path));
    if (!in) return false;

    auto flag = [](const std::string &v) { return v == "1" || v == "true"; };
    bool has_record_on_start = false;

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
        else if (key == "audio_enabled") cfg.audio_enabled = flag(val);
        else if (key == "mic_enabled") cfg.mic_enabled = flag(val);
        else if (key == "separate_audio_tracks") cfg.separate_audio_tracks = flag(val);
        else if (key == "output_device") cfg.output_device = val;
        else if (key == "mic_device") cfg.mic_device = val;
        else if (key == "minimize_to_tray") cfg.minimize_to_tray = flag(val);
        else if (key == "record_on_start") {
            cfg.record_on_start = flag(val);
            has_record_on_start = true;
        }
        else if (key == "clip_sound") cfg.clip_sound = flag(val);
        else if (key == "record_cursor") cfg.record_cursor = flag(val);
        else if (key == "clips_by_game") cfg.clips_by_game = flag(val);
        else if (key == "trim_delete_original") cfg.trim_delete_original = flag(val);
        else if (key == "hotkey_vk") cfg.hotkey_vk = atoi(val.c_str());
        else if (key == "hotkey_mods") cfg.hotkey_mods = atoi(val.c_str());
    }

    // Antes "Minimizar a la bandeja" tambien hacia arrancar la grabacion sola:
    // a quien ya lo tenia asi se le respeta.
    if (!has_record_on_start) cfg.record_on_start = cfg.minimize_to_tray;

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
    if (cfg.hotkey_vk <= 0 || cfg.hotkey_vk > 0xFE) cfg.hotkey_vk = 0x78;
    cfg.hotkey_mods &= (kModCtrl | kModAlt | kModShift);
    return true;
}

void config_load_default(AppConfig &cfg) {
    if (config_load(cfg, config_default_path())) return;
    if (config_load(cfg, config_legacy_path())) return;
    cfg.out_dir = config_default_clips_dir();
}

bool config_save(const AppConfig &cfg, const std::string &path) {
    fs::path target = fs::u8path(path);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);

    // Se escribe a un archivo aparte y despues se reemplaza el original.
    fs::path tmp = target;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;

        out << "# recorder_moment â€” ajustes\n";
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
        out << "separate_audio_tracks=" << (cfg.separate_audio_tracks ? 1 : 0) << "\n";
        out << "output_device=" << cfg.output_device << "\n";
        out << "mic_device=" << cfg.mic_device << "\n";
        out << "minimize_to_tray=" << (cfg.minimize_to_tray ? 1 : 0) << "\n";
        out << "record_on_start=" << (cfg.record_on_start ? 1 : 0) << "\n";
        out << "clip_sound=" << (cfg.clip_sound ? 1 : 0) << "\n";
        out << "record_cursor=" << (cfg.record_cursor ? 1 : 0) << "\n";
        out << "clips_by_game=" << (cfg.clips_by_game ? 1 : 0) << "\n";
        out << "trim_delete_original=" << (cfg.trim_delete_original ? 1 : 0) << "\n";
        out << "hotkey_vk=" << cfg.hotkey_vk << "\n";
        out << "hotkey_mods=" << cfg.hotkey_mods << "\n";
        if (!out.flush()) return false;
    }
    return MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string hotkey_name(int vk, int mods) {
    std::string out;
    if (mods & kModCtrl) out += "Ctrl+";
    if (mods & kModAlt) out += "Alt+";
    if (mods & kModShift) out += "Shift+";

    if (vk >= VK_F1 && vk <= VK_F24) return out + "F" + std::to_string(vk - VK_F1 + 1);

    // El nombre que le da Windows en el idioma del teclado ("Inicio", "Q"...).
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lparam = (LONG)(scan << 16);
    switch (vk) { // teclas "extendidas": sin esto salen como las del teclado numerico
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR:
        case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            lparam |= 1 << 24;
            break;
        default:
            break;
    }
    wchar_t name[64] = {0};
    if (scan && GetKeyNameTextW(lparam, name, 64) > 0) return out + wide_to_utf8(name);

    char hex[16];
    snprintf(hex, sizeof(hex), "Tecla 0x%02X", vk);
    return out + hex;
}
