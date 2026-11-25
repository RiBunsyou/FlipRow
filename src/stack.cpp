// 二维堆叠式应用切换器，由全屏 DirectComposition 浮层和一叠窗口卡片组成。
// 布局为一叠牌，正面卡片位于右侧且尺寸最大，其余卡片向左依次错开。
// Tab 键把正面卡片送到队尾，下一张成为正面。
// 层深由卡片序号与选中序号之差的模运算得到，0 为正面，n 减 1 为队尾。
// Tab 使层深减一，正面卡片的层深变为 n 减 1，因此滑向最左并淡出。
// 预览为异步快照，按窗口缓存，第二次打开时先显示缓存画面再刷新。
#include "stack.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>

#include "anim.h"
#include "com.h"
#include "gfx.h"
#include "preview.h"
#include "wins.h"

#include <d2d1.h>

namespace {

const UINT WM_ST_FRAME = WM_APP + 80;  // 帧时钟消息。
const UINT WM_ST_SHOT = WM_APP + 81;  // 快照完成消息。

const float CARD_RADIUS = 8.f;  // 卡片圆角，单位 DIP，取 Win11 的 8 像素。
const size_t MAX_CARDS = 12;  // 一次最多显示的卡片数量。

// 一叠牌的几何参数。
const int MAX_DEPTH = 8;  // 最多显示的层数，更深的层淡出。
const float STEP_RATIO = 0.06f;  // 每层的水平错开量，取卡片宽度的 6%。
const float STEP_Y = 6.f;  // 每层的垂直偏移。
const float STEP_S = 0.010f;  // 每层的缩放比例。
const float DECK_MARGIN = 40.f;  // 整叠牌左右预留的边距。

// 卡片高度取屏幕高度的 0.70，宽度按窗口宽高比计算。
const float CARD_H_RATIO = 0.70f;

struct Shot {
    int index = -1;
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
};

struct Card {
    HWND h = nullptr;
    std::wstring title;
    std::wstring app;
    RECT src{};
    float aspect = 1.6f;
    float cw = 0, ch = 0;  // 卡片尺寸，单位 DIP。
    bool mini = false;  // 是否为最小化窗口。
    ID2D1Bitmap* bmp = nullptr;  // 窗口快照。
    ID2D1Bitmap* icon = nullptr;  // 应用图标。

    // 动画状态，当前值逐帧逼近目标值。
    float x = 0, y = 0, s = 1, a = 1;
    float tx = 0, ty = 0, ts = 1, ta = 1;
};

HWND g_wnd = nullptr;
gfx::Layer g_layer;
ID2D1Layer* g_clip = nullptr;
ID2D1Layer* g_op = nullptr;
ID2D1Bitmap* g_desk = nullptr;  // 桌面缩略图，放大后作为模糊背景。

std::vector<Card> g_cards;

// 窗口快照缓存，按字节预算淘汰最久未用的条目。
struct Snap {
    ID2D1Bitmap* bmp = nullptr;
    size_t bytes = 0;
    ULONGLONG last = 0;
    float aspect = 0.f;  // 快照对应的窗口宽高比，窗口尺寸变化后该条目失效。
};
std::unordered_map<HWND, Snap> g_snap;
size_t g_snapBytes = 0;
const size_t SNAP_BUDGET = 150ull * 1024 * 1024;  // 快照缓存的内存上限。

int g_sel = 0;
int g_demoSel = -1;  // 演示模式下强制指定的正面卡片序号。
bool g_active = false;
bool g_open = false;
bool g_loggedPaint = false;
RECT g_mon{};
double g_last = 0;
float g_fade = 0.f;  // 开场压暗的进度，取值 0 到 1。
std::atomic<bool> g_cancel{false};

std::wstring BaseName(std::wstring p) {
    auto i = p.find_last_of(L"\\/");
    if (i != std::wstring::npos) p = p.substr(i + 1);
    auto d = p.find_last_of(L'.');
    if (d != std::wstring::npos && d > 0) p = p.substr(0, d);
    return p;
}

float Sc() { return gfx::Scale(g_wnd); }
float WD() { return (g_mon.right - g_mon.left) / Sc(); }
float HD() { return (g_mon.bottom - g_mon.top) / Sc(); }

int DepthOf(int i) {
    const int n = (int)g_cards.size();
    if (n <= 1) return 0;
    int d = (i - g_sel) % n;
    return d < 0 ? d + n : d;
}

void DrawCover(ID2D1DeviceContext* dc, ID2D1Bitmap* b, const D2D1_RECT_F& r) {
    if (!b || !dc) return;
    auto s = b->GetSize();
    if (s.width < 1 || s.height < 1) return;
    float k = std::max((r.right - r.left) / s.width, (r.bottom - r.top) / s.height);
    float w = s.width * k, h = s.height * k;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    dc->DrawBitmap(b, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.f,
                   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// 绘制窗口内容，保持完整可见且不裁切。
void DrawContain(ID2D1DeviceContext* dc, ID2D1Bitmap* b, const D2D1_RECT_F& r) {
    if (!b || !dc) return;
    auto s = b->GetSize();
    if (s.width < 1 || s.height < 1) return;
    float k = std::min((r.right - r.left) / s.width, (r.bottom - r.top) / s.height);
    float w = s.width * k, h = s.height * k;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    dc->DrawBitmap(b, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.f,
                   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// 抓取桌面缩略图，缩小后放大用作模糊背景。
void MakeBackdrop() {
    if (g_desk) {
        g_desk->Release();
        g_desk = nullptr;
    }
    int w = g_mon.right - g_mon.left, h = g_mon.bottom - g_mon.top;
    if (w <= 0 || h <= 0) return;
    int bw = 192, bh = std::max(1, (int)std::lround(192.0 * h / w));
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bm && bits) {
        HGDIOBJ old = SelectObject(mdc, bm);
        SetStretchBltMode(mdc, HALFTONE);
        SetBrushOrgEx(mdc, 0, 0, nullptr);
        StretchBlt(mdc, 0, 0, bw, bh, sdc, g_mon.left, g_mon.top, w, h, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        std::vector<BYTE> px((const BYTE*)bits, (const BYTE*)bits + (size_t)bw * bh * 4);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        g_desk = gfx::Bitmap(gfx::Dc(), bw, bh, px);
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}

void FreeCards() {
    for (auto& c : g_cards) {
        if (c.bmp) c.bmp->Release();  // 缓存中另有一份引用。
        if (c.icon) c.icon->Release();
    }
    g_cards.clear();
}

// 枚举窗口并生成卡片，顺序为 z 序从上到下，第 0 张为当前前台窗口。
void Collect() {
    FreeCards();
    g_sel = 0;
    auto list = wins::CollectWindows(true, true);
    for (auto& w : list) {
        if (g_cards.size() >= MAX_CARDS) break;
        Card c;
        c.h = w.h;
        c.title = w.title;
        c.src = w.src;
        c.mini = w.mini;
        c.app = BaseName(w.exe);
        float fw = (float)(w.frame.right - w.frame.left), fh = (float)(w.frame.bottom - w.frame.top);
        c.aspect = std::clamp(fw / std::max(1.f, fh), 0.45f, 2.4f);
        UINT iw = 0, ih = 0;
        std::vector<BYTE> px;
        if (wins::IconOfFile(w.exe, 64, iw, ih, px)) c.icon = gfx::Bitmap(gfx::Dc(), iw, ih, px);
        // 复用上次会话的快照，窗口尺寸变化时丢弃。
        auto it = g_snap.find(c.h);
        if (it != g_snap.end()) {
            const bool sameShape = it->second.bmp && std::abs(it->second.aspect - c.aspect) < 0.03f;
            if (sameShape) {
                it->second.last = GetTickCount64();
                it->second.bmp->AddRef();
                c.bmp = it->second.bmp;
            } else {
                g_snapBytes -= it->second.bytes;
                if (it->second.bmp) it->second.bmp->Release();
                g_snap.erase(it);
            }
        }
        g_cards.push_back(std::move(c));
        if (w.mini)
            LogDbg(L"最小化窗口：%ls frame=%ldx%ld src=%ld,%ld %ldx%ld", w.title.c_str(), w.frame.right - w.frame.left,
                   w.frame.bottom - w.frame.top, w.src.left, w.src.top, w.src.right - w.src.left, w.src.bottom - w.src.top);
    }
    Log(L"堆叠切换器：%zu 个窗口（缓存命中 %d）", g_cards.size(),
        (int)std::count_if(g_cards.begin(), g_cards.end(), [](const Card& c) { return c.bmp != nullptr; }));
}

// 快照在工作线程中采集，界面线程只创建位图和刷新。
void StartCapture(HWND wnd) {
    std::vector<HWND> hs;
    std::vector<RECT> srcs;
    for (auto& c : g_cards) {
        hs.push_back(c.h);
        srcs.push_back(c.src);
    }
    g_cancel = false;
    std::thread([hs, srcs, wnd] {
        for (size_t i = 0; i < hs.size(); i++) {
            if (g_cancel) return;
            UINT w = 0, h = 0;
            std::vector<BYTE> px;
            if (!preview::Window(hs[i], srcs[i], 1920, px, w, h)) {  // 卡片尺寸放大后，快照分辨率提高到 1920。
                Log(L"快照失败：hwnd=%p", (void*)hs[i]);
                continue;
            }
            auto* m = new Shot{ (int)i, w, h, std::move(px) };
            if (!PostMessageW(wnd, WM_ST_SHOT, 0, (LPARAM)m)) {
                delete m;
                return;
            }
        }
    }).detach();
}

// 计算一叠牌中每张卡片的位置和尺寸。
void Layout() {
    const int n = (int)g_cards.size();
    if (!n) return;
    const float W = WD(), H = HD();

    // 统一高度，宽度按窗口宽高比，整叠放不下时整体等比缩小。
    float h = H * CARD_H_RATIO;
    float maxW = 0.f;
    for (auto& c : g_cards) maxW = std::max(maxW, h * c.aspect);
    float stepX = maxW * STEP_RATIO;
    const float avail = W - DECK_MARGIN * 2.f;
    const int layers = std::min(std::max(0, n - 1), MAX_DEPTH);
    float deck = maxW + layers * stepX;
    if (deck > avail) {
        const float k = avail / deck;
        h *= k;
        maxW *= k;
        stepX *= k;
    }
    for (auto& c : g_cards) {
        c.cw = h * c.aspect;
        c.ch = h;
    }

    // 整叠牌整体居中，正面卡片向右让出后方卡片的宽度。
    const float stackW = layers * stepX;
    const float frontX = W * 0.5f + stackW * 0.5f;

    for (int i = 0; i < n; i++) {
        Card& c = g_cards[i];
        const int d = DepthOf(i);
        const int dd = std::min(d, MAX_DEPTH);
        c.tx = frontX - dd * stepX;
        c.ty = H * 0.50f + dd * STEP_Y;
        c.ts = 1.f - dd * STEP_S;
        c.ta = d > MAX_DEPTH ? 0.f : 1.f;
    }
}

// 绘制 Fluent 风格的胶囊，包含底色，强调色薄涂，描边和顶部高光。
// 深色主题下底色更实，浅色主题下更淡。
void FluentChip(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius) {
    const auto& t = gfx::T();
    gfx::Shadow(dc, r, radius, 0.7f);
    gfx::RoundRect(dc, r, radius, D2D1::ColorF(0, 0, 0, t.dark ? 0.55f : 0.38f));
    gfx::RoundRect(dc, r, radius, D2D1::ColorF(t.accent.r, t.accent.g, t.accent.b, 0.26f));
    gfx::RoundRectStroke(dc, r, radius, D2D1::ColorF(1, 1, 1, 0.18f), 1.f);
    if (auto* br = gfx::Brush()) {  // 绘制顶部高光。
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
        dc->DrawLine(D2D1::Point2F(r.left + radius, r.top + 0.5f), D2D1::Point2F(r.right - radius, r.top + 0.5f), br, 1.f);
    }
}

// 绘制带暗影的白色文字。
void ShadowText(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* tf, const D2D1_RECT_F& r, D2D1_COLOR_F color) {
    gfx::Text(dc, s, tf, D2D1::RectF(r.left, r.top + 1.5f, r.right, r.bottom + 1.5f), D2D1::ColorF(0, 0, 0, 0.55f));
    gfx::Text(dc, s, tf, r, color);
}

void PaintCard(ID2D1DeviceContext* dc, Card& c, bool sel) {
    if (c.a < 0.02f) return;
    const auto& t = gfx::T();
    D2D1_RECT_F r = D2D1::RectF(-c.cw / 2, -c.ch / 2, c.cw / 2, c.ch / 2);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(c.s, c.s) * D2D1::Matrix3x2F::Translation(c.x, c.y));

    if (!g_op) dc->CreateLayer(nullptr, &g_op);
    if (g_op)
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), c.a),
                      g_op);

    gfx::Shadow(dc, r, CARD_RADIUS, sel ? 1.8f : 1.0f);
    gfx::RoundRect(dc, r, CARD_RADIUS, t.card);

    Com<ID2D1Factory> fac;
    dc->GetFactory(fac.Put());
    Com<ID2D1RoundedRectangleGeometry> geo;
    if (fac) fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, CARD_RADIUS, CARD_RADIUS), geo.Put());
    if (geo && g_clip) {
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get()), g_clip);
        if (c.bmp) {
            DrawContain(dc, c.bmp, r);  // 保持内容完整可见。
        } else if (c.icon) {  // 尚未取得快照时显示图标占位。
            auto sz = c.icon->GetSize();
            float k = std::min(c.cw * 0.24f / sz.width, c.ch * 0.24f / sz.height);
            float iw = sz.width * k, ih = sz.height * k;
            dc->DrawBitmap(c.icon, D2D1::RectF(-iw / 2, -ih / 2, iw / 2, ih / 2), 1.f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        dc->PopLayer();
    }
    if (!sel) gfx::RoundRect(dc, r, CARD_RADIUS, D2D1::ColorF(0, 0, 0, 0.26f));  // 压暗非选中卡片。
    gfx::RoundRectStroke(dc, r, CARD_RADIUS, sel ? t.accent : t.cardStroke, sel ? 2.5f : 1.f);
    if (g_op) dc->PopLayer();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Paint() {
    if (!g_open || !g_wnd || g_cards.empty()) return;
    const float sc = Sc();
    const int pw = g_mon.right - g_mon.left, ph = g_mon.bottom - g_mon.top;
    auto* dc = gfx::Begin(g_layer, pw, ph, sc);
    if (!dc) return;
    if (!g_clip) dc->CreateLayer(nullptr, &g_clip);

    const float W = WD(), H = HD();
    const auto& t = gfx::T();
    if (!g_loggedPaint) {
        g_loggedPaint = true;
        BootLog(L"Paint: 画布 %d x %d px / %.0f x %.0f dip / scale %.2f / 卡片 %zu / 背景 %d", pw, ph, W, H, sc, g_cards.size(),
                g_desk ? 1 : 0);
    }

    // 绘制背景。
    if (g_desk) DrawCover(dc, g_desk, D2D1::RectF(0, 0, W, H));
    if (auto* br = gfx::Brush()) {
        if (!g_desk) {
            br->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.07f, 1));
            dc->FillRectangle(D2D1::RectF(0, 0, W, H), br);
        }
        br->SetColor(D2D1::ColorF(0, 0, 0, (t.dark ? 0.60f : 0.50f) * g_fade));
        dc->FillRectangle(D2D1::RectF(0, 0, W, H), br);
    }

    // 按层深从深到浅绘制卡片。
    const int n = (int)g_cards.size();
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return DepthOf(a) > DepthOf(b); });
    for (int i : order) PaintCard(dc, g_cards[i], i == g_sel);

    // 绘制选中卡片下方的标题。
    if (g_sel >= 0 && g_sel < n) {
        Card& c = g_cards[g_sel];
        auto* tf = gfx::Format(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        float tw = tf ? gfx::TextWidth(tf, c.title) : 0.f;
        if (tw > 520.f) tw = 520.f;
        const float padX = 14.f, iconSz = 18.f, gap = 8.f, tagW = c.mini ? 56.f : 0.f, hgt = 36.f;
        float pillW = padX + (c.icon ? iconSz + gap : 0.f) + tw + (c.mini ? gap + tagW : 0.f) + padX;
        // 位置取整到像素，避免动画收敛过程的亚像素移动导致文字重排。
        // 会让 DirectWrite 每帧按不同的舍入位置排版，最后一个字时隐时现
        const float cx = std::round(c.x);
        const float top = std::round(c.y + c.ch * 0.5f + 18.f);
        const float left = std::round(cx - pillW * 0.5f);
        D2D1_RECT_F pill = D2D1::RectF(left, top, left + pillW, top + hgt);
        FluentChip(dc, pill, hgt / 2);
        float x = std::round(left + padX);
        if (c.icon) {
            dc->DrawBitmap(c.icon, D2D1::RectF(x, top + (hgt - iconSz) / 2, x + iconSz, top + (hgt + iconSz) / 2), 1.f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            x += iconSz + gap;
        }
        // 文字矩形预留 3 像素余量，避免最后一个字符被替换为省略号。
        gfx::Text(dc, c.title, tf, D2D1::RectF(x, top, x + tw + 3.f, top + hgt), D2D1::ColorF(1, 1, 1, 0.96f));
        x += tw + gap;
        if (c.mini) {
            auto* tfm = gfx::Format(11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D1_RECT_F tag{ x, top + 8, x + tagW, top + hgt - 8 };
            gfx::RoundRect(dc, tag, (hgt - 16) / 2, D2D1::ColorF(1, 1, 1, 0.18f));
            gfx::Text(dc, Tr(L"最小化", L"Minimized"), tfm, tag, D2D1::ColorF(1, 1, 1, 0.9f));
        }
    }

    // 顶部状态胶囊。
    if (auto* tf = gfx::Format(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER)) {
        wchar_t buf[96] = {};
        swprintf_s(buf, Tr(L"Alt+Tab · %zu 个窗口", L"Alt+Tab · %zu windows"), g_cards.size());
        float pillW = 176.f;
        float px = std::round(W / 2 - pillW / 2);
        D2D1_RECT_F pill = D2D1::RectF(px, 26, px + pillW, 60);
        FluentChip(dc, pill, 17.f);
        gfx::Text(dc, buf, tf, pill, D2D1::ColorF(1, 1, 1, 0.97f));
    }
    // 底部操作提示。
    if (auto* tf = gfx::Format(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER)) {
        ShadowText(dc, Tr(L"Tab / ← → 切换   ·   松开 Alt 确认   ·   Esc 取消", L"Tab / arrows to cycle   ·   release Alt to switch   ·   Esc to cancel"),
                   tf, D2D1::RectF(0, H - 60, W, H - 26), D2D1::ColorF(1, 1, 1, 0.92f));
    }
    gfx::End(g_layer);
}

void Tick() {
    const double now = anim::Now();
    float dt = (float)((now - g_last) / 1000.0);
    g_last = now;
    dt = std::clamp(dt, 0.f, 0.1f);
    const float k = 1.f - std::exp(-dt * 15.f);
    for (auto& c : g_cards) {
        c.x += (c.tx - c.x) * k;
        c.y += (c.ty - c.y) * k;
        c.s += (c.ts - c.s) * k;
        c.a += (c.ta - c.a) * k;
        // 位置收敛后吸附到目标值，避免持续重绘。
        if (std::abs(c.tx - c.x) < 0.05f) c.x = c.tx;
        if (std::abs(c.ty - c.y) < 0.05f) c.y = c.ty;
        if (std::abs(c.ts - c.s) < 0.001f) c.s = c.ts;
        if (std::abs(c.ta - c.a) < 0.002f) c.a = c.ta;
    }
    g_fade += (1.f - g_fade) * k;
    if (g_fade > 0.999f) g_fade = 1.f;

    // 部分程序自身也处于置顶状态，因此周期性重新置顶。
    // 间隔约为 0.5 秒。
    static int raiseTick = 0;
    if (++raiseTick >= 30) {
        raiseTick = 0;
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    Paint();
}

bool Inside(Card& c, float mx, float my) {
    float rx = (mx - c.x) / std::max(0.01f, c.s);
    float ry = (my - c.y) / std::max(0.01f, c.s);
    return std::abs(rx) <= c.cw / 2 && std::abs(ry) <= c.ch / 2;
}

int HitTest(POINT p) {
    const float sc = Sc();
    float mx = p.x / sc, my = p.y / sc;
    int best = -1, bestD = 1 << 20;
    for (int i = 0; i < (int)g_cards.size(); i++) {
        int d = DepthOf(i);
        if (d >= bestD) continue;
        if (Inside(g_cards[i], mx, my)) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

void OnShot(Shot* s) {
    if (!s) return;
    if (g_open && s->index >= 0 && s->index < (int)g_cards.size()) {
        Card& c = g_cards[s->index];
        {
            if (ID2D1Bitmap* b = gfx::Bitmap(gfx::Dc(), s->w, s->h, s->px)) {
                const size_t bytes = (size_t)s->w * s->h * 4;
                auto it = g_snap.find(c.h);  // 替换同一窗口的旧快照。
                if (it != g_snap.end()) {
                    g_snapBytes -= it->second.bytes;
                    if (it->second.bmp) it->second.bmp->Release();
                    g_snap.erase(it);
                }
                // 超出预算时淘汰最久未用的条目。
                while (!g_snap.empty() && g_snapBytes + bytes > SNAP_BUDGET) {
                    auto lru = g_snap.begin();
                    for (auto k = g_snap.begin(); k != g_snap.end(); ++k)
                        if (k->second.last < lru->second.last) lru = k;
                    g_snapBytes -= lru->second.bytes;
                    if (lru->second.bmp) lru->second.bmp->Release();
                    g_snap.erase(lru);
                }
                g_snap[c.h] = Snap{ b, bytes, GetTickCount64(), c.aspect };
                g_snapBytes += bytes;
                b->AddRef();  // 卡片持有独立引用。
                if (c.bmp) c.bmp->Release();
                c.bmp = b;
            }
        }
    }
    delete s;
}

void HideNow() {
    g_open = false;
    gfx::FrameClock(g_wnd, WM_ST_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    g_cancel = true;
    if (g_desk) {
        g_desk->Release();
        g_desk = nullptr;
    }
    FreeCards();
    g_active = false;
    g_fade = 0.f;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ST_FRAME:
        gfx::FrameAck(h, WM_ST_FRAME);
        Tick();
        return 0;
    case WM_ST_SHOT:
        OnShot((Shot*)l);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DPICHANGED: {  // 阻止 DefWindowProc 按建议尺寸缩小全屏窗口。
        BootLog(L"WM_DPICHANGED dpi=%u 打开=%d", HIWORD(w), (int)g_open);
        if (g_open && g_mon.right > g_mon.left) {
            SetWindowPos(g_wnd, HWND_TOPMOST, g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top,
                         SWP_NOACTIVATE);
            Layout();
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        Paint();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!g_active) return 0;
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        int i = HitTest(p);
        if (i >= 0) {
            g_sel = i;
            Layout();
            stack::Release();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!g_active) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        stack::Step(delta > 0 ? -1 : 1);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

}

bool stack::Create(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"FlipRowOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
                            L"FlipRow", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    wins::AddOurs(g_wnd);
    g_layer.hwnd = g_wnd;
    return true;
}

bool stack::IsActive() { return g_active; }

void stack::Step(int dir) {
    if (!g_active) {
        Collect();
        if (g_cards.empty()) return;
        g_active = true;
        const int n = (int)g_cards.size();
        // 打开时正面为当前最前面的窗口，Tab 键将其送到队尾。
        g_sel = g_demoSel >= 0 ? std::min(g_demoSel, n - 1) : (dir > 0 ? 0 : n - 1);

        HWND fg = GetForegroundWindow();
        HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(mon, &mi)) {
            HideNow();
            return;
        }
        g_mon = mi.rcMonitor;
        const int pw = g_mon.right - g_mon.left, ph = g_mon.bottom - g_mon.top;
        SetWindowPos(g_wnd, HWND_TOPMOST, g_mon.left, g_mon.top, pw, ph, SWP_NOACTIVATE | SWP_NOZORDER);
        MakeBackdrop();  // 必须在显示浮层之前抓取，否则画面中会包含浮层本身。
        for (auto& c : g_cards) {  // 初始状态为全部堆叠在中间，作为开场动画起点。
            c.x = WD() / 2;
            c.y = HD() * 0.50f;
            c.s = 0.90f;
            c.a = 0.f;
        }
        Layout();
        g_open = true;
        g_last = anim::Now();
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        // 不带 SWP_NOZORDER，把窗口提到置顶层的最上面。
        // 部分程序自身也处于置顶状态，仅设置 WS_EX_TOPMOST 会被其覆盖。
        SetWindowPos(g_wnd, HWND_TOPMOST, g_mon.left, g_mon.top, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        gfx::FrameClock(g_wnd, WM_ST_FRAME, true);
        StartCapture(g_wnd);
        return;
    }
    const int n = (int)g_cards.size();
    if (n < 2) return;
    g_sel = ((g_sel + dir) % n + n) % n;
    Layout();
}

void stack::Key(UINT vk) {
    if (!g_active) return;
    if (vk == VK_ESCAPE) {
        HideNow();
        return;
    }
    if (vk == VK_LEFT) {
        Step(-1);
        return;
    }
    if (vk == VK_RIGHT) {
        Step(1);
        return;
    }
}

void stack::Release() {
    if (!g_active) return;
    HWND target = (g_sel >= 0 && g_sel < (int)g_cards.size()) ? g_cards[g_sel].h : nullptr;
    HideNow();
    if (target) wins::BringToFront(target);
}

void stack::Demo(int sel) {
    if (g_active) return;
    g_demoSel = sel;
    Step(1);
    g_demoSel = -1;
}

void stack::Shutdown() {
    if (g_active || g_open) HideNow();
    FreeCards();
    for (auto& kv : g_snap)
        if (kv.second.bmp) kv.second.bmp->Release();
    g_snap.clear();
    g_snapBytes = 0;
    if (g_desk) {
        g_desk->Release();
        g_desk = nullptr;
    }
    if (g_clip) {
        g_clip->Release();
        g_clip = nullptr;
    }
    if (g_op) {
        g_op->Release();
        g_op = nullptr;
    }
    gfx::Free(g_layer);
    if (g_wnd && IsWindow(g_wnd)) DestroyWindow(g_wnd);
    g_wnd = nullptr;
}
