// ============================================================================
// game_detect.cpp — ver game_detect.h
// ============================================================================
#include "game_detect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

static std::string to_utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

static std::wstring lower(std::wstring s) {
    for (wchar_t &c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::string sanitize_folder_name(const std::string &name) {
    std::wstring in = to_wide(name), out;
    for (wchar_t c : in) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) {
            out += L' ';
        } else if (c == 0x00AE || c == 0x2122 || c == 0x00A9) { // (R) (TM) (C)
            continue;
        } else {
            out += c;
        }
    }
    // Espacios repetidos -> uno; sin espacios ni puntos en los bordes.
    std::wstring clean;
    for (wchar_t c : out) {
        if (c == L' ' && (clean.empty() || clean.back() == L' ')) continue;
        clean += c;
    }
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) clean.pop_back();
    while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'.')) clean.erase(0, 1);
    if (clean.size() > 50) {
        clean.resize(50);
        while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) clean.pop_back();
    }
    // Nombres reservados de Windows (CON, NUL, COM1...).
    std::wstring l = lower(clean);
    static const wchar_t *reserved[] = {L"con", L"prn", L"aux", L"nul", L"com1", L"com2",
                                        L"com3", L"com4", L"lpt1", L"lpt2", L"lpt3"};
    for (const wchar_t *r : reserved)
        if (l == r) clean += L"_";
    return to_utf8(clean);
}

// Un campo del recurso de version del .exe (ProductName, FileDescription).
static std::wstring version_string(const std::wstring &exe, const wchar_t *field) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!size) return std::wstring();
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return std::wstring();

    struct Lang {
        WORD lang, cp;
    } *langs = nullptr;
    UINT len = 0;
    std::vector<std::wstring> keys;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &len)) {
        for (UINT i = 0; i < len / sizeof(Lang); i++) {
            wchar_t key[96];
            swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\%ls", langs[i].lang, langs[i].cp, field);
            keys.push_back(key);
        }
    }
    keys.push_back(std::wstring(L"\\StringFileInfo\\040904b0\\") + field);
    keys.push_back(std::wstring(L"\\StringFileInfo\\040904e4\\") + field);
    for (const std::wstring &k : keys) {
        wchar_t *val = nullptr;
        UINT vlen = 0;
        if (VerQueryValueW(data.data(), k.c_str(), (void **)&val, &vlen) && val && vlen > 1)
            return std::wstring(val);
    }
    return std::wstring();
}

// Nombres que no dicen nada del juego (del motor, de Windows...).
static bool generic_name(const std::wstring &s) {
    std::wstring l = lower(s);
    if (l.empty()) return true;
    static const wchar_t *bad[] = {L"operating system", L"sistema operativo", L"bootstrappackagedgame",
                                   L"unrealgame", L"ue4game", L"ue5game", L"unreal engine",
                                   L"unity player", L"launcher", L"game"};
    for (const wchar_t *b : bad) {
        if (l == b) return true;
        if ((wcscmp(b, L"operating system") == 0 || wcscmp(b, L"sistema operativo") == 0) &&
            l.find(b) != std::wstring::npos)
            return true;
    }
    return false;
}

std::string foreground_app_name() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return std::string();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return std::string();

    wchar_t title_buf[256] = L"";
    GetWindowTextW(hwnd, title_buf, 256);
    std::wstring title = title_buf;

    std::wstring exe;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) exe.assign(buf, n);
        CloseHandle(h);
    }
    std::wstring exe_name = exe.substr(exe.find_last_of(L"\\/") + 1);
    std::wstring exe_l = lower(exe_name);

    if (exe_l == L"explorer.exe") return "Escritorio";

    std::wstring name;
    if (!exe.empty() && exe_l != L"applicationframehost.exe") {
        name = version_string(exe, L"ProductName");
        if (generic_name(name)) name = version_string(exe, L"FileDescription");
        if (generic_name(name)) name.clear();
    }
    if (name.empty()) name = title;
    if (name.empty() && !exe_name.empty()) { // "VALORANT-Win64-Shipping.exe" -> "VALORANT"
        name = exe_name.substr(0, exe_name.find_last_of(L'.'));
        size_t dash = name.find(L"-Win64");
        if (dash != std::wstring::npos) name.resize(dash);
    }
    std::string out = sanitize_folder_name(to_utf8(name));
    if (out == kEditedFolder) out += " (juego)"; // no se mezcla con los recortes
    return out;
}
