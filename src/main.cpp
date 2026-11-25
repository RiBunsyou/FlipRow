// FlipRow，接管 Alt+Tab 的二维堆叠式应用切换器。
// 程序为常驻进程，通过全局键盘钩子接收 Alt+Tab，界面是一块全屏 DirectComposition 浮层。
// 窗口卡片显示快照，快照由 preview.cpp 在工作线程中异步采集。
// 实时预览，多显示器支持和提权窗口支持属于后续工作。
#include <atomic>
#include <thread>

#include "common.h"

#include <shellapi.h>

#include "gfx.h"
#include "stack.h"
#include "startup.h"
#include "tray.h"
#include "version.h"
#include "wins.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shcore.lib")

namespace {

const UINT WM_APP_SWITCH = WM_APP + 36;  // 切换消息，用于打开界面或轮换一格。
const UINT WM_APP_SWITCH_KEY = WM_APP + 37;  // 按键消息，用于取消和方向键。
const UINT WM_APP_SWITCH_END = WM_APP + 38;  // Alt 松开消息，用于提交当前选择。
const UINT WM_APP_DEMO = WM_APP + 71;
const UINT WM_APP_REHOOK = WM_APP + 72;
const UINT_PTR T_HOOK_CHECK = 1;
const UINT_PTR T_DEMO_QUIT = 2;
const UINT_PTR T_DEMO_NEXT = 3;
const UINT_PTR T_TRAY_RETRY = 4;
const wchar_t* kMutexName = L"FlipRow-7C2E1B90";
int g_demoStage = 0;
int g_demoFront = 0;

HWND g_msg = nullptr;
std::atomic<bool> g_enabled{ true };
std::atomic<HHOOK> g_kbHook{ nullptr };
std::thread g_hookThread;
DWORD g_hookThreadId = 0;
UINT g_taskbarCreated = 0;

// 键盘钩子状态，仅钩子线程访问。
bool g_winDown = false, g_winCombo = false;
bool g_altTab = false;
// 自行跟踪的修饰键状态，注入的按键不参与，避免系统状态被改乱。
bool g_altDown = false, g_ctrlDown = false, g_shiftDown = false;

// 取得前台切换资格，先注入一次 Alt 按下和松开，中间插入一个无功能键。
// 前台程序因此收不到单独的 Alt 键，不会展开菜单。
void AllowForeground() {
    keybd_event(VK_MENU, 0, 0, KEY_MARK);
    keybd_event(0xE8, 0, 0, KEY_MARK);
    keybd_event(0xE8, 0, KEYEVENTF_KEYUP, KEY_MARK);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
}

void Inject(DWORD vk, bool up) {
    INPUT in{ INPUT_KEYBOARD };
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | ((vk == VK_LWIN || vk == VK_RWIN) ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = KEY_MARK;
    SendInput(1, &in, sizeof(in));
}

// 注入一个无功能键，按住 Alt 或 Win 时插入，松开后系统不认为只按了修饰键。
void MuteKey() {
    Inject(0xE8, false);
    Inject(0xE8, true);
}

LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l) {
    if (code != HC_ACTION) return CallNextHookEx(g_kbHook.load(), code, w, l);
    auto* k = (KBDLLHOOKSTRUCT*)l;
    if (k->dwExtraInfo == KEY_MARK) return CallNextHookEx(g_kbHook.load(), code, w, l);  // 忽略本程序注入的按键。
    if (!g_enabled) return CallNextHookEx(g_kbHook.load(), code, w, l);

    bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
    const UINT vk = k->vkCode;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_altDown = down;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_ctrlDown = down;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_shiftDown = down;
    // 系统状态可能被注入的按键改乱，任一来源认为按下即算按下。
    const bool alt = g_altDown || GetAsyncKeyState(VK_MENU) < 0;
    const bool ctrl = g_ctrlDown || GetAsyncKeyState(VK_CONTROL) < 0;
    const bool shift = g_shiftDown || GetAsyncKeyState(VK_SHIFT) < 0;
    bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
    if (LogDebugOn() && (k->vkCode == VK_TAB || win || k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_ESCAPE))
        LogDbg(L"钩子：vk=%u down=%d winDown=%d alt=%d", k->vkCode, (int)down, (int)g_winDown, (int)g_altTab);

    // Win 和 Alt 的松开可能被其他窗口拦截，此处用实际按键状态兜底。
    if (!win && g_winDown && GetAsyncKeyState(VK_LWIN) >= 0 && GetAsyncKeyState(VK_RWIN) >= 0) g_winDown = false;
    if (g_altTab && !alt && k->vkCode != VK_LMENU && k->vkCode != VK_RMENU && k->vkCode != VK_MENU) {
        g_altTab = false;
        PostMessageW(g_msg, WM_APP_SWITCH_END, 0, 0);
    }

    // Win 键照常交给系统，Win+Tab 仍由系统处理。
    if (win) {
        if (down) {
            if (!g_winDown) {
                g_winDown = true;
                g_winCombo = GetAsyncKeyState(VK_CONTROL) < 0 || GetAsyncKeyState(VK_SHIFT) < 0 || GetAsyncKeyState(VK_MENU) < 0;
            }
        } else {
            g_winDown = false;
        }
        return CallNextHookEx(g_kbHook.load(), code, w, l);
    }

    // Alt+Tab 分支，负责打开界面和轮换。
    if (vk == VK_TAB) {
        if (!down) return g_altTab ? 1 : CallNextHookEx(g_kbHook.load(), code, w, l);  // 抬起事件在切换期间一并吞掉。
        const bool handled = !g_winDown && alt && !ctrl;
        Log(L"Tab：alt=%d ctrl=%d win=%d -> %ls", (int)alt, (int)ctrl, (int)g_winDown, handled ? L"接管" : L"放行");
        if (handled) {
            if (!g_altTab) MuteKey();
            g_altTab = true;
            PostMessageW(g_msg, WM_APP_SWITCH, shift ? (WPARAM)-1 : 1, 0);
            return 1;
        }
    }
    if (g_altTab && down && (k->vkCode == VK_ESCAPE || k->vkCode == VK_LEFT || k->vkCode == VK_RIGHT)) {
        if (k->vkCode == VK_ESCAPE) g_altTab = false;  // 取消本次切换，松开 Alt 时不再提交。
        PostMessageW(g_msg, WM_APP_SWITCH_KEY, k->vkCode, 0);
        return 1;
    }
    if (g_altTab && !down && (k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_MENU)) {
        g_altTab = false;
        PostMessageW(g_msg, WM_APP_SWITCH_END, 0, 0);
        return CallNextHookEx(g_kbHook.load(), code, w, l);  // Alt 松开事件照常交给系统。
    }
    return CallNextHookEx(g_kbHook.load(), code, w, l);
}

void InstallHook() {
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    if (!h) {
        Log(L"钩子：安装失败 (%lu)，5 秒后重试", GetLastError());
        return;
    }
    g_kbHook = h;
    g_winDown = g_winCombo = g_altTab = false;
    Log(L"钩子：已安装");
}

void HookThread() {
    g_hookThreadId = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    THREAD_POWER_THROTTLING_STATE s{ THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0 };
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof(s));
    InstallHook();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_APP_REHOOK) InstallHook();
    }
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
}


void TrayCommand(int id) {
    switch (id) {
    case 1: PostMessageW(g_msg, WM_APP_DEMO, 0, 0); break;
    case 3:
        g_enabled = !g_enabled;
        Log(L"热键：%ls", g_enabled ? L"开" : L"关");
        if (!g_enabled) stack::Key(VK_ESCAPE);
        break;
    case 10: case 11: case 12:  // 语言菜单项。
        SaveLangMode(id - 10);
        Log(L"界面语言：%d（0 跟随系统，1 中文，2 English）", LangMode());
        tray::Refresh();  // 重新加载托盘提示和菜单文本。
        break;
    case 5: {  // 开机自启菜单项。
        const bool on = !startup::IsEnabled();
        const bool ok = startup::SetEnabled(on);
        Log(L"开机自启：%ls（%ls）", on ? L"开" : L"关", ok ? L"成功" : L"失败");
        break;
    }
    case 6: {  // 关于对话框，显示版本号，项目地址和许可证。
        wchar_t msg[512] = {};
        swprintf_s(msg, Tr(L"FlipRow %ls\n二维堆叠式 Alt+Tab 窗口切换器\n\nhttps://github.com/RiBunsyou/FlipRow\nGNU 通用公共许可证第 3 版",
                           L"FlipRow %ls\n2D stacked Alt+Tab switcher\n\nhttps://github.com/RiBunsyou/FlipRow\nGNU General Public License v3.0"),
                   FLIPROW_VERSION_WSTRING);
        MessageBoxW(nullptr, msg, Tr(L"关于 FlipRow", L"About FlipRow"), MB_ICONINFORMATION);
        break;
    }
    case 4: PostMessageW(g_msg, WM_CLOSE, 0, 0); break;
    }
}

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated && g_taskbarCreated) {
        tray::OnTaskbarCreated();
        return 0;
    }
    switch (m) {
    case WM_APP_SWITCH: stack::Step((int)(INT_PTR)w); return 0;
    case WM_APP_SWITCH_KEY: stack::Key((UINT)w); return 0;
    case WM_APP_SWITCH_END: stack::Release(); return 0;
    case WM_APP_DEMO:
        AllowForeground();
        stack::Demo(g_demoFront ? 0 : -1);
        return 0;
    case tray::WM_TRAY_CALLBACK: tray::OnCallback(w, l); return 0;
    case WM_SETTINGCHANGE:
        // 系统深浅色切换时发送该消息。
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) tray::Refresh();
        return 0;
    case WM_THEMECHANGED:
        tray::Refresh();
        return 0;
    case WM_TIMER:
        if (w == T_HOOK_CHECK) {
            // 钩子可能被系统静默移除，没有切换时每隔一分钟重装一次。
            static int ticks = 0;
            const bool reinstall = !g_kbHook || (++ticks % 12 == 0 && !g_altTab);
            if (reinstall && g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_APP_REHOOK, 0, 0);
            return 0;
        }
        if (w == T_DEMO_QUIT) {
            KillTimer(h, T_DEMO_QUIT);
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (w == T_TRAY_RETRY) {
            // 登录初期任务栏尚未就绪，添加托盘图标会失败，这里周期性重试。
            static int tries = 0;
            if (tray::Present()) {
                KillTimer(h, T_TRAY_RETRY);
                Log(L"托盘图标：补加成功");
                return 0;
            }
            if (++tries > 40) {
                KillTimer(h, T_TRAY_RETRY);
                Log(L"托盘图标：补加放弃");
                return 0;
            }
            tray::Add(g_msg, TrayCommand, [] { return g_enabled.load(); });
            return 0;
        }
        if (w == T_DEMO_NEXT) {  // 演示模式，连续两次打开界面以验证第二次的预览。
            if (g_demoStage == 0) {
                stack::Key(VK_ESCAPE);
                g_demoStage = 1;
                SetTimer(h, T_DEMO_NEXT, 1400, nullptr);
            } else if (g_demoStage == 1) {
                g_demoStage = 2;
                stack::Demo();
            }
            return 0;
        }
        return 0;
    case WM_CLOSE:
        tray::Remove();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Cleanup() {
    KillTimer(g_msg, T_HOOK_CHECK);
    KillTimer(g_msg, T_DEMO_QUIT);
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread.joinable()) g_hookThread.join();
    tray::Remove();
    stack::Shutdown();
    wins::Shutdown();
    gfx::Shutdown();
    Log(L"退出");
    LogFlush();
}

}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    BootLog(L"启动：%ls", GetCommandLineW());
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool noHook = false, demo = false, demo2 = false, demo0 = false, wantQuit = false, debug = false;
    bool autoSet = false, autoOn = false, versionOnly = false;
    for (int i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--no-hook")) noHook = true;
        else if (!_wcsicmp(argv[i], L"--demo")) demo = true;
        else if (!_wcsicmp(argv[i], L"--demo2")) demo2 = true;
        else if (!_wcsicmp(argv[i], L"--demo0")) demo0 = true;
        else if (!_wcsicmp(argv[i], L"--autostart") && i + 1 < argc) {
            autoOn = !_wcsicmp(argv[++i], L"on");
            autoSet = true;
        } else if (!_wcsicmp(argv[i], L"--version")) versionOnly = true;
        else if (!_wcsicmp(argv[i], L"--quit")) wantQuit = true;
        else if (!_wcsicmp(argv[i], L"--debug")) debug = true;
    }
    if (argv) LocalFree(argv);
    if (demo || demo2 || demo0) noHook = true;  // 演示模式不接管用户热键。
    g_demoFront = demo0 ? 1 : 0;

    // 命令行输出版本号，有控制台就打印，同时写入启动日志便于核对。
    if (versionOnly) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* out = nullptr;
            freopen_s(&out, "CONOUT$", "w", stdout);
            wprintf(L"FlipRow %ls\n", FLIPROW_VERSION_WSTRING);
            fflush(stdout);
            if (out) fclose(out);
            FreeConsole();
        }
        BootLog(L"FlipRow %ls", FLIPROW_VERSION_WSTRING);
        return 0;
    }

    // 命令行设置开机自启，设置完成后退出。
    if (autoSet) {
        InitLog(debug);
        const bool ok = startup::SetEnabled(autoOn);
        Log(L"命令行：开机自启 %ls -> %ls", autoOn ? L"开" : L"关", ok ? L"成功" : L"失败");
        LogFlush();
        return ok ? 0 : 1;
    }


    // 单实例控制，重复启动时打开切换器，--quit 用于退出已运行的实例。
    HANDLE once = CreateMutexW(nullptr, TRUE, L"FlipRow-7C2E1B90");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        EnumWindows(
            [](HWND h, LPARAM l) -> BOOL {
                wchar_t cls[64] = {};
                GetClassNameW(h, cls, 64);
                if (!wcscmp(cls, L"FlipRowMsg")) {
                    *(HWND*)l = h;
                    return FALSE;
                }
                return TRUE;
            },
            (LPARAM)&other);
        if (other) PostMessageW(other, wantQuit ? WM_CLOSE : WM_APP_DEMO, 0, 0);
        return 0;
    }
    if (wantQuit) return 0;

    LoadConfig();  // 读取配置文件。
    InitLog(debug);
    BootLog(L"InitLog 完成，日志写入 FlipRow.log");
    Log(L"FlipRow 启动%s", debug ? L"（调试）" : L"");
    Log(L"界面语言：%d / 中文=%d", LangMode(), (int)IsChineseUi());
    Log(L"开机自启：%ls", startup::IsEnabled() ? L"已开启" : L"未开启");
    // 诊断输出，记录窗口站，桌面，完整性级别和任务栏窗口，用于排查登录启动时的托盘失败。
    {
        wchar_t ws[64] = {}, dt[64] = {};
        DWORD n = 0;
        GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, ws, sizeof(ws), &n);
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, dt, sizeof(dt), &n);
        DWORD rid = 0;
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            BYTE buf[64] = {};
            DWORD cb = 0;
            if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &cb)) {
                SID* sid = (SID*)((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
                rid = sid->SubAuthority[sid->SubAuthorityCount - 1];
            }
            CloseHandle(tok);
        }
        Log(L"诊断：窗口站 %ls，桌面 %ls，完整性 0x%X，管理员 %d，任务栏 0x%p", ws, dt, rid, (int)startup::IsElevated(),
            (void*)FindWindowW(L"Shell_TrayWnd", nullptr));
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) Log(L"COM 初始化失败");

    if (!gfx::Init()) {
        BootLog(L"gfx 初始化失败");
        MessageBoxW(nullptr, Tr(L"Direct2D / DirectWrite 初始化失败。", L"Direct2D / DirectWrite initialization failed."), L"FlipRow",
                   MB_ICONERROR);
        return 1;
    }
    BootLog(L"gfx 初始化完成");
    if (!stack::Create(hi)) {
        BootLog(L"浮层窗口创建失败");
        MessageBoxW(nullptr, Tr(L"创建浮层窗口失败。", L"Failed to create the overlay window."), L"FlipRow", MB_ICONERROR);
        return 1;
    }
    BootLog(L"浮层窗口创建完成");

    WNDCLASSW mc{};
    mc.lpfnWndProc = MsgProc;
    mc.hInstance = hi;
    mc.lpszClassName = L"FlipRowMsg";
    mc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&mc);
    g_msg = CreateWindowExW(0, mc.lpszClassName, L"FlipRow", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    if (!g_msg) return 1;
    wins::AddOurs(g_msg);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (!tray::Add(g_msg, TrayCommand, [] { return g_enabled.load(); })) {
        SetTimer(g_msg, T_TRAY_RETRY, 3000, nullptr);  // 任务栏未就绪时定时补加。
    }

    if (!noHook) g_hookThread = std::thread(HookThread);
    SetTimer(g_msg, T_HOOK_CHECK, 5000, nullptr);
    if (demo || demo2 || demo0) {
        PostMessageW(g_msg, WM_APP_DEMO, 0, 0);
        if (demo2) {  // 同一进程内连续打开两次。
            SetTimer(g_msg, T_DEMO_NEXT, 2600, nullptr);
            SetTimer(g_msg, T_DEMO_QUIT, 13000, nullptr);
        } else {
            SetTimer(g_msg, T_DEMO_QUIT, 8000, nullptr);  // 演示模式在 8 秒后自动退出。
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Cleanup();
    return 0;
}
