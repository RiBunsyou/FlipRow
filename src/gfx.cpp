#include "gfx.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "com.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

ID2D1Factory1* g_f1 = nullptr;
IDWriteFactory* g_dw = nullptr;
ID2D1DeviceContext* g_dc = nullptr;
ID3D11Device* g_d3d = nullptr;
IDXGIDevice* g_dxgi = nullptr;
IDCompositionDevice* g_dcomp = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
std::wstring g_familyText = L"Segoe UI";
std::wstring g_familyDisplay = L"Segoe UI";
bool g_needRecreate = false;

bool HasFamily(const wchar_t* name) {
    if (!g_dw) return false;
    Com<IDWriteFontCollection> fc;
    if (FAILED(g_dw->GetSystemFontCollection(fc.Put())) || !fc) return false;
    UINT32 i = 0;
    BOOL ok = FALSE;
    fc->FindFamilyName(name, &i, &ok);
    return ok != FALSE;
}

// 选取点亮屏幕的显卡，避免双显卡笔记本跨卡拷贝画面。
struct AbrirPorNome {
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct FecharAdaptador {
    UINT hAdapter;
};
Com<IDXGIAdapter> AdapterOnScreen() {
    using FnAbrir = LONG(APIENTRY*)(AbrirPorNome*);
    using FnFechar = LONG(APIENTRY*)(FecharAdaptador*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    auto abrir = gdi ? (FnAbrir)GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
    auto fechar = gdi ? (FnFechar)GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr;
    if (!abrir) return {};
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) return {};
    AbrirPorNome a{};
    lstrcpynW(a.DeviceName, mi.szDevice, 32);
    if (abrir(&a) != 0) return {};
    if (fechar) {
        FecharAdaptador f{ a.hAdapter };
        fechar(&f);
    }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    using FnFab = HRESULT(WINAPI*)(REFIID, void**);
    auto fab = dxgi ? (FnFab)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!fab) return {};
    Com<IDXGIFactory4> f4;
    if (FAILED(fab(__uuidof(IDXGIFactory4), f4.PutVoid())) || !f4) return {};
    Com<IDXGIAdapter> ad;
    if (FAILED(f4->EnumAdapterByLuid(a.AdapterLuid, __uuidof(IDXGIAdapter), ad.PutVoid()))) return {};
    return ad;
}

void BindBack(gfx::Layer& l) {
    Com<IDXGISurface> surf;
    if (FAILED(l.swap->GetBuffer(0, IID_PPV_ARGS(surf.Put()))) || !surf) return;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_dc->CreateBitmapFromDxgiSurface(surf.Get(), &props, &l.back);
}

// 帧时钟，所有窗口共用一个后台线程。
struct Clock {
    HWND hwnd;
    UINT msg;
    std::atomic<bool> on{ true };
    std::atomic<bool> pending{ false };
};
std::vector<Clock*> g_clocks;
std::mutex g_clocksMx;
std::thread g_clockThread;
std::atomic<bool> g_clockStop{ false };
HANDLE g_clockWake = nullptr;

void ClockLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    THREAD_POWER_THROTTLING_STATE s{ THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0 };
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof(s));
    while (!g_clockStop) {
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(g_clocksMx);
            for (auto* c : g_clocks) any = any || c->on;
        }
        if (!any) {
            WaitForSingleObject(g_clockWake, 250);
            continue;
        }
        DwmFlush();  // 等待垂直同步。
        if (g_clockStop) break;
        std::lock_guard<std::mutex> lk(g_clocksMx);
        for (auto* c : g_clocks) {
            if (!c->on || !IsWindow(c->hwnd)) continue;
            if (c->pending) continue;
            c->pending = true;
            if (!PostMessageW(c->hwnd, c->msg, 0, 0)) c->pending = false;
        }
    }
}

// 字体格式缓存，避免每帧创建 IDWriteTextFormat。
struct FontKey {
    int size10;
    int weight;
    int align;
    bool display;
    bool operator<(const FontKey& o) const {
        if (size10 != o.size10) return size10 < o.size10;
        if (weight != o.weight) return weight < o.weight;
        if (align != o.align) return align < o.align;
        return display < o.display;
    }
};
std::map<FontKey, IDWriteTextFormat*> g_fonts;
std::mutex g_fontMx;

D2D1_COLOR_F C(int r, int g, int b, float a = 1.f) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = def, cb = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS) return def;
    return v;
}

}

const gfx::Fluent& gfx::T() {
    static Fluent t;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 2000) return t;
    last = now;
    t = Fluent{};
    t.dark = Dark();
    // 系统强调色存放于 DWM 注册表项，格式为 AABBGGRR。
    DWORD acc = RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0);
    if (!acc) acc = RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentColorMenu", 0);
    float ar = (acc & 0xFF) / 255.f, ag = ((acc >> 8) & 0xFF) / 255.f, ab = ((acc >> 16) & 0xFF) / 255.f;
    // 强调色为灰色时对比度不足，此时改用 Fluent 蓝。
    float mx = std::max({ ar, ag, ab }), mn = std::min({ ar, ag, ab });
    if (!acc || mx - mn < 0.06f) {
        ar = 0.00f;
        ag = 0.47f;
        ab = 0.83f;
    }
    if (t.dark) {  // 深色主题下提高强调色亮度。
        ar = ar + (1 - ar) * 0.25f;
        ag = ag + (1 - ag) * 0.25f;
        ab = ab + (1 - ab) * 0.25f;
    }
    t.accent = D2D1::ColorF(ar, ag, ab, 1.f);
    t.accentSoft = D2D1::ColorF(ar, ag, ab, 0.16f);
    float lum = 0.2126f * ar + 0.7152f * ag + 0.0722f * ab;
    t.onAccent = lum > 0.6f ? C(20, 20, 20) : C(255, 255, 255);
    if (t.dark) {
        t.text = C(255, 255, 255, 0.98f);
        t.text2 = C(255, 255, 255, 0.72f);
        t.text3 = C(255, 255, 255, 0.45f);
        t.card = C(39, 39, 42);
        t.cardStroke = C(255, 255, 255, 0.08f);
        t.shadow = C(0, 0, 0, 0.45f);
        t.bar = C(28, 28, 32, 0.86f);
        t.barStroke = C(255, 255, 255, 0.07f);
        t.hover = C(255, 255, 255, 0.09f);
        t.pressed = C(255, 255, 255, 0.04f);
        t.danger = C(255, 153, 164);
        t.dangerSoft = C(255, 99, 71, 0.20f);
        t.scrim = C(0, 0, 0, 0.42f);
        t.tile = C(44, 44, 48, 0.94f);
        t.tileStroke = C(255, 255, 255, 0.10f);
    } else {
        t.text = C(26, 26, 26);
        t.text2 = C(26, 26, 26, 0.66f);
        t.text3 = C(26, 26, 26, 0.44f);
        t.card = C(250, 250, 250);
        t.cardStroke = C(0, 0, 0, 0.07f);
        t.shadow = C(0, 0, 0, 0.22f);
        t.bar = C(243, 243, 246, 0.88f);
        t.barStroke = C(0, 0, 0, 0.06f);
        t.hover = C(0, 0, 0, 0.06f);
        t.pressed = C(0, 0, 0, 0.03f);
        t.danger = C(196, 43, 28);
        t.dangerSoft = C(196, 43, 28, 0.12f);
        t.scrim = C(0, 0, 0, 0.36f);
        t.tile = C(249, 249, 251, 0.96f);
        t.tileStroke = C(0, 0, 0, 0.08f);
    }
    return t;
}

bool gfx::Init() {
    if (g_dc) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&g_f1)) || !g_f1)
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw)) || !g_dw) return false;
    for (auto f : { L"Segoe UI Variable Text", L"Segoe UI" })
        if (HasFamily(f)) {
            g_familyText = f;
            break;
        }
    for (auto f : { L"Segoe UI Variable Display", L"Segoe UI" })
        if (HasFamily(f)) {
            g_familyDisplay = f;
            break;
        }
    Log(L"gfx: 字体 %ls / %ls", g_familyText.c_str(), g_familyDisplay.c_str());
    g_clockWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_clockThread = std::thread(ClockLoop);
    return true;
}

void gfx::Shutdown() {
    g_clockStop = true;
    if (g_clockWake) SetEvent(g_clockWake);
    if (g_clockThread.joinable()) g_clockThread.join();
    if (g_clockWake) {
        CloseHandle(g_clockWake);
        g_clockWake = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_clocksMx);
        for (auto* c : g_clocks) delete c;
        g_clocks.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_fontMx);
        for (auto& [k, f] : g_fonts)
            if (f) f->Release();
        g_fonts.clear();
    }
    if (g_brush) {
        g_brush->Release();
        g_brush = nullptr;
    }
    if (g_dcomp) {
        g_dcomp->Release();
        g_dcomp = nullptr;
    }
    if (g_dc) {
        g_dc->Release();
        g_dc = nullptr;
    }
    if (g_dxgi) {
        g_dxgi->Release();
        g_dxgi = nullptr;
    }
    if (g_d3d) {
        g_d3d->Release();
        g_d3d = nullptr;
    }
    if (g_f1) {
        g_f1->Release();
        g_f1 = nullptr;
    }
    if (g_dw) {
        g_dw->Release();
        g_dw = nullptr;
    }
}

ID2D1DeviceContext* gfx::Dc() {
    if (g_dc && !g_needRecreate) return g_dc;
    if (g_dc) {  // 设备丢失后重建全部资源。
        if (g_brush) { g_brush->Release(); g_brush = nullptr; }
        g_dc->Release(); g_dc = nullptr;
        if (g_dcomp) { g_dcomp->Release(); g_dcomp = nullptr; }
        if (g_dxgi) { g_dxgi->Release(); g_dxgi = nullptr; }
        if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
        g_needRecreate = false;
    }
    if (!g_f1) return nullptr;
    Com<IDXGIAdapter> ad = AdapterOnScreen();
    bool onScreen = false;
    HRESULT hr = E_FAIL;
    if (ad) {
        hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
        if (SUCCEEDED(hr)) onScreen = true;
    }
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr) || !g_d3d) {
        Log(L"gfx: 没有 D3D11 设备 (0x%08X)", (unsigned)hr);
        return nullptr;
    }
    Log(L"gfx: 显卡%ls", onScreen ? L"（点亮屏幕的那块）" : L"（默认）");
    g_d3d->QueryInterface(IID_PPV_ARGS(&g_dxgi));
    Com<ID2D1Device> dev2d;
    if (FAILED(g_f1->CreateDevice(g_dxgi, dev2d.Put())) || !dev2d) return nullptr;
    if (FAILED(dev2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc)) || !g_dc) return nullptr;
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(DCompositionCreateDevice(g_dxgi, IID_PPV_ARGS(&g_dcomp)))) g_dcomp = nullptr;
    return g_dc;
}

ID2D1DeviceContext* gfx::Begin(Layer& l, int w, int h, float scale) {
    if (!Dc()) return nullptr;
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (!l.swap) {
        Com<IDXGIFactory2> fac;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.Put()))) || !fac) return nullptr;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        HRESULT hr = fac->CreateSwapChainForComposition(g_d3d, &d, nullptr, &l.swap);
        if (FAILED(hr) || !l.swap || !g_dcomp) {
            Log(L"gfx: 交换链失败 (0x%08X)", (unsigned)hr);
            return nullptr;
        }
        g_dcomp->CreateTargetForHwnd(l.hwnd, TRUE, &l.target);
        g_dcomp->CreateVisual(&l.visual);
        if (!l.target || !l.visual) return nullptr;
        l.visual->SetContent(l.swap);
        l.target->SetRoot(l.visual);
        g_dcomp->Commit();
        l.w = w;
        l.h = h;
        BindBack(l);
    } else if (l.w != w || l.h != h) {
        g_dc->SetTarget(nullptr);
        if (l.back) {
            l.back->Release();
            l.back = nullptr;
        }
        l.swap->ResizeBuffers(2, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        l.w = w;
        l.h = h;
        BindBack(l);
    }
    if (!l.back) return nullptr;
    g_dc->SetTarget(l.back);
    g_dc->SetDpi(96 * scale, 96 * scale);
    g_dc->BeginDraw();
    g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return g_dc;
}

void gfx::End(Layer& l) {
    if (!g_dc) return;
    HRESULT hr = g_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        Log(L"gfx: 设备丢了，重建");
        g_needRecreate = true;
        Free(l);
        return;
    }
    if (FAILED(hr)) LogDbg(L"gfx: EndDraw 0x%08X", (unsigned)hr);
    if (l.swap) l.swap->Present(0, 0);
    g_dc->SetTarget(nullptr);
}

void gfx::Free(Layer& l) {
    if (l.back) {
        l.back->Release();
        l.back = nullptr;
    }
    if (l.visual) {
        l.visual->Release();
        l.visual = nullptr;
    }
    if (l.target) {
        l.target->Release();
        l.target = nullptr;
    }
    if (l.swap) {
        l.swap->Release();
        l.swap = nullptr;
    }
    l.w = l.h = 0;
}

void gfx::FrameClock(HWND hwnd, UINT msg, bool on) {
    {
        std::lock_guard<std::mutex> lk(g_clocksMx);
        for (auto* c : g_clocks)
            if (c->hwnd == hwnd && c->msg == msg) {
                c->on = on;
                if (on && g_clockWake) SetEvent(g_clockWake);
                return;
            }
        if (!on) return;
        g_clocks.push_back(new Clock{ hwnd, msg, true, false });
    }
    if (g_clockWake) SetEvent(g_clockWake);
}

void gfx::FrameAck(HWND hwnd, UINT msg) {
    std::lock_guard<std::mutex> lk(g_clocksMx);
    for (auto* c : g_clocks)
        if (c->hwnd == hwnd && c->msg == msg) {
            c->pending = false;
            return;
        }
}

bool gfx::Dark() {
    static ULONGLONG last = 0;
    static bool dark = false;
    ULONGLONG now = GetTickCount64();
    if (now - last > 2000) {
        dark = RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
        last = now;
    }
    return dark;
}

float gfx::Scale(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return dpi ? dpi / 96.f : 1.f;
}

IDWriteTextFormat* gfx::Format(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool display) {
    if (!g_dw) return nullptr;
    FontKey k{ (int)std::lround(size * 10), (int)w, (int)a, display };
    {
        std::lock_guard<std::mutex> lk(g_fontMx);
        auto it = g_fonts.find(k);
        if (it != g_fonts.end()) return it->second;
    }
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(g_dw->CreateTextFormat((display ? g_familyDisplay : g_familyText).c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &tf)) ||
        !tf)
        return nullptr;
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    Com<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(tf, ellipsis.Put())) && ellipsis) tf->SetTrimming(&tr, ellipsis.Get());
    {
        std::lock_guard<std::mutex> lk(g_fontMx);
        auto it = g_fonts.find(k);
        if (it != g_fonts.end()) {  // 另一线程已创建同名格式，此处直接复用。
            tf->Release();
            return it->second;
        }
        g_fonts[k] = tf;
    }
    return tf;
}

float gfx::TextWidth(IDWriteTextFormat* tf, const std::wstring& s) {
    if (!g_dw || !tf) return 0;
    Com<IDWriteTextLayout> tl;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 10000, 100, tl.Put())) || !tl) return 0;
    DWRITE_TEXT_METRICS tm{};
    tl->GetMetrics(&tm);
    return tm.widthIncludingTrailingWhitespace;
}

ID2D1SolidColorBrush* gfx::Brush() {
    if (!Dc()) return nullptr;
    if (!g_brush) g_dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_brush);
    return g_brush;
}

ID2D1Bitmap* gfx::Bitmap(ID2D1RenderTarget* rt, UINT w, UINT h, const std::vector<BYTE>& bgra) {
    if (!rt || !w || !h || bgra.size() < (size_t)w * h * 4) return nullptr;
    ID2D1Bitmap* b = nullptr;
    rt->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), w * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    return b;
}

bool gfx::LoadImage(const std::wstring& path, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    static IWICImagingFactory* wic = nullptr;
    static bool tried = false;
    static std::mutex wicMx;
    if (!wic) {
        std::lock_guard<std::mutex> lk(wicMx);
        if (!wic && !tried) {
            tried = true;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        }
    }
    if (!wic || path.empty()) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.Put())) || !dec)
        return false;
    bool ok = false;
    Com<IWICBitmapFrameDecode> fr;
    Com<IWICFormatConverter> cv;
    if (SUCCEEDED(dec->GetFrame(0, fr.Put())) && SUCCEEDED(wic->CreateFormatConverter(cv.Put())) &&
        SUCCEEDED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        cv->GetSize(&w, &h);
        bgra.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
    }
    return ok;
}

void gfx::Downscale(UINT& w, UINT& h, std::vector<BYTE>& px, UINT maxSide) {
    if (!w || !h || (w <= maxSide && h <= maxSide) || px.size() < (size_t)w * h * 4) return;
    double k = (double)maxSide / std::max(w, h);
    UINT nw = std::max<UINT>(1, (UINT)std::lround(w * k)), nh = std::max<UINT>(1, (UINT)std::lround(h * k));
    double sx = (double)w / nw, sy = (double)h / nh;
    std::vector<BYTE> out((size_t)nw * nh * 4);
    for (UINT y = 0; y < nh; y++) {
        double y0 = y * sy, y1 = y0 + sy;
        for (UINT x = 0; x < nw; x++) {
            double x0 = x * sx, x1 = x0 + sx, acc[4] = {}, area = 0;
            for (UINT yy = (UINT)y0; yy < h && yy < y1; yy++) {
                double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                for (UINT xx = (UINT)x0; xx < w && xx < x1; xx++) {
                    double wgt = wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
                    const BYTE* p = &px[((size_t)yy * w + xx) * 4];
                    for (int c = 0; c < 4; c++) acc[c] += p[c] * wgt;
                    area += wgt;
                }
            }
            BYTE* o = &out[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = (BYTE)std::clamp(std::lround(acc[c] / area), 0L, 255L);
        }
    }
    px.swap(out);
    w = nw;
    h = nh;
}

void gfx::TrimToSquare(UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    int x0 = (int)w, y0 = (int)h, x1 = -1, y1 = -1;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++)
            if (bgra[((size_t)y * w + x) * 4 + 3] > 24) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1, s = std::max(cw, ch);
    if (s >= (int)w - 1 && s >= (int)h - 1) return;
    std::vector<BYTE> out((size_t)s * s * 4, 0);
    int ox = (s - cw) / 2, oy = (s - ch) / 2;
    for (int y = 0; y < ch; y++) memcpy(&out[((size_t)(y + oy) * s + ox) * 4], &bgra[((size_t)(y + y0) * w + x0) * 4], (size_t)cw * 4);
    bgra.swap(out);
    w = h = (UINT)s;
}

void gfx::RoundRect(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, D2D1_COLOR_F fill) {
    auto* br = Brush();
    if (!dc || !br) return;
    br->SetColor(fill);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), br);
}

void gfx::RoundRectStroke(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, D2D1_COLOR_F stroke, float width) {
    auto* br = Brush();
    if (!dc || !br) return;
    br->SetColor(stroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), br, width);
}

void gfx::Shadow(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, float strength) {
    auto* br = Brush();
    if (!dc || !br) return;
    const Fluent& t = T();
    for (int i = 4; i >= 1; i--) {
        float d = i * 1.8f;
        br->SetColor(D2D1::ColorF(t.shadow.r, t.shadow.g, t.shadow.b, t.shadow.a * 0.11f * strength));
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left - d, r.top - d + 3.f, r.right + d, r.bottom + d + 3.f), radius + d, radius + d), br);
    }
}

void gfx::Text(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F color) {
    auto* br = Brush();
    if (!dc || !br || !tf || s.empty()) return;
    br->SetColor(color);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void gfx::IconPlus(ID2D1DeviceContext* dc, D2D1_POINT_2F c, D2D1_COLOR_F color, float half) {
    auto* br = Brush();
    if (!dc || !br) return;
    br->SetColor(color);
    dc->DrawLine({ c.x - half, c.y }, { c.x + half, c.y }, br, 1.6f);
    dc->DrawLine({ c.x, c.y - half }, { c.x, c.y + half }, br, 1.6f);
}

void gfx::IconClose(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float half, D2D1_COLOR_F color, float stroke) {
    auto* br = Brush();
    if (!dc || !br) return;
    br->SetColor(color);
    dc->DrawLine({ c.x - half, c.y - half }, { c.x + half, c.y + half }, br, stroke);
    dc->DrawLine({ c.x - half, c.y + half }, { c.x + half, c.y - half }, br, stroke);
}


