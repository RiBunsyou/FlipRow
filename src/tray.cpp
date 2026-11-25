#include "tray.h"

#include <shellapi.h>

#include "startup.h"

namespace {

HWND g_wnd = nullptr;
HICON g_icon = nullptr;
std::function<void(int)> g_onMenu;
std::function<bool()> g_hotkeysOn;
bool g_present = false;

// 托盘图标使用单色线条，颜色随任务栏深浅色变化。
// 资源 2 为黑色图标，资源 3 为白色图标。
HICON LoadTrayIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    DWORD light = 1, cb = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &light, &cb) != ERROR_SUCCESS)
        light = 1;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HICON ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(light ? 2 : 3), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!ic) ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!ic) ic = LoadIconW(nullptr, IDI_APPLICATION);
    return ic;
}

const wchar_t* Tip() { return Tr(L"FlipRow：Alt+Tab 2D 堆叠切换器", L"FlipRow - 2D stacked Alt+Tab switcher"); }

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, 1, Tr(L"打开切换器（预览）", L"Open switcher (preview)"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_hotkeysOn && g_hotkeysOn() ? MF_CHECKED : 0), 3, Tr(L"接管 Alt+Tab", L"Take over Alt+Tab"));
    AppendMenuW(m, MF_STRING | (startup::IsEnabled() ? MF_CHECKED : 0), 5, Tr(L"开机自启", L"Start with Windows"));
    // 语言子菜单。
    HMENU lang = CreatePopupMenu();
    if (lang) {
        const int mode = LangMode();
        AppendMenuW(lang, MF_STRING | (mode == 0 ? MF_CHECKED : 0), 10, Tr(L"跟随系统", L"Follow system"));
        AppendMenuW(lang, MF_STRING | (mode == 1 ? MF_CHECKED : 0), 11, L"中文");
        AppendMenuW(lang, MF_STRING | (mode == 2 ? MF_CHECKED : 0), 12, L"English");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)lang, Tr(L"语言", L"Language"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, Tr(L"退出", L"Exit"));
    POINT p;
    GetCursorPos(&p);
    // 先把菜单窗口置于前台，菜单关闭后再发送 WM_NULL。
    SetForegroundWindow(g_wnd);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd && g_onMenu) g_onMenu(cmd);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
}

}

bool tray::Add(HWND msgWnd, std::function<void(int)> onMenu, std::function<bool()> hotkeysOn) {
    g_wnd = msgWnd;
    g_onMenu = std::move(onMenu);
    g_hotkeysOn = std::move(hotkeysOn);
    g_icon = LoadTrayIcon();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, Tip(), 128);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    DWORD err = GetLastError();
    if (ok) {
        g_present = true;
        NOTIFYICONDATAW v = nid;
        v.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &v);
    }
    Log(L"托盘图标：%ls（错误 %lu）", ok ? L"已添加" : L"添加失败", ok ? 0 : err);
    return ok != FALSE;
}

void tray::Remove() {
    if (!g_wnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_present = false;
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}

void tray::OnCallback(WPARAM w, LPARAM l) {
    (void)w;
    // 通知消息版本 4 使用低字表示鼠标消息，两种版本统一按低字解析。
    UINT msg = LOWORD(l);
    switch (msg) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        if (g_onMenu) g_onMenu(1);
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowMenu();
        break;
    }
}

void tray::OnTaskbarCreated() {
    if (!g_wnd) return;
    Log(L"托盘图标：Explorer 重启了，重新添加");
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = g_icon ? g_icon : LoadTrayIcon();
    lstrcpynW(nid.szTip, Tip(), 128);
    g_present = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

bool tray::Present() { return g_present; }

// 系统主题变化后重新加载图标。
void tray::Refresh() {
    if (!g_wnd) return;
    auto onMenu = g_onMenu;
    auto hot = g_hotkeysOn;
    Remove();
    Add(g_wnd, std::move(onMenu), std::move(hot));
}
