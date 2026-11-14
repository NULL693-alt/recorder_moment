// ============================================================================
// gui.cpp — interfaz grafica: Dear ImGui sobre Direct3D 11 + bandeja (tray)
//
// La ventana hace tres cosas:
//   1. Mostrar y editar los ajustes (se guardan en recorder_config.ini).
//   2. Arrancar/parar el grabador y pedirle clips (el Recorder corre en un
//      hilo aparte; aca solo se lo controla y se mira su estado).
//   3. Vivir en la bandeja del sistema, asi se puede cerrar la ventana y
//      seguir grabando de fondo.
//
// El dibujado es el clasico de ImGui: cada vuelta del bucle de mensajes se
// arma un frame, se dibuja la interfaz y se presenta el swapchain de D3D11.
// ============================================================================

#include "gui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "audio_devices.h"
#include "clip_editor.h"
#include "config.h"
#include "game_detect.h"
#include "recorder.h"

// El backend de Win32 deja esta declaracion dentro de un '#if 0' (para no
// arrastrar <windows.h> a quien incluya su header), asi que la ponemos aca.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

// Mensajes propios de la ventana.
#define WM_TRAYICON (WM_APP + 1)
#define WM_CLIP_NOTICE (WM_APP + 3) // termino un pedido de clip (lParam = ClipNotice*)
#define TRAY_UID 1

// Lo que manda el hilo de guardado a la ventana al terminar un clip.
struct ClipNotice {
    bool ok;
    std::string msg; // ruta del clip, o por que fallo
};

// Id del icono dentro de resources/recorder.rc.
#define IDI_APPICON 101

// Ids del menu de la bandeja.
enum {
    IDM_SHOW = 1001,
    IDM_STARTSTOP,
    IDM_SAVE,
    IDM_OPEN,
    IDM_OPEN_LAST,
    IDM_EDIT_LAST,
    IDM_EXIT,
};

// ---------------------------------------------------------------------------
// Direct3D 11
// ---------------------------------------------------------------------------
static ID3D11Device *g_d3dDevice = nullptr;
static ID3D11DeviceContext *g_d3dContext = nullptr;
static IDXGISwapChain *g_swapChain = nullptr;
static bool g_swapChainOccluded = false;
static UINT g_resizeWidth = 0, g_resizeHeight = 0;
static ID3D11RenderTargetView *g_renderTarget = nullptr;

// ---------------------------------------------------------------------------
// Estado de la aplicacion
// ---------------------------------------------------------------------------
struct GuiState {
    AppConfig cfg;                  // ajustes actuales
    std::string config_path;        // ruta de recorder_config.ini
    char out_dir_buf[512] = {0};    // carpeta de clips (buffer del InputText)

    std::unique_ptr<Recorder> recorder;
    std::thread rec_thread;
    std::atomic<bool> rec_finished{false};
    bool rec_seen_running = false;  // ya estuvo grabando (para detectar el fin)
    bool stopping = false;          // se pidio detener; el hilo todavia esta cerrando
    ULONGLONG restart_at = 0;       // la grabacion se corto sola: cuando reiniciarla (0 = no)
    std::vector<ULONGLONG> restart_times; // reinicios del ultimo minuto

    std::mutex log_mutex;
    std::vector<std::string> log_lines;
    std::atomic<unsigned long> log_version{0};
    std::vector<std::string> log_snapshot;
    unsigned long log_local_version = 0;
    bool log_auto_scroll = true;

    std::vector<AudioDeviceInfo> outputs;
    std::vector<AudioDeviceInfo> inputs;
    int out_sel = 0;
    int mic_sel = 0;

    bool tray_added = false;
    bool want_quit = false;
    bool startup_enabled = false;   // "Iniciar con Windows"
    std::wstring startup_other;     // si la clave Run apunta a OTRA copia del .exe, su ruta
    std::wstring last_clip;         // ultimo clip guardado (para abrirlo desde el aviso)

    std::vector<MonitorDesc> monitors; // monitores, en el orden de la captura
    bool capturing_hotkey = false;     // esperando la tecla nueva para el atajo
    bool capture_armed = false;        // ya se soltaron todas las teclas (empieza a escuchar)
};

static GuiState g;
static HWND g_hwnd = nullptr;

// Pestaña "Editor": un clip abierto, su vista previa y el tramo a recortar.
struct ClipEntry {
    std::string path;  // ruta completa (UTF-8)
    std::string label; // nombre + tamaño, para la lista
};

struct EditorState {
    std::vector<ClipEntry> clips;  // clips de la carpeta, el mas nuevo primero
    bool list_dirty = true;
    std::string listed_dir;
    std::string path;              // clip abierto
    std::string open_error;
    std::string open_request;      // pedido de abrir (desde la bandeja)
    bool auto_opened = false;      // ya se abrio solo el mas nuevo
    bool select_tab = false;       // traer la pestaña al frente
    bool active = false;           // la pestaña se esta viendo

    ClipPlayer player;
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint8_t> frame;

    double sel_start = 0.0, sel_end = 0.0; // tramo a guardar (segundos)
    int drag = -1;                 // arrastrando: 0 cabezal, 1 inicio, 2 fin
    double last_seek = -1.0;

    std::thread save_thread;
    std::atomic<bool> saving{false}, save_finished{false};
    bool save_ok = false;
    bool delete_after = false;     // al terminar, el original va a la Papelera
    std::string save_msg;          // resultado del ultimo recorte
    std::string saved_path;
};

static EditorState ed;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string wide_to_utf8(const wchar_t *w) {
    if (!w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out((size_t)(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring out((size_t)(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

static int index_of_device(const std::vector<AudioDeviceInfo> &list, const std::string &id) {
    for (int i = 0; i < (int)list.size(); i++)
        if (list[i].id == id) return i;
    return 0;
}

// Guarda en cfg lo que el usuario toco en la ventana (ids de dispositivos
// incluidos) y lo baja a disco.
static void sync_config_from_ui() {
    g.cfg.out_dir = g.out_dir_buf;
    if (g.cfg.out_dir.empty()) g.cfg.out_dir = ".";
    if (!g.outputs.empty() && g.out_sel >= 0 && g.out_sel < (int)g.outputs.size())
        g.cfg.output_device = g.outputs[g.out_sel].id;
    if (!g.inputs.empty() && g.mic_sel >= 0 && g.mic_sel < (int)g.inputs.size())
        g.cfg.mic_device = g.inputs[g.mic_sel].id;
}

static void save_config() {
    sync_config_from_ui();
    config_save(g.cfg, g.config_path);
}

static void refresh_devices() {
    std::string out_id = g.outputs.empty() ? g.cfg.output_device
                                           : g.outputs[(size_t)g.out_sel].id;
    std::string mic_id = g.inputs.empty() ? g.cfg.mic_device
                                          : g.inputs[(size_t)g.mic_sel].id;
    g.outputs = list_output_devices();
    g.inputs = list_input_devices();
    g.monitors = DxgiCapture::enumerate_monitors();
    g.out_sel = index_of_device(g.outputs, out_id);
    g.mic_sel = index_of_device(g.inputs, mic_id);
}

// ---------------------------------------------------------------------------
// Grabador: arrancar / parar / recoger
// ---------------------------------------------------------------------------
static void start_recording() {
    if (g.recorder) return;
    g.restart_at = 0;

    save_config();
    Recorder::clear_stop();
    g.rec_finished.store(false);
    g.rec_seen_running = false;

    g.recorder = std::make_unique<Recorder>(g.cfg);
    g.recorder->set_log_callback([](const std::string &m) {
        std::lock_guard<std::mutex> lk(g.log_mutex);
        g.log_lines.push_back(m);
        if (g.log_lines.size() > 500)
            g.log_lines.erase(g.log_lines.begin(), g.log_lines.begin() + 100);
        g.log_version.fetch_add(1);
    });
    g.recorder->set_clip_callback([](bool ok, const std::string &msg) {
        // Llega desde el hilo de guardado: se lo pasamos a la ventana, que es la
        // que puede tocar el icono de la bandeja.
        ClipNotice *n = new ClipNotice{ok, msg};
        if (!PostMessageW(g_hwnd, WM_CLIP_NOTICE, 0, (LPARAM)n)) delete n;
    });

    Recorder *r = g.recorder.get();
    g.rec_thread = std::thread([r]() {
        r->run();
        g.rec_finished.store(true);
    });
}

// Solo pide detener: el hilo de grabacion cierra todo (audio, el ultimo clip
// que se este escribiendo...) por su cuenta y reap_recorder() lo junta cuando
// termina. Asi la ventana no se congela mientras tanto.
static void stop_recording() {
    if (!g.recorder || g.stopping) return;
    Recorder::request_stop();
    g.stopping = true;
}

// Se llama cada vuelta: si el hilo de grabacion termino (error o parada), lo
// junta y limpia.
//
// Si se corto sola (nadie apreto "Detener"), avisa con el cartel rojo y, si
// venia grabando, la vuelve a arrancar a los 2 s. Para no quedar dando vueltas
// con algo que no tiene arreglo, se rinde al tercer corte en un minuto.
static void show_toast(bool ok, const std::wstring &title, const std::wstring &text);

static void gui_log(const std::string &m) {
    std::lock_guard<std::mutex> lk(g.log_mutex);
    g.log_lines.push_back(m);
    g.log_version.fetch_add(1);
}

static void reap_recorder() {
    if (!g.recorder) {
        if (g.restart_at && GetTickCount64() >= g.restart_at) start_recording();
        return;
    }
    if (!g.rec_finished.load()) return;
    if (g.rec_thread.joinable()) g.rec_thread.join();
    const bool on_its_own = !g.stopping && !g.want_quit;
    const bool was_recording = g.recorder->ever_ran();
    g.recorder.reset();
    g.rec_finished.store(false);
    g.rec_seen_running = false;
    g.stopping = false;
    Recorder::clear_stop();
    if (!on_its_own) return;

    const ULONGLONG now = GetTickCount64();
    while (!g.restart_times.empty() && now - g.restart_times.front() > 60000)
        g.restart_times.erase(g.restart_times.begin());
    if (g.cfg.clip_sound) MessageBeep(MB_ICONHAND);
    if (was_recording && g.restart_times.size() < 3) {
        g.restart_times.push_back(now);
        g.restart_at = now + 2000;
        gui_log("[grabador] la grabacion se corto sola: se reinicia en 2 s");
        show_toast(false, L"La grabacion se corto", L"Se reinicia sola en un momento...");
    } else {
        gui_log(was_recording
                    ? "[grabador] la grabacion se corto varias veces seguidas: queda detenida"
                    : "[grabador] no se pudo iniciar la grabacion (mira los mensajes de arriba)");
        show_toast(false, was_recording ? L"La grabacion se detuvo" : L"No se pudo iniciar la grabacion",
                   L"Abri recorder_moment para ver que paso");
    }
}

// true mientras graba de verdad (ni arrancando ni deteniendose).
static bool is_recording() { return g.recorder && !g.rec_finished.load() && !g.stopping; }

static void request_save_clip() {
    if (is_recording()) g.recorder->request_save();
}

// Icono de la aplicacion (el del .rc), o el generico de Windows si falta.
static HICON load_app_icon() {
    HICON ico = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    if (!ico) ico = LoadIcon(nullptr, IDI_APPLICATION);
    return ico;
}

// ---------------------------------------------------------------------------
// Arranque con Windows
//
// Usamos la clave "Run" del usuario (HKEY_CURRENT_USER), asi no hace falta ser
// administrador. Windows lee esa clave al iniciar sesion y ejecuta lo que haya
// anotado ahi.
// ---------------------------------------------------------------------------
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"recorder_moment"

// Ruta completa del .exe que se esta ejecutando.
static std::wstring exe_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(buf, n);
}

// Compara dos rutas de Windows sin distinguir mayusculas (van con '\').
static bool same_path(const std::wstring &a, const std::wstring &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

// Ruta del .exe anotada en la clave Run (sin comillas); vacio si no hay.
static std::wstring startup_stored_path() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::wstring();
    wchar_t buf[1024] = {0};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LONG r = RegQueryValueExW(key, RUN_VALUE, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::wstring();

    // Lo guardamos como "C:\ruta\recorder.exe" --startup: nos quedamos con la
    // ruta de adentro de las comillas.
    std::wstring stored(buf);
    if (!stored.empty() && stored.front() == L'"') {
        size_t close = stored.find(L'"', 1);
        stored = stored.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
    }
    return stored;
}

// Valor de la clave Run para este .exe: la ruta entre comillas (por si tiene
// espacios) y --startup para saber que lo abrio Windows.
static std::wstring startup_value() { return L"\"" + exe_path() + L"\" --startup"; }

// true si la clave Run ya apunta a este mismo .exe. Si apunta a otra copia (por
// ejemplo, la de dist\ mientras corres la de build\), la casilla sale
// desmarcada y la interfaz dice cual es la que arranca.
static bool startup_registered() {
    std::wstring stored = startup_stored_path();
    if (stored.empty()) return false;
    std::wstring exe = exe_path();
    if (exe.empty()) return true;
    return same_path(stored, exe);
}

static void refresh_startup_state() {
    g.startup_enabled = startup_registered();
    std::wstring stored = startup_stored_path();
    g.startup_other = (!stored.empty() && !g.startup_enabled) ? stored : std::wstring();
}

// Anota (o borra) el arranque automatico. Devuelve false si Windows lo rechazo.
static bool set_startup_registered(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (enabled) {
        if (!exe_path().empty()) {
            std::wstring value = startup_value();
            LONG r = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)value.c_str(),
                                    (DWORD)((value.size() + 1) * sizeof(wchar_t)));
            ok = (r == ERROR_SUCCESS);
        }
    } else {
        LONG r = RegDeleteValueW(key, RUN_VALUE);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

// ---------------------------------------------------------------------------
// Bandeja del sistema
// ---------------------------------------------------------------------------
static void tray_add(HWND hwnd) {
    if (g.tray_added) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = load_app_icon();
    wcscpy_s(nid.szTip, L"recorder_moment");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    g.tray_added = true;
}

static void tray_remove(HWND hwnd) {
    if (!g.tray_added) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.tray_added = false;
}

static void open_clips_folder() {
    std::string dir = g.out_dir_buf[0] ? g.out_dir_buf : ".";
    std::wstring w = utf8_to_wide(dir);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Aviso en pantalla ("Clip guardado")
//
// Una ventanita propia en la esquina superior derecha del monitor donde estas
// jugando, que se va sola a los 2,5 s. No usamos las notificaciones de Windows
// porque se pueden tener apagadas (o el "No molestar" las oculta mientras
// jugas). La ventana no roba el foco, deja pasar los clics y esta excluida de
// la captura, asi que no aparece en los clips. (En pantalla completa exclusiva
// no se ve: ahi queda solo el sonido.)
// ---------------------------------------------------------------------------
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#define TOAST_TIMER 1

static HWND g_toast = nullptr;
static bool g_toast_ok = true;
static std::wstring g_toast_title, g_toast_text;
static float g_toast_scale = 1.0f;

static LRESULT WINAPI ToastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const float s = g_toast_scale;

            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 36));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            RECT bar = {0, 0, (int)(6 * s), rc.bottom};
            HBRUSH accent = CreateSolidBrush(g_toast_ok ? RGB(46, 204, 113) : RGB(231, 76, 60));
            FillRect(dc, &bar, accent);
            DeleteObject(accent);

            SetBkMode(dc, TRANSPARENT);
            HFONT title = CreateFontW(-(int)(17 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                      DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT body = CreateFontW(-(int)(13 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            int left = (int)(20 * s);
            RECT rt = {left, (int)(12 * s), rc.right - (int)(12 * s), (int)(36 * s)};
            HGDIOBJ old = SelectObject(dc, title);
            SetTextColor(dc, RGB(240, 240, 245));
            DrawTextW(dc, g_toast_title.c_str(), -1, &rt, DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT rb = {left, (int)(38 * s), rc.right - (int)(12 * s), rc.bottom - (int)(8 * s)};
            SelectObject(dc, body);
            SetTextColor(dc, RGB(170, 170, 180));
            DrawTextW(dc, g_toast_text.c_str(), -1, &rb, DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            DeleteObject(title);
            DeleteObject(body);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            KillTimer(hwnd, TOAST_TIMER);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void create_toast(HINSTANCE inst) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RecorderMomentToast";
    RegisterClassExW(&wc);

    g_toast = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                  WS_EX_LAYERED | WS_EX_TRANSPARENT,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                              inst, nullptr);
    if (!g_toast) return;
    SetLayeredWindowAttributes(g_toast, 0, 235, LWA_ALPHA);
    // Que no salga en los clips (ni en OBS/Discord). Windows 10 2004+.
    SetWindowDisplayAffinity(g_toast, WDA_EXCLUDEFROMCAPTURE);
    // Esquinas redondeadas en Windows 11 (en Windows 10 se ignora).
    DWORD round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_toast, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round,
                          sizeof(round));
}

static void show_toast(bool ok, const std::wstring &title, const std::wstring &text) {
    if (!g_toast) return;
    g_toast_ok = ok;
    g_toast_title = title;
    g_toast_text = text;

    // En el monitor de la ventana que tenga el foco (el juego).
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    g_toast_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    if (g_toast_scale <= 0.0f) g_toast_scale = 1.0f;

    int w = (int)(340 * g_toast_scale);
    int h = (int)(64 * g_toast_scale);
    int margin = (int)(24 * g_toast_scale);
    int x = mi.rcMonitor.right - w - margin;
    int y = mi.rcMonitor.top + margin;
    SetWindowPos(g_toast, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toast, nullptr, TRUE);
    SetTimer(g_toast, TOAST_TIMER, 2500, nullptr);
}

// Aviso al terminar un clip: el cartel en pantalla y, si esta activado, un
// sonido corto (se oye aunque estes en un juego en pantalla completa).
static void notify_clip(bool ok, const std::string &msg) {
    if (g.cfg.clip_sound) MessageBeep(ok ? MB_OK : MB_ICONHAND);

    if (ok) {
        ed.list_dirty = true; // que aparezca en la lista del editor
        g.last_clip = utf8_to_wide(msg);
        size_t slash = g.last_clip.find_last_of(L"\\/");
        show_toast(true, L"Clip guardado",
                   slash == std::wstring::npos ? g.last_clip : g.last_clip.substr(slash + 1));
    } else {
        show_toast(false, L"No se guardo el clip", utf8_to_wide(msg));
    }
}

// Abre el Explorador con el ultimo clip seleccionado (o la carpeta de clips).
static void open_last_clip() {
    if (g.last_clip.empty()) {
        open_clips_folder();
        return;
    }
    std::wstring args = L"/select,\"" + g.last_clip + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void show_window(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}


static void tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    bool running = is_recording();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Mostrar ventana");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.stopping ? MF_GRAYED : 0u), IDM_STARTSTOP,
                g.stopping ? L"Deteniendo..."
                           : (g.recorder ? L"Detener grabacion" : L"Iniciar grabacion"));
    AppendMenuW(menu, MF_STRING | (running ? 0u : MF_GRAYED), IDM_SAVE,
                L"Guardar clip");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Abrir carpeta de clips");
    AppendMenuW(menu, MF_STRING | (g.last_clip.empty() ? MF_GRAYED : 0u), IDM_OPEN_LAST,
                L"Mostrar el ultimo clip");
    AppendMenuW(menu, MF_STRING | (g.last_clip.empty() ? MF_GRAYED : 0u), IDM_EDIT_LAST,
                L"Recortar el ultimo clip");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Salir");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessage(hwnd, WM_NULL, 0, 0);
}

static void tray_command(HWND hwnd, int id) {
    switch (id) {
        case IDM_SHOW:
            show_window(hwnd);
            break;
        case IDM_STARTSTOP:
            if (g.recorder)
                stop_recording();
            else
                start_recording();
            break;
        case IDM_SAVE:
            request_save_clip();
            break;
        case IDM_OPEN:
            open_clips_folder();
            break;
        case IDM_OPEN_LAST:
            open_last_clip();
            break;
        case IDM_EDIT_LAST:
            if (!g.last_clip.empty()) {
                ed.open_request = wide_to_utf8(g.last_clip.c_str());
                ed.select_tab = true;
                show_window(hwnd);
            }
            break;
        case IDM_EXIT:
            g.want_quit = true;
            DestroyWindow(hwnd);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Direct3D 11: creacion y limpieza (lo estandar de los ejemplos de ImGui)
// ---------------------------------------------------------------------------
static void create_render_target() {
    ID3D11Texture2D *backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

static bool create_device_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
        &sd, &g_swapChain, &g_d3dDevice, &level, &g_d3dContext);
    if (res == DXGI_ERROR_UNSUPPORTED) { // sin GPU: probamos el render por software
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
            &sd, &g_swapChain, &g_d3dDevice, &level, &g_d3dContext);
    }
    if (res != S_OK) return false;

    create_render_target();
    return true;
}

static void cleanup_device_d3d() {
    cleanup_render_target();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_d3dContext) { g_d3dContext->Release(); g_d3dContext = nullptr; }
    if (g_d3dDevice) { g_d3dDevice->Release(); g_d3dDevice = nullptr; }
}

// ---------------------------------------------------------------------------
// Ventana
// ---------------------------------------------------------------------------
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeWidth = (UINT)LOWORD(lParam);
            g_resizeHeight = (UINT)HIWORD(lParam);
            return 0;

        case WM_SYSCOMMAND:
            // Bloquea el menu de sistema de Alt (como en los ejemplos de ImGui).
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_TRAYICON:
            switch (LOWORD(lParam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    tray_menu(hwnd);
                    return 0;
                case WM_LBUTTONDBLCLK:
                    show_window(hwnd);
                    return 0;
                default:
                    return 0;
            }

        case WM_CLIP_NOTICE: {
            std::unique_ptr<ClipNotice> n(reinterpret_cast<ClipNotice *>(lParam));
            notify_clip(n->ok, n->msg);
            return 0;
        }

        case WM_COMMAND:
            tray_command(hwnd, LOWORD(wParam));
            return 0;

        case WM_RECORDER_SHOW: // alguien abrio el programa otra vez
            show_window(hwnd);
            return 0;

        case WM_CLOSE:
            if (g.cfg.minimize_to_tray && !g.want_quit) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Interfaz
// ---------------------------------------------------------------------------
static void draw_device_combo(const char *label, const std::vector<AudioDeviceInfo> &list,
                              int &sel) {
    ImGui::TextUnformatted(label);
    if (list.empty()) {
        ImGui::TextDisabled("(no se encontraron dispositivos)");
        return;
    }
    if (sel < 0 || sel >= (int)list.size()) sel = 0;
    ImGui::SetNextItemWidth(-1.0f);
    std::string hidden_label = "##";
    hidden_label += label;
    if (ImGui::BeginCombo(hidden_label.c_str(), list[(size_t)sel].name.c_str())) {
        for (int i = 0; i < (int)list.size(); i++) {
            bool selected = (i == sel);
            if (ImGui::Selectable(list[(size_t)i].name.c_str(), selected)) sel = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// Alturas de salida ofrecidas. 0 = la resolucion nativa del monitor.
static const struct {
    const char *label;
    int height;
} kResolutions[] = {
    {"Nativa (Resolucion original)", 0}, {"2160p (4K Ultra HD)", 2160},
    {"1440p (2K Quad HD)", 1440},        {"1080p (Full HD)", 1080},
    {"720p (HD)", 720},                  {"480p (SD)", 480},
    {"360p (Baja)", 360},
};

static void draw_resolution_combo() {
    ImGui::Text("Resolucion de salida");
    const int n = (int)(sizeof(kResolutions) / sizeof(kResolutions[0]));
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (kResolutions[i].height == g.cfg.out_height) cur = i;

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##res_combo", kResolutions[cur].label)) {
        for (int i = 0; i < n; i++) {
            bool selected = (i == cur);
            if (ImGui::Selectable(kResolutions[i].label, selected))
                g.cfg.out_height = kResolutions[i].height;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Menos resolucion = archivos mas livianos");
}

// Codec de video. "id" es lo que va en recorder_config.ini.
static const struct {
    const char *label;
    const char *id;
} kCodecs[] = {
    {"H.264 (se reproduce en todos lados)", "h264"},
    {"AV1 (mejor calidad por Mbps, necesita GPU nueva)", "av1"},
};

static void draw_codec_combo() {
    ImGui::Text("Codec de video");
    const int n = (int)(sizeof(kCodecs) / sizeof(kCodecs[0]));
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (g.cfg.video_codec == kCodecs[i].id) cur = i;

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##codec_combo", kCodecs[cur].label)) {
        for (int i = 0; i < n; i++) {
            bool selected = (i == cur);
            if (ImGui::Selectable(kCodecs[i].label, selected)) g.cfg.video_codec = kCodecs[i].id;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (g.cfg.video_codec == "av1") {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Con AV1 podes bajar los Mbps un 30-40%% y se ve igual (clips mas "
                           "livianos). Para verlos en Windows hace falta la \"Extension de "
                           "video AV1\" (gratis en Microsoft Store); VLC, Chrome y Discord ya "
                           "lo reproducen. Si tu GPU no codifica AV1, se graba en H.264.");
        ImGui::PopStyleColor();
    }
}

// Monitor a grabar, por nombre (en el mismo orden en que lo busca la captura).
static void draw_monitor_combo() {
    ImGui::Text("Monitor");
    ImGui::SetNextItemWidth(-1.0f);
    if (g.monitors.empty()) { // no se pudieron listar: queda el numero
        ImGui::SliderInt("##monitor", &g.cfg.monitor, 0, 7);
        return;
    }
    auto label = [](int i) {
        const MonitorDesc &m = g.monitors[(size_t)i];
        char buf[256];
        snprintf(buf, sizeof(buf), "%d: %s (%dx%d%s)", i + 1, m.name.c_str(), m.width, m.height,
                 m.primary ? ", principal" : "");
        return std::string(buf);
    };
    int cur = g.cfg.monitor;
    std::string preview = (cur >= 0 && cur < (int)g.monitors.size())
                              ? label(cur)
                              : "Monitor " + std::to_string(cur + 1) + " (no conectado)";
    if (ImGui::BeginCombo("##monitor_combo", preview.c_str())) {
        for (int i = 0; i < (int)g.monitors.size(); i++) {
            bool selected = (i == cur);
            if (ImGui::Selectable(label(i).c_str(), selected)) g.cfg.monitor = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// Teclas que no sirven solas como atajo: botones del mouse y modificadores.
static bool is_modifier_or_mouse(int vk) {
    switch (vk) {
        case VK_LBUTTON: case VK_RBUTTON: case VK_CANCEL: case VK_MBUTTON:
        case VK_XBUTTON1: case VK_XBUTTON2:
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

// Atajo para guardar clip. Al tocar "Cambiar" se espera la proxima tecla (con
// Ctrl/Alt/Shift si estan apretados); Esc cancela.
static void draw_hotkey_setting() {
    ImGui::Text("Atajo para guardar un clip");
    if (!g.capturing_hotkey) {
        std::string name = hotkey_name(g.cfg.hotkey_vk, g.cfg.hotkey_mods);
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::InputText("##hotkey", &name[0], name.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Cambiar", ImVec2(110, 0))) {
            g.capturing_hotkey = true;
            g.capture_armed = false; // primero hay que soltar todo (el clic incluido)
        }
        return;
    }

    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1.0f),
                       "Apreta la tecla o combinacion (Esc para cancelar)...");
    bool any_down = false;
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (is_modifier_or_mouse(vk) || !(GetAsyncKeyState(vk) & 0x8000)) continue;
        any_down = true;
        if (!g.capture_armed) break;
        if (vk != VK_ESCAPE) {
            int mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= kModAlt;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= kModShift;
            g.cfg.hotkey_vk = vk;
            g.cfg.hotkey_mods = mods;
        }
        g.capturing_hotkey = false;
        break;
    }
    if (!any_down) g.capture_armed = true;
}

// Pistas de audio: la mezcla siempre; sistema y microfono aparte si se pide.
static void draw_audio_tracks_setting() {
    ImGui::Checkbox("Guardar tambien el sistema y el microfono en pistas separadas",
                    &g.cfg.separate_audio_tracks);
    if (g.cfg.separate_audio_tracks) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("El clip lleva 3 pistas: la mezcla (la que suena en cualquier "
                           "reproductor), solo el sistema y solo el microfono. Sirve para "
                           "editar (DaVinci, Premiere, CapCut...) y subir o bajar tu voz.");
        ImGui::PopStyleColor();
    }
}

static void draw_settings(bool running) {
    ImGui::SeparatorText("Ajustes");

    ImGui::BeginDisabled(running);

    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##outdir", g.out_dir_buf, sizeof(g.out_dir_buf));
    ImGui::SameLine();
    if (ImGui::Button("Carpeta...", ImVec2(110, 0))) {
        BROWSEINFOW bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = g_hwnd;
        bi.lpszTitle = L"Elegi la carpeta donde guardar los clips";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) {
                std::string s = wide_to_utf8(path);
                if (!s.empty()) {
                    std::snprintf(g.out_dir_buf, sizeof(g.out_dir_buf), "%s", s.c_str());
                }
            }
            CoTaskMemFree(pidl);
        }
    }
    ImGui::TextDisabled("Carpeta donde se guardan los clips (si no existe, se crea)");
    ImGui::Checkbox("Una carpeta por juego", &g.cfg.clips_by_game);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cada clip va a una subcarpeta con el nombre del juego que tenia el "
                          "foco al guardarlo (por ejemplo \"VALORANT\").");

    draw_hotkey_setting();
    draw_monitor_combo();

    ImGui::Text("Cuadros por segundo (FPS)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##fps", &g.cfg.fps, 15, 240);

    ImGui::Text("Calidad de video (Mbps)");
    ImGui::SetNextItemWidth(-1.0f);
    int mbps = g.cfg.kbps / 1000;
    if (ImGui::SliderInt("##bitrate", &mbps, 2, 100, "%d Mbps",
                         ImGuiSliderFlags_AlwaysClamp)) {
        g.cfg.kbps = mbps * 1000;
    }
    draw_codec_combo();
    draw_resolution_combo();

    ImGui::Text("Duracion del buffer (segundos)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##duration", &g.cfg.clip_seconds, 5, 600, "%d s");

    ImGui::Checkbox("Grabar audio (sistema + microfono)", &g.cfg.audio_enabled);
    ImGui::BeginDisabled(!g.cfg.audio_enabled);
    ImGui::Checkbox("Incluir el microfono", &g.cfg.mic_enabled);
    draw_device_combo("Dispositivo de salida (audio del sistema)", g.outputs, g.out_sel);
    draw_device_combo("Entrada de microfono", g.inputs, g.mic_sel);
    ImGui::Text("Volumen del microfono");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##micvol", &g.cfg.mic_volume, 0, 200, "%d %%",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::Text("Calidad de audio (Bitrate kbps)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##audiobitrate", &g.cfg.audio_kbps, 64, 320, "%d kbps",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::BeginDisabled(!g.cfg.mic_enabled);
    draw_audio_tracks_setting();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (ImGui::Button("Refrescar dispositivos y monitores")) refresh_devices();

    ImGui::Checkbox("Grabar el cursor del mouse", &g.cfg.record_cursor);

    ImGui::EndDisabled();

    // Esto se puede cambiar mientras graba: se lee al guardar cada clip.
    ImGui::Checkbox("Sonido al guardar un clip", &g.cfg.clip_sound);

    ImGui::Spacing();
    ImGui::SeparatorText("Arranque");
    ImGui::Checkbox("Empezar a grabar al abrir el programa", &g.cfg.record_on_start);
    ImGui::Checkbox("Minimizar a la bandeja al cerrar", &g.cfg.minimize_to_tray);
    if (ImGui::Checkbox("Iniciar con Windows", &g.startup_enabled)) {
        // Mostramos siempre el estado real de la clave (por si Windows rechazo
        // el cambio).
        set_startup_registered(g.startup_enabled);
        refresh_startup_state();
    }
    if (g.startup_enabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Se abre solo al encender la PC. Si \"Minimizar a la bandeja\" "
                           "esta marcado, arranca escondido junto al reloj.");
        ImGui::PopStyleColor();
    } else if (!g.startup_other.empty()) {
        std::string other = wide_to_utf8(g.startup_other.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Windows abre otra copia al encender la PC: %s. Marca la casilla "
                           "para que abra esta.",
                           other.c_str());
        ImGui::PopStyleColor();
    }
}

static void draw_controls() {
    ImGui::SeparatorText("Grabacion");

    const std::string key = hotkey_name(g.cfg.hotkey_vk, g.cfg.hotkey_mods);
    const std::string save_label = "Guardar clip (" + key + ")";

    if (!g.recorder) {
        if (ImGui::Button("Iniciar grabacion", ImVec2(180, 34))) start_recording();
        if (g.restart_at) {
            ImGui::SameLine();
            if (ImGui::Button("Cancelar reinicio", ImVec2(160, 34))) g.restart_at = 0;
        }
    } else {
        ImGui::BeginDisabled(g.stopping);
        if (ImGui::Button("Detener", ImVec2(110, 34))) stop_recording();
        ImGui::SameLine();
        if (ImGui::Button(save_label.c_str(), ImVec2(220, 34))) request_save_clip();
        ImGui::EndDisabled();
    }

    ImGui::Spacing();

    if (g.stopping) {
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1.0f), "Deteniendo...");
    } else if (g.recorder && g.recorder->is_running()) {
        long frames = g.recorder->frames();
        double buffered = g.recorder->buffered_seconds();
        double pct = g.cfg.clip_seconds > 0 ? buffered / (double)g.cfg.clip_seconds : 0.0;
        if (pct > 1.0) pct = 1.0;

        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Grabando");
        ImGui::Text("Frames: %ld", frames);
        ImGui::Text("Buffer: %.0f s / %d s", buffered, g.cfg.clip_seconds);
        ImGui::ProgressBar((float)pct, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("Apreta %s (o Guardar clip) para guardar los ultimos %d s",
                            key.c_str(), g.cfg.clip_seconds);
    } else if (g.recorder) {
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1.0f), "Iniciando...");
    } else if (g.restart_at) {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1.0f),
                           "La grabacion se corto sola: reiniciando...");
    } else {
        ImGui::TextDisabled("Detenido");
    }

    if (ImGui::Button("Abrir carpeta de clips")) open_clips_folder();
}

static void draw_log() {
    ImGui::SeparatorText("Mensajes");

    if (g.log_local_version != g.log_version.load()) {
        std::lock_guard<std::mutex> lk(g.log_mutex);
        g.log_snapshot = g.log_lines;
        g.log_local_version = g.log_version.load();
    }

    ImGui::BeginChild("##log", ImVec2(0, 180), ImGuiChildFlags_Borders);
    for (const std::string &line : g.log_snapshot) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) g.log_auto_scroll = true;
    if (g.log_auto_scroll) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Editor de clips: elegir un clip, verlo, marcar inicio/fin y guardar el pedazo
// (sin recodificar, ver clip_editor.h).
// ---------------------------------------------------------------------------
static constexpr double kMinTrim = 0.5; // el recorte mas corto (segundos)

static std::string file_name_of(const std::string &path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "1:05.3"
static std::string fmt_time(double t) {
    if (t < 0) t = 0;
    int tenths = (int)std::llround(t * 10.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
    return buf;
}

static void editor_release_texture() {
    if (ed.srv) { ed.srv->Release(); ed.srv = nullptr; }
    if (ed.tex) { ed.tex->Release(); ed.tex = nullptr; }
    ed.tex_w = ed.tex_h = 0;
}

// Sube el ultimo cuadro del reproductor a la textura que dibuja ImGui.
static void editor_upload_frame() {
    int w = 0, h = 0;
    if (!ed.player.take_frame(ed.frame, w, h)) return;
    if (!ed.tex || w != ed.tex_w || h != ed.tex_h) {
        editor_release_texture();
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3dDevice->CreateTexture2D(&td, nullptr, &ed.tex))) return;
        if (FAILED(g_d3dDevice->CreateShaderResourceView(ed.tex, nullptr, &ed.srv))) {
            editor_release_texture();
            return;
        }
        ed.tex_w = w;
        ed.tex_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_d3dContext->Map(ed.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        for (int y = 0; y < h; y++)
            memcpy((uint8_t *)ms.pData + (size_t)y * ms.RowPitch,
                   ed.frame.data() + (size_t)y * w * 4, (size_t)w * 4);
        g_d3dContext->Unmap(ed.tex, 0);
    }
}

// Lista los .mp4 de la carpeta de clips, el mas nuevo primero.
static void editor_refresh_list() {
    namespace fs = std::filesystem;
    ed.list_dirty = false;
    ed.listed_dir = g.out_dir_buf;
    ed.clips.clear();

    struct Item {
        fs::file_time_type when;
        ClipEntry e;
    };
    std::vector<Item> items;
    std::error_code ec;
    // Tambien las subcarpetas (una por juego, y "Clips Edit\<juego>").
    fs::path dir = fs::u8path(ed.listed_dir.empty() ? "." : ed.listed_dir);
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 2) it.disable_recursion_pending();
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().u8string();
        for (char &c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".mp4") continue;
        Item item;
        item.when = it->last_write_time(ec);
        item.e.path = it->path().u8string();
        double mb = (double)it->file_size(ec) / (1024.0 * 1024.0);
        char size[32];
        std::snprintf(size, sizeof(size), "  (%.1f MB)", mb);
        // "VALORANT / clip_....mp4  (12.3 MB)"
        std::string folder = it->path().parent_path().lexically_relative(dir).u8string();
        for (char &c : folder)
            if (c == '\\') c = '/';
        if (folder == ".") folder.clear();
        item.e.label = (folder.empty() ? "" : folder + " / ") +
                       it->path().filename().u8string() + size;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.when > b.when; });
    for (Item &i : items) ed.clips.push_back(std::move(i.e));
}

static void editor_open(const std::string &path) {
    ed.player.close();
    editor_release_texture();
    ed.path = path;
    ed.open_error.clear();
    ed.drag = -1;
    std::string err;
    if (!ed.player.open(path, 1280, 720, &err)) {
        ed.open_error = err;
        return;
    }
    ed.sel_start = 0.0;
    ed.sel_end = ed.player.info().duration;
}

static std::string pick_clip_file() {
    wchar_t buf[2048] = L"";
    std::wstring dir = utf8_to_wide(g.out_dir_buf);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Videos MP4 (*.mp4)\0*.mp4\0Todos los archivos\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Elegi el clip a recortar";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return std::string();
    return wide_to_utf8(buf);
}

static void editor_mark_start() {
    double t = keyframe_at_or_before(ed.player.info(), ed.player.position());
    ed.sel_start = t;
    if (ed.sel_end < ed.sel_start + kMinTrim) ed.sel_end = ed.player.info().duration;
}

static void editor_mark_end() {
    double t = ed.player.position();
    if (t < ed.sel_start + kMinTrim) return; // antes del inicio: no tiene sentido
    ed.sel_end = t;
}

static void editor_toggle_play() {
    if (ed.player.playing())
        ed.player.pause();
    else
        ed.player.play();
}

static void editor_start_save() {
    if (ed.saving.load() || ed.path.empty()) return;
    if (ed.save_thread.joinable()) ed.save_thread.join();
    ed.player.pause();
    ed.saving.store(true);
    ed.save_finished.store(false);
    ed.save_msg.clear();
    std::string in = ed.path;
    std::string out = edited_clip_path(in, g.out_dir_buf); // carpeta "Clips Edit"
    double s = ed.sel_start, e = ed.sel_end;
    ed.delete_after = g.cfg.trim_delete_original;
    ed.save_thread = std::thread([in, out, s, e]() {
        std::string err;
        double dur = 0;
        bool ok = trim_clip(in, out, s, e, &err, &dur);
        ed.save_ok = ok;
        if (ok) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " (%.1f s)", dur);
            ed.save_msg = "Guardado: " + file_name_of(out) + buf;
            ed.saved_path = out;
        } else {
            ed.save_msg = "No se pudo recortar: " + err;
        }
        ed.save_finished.store(true);
    });
}

// Manda un archivo a la Papelera (se puede recuperar desde ahi).
static bool recycle_file(const std::string &path) {
    std::error_code ec;
    std::wstring w = std::filesystem::absolute(std::filesystem::u8path(path), ec).wstring();
    w.push_back(L'\0'); // la lista termina con doble \0
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = w.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
           !std::filesystem::exists(std::filesystem::u8path(path), ec);
}

// Termino el hilo de guardado: lo juntamos, borramos el original si hay que
// hacerlo, y abrimos el recorte.
static void editor_poll_save() {
    if (!ed.save_finished.load()) return;
    if (ed.save_thread.joinable()) ed.save_thread.join();
    ed.save_finished.store(false);
    ed.saving.store(false);
    ed.list_dirty = true;
    if (ed.save_ok) {
        std::string original = ed.path;
        if (ed.delete_after) {
            ed.player.close(); // suelta el archivo, si no Windows no deja borrarlo
            if (recycle_file(original)) {
                ed.save_msg += "  -  el original se mando a la Papelera";
                if (same_path(g.last_clip, utf8_to_wide(original)))
                    g.last_clip = utf8_to_wide(ed.saved_path);
            }
            else
                ed.save_msg += "  -  no se pudo borrar el original";
        }
        editor_open(ed.saved_path); // queda abierto el recorte
        if (g.cfg.clip_sound) MessageBeep(MB_OK);
    } else if (g.cfg.clip_sound) {
        MessageBeep(MB_ICONHAND);
    }
}

// Linea de tiempo: el tramo elegido en azul, los cuadros clave como rayitas
// abajo, el cabezal en blanco. Se hace clic/arrastra para moverse y se agarran
// los bordes del tramo para cambiar inicio y fin.
static void draw_timeline(double dur) {
    const float s = ImGui::GetStyle().FontScaleDpi;
    const float h = ImGui::GetFrameHeight() * 1.8f;
    const float w = std::max(50.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiIO &io = ImGui::GetIO();

    auto x_of = [&](double t) { return p0.x + (float)(dur > 0 ? t / dur : 0.0) * w; };
    auto t_of = [&](float x) {
        double f = (x - p0.x) / w;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        return f * dur;
    };

    const float grab = 8.0f * s;
    float xs = x_of(ed.sel_start), xe = x_of(ed.sel_end);

    // Interaccion.
    if (ImGui::IsItemActivated()) {
        float mx = io.MousePos.x;
        if (std::fabs(mx - xe) <= grab && std::fabs(mx - xe) <= std::fabs(mx - xs))
            ed.drag = 2;
        else if (std::fabs(mx - xs) <= grab)
            ed.drag = 1;
        else
            ed.drag = 0;
        ed.last_seek = -1.0;
        ed.player.pause();
    }
    if (ImGui::IsItemActive() && ed.drag >= 0) {
        double t = t_of(io.MousePos.x);
        if (ed.drag == 1) {
            ed.sel_start = std::max(0.0, std::min(t, ed.sel_end - kMinTrim));
            t = ed.sel_start;
        } else if (ed.drag == 2) {
            ed.sel_end = std::min(dur, std::max(t, ed.sel_start + kMinTrim));
            t = ed.sel_end;
        }
        if (std::fabs(t - ed.last_seek) > 1e-3) {
            ed.player.seek(t);
            ed.last_seek = t;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (ed.drag == 1) { // el inicio va al cuadro clave
            ed.sel_start = keyframe_at_or_before(ed.player.info(), ed.sel_start);
            ed.player.seek(ed.sel_start);
        }
        ed.drag = -1;
    }
    if (ImGui::IsItemHovered()) {
        float mx = io.MousePos.x;
        if (std::fabs(mx - xs) <= grab || std::fabs(mx - xe) <= grab)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetTooltip("%s", fmt_time(t_of(mx)).c_str());
    }

    // Dibujo.
    xs = x_of(ed.sel_start);
    xe = x_of(ed.sel_end);
    const float r = 4.0f * s;
    dl->AddRectFilled(p0, p1, IM_COL32(38, 38, 46, 255), r);
    dl->AddRectFilled(ImVec2(xs, p0.y), ImVec2(xe, p1.y), IM_COL32(66, 150, 250, 85));
    for (double k : ed.player.info().keyframes) {
        float x = x_of(k);
        dl->AddLine(ImVec2(x, p1.y - h * 0.22f), ImVec2(x, p1.y), IM_COL32(160, 160, 170, 110));
    }
    const float hw = 3.0f * s;
    dl->AddRectFilled(ImVec2(xs - hw, p0.y), ImVec2(xs + hw, p1.y), IM_COL32(46, 204, 113, 255),
                      2.0f * s);
    dl->AddRectFilled(ImVec2(xe - hw, p0.y), ImVec2(xe + hw, p1.y), IM_COL32(231, 76, 60, 255),
                      2.0f * s);
    float xp = x_of(ed.player.position());
    dl->AddLine(ImVec2(xp, p0.y - 3.0f * s), ImVec2(xp, p1.y + 3.0f * s),
                IM_COL32(255, 255, 255, 255), 2.0f * s);
}

static void draw_editor() {
    editor_poll_save();
    if (ed.list_dirty || ed.listed_dir != g.out_dir_buf) editor_refresh_list();
    if (!ed.open_request.empty()) {
        std::string req;
        req.swap(ed.open_request);
        editor_open(req);
        ed.auto_opened = true;
    } else if (!ed.auto_opened && !ed.player.is_open() && !ed.clips.empty()) {
        editor_open(ed.clips[0].path); // de entrada, el clip mas nuevo
        ed.auto_opened = true;
    }

    const float s = ImGui::GetStyle().FontScaleDpi;
    ImGuiStyle &style = ImGui::GetStyle();

    // --- Que clip ---
    ImGui::SeparatorText("Clip");
    const float bw_refresh = ImGui::CalcTextSize("Refrescar").x + style.FramePadding.x * 2;
    const float bw_open = ImGui::CalcTextSize("Abrir otro...").x + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(bw_refresh + bw_open + style.ItemSpacing.x * 2));
    std::string current = ed.path.empty() ? "(ningun clip)" : file_name_of(ed.path);
    if (ImGui::BeginCombo("##clip", current.c_str())) {
        for (const ClipEntry &c : ed.clips) {
            if (ImGui::Selectable(c.label.c_str(), c.path == ed.path)) editor_open(c.path);
        }
        if (ed.clips.empty()) ImGui::TextDisabled("No hay clips en la carpeta");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refrescar")) ed.list_dirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Abrir otro...")) {
        std::string p = pick_clip_file();
        if (!p.empty()) editor_open(p);
    }
    if (ed.clips.empty() && ed.path.empty())
        ImGui::TextDisabled("Todavia no hay clips en %s", ed.listed_dir.c_str());
    if (!ed.open_error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f), "%s", ed.open_error.c_str());
    if (!ed.player.is_open()) return;

    editor_upload_frame();
    const ClipInfo &info = ed.player.info();
    const double dur = info.duration;

    // --- Vista previa ---
    // Lo que queda debajo (linea de tiempo, botones y el recorte) mide unas 9
    // lineas; el video usa el resto, sin deformarse.
    const float line = ImGui::GetFrameHeightWithSpacing();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float max_h = std::max(120.0f * s, ImGui::GetContentRegionAvail().y - line * 9.5f);
    const float aspect = info.height > 0 ? info.width / (float)info.height : 16.0f / 9.0f;
    float pw = avail_w, ph = pw / aspect;
    if (ph > max_h) {
        ph = max_h;
        pw = ph * aspect;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - pw) * 0.5f);
    if (ed.srv) {
        ImGui::Image((ImTextureID)(intptr_t)ed.srv, ImVec2(pw, ph));
        if (ImGui::IsItemClicked()) editor_toggle_play();
    } else {
        ImGui::Dummy(ImVec2(pw, ph));
    }

    // --- Linea de tiempo ---
    draw_timeline(dur);
    ImGui::Text("%s / %s", fmt_time(ed.player.position()).c_str(), fmt_time(dur).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  %dx%d, %.0f fps, %s%s", info.width, info.height, info.fps,
                        info.video_codec.c_str(),
                        info.audio_tracks == 0       ? ", sin audio"
                        : ed.player.has_audio()      ? ""
                                                     : ", sin salida de audio");

    // --- Controles ---
    const bool playing = ed.player.playing();
    if (ImGui::Button(playing ? "Pausa" : "Reproducir", ImVec2(100 * s, 0))) editor_toggle_play();
    ImGui::SameLine();
    if (ImGui::Button("-5 s")) ed.player.seek(ed.player.position() - 5.0);
    ImGui::SameLine();
    if (ImGui::Button("+5 s")) ed.player.seek(ed.player.position() + 5.0);
    ImGui::SameLine();
    if (ImGui::Button("Marcar inicio (I)")) editor_mark_start();
    ImGui::SameLine();
    if (ImGui::Button("Marcar fin (O)")) editor_mark_end();
    ImGui::TextDisabled("Espacio: reproducir/pausa   Flechas: 1 s   Clic en el video: pausa");

    // --- Recorte ---
    ImGui::SeparatorText("Recorte");
    ImGui::Text("Desde %s hasta %s  (%.1f s de %.1f s)", fmt_time(ed.sel_start).c_str(),
                fmt_time(ed.sel_end).c_str(), ed.sel_end - ed.sel_start, dur);
    if (ImGui::Button("Ver el recorte")) ed.player.play_range(ed.sel_start, ed.sel_end);
    ImGui::SameLine();
    if (ImGui::Button("Todo el clip")) {
        ed.sel_start = 0.0;
        ed.sel_end = dur;
    }
    ImGui::SameLine();
    const bool saving = ed.saving.load();
    ImGui::BeginDisabled(saving);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.48f, 0.28f, 1.0f));
    if (ImGui::Button(saving ? "Guardando..." : "Guardar recorte")) editor_start_save();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    if (!ed.save_msg.empty() && !saving) {
        ImGui::TextColored(ed.save_ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                      : ImVec4(0.95f, 0.4f, 0.35f, 1.0f),
                           "%s", ed.save_msg.c_str());
        if (ed.save_ok) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Abrir")) {
                std::wstring w = utf8_to_wide(ed.saved_path);
                ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mostrar en la carpeta")) {
                std::error_code ec;
                std::wstring w =
                    std::filesystem::absolute(std::filesystem::u8path(ed.saved_path), ec).wstring();
                std::wstring args = L"/select,\"" + w + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                              SW_SHOWNORMAL);
            }
        }
    }
    if (ImGui::Checkbox("Borrar el clip original al guardar (va a la Papelera)",
                        &g.cfg.trim_delete_original))
        save_config();
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("El recorte se guarda en la carpeta \"%s\" (dentro de la carpeta de "
                       "clips, separado por juego). El inicio cae en un cuadro clave (hay uno "
                       "cada 2 s): asi el corte es instantaneo y sin perder calidad.",
                       kEditedFolder);
    ImGui::PopStyleColor();

    // --- Teclado ---
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) editor_toggle_play();
        if (ImGui::IsKeyPressed(ImGuiKey_I, false)) editor_mark_start();
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) editor_mark_end();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ed.player.seek(ed.player.position() - 1.0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ed.player.seek(ed.player.position() + 1.0);
    }
}

static void draw_ui() {
    // Los ajustes se bloquean mientras haya un grabador (arrancando, grabando
    // o deteniendose): se leen al iniciar.
    bool busy = g.recorder != nullptr;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("recorder_moment", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    bool editor_active = false;
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Grabar")) {
            draw_controls();
            ImGui::Spacing();
            draw_settings(busy);
            ImGui::Spacing();
            draw_log();
            ImGui::EndTabItem();
        }
        ImGuiTabItemFlags flags = ed.select_tab ? ImGuiTabItemFlags_SetSelected : 0;
        ed.select_tab = false;
        if (ImGui::BeginTabItem("Recortar clips", nullptr, flags)) {
            editor_active = true;
            draw_editor();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    // Fuera del editor no sigue sonando. Y en el editor, Espacio es
    // reproducir/pausa (no "apretar el boton que tiene el foco").
    if (ed.active && !editor_active) ed.player.pause();
    ed.active = editor_active;
    ImGuiIO &io = ImGui::GetIO();
    if (editor_active)
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

// ---------------------------------------------------------------------------
// Arranque
// ---------------------------------------------------------------------------
int run_gui(bool started_by_windows) {
    ImGui_ImplWin32_EnableDpiAwareness();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Config guardada (en %APPDATA%; si todavia no hay, la vieja de al lado del
    // .exe). Se guarda siempre en %APPDATA%.
    g.config_path = config_default_path();
    config_load_default(g.cfg);
    std::snprintf(g.out_dir_buf, sizeof(g.out_dir_buf), "%s", g.cfg.out_dir.c_str());
    refresh_devices();

    // Estado real del arranque automatico (la clave Run manda, no el .ini).
    refresh_startup_state();

    // Si "out_dir" es relativo (el valor por defecto es "."), lo anclamos a la
    // carpeta del .exe. Asi, cuando Windows abre el programa al iniciar sesion
    // (que arranca en otra carpeta), los clips no caen en un lugar raro.
    {
        std::wstring exe = exe_path();
        size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring dir = exe.substr(0, slash);
            SetCurrentDirectoryW(dir.c_str());
        }
    }

    // Ventana (clase + ventana).
    HICON app_icon = load_app_icon();
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = app_icon;
    wc.hIconSm = app_icon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = RECORDER_WINDOW_CLASS;
    RegisterClassExW(&wc);

    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    if (scale <= 0.0f) scale = 1.0f;

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"recorder_moment",
                              WS_OVERLAPPEDWINDOW, 80, 80,
                              (int)(600 * scale), (int)(800 * scale), nullptr, nullptr,
                              wc.hInstance, nullptr);
    g_hwnd = hwnd;
    SetWindowTextW(hwnd, L"recorder_moment");

    if (!create_device_d3d(hwnd)) {
        cleanup_device_d3d();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"No se pudo iniciar Direct3D 11.", L"recorder_moment",
                    MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // Escondido en la bandeja solo cuando lo abre Windows al encender la PC; si
    // lo abris vos, se muestra la ventana.
    bool start_hidden = started_by_windows && g.cfg.minimize_to_tray;
    ShowWindow(hwnd, start_hidden ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    tray_add(hwnd);
    create_toast(wc.hInstance);

    // ImGui.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no ensuciamos la carpeta con imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);

    // "Empezar a grabar al abrir el programa".
    if (g.cfg.record_on_start) start_recording();

    const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Escondida en la bandeja o minimizada: no dibujamos nada. Dormimos
        // hasta que llegue un mensaje (o 250 ms, para juntar el grabador si se
        // corto solo), asi en segundo plano no gasta ni CPU ni GPU.
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            ed.player.pause(); // el editor no sigue sonando escondido
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            reap_recorder();
            continue;
        }

        // Si la ventana esta tapada por otra no gastamos GPU.
        if (g_swapChainOccluded &&
            g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            reap_recorder();
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            cleanup_render_target();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                       DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            create_render_target();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        g_d3dContext->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_d3dContext->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swapChain->Present(1, 0);
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        reap_recorder();
    }

    // Salida ordenada: cortamos la grabacion y guardamos los ajustes.
    if (g.recorder) {
        Recorder::request_stop();
        if (g.rec_thread.joinable()) g.rec_thread.join();
        g.recorder.reset();
    }
    save_config();

    // Editor: el recorte que se este guardando termina; el reproductor se cierra.
    if (ed.save_thread.joinable()) ed.save_thread.join();
    ed.player.close();
    editor_release_texture();

    tray_remove(hwnd);
    if (g_toast) DestroyWindow(g_toast);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device_d3d();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
