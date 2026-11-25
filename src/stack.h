// 二维堆叠式应用切换器。
// 选中的窗口位于最上层，其余窗口向左后方堆叠，Tab 键轮换正面卡片。
#pragma once

#include "common.h"

namespace stack {

bool Create(HINSTANCE hi);
void Shutdown();

bool IsActive();

// 供钩子线程调用，函数内部只发送消息，实际处理在主线程完成。
void Step(int dir);  // 首次调用打开界面，后续调用轮换一格。
void Key(UINT vk);  // 处理取消和方向键。
void Release();  // 提交当前选择。

void Demo(int sel = -1);  // 直接打开界面，不依赖按键，sel 大于等于 0 时指定正面卡片序号。

}
