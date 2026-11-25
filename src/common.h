// 公共头文件，包含日志，语言，调试开关和注入按键标记。
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

// 本程序注入的按键带有该标记，键盘钩子据此放行。
constexpr ULONG_PTR KEY_MARK = 0x4D544153;  // 注入按键的标记值。

void InitLog(bool debug);
void Log(const wchar_t* fmt, ...);  // 写入日志，线程安全，不阻塞界面。
void LogFlush();  // 等待日志队列写完后关闭文件。
bool LogDebugOn();
// 诊断输出，写入 exe 所在目录的 FlipRow.boot.log。
void BootLog(const wchar_t* fmt, ...);
#define LogDbg(...)                 \
    do {                            \
        if (LogDebugOn()) Log(__VA_ARGS__); \
    } while (0)

bool IsChineseUi();  // 返回界面语言是否为中文。
#define Tr(zh, en) (IsChineseUi() ? (zh) : (en))

// 语言模式，0 表示跟随系统，1 表示中文，2 表示英文。
int LangMode();
void SetLangMode(int mode);
void LoadConfig();  // 读取配置文件中的语言设置。
void SaveLangMode(int mode);  // 写入语言设置。
