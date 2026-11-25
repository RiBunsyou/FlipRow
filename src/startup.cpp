// 开机自启模块，在启动文件夹放一个快捷方式，由资源管理器在登录后启动。
// 程序清单要求管理员权限，快捷方式启动时由系统按 UAC 策略提权。
// 计划任务方案在部分机器上会以低完整性级别启动，导致托盘和键盘钩子失效，因此不再使用。
#include "startup.h"

#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const wchar_t* kLnkName = L"FlipRow.lnk";
const wchar_t* kTaskName = L"FlipRow";  // 旧方案创建的计划任务，启用和关闭时一并清理。

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

std::wstring AppDir() {
    std::wstring p = ExePath();
    auto i = p.find_last_of(L'\\');
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

// 返回启动文件夹中快捷方式的完整路径。
std::wstring StartupLnk() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) return std::wstring();
    std::wstring lnk = std::wstring(p) + L"\\" + kLnkName;
    CoTaskMemFree(p);
    return lnk;
}

// 创建指向当前 exe 的快捷方式。
bool MakeLnk(const std::wstring& lnk) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IShellLinkW* sl = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(ExePath().c_str());
        sl->SetWorkingDirectory(AppDir().c_str());
        sl->SetDescription(L"FlipRow");
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (com) CoUninitialize();
    return ok;
}

// 删除旧方案留下的计划任务，没有任务时视为成功。
bool DeleteTask() {
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"schtasks.exe /delete /tn \"" + std::wstring(kTaskName) + L"\" /f";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return true;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

bool startup::IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD cb = sizeof(el);
    bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

int startup::Method() {
    std::wstring lnk = StartupLnk();
    if (!lnk.empty() && GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
    return 0;
}

bool startup::IsEnabled() { return Method() != 0; }

bool startup::SetEnabled(bool on) {
    std::wstring lnk = StartupLnk();
    if (lnk.empty()) return false;
    if (!on) {
        const bool removed = DeleteFileW(lnk.c_str()) != FALSE;
        DeleteTask();
        Log(L"开机自启：删除快捷方式 %ls", removed ? L"成功" : L"未找到");
        return true;
    }
    if (!MakeLnk(lnk)) {
        Log(L"开机自启：创建快捷方式失败");
        return false;
    }
    DeleteTask();
    Log(L"开机自启：快捷方式已创建 %ls", lnk.c_str());
    return true;
}
