// 窗口快照模块，先用 DWM 缩略图把窗口内容贴到隐藏窗口，再用 PrintWindow 整块抓取。
// 目标窗口不会收到消息，卡死的窗口不会阻塞调用方。
// 最小化窗口可以取到最后一帧。
// 后续计划接入 Windows.Graphics.Capture 实现实时预览。
#pragma once

#include "common.h"

namespace preview {

// src 为窗口坐标中的区域，maxSide 大于 0 时按比例缩小。
bool Window(HWND h, const RECT& src, UINT maxSide, std::vector<BYTE>& px, UINT& w, UINT& hh);

// 返回隐藏窗口句柄，供测试和调试使用。
HWND CaptureWindow();

}
