// 窗口快照模块，先用 DWM 缩略图把窗口内容贴到隐藏窗口，再用 PrintWindow 整块抓取。
// 目标窗口不会收到消息，卡死的窗口不会阻塞调用方。
// 最小化窗口可以取到最后一帧。
// 每次抓取都新建隐藏窗口，复用同一窗口时时 DWM 可能停止合成缩略图。
// 复用会导致首次抓取正常，后续抓到空白。
// 后续计划接入 Windows.Graphics.Capture 实现实时预览。
#include "preview.h"

#include <dwmapi.h>

#include "com.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

const wchar_t* kCapClass = L"FlipRowCap";
bool g_classReady = false;

bool EnsureClass() {
    if (g_classReady) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCapClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_classReady = true;
    return true;
}

// 创建一次性的隐藏窗口，DWM 仍会向其合成缩略图。
HWND MakeStage(UINT sw, UINT sh) {
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, kCapClass, L"",
                             WS_POPUP, 0, 0, (int)sw, (int)sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return nullptr;
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(h, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

bool ReadDib(HBITMAP bm, void* bits, UINT w, UINT h, std::vector<BYTE>& px) {
    if (!bm || !bits) return false;
    GdiFlush();
    px.assign((const BYTE*)bits, (const BYTE*)bits + (size_t)w * h * 4);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // PrintWindow 输出的 alpha 为 0，此处补为不透明。
    return true;
}

}

HWND preview::CaptureWindow() { return nullptr; }

bool preview::Window(HWND h, const RECT& src, UINT maxSide, std::vector<BYTE>& px, UINT& w, UINT& hh) {
    px.clear();
    w = hh = 0;
    if (!h || !IsWindow(h)) return false;
    UINT sw = (UINT)std::max(0L, src.right - src.left);
    UINT sh = (UINT)std::max(0L, src.bottom - src.top);
    if (sw < 8 || sh < 8 || sw > 16384 || sh > 16384) return false;
    if (!EnsureClass()) return false;

    HWND stage = MakeStage(sw, sh);
    if (!stage) return false;

    HTHUMBNAIL th = nullptr;
    if (FAILED(DwmRegisterThumbnail(stage, h, &th)) || !th) {
        DestroyWindow(stage);
        return false;
    }
    DWM_THUMBNAIL_PROPERTIES tp{};
    tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    tp.rcSource = src;
    tp.rcDestination = { 0, 0, (LONG)sw, (LONG)sh };
    tp.fVisible = TRUE;
    tp.opacity = 255;
    tp.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(th, &tp);

    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)sw, -(LONG)sh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool got = false;
    if (bm && bits) {
        HGDIOBJ old = SelectObject(mdc, bm);
        for (int tries = 0; tries < 4 && !got; tries++) {
            if (tries) {
                DwmFlush();  // 等待 DWM 将缩略图绘制到该窗口。
                Sleep(8);
            }
            if (!PrintWindow(stage, mdc, PW_RENDERFULLCONTENT)) continue;
            GdiFlush();
            const uint32_t* p = (const uint32_t*)bits;
            int lit = 0;
            for (UINT y = 0; y < sh; y += 11)
                for (UINT x = 0; x < sw; x += 11)
                    if (p[(size_t)y * sw + x] & 0xFFFFFF) lit++;  // 全黑表示未取到画面，常见于受保护进程或 DWM 不提供画面的窗口。
            if (lit > 3) got = true;
        }
        if (got) {
            UINT ow = sw, oh = sh;
            if (maxSide && (sw > maxSide || sh > maxSide)) {
                float k = (float)maxSide / (float)std::max(sw, sh);
                ow = std::max<UINT>(1, (UINT)std::lround(sw * k));
                oh = std::max<UINT>(1, (UINT)std::lround(sh * k));
            }
            if (ow != sw || oh != sh) {  // 用 GDI 缩放，速度高于逐像素计算。
                BITMAPINFO bi2{};
                bi2.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)ow, -(LONG)oh, 1, 32, BI_RGB };
                void* bits2 = nullptr;
                HBITMAP bm2 = CreateDIBSection(sdc, &bi2, DIB_RGB_COLORS, &bits2, nullptr, 0);
                if (bm2 && bits2) {
                    HDC mdc2 = CreateCompatibleDC(sdc);
                    HGDIOBJ old2 = SelectObject(mdc2, bm2);
                    SetStretchBltMode(mdc2, HALFTONE);
                    SetBrushOrgEx(mdc2, 0, 0, nullptr);
                    StretchBlt(mdc2, 0, 0, (int)ow, (int)oh, mdc, 0, 0, (int)sw, (int)sh, SRCCOPY);
                    GdiFlush();
                    got = ReadDib(bm2, bits2, ow, oh, px);
                    SelectObject(mdc2, old2);
                    DeleteDC(mdc2);
                    DeleteObject(bm2);
                    w = ow;
                    hh = oh;
                } else {
                    got = false;
                }
            } else {
                got = ReadDib(bm, bits, sw, sh, px);
                w = sw;
                hh = sh;
            }
        }
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    DwmUnregisterThumbnail(th);
    DestroyWindow(stage);
    if (!got) {
        px.clear();
        w = hh = 0;
    }
    return got;
}
