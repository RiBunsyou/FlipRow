// 开机自启模块，通过计划任务在登录时以最高权限启动本程序。
#pragma once

#include "common.h"

namespace startup {

bool IsElevated();           // 返回令牌是否带提权标志。
bool IsHighIntegrity();      // 返回进程完整性级别是否达到 High。
bool IsEnabled();            // 返回自启是否开启，结果缓存 2 秒。
bool SetEnabled(bool on);    // 设置自启开关，返回是否成功。

}  // namespace startup
