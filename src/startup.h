// 开机自启模块。
#pragma once

#include "common.h"

namespace startup {

bool IsElevated();  // 返回当前进程是否具有管理员权限。
int  Method();  // 返回自启状态，0 表示未开启，1 表示任务计划中的 FlipRow 任务。
bool IsEnabled();  // 返回自启是否开启。
bool SetEnabled(bool on);  // 设置自启开关，返回是否成功。

}
