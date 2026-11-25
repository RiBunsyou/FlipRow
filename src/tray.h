// 托盘图标模块，负责添加，删除，右键菜单和资源管理器重启后的恢复。
// 菜单动作通过回调交给 main 处理。
#pragma once

#include "common.h"

#include <functional>

namespace tray {
constexpr UINT WM_TRAY_CALLBACK = WM_APP + 70;  // 托盘回调消息。

bool Add(HWND msgWnd, std::function<void(int)> onMenu, std::function<bool()> hotkeysOn);
void Remove();
void OnCallback(WPARAM w, LPARAM l);  // 处理托盘图标回调消息。
void OnTaskbarCreated();  // 资源管理器重启后重新添加图标。
void Refresh();  // 系统主题变化后重新加载图标。
bool Present();
}
