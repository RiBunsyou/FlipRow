// 开机自启模块，在用户注册表的 Run 项写入本程序路径。
// 程序以普通权限运行，登录时由系统直接启动，不会触发 UAC。
#include "startup.h"

#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kValue = L"FlipRow";
const wchar_t* kLnkName = L"FlipRow.lnk";
const wchar_t* kTaskName = L"FlipRow";  // 旧方案使用的计划任务名，启用和关闭时一并清理。

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

std::wstring Command() { return L"\"" + ExePath() + L"\""; }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// 读取 Run 项中本程序的值。
bool ReadValue(std::wstring& out) {
    DWORD type = 0, cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &cb) != ERROR_SUCCESS ||
        cb < sizeof(wchar_t))
        return false;
    out.assign(cb / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, out.data(), &cb) != ERROR_SUCCESS)
        return false;
    out.resize(wcslen(out.c_str()));
    return true;
}

// 返回启动文件夹中旧方案快捷方式的路径。
std::wstring StartupLnk() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) return std::wstring();
    std::wstring lnk = std::wstring(p) + L"\\" + kLnkName;
    CoTaskMemFree(p);
    return lnk;
}

// 清理旧方案留下的计划任务和启动文件夹快捷方式。
void CleanupLegacy() {
    std::wstring lnk = StartupLnk();
    if (!lnk.empty()) DeleteFileW(lnk.c_str());

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"schtasks.exe /delete /tn \"" + std::wstring(kTaskName) + L"\" /f";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
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

bool startup::IsHighIntegrity() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    BYTE buf[128] = {};
    DWORD cb = 0;
    DWORD rid = 0;
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &cb)) {
        SID* sid = (SID*)((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
        rid = sid->SubAuthority[sid->SubAuthorityCount - 1];
    }
    CloseHandle(tok);
    return rid >= SECURITY_MANDATORY_HIGH_RID;
}

int startup::Method() {
    std::wstring v;
    if (ReadValue(v) && Lower(v) == Lower(Command())) return 1;
    return 0;
}

bool startup::IsEnabled() { return Method() != 0; }

bool startup::SetEnabled(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        Log(L"开机自启：打开注册表项失败 %lu", GetLastError());
        return false;
    }
    bool ok = false;
    if (on) {
        std::wstring cmd = Command();
        LONG rc = RegSetValueExW(k, kValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        ok = rc == ERROR_SUCCESS;
        Log(L"开机自启：写注册表 %ls（%ld）", ok ? L"成功" : L"失败", rc);
    } else {
        LONG rc = RegDeleteValueW(k, kValue);
        ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
        Log(L"开机自启：删除注册表值 %ls（%ld）", ok ? L"成功" : L"失败", rc);
    }
    RegCloseKey(k);
    CleanupLegacy();
    return ok;
}
