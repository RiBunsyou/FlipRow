// 绘图模块，封装 Direct2D 和 DirectComposition，并提供 Fluent 配色。
#pragma once

#include "common.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dcomp.h>
#include <dwrite.h>

namespace gfx {

// Fluent 调色板，随系统深浅色和强调色变化。
struct Fluent {
    bool dark = true;
    D2D1_COLOR_F accent{};  // 强调色。
    D2D1_COLOR_F accentSoft{};  // 强调色的 16% 透明版本。
    D2D1_COLOR_F onAccent{};  // 强调色上的前景色。
    D2D1_COLOR_F text{}, text2{}, text3{};
    D2D1_COLOR_F card{}, cardStroke{}, shadow{};
    D2D1_COLOR_F bar{}, barStroke{};
    D2D1_COLOR_F hover{}, pressed{};
    D2D1_COLOR_F danger{}, dangerSoft{};
    D2D1_COLOR_F scrim{};  // 全屏压暗色。
    D2D1_COLOR_F tile{}, tileStroke{};  // 切换器面板的底色和描边色。
};
const Fluent& T();

bool Init();
void Shutdown();
bool Dark();  // 返回系统是否使用深色主题。
float Scale(HWND h);
ID2D1DeviceContext* Dc();  // 返回共享的设备上下文。

struct Layer {  // 窗口的 DirectComposition 表面。
    HWND hwnd = nullptr;
    IDXGISwapChain1* swap = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID2D1Bitmap1* back = nullptr;
    int w = 0, h = 0;
};
ID2D1DeviceContext* Begin(Layer& l, int w, int h, float scale);  // 开始一帧绘制，画面清空为透明。
void End(Layer& l);
void Free(Layer& l);

// 帧时钟，后台线程按屏幕刷新节奏向窗口发送消息。
void FrameClock(HWND hwnd, UINT msg, bool on);
void FrameAck(HWND hwnd, UINT msg);

// 返回缓存的文字格式，带省略号裁剪。
IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING, bool display = false);
float TextWidth(IDWriteTextFormat* tf, const std::wstring& s);
ID2D1SolidColorBrush* Brush();  // 返回复用的实色画刷。

ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, UINT w, UINT h, const std::vector<BYTE>& bgra);
bool LoadImage(const std::wstring& path, UINT& w, UINT& h, std::vector<BYTE>& bgra);  // 从文件加载图像，输出预乘 BGRA 像素。
void Downscale(UINT& w, UINT& h, std::vector<BYTE>& bgra, UINT maxSide);
void TrimToSquare(UINT& w, UINT& h, std::vector<BYTE>& bgra);

void RoundRect(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, D2D1_COLOR_F fill);
void RoundRectStroke(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, D2D1_COLOR_F stroke, float width = 1.f);
void Shadow(ID2D1DeviceContext* dc, D2D1_RECT_F r, float radius, float strength = 1.f);  // 绘制卡片投影。
void Text(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F color);
void IconPlus(ID2D1DeviceContext* dc, D2D1_POINT_2F c, D2D1_COLOR_F color, float half = 7.f);
void IconClose(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float half, D2D1_COLOR_F color, float stroke = 1.8f);

}
