// 开机自启模块，在用户注册表 Run 项写入本程序路径。
#pragma once

#include "common.h"

namespace startup {

bool IsElevated();           // 返回令牌是否带提权标志。
bool IsHighIntegrity();      // 返回进程完整性级别是否达到 High。
int  Method();               // 返回自启状态，0 表示未开启，1 表示 Run 项指向本程序。
bool IsEnabled();            // 返回自启是否开启。
bool SetEnabled(bool on);    // 设置自启开关，返回是否成功。

}  // namespace startup
