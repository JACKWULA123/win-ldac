// win-ldac.exe — Dear ImGui + DirectX 11 GUI host for the core_engine.
//
// Threading: the engine runs on a worker std::thread; the main thread
// runs the Win32 message pump + ImGui frame loop. Cross-thread access
// to engine state goes through engine_get_status_snapshot() and the
// engine_post_*() command APIs, both of which are thread-safe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>
#include <shellapi.h>
#include <tchar.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"

extern "C" {
#include "btstack_util.h"           // sscanf_bd_addr
#include "engine/engine.h"
#include "app/config_file.h"
}

#include "ui/status_window.h"
#include "resources.h"

#define LOCAL_NAME         "win-ldac"

// ── Fixed client area ──────────────────────────────────────────────────
static constexpr int kClientW = 500;
static constexpr int kClientH = 360;

// ── DX11 plumbing ──────────────────────────────────────────────────────
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                             &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hWnd;
    sd.SampleDesc.Count  = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)         { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)  { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)         { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

// ── Asset folder resolution ────────────────────────────────────────────
// Walks up from the exe directory looking for an `assets/` folder. Same
// pattern as engine.c's firmware folder resolver, so a dev build at
// build\gui\Release\win-ldac.exe finds <repo>\assets too.
static std::wstring resolve_asset(const wchar_t* filename) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"";
    wchar_t* sep = std::wcsrchr(exe, L'\\');
    if (!sep) return L"";
    *sep = L'\0';
    std::wstring base = exe;
    for (int i = 0; i < 5; ++i) {
        std::wstring candidate = base + L"\\assets\\" + filename;
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
        size_t pos = base.find_last_of(L'\\');
        if (pos == std::wstring::npos) break;
        base.resize(pos);
    }
    return L"";
}

// ── Texture loader (WIC → DX11 SRV) ───────────────────────────────────
static bool load_texture_from_file(const wchar_t* path,
                                   ID3D11ShaderResourceView** out_srv,
                                   int* out_w, int* out_h) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { factory->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    decoder->GetFrame(0, &frame);

    IWICFormatConverter* conv = nullptr;
    factory->CreateFormatConverter(&conv);
    conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());

    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = pixels.data();
    sub.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, out_srv);
    tex->Release();

    *out_w = w; *out_h = h;
    return true;
}

// ── System tray (M9) ───────────────────────────────────────────────────
// Window message for tray icon callbacks. WM_APP is the start of the
// recommended user-defined range.
#define WM_TRAY_CALLBACK   (WM_APP + 1)
#define TRAY_MENU_OPEN     1
#define TRAY_MENU_AUTOSTART 2
#define TRAY_MENU_QUIT     3
#define TRAY_ICON_UID      1

static NOTIFYICONDATAW g_tray_nid{};
static bool            g_tray_installed = false;
static bool            g_quit_requested = false;     // set by tray Quit / GUI Quit
static wchar_t         g_last_tooltip[128] = L"";

static void tray_install(HWND hwnd) {
    g_tray_nid.cbSize           = sizeof(g_tray_nid);
    g_tray_nid.hWnd             = hwnd;
    g_tray_nid.uID              = TRAY_ICON_UID;
    g_tray_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray_nid.uCallbackMessage = WM_TRAY_CALLBACK;
    g_tray_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr),
                                 MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (!g_tray_nid.hIcon) {
        // Fallback if .ico wasn't bundled (e.g. dev build before assets
        // are placed): use the default Windows app icon so the tray
        // entry is at least visible.
        g_tray_nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    }
    wcscpy_s(g_tray_nid.szTip,
             sizeof(g_tray_nid.szTip) / sizeof(wchar_t),
             L"win-ldac");
    g_tray_installed = Shell_NotifyIconW(NIM_ADD, &g_tray_nid) != FALSE;
}

static void tray_remove() {
    if (!g_tray_installed) return;
    Shell_NotifyIconW(NIM_DELETE, &g_tray_nid);
    g_tray_installed = false;
}

static void tray_update_tooltip(const wchar_t* tip) {
    if (!g_tray_installed) return;
    if (wcscmp(g_last_tooltip, tip) == 0) return;
    wcscpy_s(g_last_tooltip,
             sizeof(g_last_tooltip) / sizeof(wchar_t), tip);
    g_tray_nid.uFlags = NIF_TIP;
    wcscpy_s(g_tray_nid.szTip,
             sizeof(g_tray_nid.szTip) / sizeof(wchar_t), tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray_nid);
}

// ── Auto-start (HKCU\...\Run) ──────────────────────────────────────────
static const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"win-ldac";

static bool auto_start_get() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key)
        != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(key, kRunValue, nullptr, nullptr,
                              nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static bool auto_start_set(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key)
        != ERROR_SUCCESS) return false;
    bool ok;
    if (enable) {
        wchar_t exe[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            RegCloseKey(key);
            return false;
        }
        wchar_t quoted[MAX_PATH + 2];
        _snwprintf_s(quoted, MAX_PATH + 2, _TRUNCATE, L"\"%s\"", exe);
        DWORD bytes = (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t));
        ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            (const BYTE*)quoted, bytes) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(key, kRunValue);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

// Build the tray menu and run it. Returns the command id chosen.
static int tray_show_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TRAY_MENU_OPEN, L"Open win-ldac");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT autoflag = auto_start_get() ? MF_CHECKED : MF_UNCHECKED;
    AppendMenuW(menu, MF_STRING | autoflag, TRAY_MENU_AUTOSTART,
                L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TRAY_MENU_QUIT, L"Quit");
    // TrackPopupMenu wants the window in foreground or the menu won't
    // auto-dismiss on focus loss.
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu,
                             TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessage(hwnd, WM_NULL, 0, 0);  // flushes any stale messages
    return cmd;
}

// ── Engine log buffer ─────────────────────────────────────────────────
static std::mutex          g_log_mutex;
static std::vector<char>   g_log_storage;
static constexpr size_t    kLogMax = 64 * 1024;

static void engine_log_sink(const char* line) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    size_t n = std::strlen(line);
    if (g_log_storage.size() + n + 1 > kLogMax) {
        size_t drop = (g_log_storage.size() + n + 1) - kLogMax;
        if (drop > g_log_storage.size()) drop = g_log_storage.size();
        g_log_storage.erase(g_log_storage.begin(),
                            g_log_storage.begin() + drop);
    }
    g_log_storage.insert(g_log_storage.end(), line, line + n);
    g_log_storage.push_back('\n');
}

// ── Engine worker thread ──────────────────────────────────────────────
static std::atomic<bool> g_engine_thread_done{false};

static void engine_thread_main(engine_config_t cfg) {
    if (engine_init(&cfg) == 0) {
        engine_run();
        engine_shutdown();
    }
    g_engine_thread_done.store(true);
}

// ── Theme ──────────────────────────────────────────────────────────────
static void apply_light_theme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry — tuned for a 500×400 client area.
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 14;
    s.ScrollbarSize     = 10;

    s.WindowRounding    = 0;
    s.ChildRounding     = 10;
    s.FrameRounding     = 8;
    s.GrabRounding      = 6;
    s.PopupRounding     = 6;

    s.ChildBorderSize   = 1;
    s.FrameBorderSize   = 0;

    auto& c = s.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.965f, 0.965f, 0.955f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.85f, 0.86f, 0.88f, 1.00f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.27f, 0.49f, 0.96f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.27f, 0.49f, 0.96f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.20f, 0.42f, 0.90f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.98f, 0.98f, 0.99f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.94f, 0.95f, 0.97f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.93f, 0.95f, 0.99f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.90f, 0.93f, 0.98f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.87f, 0.91f, 0.97f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.86f, 0.87f, 0.89f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.68f, 0.70f, 0.74f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.58f, 0.61f, 0.66f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.34f, 0.66f, 0.90f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.34f, 0.66f, 0.90f, 1.00f);
}

// ── Win32 plumbing ────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT,
                                                             WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        // X button → hide to tray. Quit lives behind the tray menu or
        // the in-app Quit button (both set g_quit_requested, which the
        // main loop services with DestroyWindow → WM_DESTROY).
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_TRAY_CALLBACK:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            int cmd = tray_show_menu(hWnd);
            switch (cmd) {
            case TRAY_MENU_OPEN:
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
                break;
            case TRAY_MENU_AUTOSTART:
                auto_start_set(!auto_start_get());
                break;
            case TRAY_MENU_QUIT:
                g_quit_requested = true;
                break;
            }
            return 0;
        }
        }
        return 0;
    case WM_DESTROY:
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Main ───────────────────────────────────────────────────────────────
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 1. Window. Fixed client area: no thick frame, no maximize.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"win-ldac";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    ::RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, kClientW, kClientH};
    ::AdjustWindowRectEx(&rc, style, FALSE, 0);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"win-ldac", style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);

    // 2. D3D.
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Failed to create D3D11 device.",
                    L"win-ldac", MB_OK | MB_ICONERROR);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // System tray icon. Installed after the window exists so its
    // callback messages have somewhere to land. Tooltip is refined
    // each frame based on engine status.
    tray_install(hwnd);

    // 3. ImGui + ImPlot.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // don't persist layout

    apply_light_theme();

    // 4. Font. Try assets/MapleMono-Regular.ttf; if not present, fall
    //    back to the embedded ProggyClean default. The mockup uses
    //    Maple Mono — see https://github.com/subframe7536/Maple-font.
    win_ldac::UiResources ui{};
    std::wstring font_path = resolve_asset(L"MapleMono-Regular.ttf");
    if (!font_path.empty()) {
        char utf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, font_path.c_str(), -1,
                            utf8, MAX_PATH, nullptr, nullptr);
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        ui.font = io.Fonts->AddFontFromFileTTF(utf8, 16.0f, &cfg);
        if (ui.font) {
            ui.font_loaded_name = "Maple Mono";
        }
    }
    if (!ui.font) {
        ui.font = io.Fonts->AddFontDefault();
        ui.font_loaded_name = "(default — place MapleMono-Regular.ttf in assets/)";
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 5. Monkey watermark texture.
    std::wstring monkey_path = resolve_asset(L"monkey.png");
    if (!monkey_path.empty()) {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        if (load_texture_from_file(monkey_path.c_str(), &srv, &w, &h)) {
            ui.monkey_tex = (ImTextureID)(intptr_t)srv;
            ui.monkey_aspect = h > 0 ? (float)w / (float)h : 1.0f;
        }
    }

    // 6. Engine. Load persisted config (target addr / preferred bitrate)
    //    and seed the engine. First-run users have no config and start
    //    in the "awaiting pairing" idle state.
    win_ldac_config_t persisted{};
    bool have_config = win_ldac_config_load(&persisted);
    if (have_config) {
        if (persisted.has_target) {
            char addr_str[18];
            std::snprintf(addr_str, sizeof(addr_str),
                          "%02X:%02X:%02X:%02X:%02X:%02X",
                          persisted.target_addr[0], persisted.target_addr[1],
                          persisted.target_addr[2], persisted.target_addr[3],
                          persisted.target_addr[4], persisted.target_addr[5]);
            engine_log_sink("[main] config: target = ");
            engine_log_sink(addr_str);
        } else {
            engine_log_sink("[main] config: no target (unpaired)");
        }
    } else {
        engine_log_sink("[main] no config file — first run, unpaired");
    }
    if (persisted.target_name[0]) {
        std::strncpy(ui.target_name, persisted.target_name,
                     sizeof(ui.target_name) - 1);
    }

    // Wire persistence callbacks. status_window invokes these after
    // the user pairs / unpairs / toggles bitrate. main.cpp owns the
    // canonical config and writes the file via win_ldac_config_save.
    static win_ldac::UiResources* s_ui_for_cb = &ui;  // captured by C-style fn ptrs

    ui.on_pair = [](const bd_addr_t addr, const char* name) {
        win_ldac_config_t cfg{};
        cfg.has_target = true;
        memcpy(cfg.target_addr, addr, sizeof(bd_addr_t));
        if (name) {
            std::strncpy(cfg.target_name, name, sizeof(cfg.target_name) - 1);
            std::strncpy(s_ui_for_cb->target_name, name,
                         sizeof(s_ui_for_cb->target_name) - 1);
        } else {
            s_ui_for_cb->target_name[0] = '\0';
        }
        // Preserve last bitrate mode on disk. Snapshot will overwrite
        // on next bitrate persist callback anyway.
        cfg.bitrate_mode = WIN_LDAC_CFG_BITRATE_FIXED_HQ;
        win_ldac_config_save(&cfg);
        engine_post_set_target(addr);
    };

    ui.on_unpair = []() {
        win_ldac_config_t cfg{};   // has_target = false
        win_ldac_config_save(&cfg);
        s_ui_for_cb->target_name[0] = '\0';
        engine_post_clear_target();
    };

    ui.on_bitrate_persist = [](bool adaptive) {
        // Read current target back from the live snapshot so we don't
        // accidentally clobber it.
        engine_status_t st{};
        engine_get_status_snapshot(&st);
        win_ldac_config_t cfg{};
        cfg.has_target = st.has_target;
        if (st.has_target) {
            memcpy(cfg.target_addr, st.target_addr, sizeof(bd_addr_t));
            std::strncpy(cfg.target_name, s_ui_for_cb->target_name,
                         sizeof(cfg.target_name) - 1);
        }
        cfg.bitrate_mode = adaptive ? WIN_LDAC_CFG_BITRATE_ADAPTIVE
                                    : WIN_LDAC_CFG_BITRATE_FIXED_HQ;
        win_ldac_config_save(&cfg);
    };

    engine_config_t cfg{};
    cfg.local_name             = LOCAL_NAME;
    cfg.reconnect_interval_ms  = 5000;
    cfg.initial_bitrate_mode   = persisted.bitrate_mode == WIN_LDAC_CFG_BITRATE_ADAPTIVE
                                 ? ENGINE_BITRATE_ADAPTIVE
                                 : ENGINE_BITRATE_FIXED_HQ;
    if (persisted.has_target) {
        memcpy(cfg.target_addr, persisted.target_addr, sizeof(bd_addr_t));
    }
    // else target_addr stays {0}, engine treats as unpaired

    engine_set_log_callback(&engine_log_sink);
    std::thread engine_thread(&engine_thread_main, cfg);

    // 7. Main loop.
    win_ldac::StatusSamples samples;
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        engine_status_t st{};
        engine_get_status_snapshot(&st);

        uint64_t now_ms = GetTickCount64();
        if (st.state == ENGINE_STATE_STREAMING && !st.idle_paused) {
            samples.maybe_push(st.effective_kbps, now_ms);
        }

        // Tray tooltip — refresh from snapshot. tray_update_tooltip
        // debounces against the previous value, so the NIM_MODIFY
        // syscall only fires when the text actually changes.
        wchar_t tip[128];
        if (st.wasapi_unsupported_rate_hz != 0) {
            _snwprintf_s(tip, 128, _TRUNCATE,
                         L"win-ldac — Unsupported sample rate %d Hz",
                         st.wasapi_unsupported_rate_hz);
        } else if (!st.has_target) {
            wcscpy_s(tip, 128, L"win-ldac — Not paired");
        } else if (st.state == ENGINE_STATE_STREAMING && !st.idle_paused) {
            _snwprintf_s(tip, 128, _TRUNCATE,
                         L"win-ldac — Streaming LDAC %d kbps",
                         st.effective_kbps);
        } else if (st.idle_paused) {
            wcscpy_s(tip, 128, L"win-ldac — Idle (silence)");
        } else if (st.state == ENGINE_STATE_CONNECTING ||
                   st.state == ENGINE_STATE_NEGOTIATING) {
            wcscpy_s(tip, 128, L"win-ldac — Connecting…");
        } else {
            wcscpy_s(tip, 128, L"win-ldac — Disconnected");
        }
        tray_update_tooltip(tip);

        // Only spend CPU on ImGui rendering when the window is actually
        // visible — when hidden in the tray we just service messages
        // and keep the engine humming.
        bool visible = ::IsWindowVisible(hwnd) != FALSE;
        if (visible) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (win_ldac::draw_status_window(&st, &samples, ui)) {
                g_quit_requested = true;
            }

            ImGui::Render();
            const float clear_color[4] = {
                ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x,
                ImGui::GetStyle().Colors[ImGuiCol_WindowBg].y,
                ImGui::GetStyle().Colors[ImGuiCol_WindowBg].z,
                1.0f
            };
            g_pd3dDeviceContext->OMSetRenderTargets(
                1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(
                g_mainRenderTargetView, clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_pSwapChain->Present(1, 0);
        } else {
            // Sleep a tiny bit so we don't peg a CPU core just polling
            // PeekMessage when hidden.
            Sleep(50);
        }

        if (g_quit_requested) {
            g_quit_requested = false;
            engine_request_stop();
            DestroyWindow(hwnd);
        }
    }

    // 8. Teardown.
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (ui.monkey_tex) {
        ((ID3D11ShaderResourceView*)(intptr_t)ui.monkey_tex)->Release();
    }
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    engine_request_stop();
    if (engine_thread.joinable()) engine_thread.join();
    CoUninitialize();
    return 0;
}
