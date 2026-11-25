// 开机自启模块，通过任务计划程序创建登录时以最高权限运行的任务。
// 注册表启动项只能启动普通权限进程，开机时会被 UAC 拦截，因此改用任务计划程序。
#include "startup.h"

#pragma comment(lib, "advapi32.lib")

namespace {

const wchar_t* kTaskName = L"FlipRow";

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

// 启动命令并等待其结束，返回退出码。
bool RunHidden(const std::wstring& cmdline, DWORD& code) {
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"开机自启：启动命令失败 %lu", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD c = 1;
    GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    code = c;
    return true;
}

}

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
    DWORD code = 1;
    std::wstring q = L"schtasks.exe /query /tn \"" + std::wstring(kTaskName) + L"\"";
    if (RunHidden(q, code) && code == 0) return 1;  // 退出码为 0 表示任务已存在。
    return 0;
}

bool startup::IsEnabled() { return Method() != 0; }

bool startup::SetEnabled(bool on) {
    if (!IsElevated()) {
        Log(L"开机自启：当前不是管理员，任务计划建不了");
        return false;
    }
    DWORD code = 1;
    if (!on) {
        std::wstring del = L"schtasks.exe /delete /tn \"" + std::wstring(kTaskName) + L"\" /f";
        if (!RunHidden(del, code)) return false;
        Log(L"开机自启：删除任务（退出码 %lu）", code);
        return code == 0;
    }
    // 参数中 exe 路径先用引号包裹，再由外层引号包裹。
    std::wstring inner = L"\\\"" + ExePath() + L"\\\"";
    std::wstring create = L"schtasks.exe /create /tn \"" + std::wstring(kTaskName) + L"\" /tr \"" + inner +
                          L"\" /sc onlogon /rl highest /f";
    if (!RunHidden(create, code)) return false;
    Log(L"开机自启：创建任务（退出码 %lu）%ls", code, create.c_str());
    return code == 0;
}
