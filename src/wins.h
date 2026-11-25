// 系统窗口层封装，包含窗口筛选，应用分组，图标，激活和虚拟桌面。
// 窗口筛选规则与系统 Alt+Tab 保持一致。
#pragma once

#include "common.h"

namespace wins {

struct Win {
    HWND h = nullptr;
    std::wstring title;
    RECT frame{};  // 窗口在屏幕上的可见矩形。
    RECT src{};  // 缩略图源区域，窗口坐标。
    std::wstring exe;
    bool mini = false;  // 是否为最小化窗口。
};

struct App {  // 切换器中的一个应用条目。
    std::wstring id;  // 应用标识，程序路径或 AUMID。
    std::wstring name;
    std::wstring exe;
    std::wstring aumid;
    std::wstring launch;
    std::vector<HWND> windows;
    bool allMinimized = false;
};

struct Desks {
    std::vector<std::wstring> names;
    int current = 0;
};

bool IsOurs(HWND h);
void AddOurs(HWND h);
bool IsDeskClass(const wchar_t* cls);
bool LoneOwned(HWND h);
bool IsAltTabWindow(HWND h);

std::wstring ExeOfWindow(HWND h);  // 返回窗口所属程序的路径，无法获取时为空。
std::wstring AppIdOfWindow(HWND h);

std::vector<Win> CollectWindows(bool currentDesktopOnly, bool includeMinimized = false);
std::vector<App> CollectApps();

bool IconOf(const App& a, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra);
bool IconOfFile(const std::wstring& path, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra);

void Activate(const App& a);
void Hide(const App& a);
void Quit(const App& a);
void BringToFront(HWND h);
void CloseWindow(HWND h);

const Desks& GetDesks();  // 返回桌面列表，结果缓存 2 秒。
void RefreshDesks();
void SwitchDesktop(int index);
void NewDesktop();
void CloseDesktop(int index);

void Shutdown();

}
