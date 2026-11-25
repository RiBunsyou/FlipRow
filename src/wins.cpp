#include "wins.h"

#include <appmodel.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#pragma warning(disable : 4005)  // 该头文件重复定义了同一宏，此处关闭对应警告。
#include <propkey.h>

#include <unordered_map>

#include "com.h"
#include "gfx.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::vector<HWND> g_ours;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// 窗口信息缓存，进程和 exe 信息不会变化。
struct Cached {
    DWORD pid = 0, tid = 0;
    ULONGLONG since = 0;
    std::wstring exe, aumid, appId;
};
std::unordered_map<HWND, Cached> g_winCache;

Cached& GetCached(HWND h) {
    DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
    if (g_winCache.size() > 256)  // 缓存超过上限时清理已关闭窗口的条目。
        for (auto it = g_winCache.begin(); it != g_winCache.end();)
            it = IsWindow(it->first) ? std::next(it) : g_winCache.erase(it);
    auto& c = g_winCache[h];
    if (c.pid != pid || c.tid != tid) {
        c = {};
        c.pid = pid;
        c.tid = tid;
        c.since = GetTickCount64();
    }
    return c;
}

std::wstring ExeOfPid(DWORD pid) {
    std::wstring out;
    if (!pid || pid == GetCurrentProcessId()) return out;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(hp, 0, buf, &n)) out = buf;
        CloseHandle(hp);
    }
    return out;
}

std::wstring AumidOfPid(DWORD pid) {
    std::wstring out;
    if (!pid) return out;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH] = {};
        UINT32 n = APPLICATION_USER_MODEL_ID_MAX_LENGTH;
        if (GetApplicationUserModelId(hp, &n, buf) == ERROR_SUCCESS) out = buf;
        CloseHandle(hp);
    }
    return out;
}

std::wstring AumidOfWindow(HWND h) {
    Cached& c = GetCached(h);
    if (!c.aumid.empty()) return c.aumid;
    std::wstring out;
    Com<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(ps.Put()))) && ps) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        PropVariantClear(&v);
    }
    if (out.empty()) out = AumidOfPid(c.pid);
    c.aumid = out;
    return out;
}

std::wstring DisplayNameOf(const std::wstring& parsing) {
    Com<IShellItem> si;
    std::wstring out;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(si.Put()))) && si) {
        PWSTR n = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) {
            out = n;
            CoTaskMemFree(n);
        }
    }
    return out;
}

std::wstring ExeDisplayName(const std::wstring& exe) {
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (n) {
        std::vector<BYTE> buf(n);
        if (GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) {
            struct LangCp {
                WORD lang, cp;
            } *tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                wchar_t key[80];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
                wchar_t* desc = nullptr;
                UINT dl = 0;
                if (VerQueryValueW(buf.data(), key, (void**)&desc, &dl) && dl > 1 && desc[0]) return desc;
            }
        }
    }
    std::wstring f = exe.substr(exe.find_last_of(L'\\') == std::wstring::npos ? 0 : exe.find_last_of(L'\\') + 1);
    if (f.size() > 4 && !_wcsicmp(f.c_str() + f.size() - 4, L".exe")) f.resize(f.size() - 4);
    return f;
}

void SendKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | ((vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = KEY_MARK;
    SendInput(1, &in, sizeof(in));
}

void WinCtrl(WORD vk) {  // 虚拟桌面快捷键处理。
    SendKey(VK_LWIN, false);
    SendKey(VK_LCONTROL, false);
    SendKey(vk, false);
    SendKey(vk, true);
    SendKey(VK_LCONTROL, true);
    SendKey(VK_LWIN, true);
}

bool RegBin(HKEY root, const wchar_t* key, const wchar_t* name, std::vector<BYTE>& out) {
    DWORD cb = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_BINARY, nullptr, nullptr, &cb) != ERROR_SUCCESS || !cb) return false;
    out.resize(cb);
    return RegGetValueW(root, key, name, RRF_RT_REG_BINARY, nullptr, out.data(), &cb) == ERROR_SUCCESS;
}

// 获取系统提供的程序图标，输出预乘 BGRA 像素。
bool ShellImage(const std::wstring& parsing, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    Com<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(f.Put()))) || !f) return false;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &hb);
    if (FAILED(hr) || !hb) return false;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    w = (UINT)bm.bmWidth;
    h = (UINT)std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
    bgra.resize((size_t)w * h * 4);
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, hb, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(hb);
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < bgra.size(); i += 4) {
        if (bgra[i + 3]) anyAlpha = true;
        if (bgra[i] > bgra[i + 3] || bgra[i + 1] > bgra[i + 3] || bgra[i + 2] > bgra[i + 3]) straight = true;
    }
    if (!anyAlpha)
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    else if (straight)
        for (size_t i = 0; i < bgra.size(); i += 4)
            for (int c = 0; c < 3; c++) bgra[i + c] = (BYTE)(bgra[i + c] * bgra[i + 3] / 255);
    gfx::TrimToSquare(w, h, bgra);
    return true;
}

// 虚拟桌面管理器，只创建一次。
Com<IVirtualDesktopManager> g_vdm;
bool g_vdmTried = false;
IVirtualDesktopManager* Vdm() {
    if (!g_vdmTried) {
        g_vdmTried = true;
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(g_vdm.Put()));
    }
    return g_vdm.Get();
}

// 桌面列表缓存。
wins::Desks g_desks;
ULONGLONG g_desksAt = 0;

}

bool wins::IsOurs(HWND h) { return std::find(g_ours.begin(), g_ours.end(), h) != g_ours.end(); }

void wins::AddOurs(HWND h) {
    if (h && std::find(g_ours.begin(), g_ours.end(), h) == g_ours.end()) g_ours.push_back(h);
}

bool wins::IsDeskClass(const wchar_t* cls) {
    return cls && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
}

bool wins::LoneOwned(HWND h) {
    HWND owner = GetWindow(h, GW_OWNER);
    if (!owner || !IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE), st = GetWindowLongPtrW(h, GWL_STYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if (!(st & WS_CAPTION)) return false;
    HWND root = GetAncestor(h, GA_ROOTOWNER);
    if (!root || root == h) return false;
    for (HWND o = owner; o; o = GetWindow(o, GW_OWNER)) {
        if (!IsWindowVisible(o)) continue;
        DWORD cl = 0;
        DwmGetWindowAttribute(o, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) continue;
        LONG_PTR oex = GetWindowLongPtrW(o, GWL_EXSTYLE);
        if ((oex & WS_EX_APPWINDOW) ||
            (!(oex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !GetWindow(o, GW_OWNER) && GetWindowTextLengthW(o) > 0))
            return false;
    }
    return true;
}

bool wins::IsAltTabWindow(HWND h) {
    if (!h || !IsWindowVisible(h) || IsOurs(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    HWND owner = GetWindow(h, GW_OWNER);
    bool lone = false;
    if (ex & WS_EX_APPWINDOW) {
    } else if (owner && LoneOwned(h)) {
        lone = true;
    } else if (owner || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE)) {
        return false;
    }
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (IsDeskClass(cls)) return false;
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    return GetWindowTextLengthW(h) > 0 || lone;
}

std::wstring wins::ExeOfWindow(HWND h) {
    if (!h) return {};
    Cached& c = GetCached(h);
    if (c.exe.empty()) c.exe = ExeOfPid(c.pid);
    return c.exe;
}

std::wstring wins::AppIdOfWindow(HWND h) {
    Cached& c = GetCached(h);
    if (!c.appId.empty()) return c.appId;
    std::wstring aumid = AumidOfWindow(h);
    c.appId = !aumid.empty() ? L"aumid:" + Lower(aumid) : Lower(c.exe.empty() ? ExeOfWindow(h) : c.exe);
    return c.appId;
}

std::vector<wins::Win> wins::CollectWindows(bool currentDesktopOnly, bool includeMinimized) {
    std::vector<Win> out;
    IVirtualDesktopManager* vdm = currentDesktopOnly ? Vdm() : nullptr;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsAltTabWindow(h)) continue;
        // 登录初期常有程序还卡在启动流程里，对这类窗口发同步消息会让本进程一起卡住。
        if (IsHungAppWindow(h)) continue;
        const bool mini = IsIconic(h) != FALSE;
        if (mini && !includeMinimized) continue;
        BOOL on = TRUE;
        if (vdm && SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(h, &on)) && !on) continue;

        RECT frame{};
        RECT src{};
        bool srcReady = false;
        if (mini) {
            // 最小化后 GetWindowRect 返回图标大小，DWM 的边框也是图标框。
            // 可靠的数据来源是 GetWindowPlacement 返回的还原矩形。
            RECT normal{};
            WINDOWPLACEMENT wp{ sizeof(wp) };
            if (GetWindowPlacement(h, &wp)) normal = wp.rcNormalPosition;
            if (normal.right - normal.left < 60 || normal.bottom - normal.top < 40) continue;

            // 还原矩形包含四周不可见的边框。
            // 最小化时客户区为 0，边框宽度按 DPI 的系统度量推算。
            // 宽度等于 SM_CXSIZEFRAME 与 SM_CXPADDEDBORDER 之和。
            // 缩略图源区域必须从边框内侧开始，否则画面位移并被裁切。
            const UINT dpi = GetDpiForWindow(h);
            const LONG b = std::max(1L, (LONG)(GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)));
            const LONG visW = std::max(40L, (normal.right - normal.left) - 2 * b);
            const LONG visH = std::max(40L, (normal.bottom - normal.top) - 2 * b);
            frame = { normal.left + b, normal.top + b, normal.left + b + visW, normal.top + b + visH };
            src = { b, b, b + visW, b + visH };
            srcReady = true;
        } else {
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) GetWindowRect(h, &frame);
            if (frame.right - frame.left < 60 || frame.bottom - frame.top < 40) continue;
            RECT x;
            if (!IntersectRect(&x, &frame, &mi.rcMonitor)) continue;
        }

        Win w;
        w.h = h;
        w.frame = frame;
        w.mini = mini;
        if (!srcReady) {
            RECT wr{};
            GetWindowRect(h, &wr);
            LONG ww = wr.right - wr.left, wh = wr.bottom - wr.top;
            src = { frame.left - wr.left, frame.top - wr.top, frame.right - wr.left, frame.bottom - wr.top };
            src.left = std::clamp(src.left, 0L, ww);
            src.top = std::clamp(src.top, 0L, wh);
            src.right = std::clamp(src.right, 0L, ww);
            src.bottom = std::clamp(src.bottom, 0L, wh);
            if (src.right - src.left < 20 || src.bottom - src.top < 20) src = { 0, 0, ww, wh };
        }
        w.src = src;
        w.exe = ExeOfWindow(h);
        // 标题通过带超时的消息获取，目标窗口无响应时最多等 100 毫秒。
        wchar_t t[512] = {};
        DWORD_PTR copied = 0;
        if (SendMessageTimeoutW(h, WM_GETTEXT, 512, (LPARAM)t, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) == 0) t[0] = 0;
        w.title = t;
        out.push_back(std::move(w));
    }
    return out;
}

std::vector<wins::App> wins::CollectApps() {
    std::vector<App> out;
    std::unordered_map<std::wstring, size_t> idx;
    auto make = [&](HWND h) -> App* {
        std::wstring aumid = AumidOfWindow(h);
        std::wstring exe = Lower(ExeOfWindow(h));
        std::wstring id = !aumid.empty() ? L"aumid:" + Lower(aumid) : exe;
        if (id.empty()) return nullptr;
        auto it = idx.find(id);
        if (it != idx.end()) return &out[it->second];
        App a;
        a.id = id;
        a.exe = exe;
        a.aumid = aumid;
        if (!aumid.empty()) {
            a.launch = L"shell:AppsFolder\\" + aumid;
            a.name = DisplayNameOf(a.launch);
        }
        if (a.name.empty() && !exe.empty()) a.name = ExeDisplayName(exe);
        if (a.name.empty()) return nullptr;
        if (a.launch.empty()) a.launch = exe;
        idx[id] = out.size();
        out.push_back(std::move(a));
        return &out[idx[id]];
    };
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsAltTabWindow(h) || IsIconic(h)) continue;
        if (App* a = make(h)) a->windows.push_back(h);
    }
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {  // 该循环处理只有最小化窗口的程序。
        if (!IsAltTabWindow(h) || !IsIconic(h)) continue;
        if (App* a = make(h)) {
            if (std::find(a->windows.begin(), a->windows.end(), h) == a->windows.end()) a->windows.push_back(h);
        }
    }
    for (auto& a : out) {
        bool any = false;
        for (HWND h : a.windows)
            if (!IsIconic(h)) any = true;
        a.allMinimized = !any;
    }
    return out;
}

bool wins::IconOf(const App& a, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    if (!a.aumid.empty() && ShellImage(L"shell:AppsFolder\\" + a.aumid, px, w, h, bgra)) return true;
    if (!a.exe.empty() && ShellImage(a.exe, px, w, h, bgra)) return true;
    if (!a.launch.empty() && a.launch != a.exe && ShellImage(a.launch, px, w, h, bgra)) return true;
    return false;
}

bool wins::IconOfFile(const std::wstring& path, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    return !path.empty() && ShellImage(path, px, w, h, bgra);
}

void wins::BringToFront(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (GetForegroundWindow() == h) return;
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD me = GetCurrentThreadId();
    bool attached = false;
    if (fgThread && fgThread != me) attached = AttachThreadInput(me, fgThread, TRUE) != FALSE;
    SetForegroundWindow(h);
    BringWindowToTop(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    if (GetForegroundWindow() == h) return;
    // 前台锁会阻止任意程序切换前台，先取得最后输入资格后重试。
    // 注入 Alt 和一个无功能键，前台程序不会看到单独的 Alt。
    keybd_event(VK_MENU, 0, 0, KEY_MARK);
    keybd_event(0xE8, 0, 0, KEY_MARK);
    keybd_event(0xE8, 0, KEYEVENTF_KEYUP, KEY_MARK);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
    SetForegroundWindow(h);
    if (GetForegroundWindow() == h) return;
    // 最后一种方式为未文档化的切换函数。
    using Fn = void(WINAPI*)(HWND, BOOL);
    static Fn sw = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SwitchToThisWindow");
    if (sw) sw(h, TRUE);
    LogDbg(L"前台：%ls（0x%p）", GetForegroundWindow() == h ? L"已切换" : L"切换失败", (void*)h);
}

void wins::Activate(const App& a) {
    if (a.windows.empty()) return;
    HWND first = nullptr;
    for (HWND h : a.windows)
        if (!IsIconic(h)) {
            first = h;
            break;
        }
    if (!first) {
        first = a.windows[0];
        ShowWindowAsync(first, SW_RESTORE);
        BringToFront(first);
        Log(L"切换器：还原最小化的 %ls", a.name.c_str());
        return;
    }
    for (int i = (int)a.windows.size() - 1; i >= 0; i--)
        if (!IsIconic(a.windows[i]))
            SetWindowPos(a.windows[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    BringToFront(first);
    Log(L"切换器：%ls（%zu 个窗口）", a.name.c_str(), a.windows.size());
}

void wins::Hide(const App& a) {
    std::vector<HWND> hs;
    for (HWND h : a.windows)
        if (!IsIconic(h)) hs.push_back(h);
    for (HWND h : hs) ShowWindowAsync(h, SW_MINIMIZE);
    Log(L"切换器：隐藏 %ls（%zu 个窗口）", a.name.c_str(), hs.size());
}

void wins::Quit(const App& a) {
    std::vector<DWORD> pids;
    for (HWND h : a.windows) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::wstring base = Lower(ExeOfWindow(h));
        base = base.substr(base.find_last_of(L'\\') == std::wstring::npos ? 0 : base.find_last_of(L'\\') + 1);
        if (pid && pid != GetCurrentProcessId() && base != L"explorer.exe" && std::find(pids.begin(), pids.end(), pid) == pids.end())
            pids.push_back(pid);
        else if (base == L"explorer.exe")
            PostMessageW(h, WM_CLOSE, 0, 0);
    }
    for (DWORD pid : pids)
        if (HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(hp, 1);
            CloseHandle(hp);
        }
    Log(L"切换器：结束 %ls（%zu 个进程）", a.name.c_str(), pids.size());
}

void wins::CloseWindow(HWND h) { PostMessageW(h, WM_CLOSE, 0, 0); }

void wins::RefreshDesks() {
    g_desks = {};
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
    std::vector<BYTE> ids, cur;
    if (RegBin(HKEY_CURRENT_USER, key, L"VirtualDesktopIDs", ids) && ids.size() >= 16) {
        for (size_t i = 0; i + 16 <= ids.size(); i += 16) {
            GUID g;
            memcpy(&g, ids.data() + i, 16);
            wchar_t gs[64] = {};
            StringFromGUID2(g, gs, 64);
            std::wstring sub = std::wstring(key) + L"\\Desktops\\" + gs;
            wchar_t name[128] = {};
            DWORD nb = sizeof(name);
            std::wstring n;
            if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"Name", RRF_RT_REG_SZ, nullptr, name, &nb) == ERROR_SUCCESS && name[0])
                n = name;
            else
                n = Tr(L"桌面 ", L"Desktop ") + std::to_wstring(g_desks.names.size() + 1);
            if (RegBin(HKEY_CURRENT_USER, key, L"CurrentVirtualDesktop", cur) && cur.size() == 16 && !memcmp(cur.data(), ids.data() + i, 16))
                g_desks.current = (int)g_desks.names.size();
            g_desks.names.push_back(n);
        }
    }
    if (g_desks.names.empty()) g_desks.names.push_back(Tr(L"桌面 1", L"Desktop 1"));
    g_desksAt = GetTickCount64();
}

const wins::Desks& wins::GetDesks() {
    if (g_desks.names.empty() || GetTickCount64() - g_desksAt > 2000) RefreshDesks();
    return g_desks;
}

void wins::SwitchDesktop(int index) {
    const Desks& d = GetDesks();
    int delta = index - d.current;
    if (!delta) return;
    for (int k = 0; k < std::abs(delta); k++) WinCtrl(delta > 0 ? VK_RIGHT : VK_LEFT);
    Log(L"桌面：从 %d 切到 %d", d.current + 1, index + 1);
    g_desks.current = std::clamp(index, 0, (int)g_desks.names.size() - 1);  // 先按预期值更新，2 秒后重新读取实际值。
    g_desksAt = GetTickCount64();
}

void wins::NewDesktop() {
    WinCtrl('D');
    Log(L"桌面：新建");
    g_desksAt = 0;  // 使下一次读取重新查询。
}

void wins::CloseDesktop(int index) {
    SwitchDesktop(index);
    WinCtrl(VK_F4);
    Log(L"桌面：关闭第 %d 个", index + 1);
    g_desksAt = 0;
}

void wins::Shutdown() {
    g_winCache.clear();
    g_ours.clear();
    g_vdm.Reset();
    g_desks = {};
}

