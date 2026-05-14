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
}

#include "ui/status_window.h"

// ── Hard-coded target (M8 will replace with config file + pairing) ─────
#define TARGET_DEVICE_ADDR "88:C9:E8:F7:D5:F3"
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
    case WM_DESTROY:
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

    // 6. Engine.
    engine_config_t cfg{};
    cfg.local_name             = LOCAL_NAME;
    cfg.reconnect_interval_ms  = 5000;
    cfg.initial_bitrate_mode   = ENGINE_BITRATE_FIXED_HQ;
    sscanf_bd_addr(TARGET_DEVICE_ADDR, cfg.target_addr);

    engine_set_log_callback(&engine_log_sink);
    std::thread engine_thread(&engine_thread_main, cfg);

    // 7. Main loop.
    win_ldac::StatusSamples samples;
    bool done = false;
    bool stop_requested = false;
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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        engine_status_t st{};
        engine_get_status_snapshot(&st);

        uint64_t now_ms = GetTickCount64();
        if (st.state == ENGINE_STATE_STREAMING && !st.idle_paused) {
            samples.maybe_push(st.effective_kbps, now_ms);
        }

        if (win_ldac::draw_status_window(&st, &samples, ui)) {
            stop_requested = true;
        }

        ImGui::Render();
        const float clear_color[4] = {
            ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x,
            ImGui::GetStyle().Colors[ImGuiCol_WindowBg].y,
            ImGui::GetStyle().Colors[ImGuiCol_WindowBg].z,
            1.0f
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);

        if (stop_requested) {
            stop_requested = false;
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
