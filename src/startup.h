// 开机自启模块，在启动文件夹中放一个指向本程序的快捷方式。
#pragma once

#include "common.h"

namespace startup {

bool IsElevated();           // 返回当前进程是否具有管理员权限。
int  Method();               // 返回自启状态，0 表示未开启，1 表示启动文件夹中存在快捷方式。
bool IsEnabled();            // 返回自启是否开启。
bool SetEnabled(bool on);    // 设置自启开关，返回是否成功。

}  // namespace startup
