// 开机自启模块，通过计划任务在用户登录时以最高权限启动本程序。
// 使用计划任务而不是注册表 Run 项，因为 Run 项启动的进程不会被提权，
// 而接管 Alt+Tab 需要一个与管理员窗口同级（High 完整性）的键盘钩子。
// 计划任务由任务计划服务在登录时创建进程，不会出现 UAC 提示。
#include "startup.h"

#include "com.h"

#include <comdef.h>
#include <sddl.h>
#include <shlobj.h>
#include <taskschd.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t* kTaskName = L"FlipRow";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"FlipRow";
const wchar_t* kLnkName = L"FlipRow.lnk";

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

// 计划任务的操作命令行需要带引号，否则含空格的安装路径会被拆成多段。
std::wstring ExeCommand() {
    std::wstring exe = ExePath();
    return exe.find(L' ') == std::wstring::npos ? exe : L"\"" + exe + L"\"";
}

// 返回当前用户 SID 字符串，计划任务以该身份在登录时运行。
std::wstring CurrentUser() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    std::wstring sid;
    BYTE buf[256] = {};
    DWORD cb = 0;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &cb)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) {
            sid = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return sid;
}

// 连接本机任务计划服务并取得根目录。
bool Connect(Com<ITaskService>& svc, Com<ITaskFolder>& root) {
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(svc.Put())))) return false;
    if (FAILED(svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) return false;
    return SUCCEEDED(svc->GetFolder(_bstr_t(L"\\"), root.Put()));
}

bool TaskExists() {
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!Connect(svc, root)) return false;
    Com<IRegisteredTask> task;
    return root->GetTask(_bstr_t(kTaskName), task.Put()) == S_OK;
}

bool CreateTask() {
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!Connect(svc, root)) {
        Log(L"开机自启：无法连接任务计划服务");
        return false;
    }
    const std::wstring user = CurrentUser();
    if (user.empty()) {
        Log(L"开机自启：无法取得当前用户 SID");
        return false;
    }
    const std::wstring exe = ExeCommand();

    Com<ITaskDefinition> def;
    HRESULT hr = svc->NewTask(0, def.Put());
    if (FAILED(hr)) {
        Log(L"开机自启：新建任务定义失败 0x%08X", (unsigned)hr);
        return false;
    }
    {
        Com<IRegistrationInfo> info;
        if (SUCCEEDED(def->get_RegistrationInfo(info.Put()))) {
            info->put_Author(_bstr_t(L"FlipRow"));
            info->put_Description(_bstr_t(L"FlipRow Alt+Tab switcher, started at logon with highest privileges."));
        }
    }
    {
        // 交互式令牌加最高权限：计划任务服务用用户的完整管理员令牌启动进程，不触发 UAC。
        Com<IPrincipal> pr;
        if (FAILED(def->get_Principal(pr.Put()))) {
            Log(L"开机自启：设置任务主体失败");
            return false;
        }
        pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        pr->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }
    {
        Com<ITaskSettings> st;
        if (SUCCEEDED(def->get_Settings(st.Put()))) {
            st->put_StartWhenAvailable(VARIANT_TRUE);
            // 笔记本不插电时也要启动，schtasks 的默认值会跳过。
            st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            st->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));  // 常驻程序不限制运行时长。
            st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
            st->put_AllowHardTerminate(VARIANT_TRUE);
        }
    }
    {
        Com<ITriggerCollection> trs;
        if (FAILED(def->get_Triggers(trs.Put()))) {
            Log(L"开机自启：设置触发器失败");
            return false;
        }
        Com<ITrigger> tr;
        if (FAILED(trs->Create(TASK_TRIGGER_LOGON, tr.Put()))) {
            Log(L"开机自启：创建登录触发器失败");
            return false;
        }
        Com<ILogonTrigger> lt;
        if (SUCCEEDED(tr->QueryInterface(IID_PPV_ARGS(lt.Put())))) {
            lt->put_UserId(_bstr_t(user.c_str()));
            lt->put_Delay(_bstr_t(L"PT5S"));  // 等资源管理器就绪，减少托盘和钩子抢跑。
        }
    }
    {
        Com<IActionCollection> acs;
        if (FAILED(def->get_Actions(acs.Put()))) {
            Log(L"开机自启：设置操作失败");
            return false;
        }
        Com<IAction> ac;
        if (FAILED(acs->Create(TASK_ACTION_EXEC, ac.Put()))) {
            Log(L"开机自启：创建启动操作失败");
            return false;
        }
        Com<IExecAction> ex;
        if (SUCCEEDED(ac->QueryInterface(IID_PPV_ARGS(ex.Put())))) ex->put_Path(_bstr_t(exe.c_str()));
    }

    Com<IRegisteredTask> task;
    hr = root->RegisterTaskDefinition(_bstr_t(kTaskName), def.Get(), TASK_CREATE_OR_UPDATE, _variant_t(user.c_str()),
                                      _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(), task.Put());
    if (FAILED(hr)) {
        Log(L"开机自启：注册计划任务失败 0x%08X", (unsigned)hr);
        return false;
    }
    Log(L"开机自启：已注册计划任务 %ls", kTaskName);
    return true;
}

bool DeleteTask() {
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!Connect(svc, root)) {
        Log(L"开机自启：无法连接任务计划服务");
        return false;
    }
    const HRESULT hr = root->DeleteTask(_bstr_t(kTaskName), 0);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        Log(L"开机自启：已删除计划任务 %ls", kTaskName);
        return true;
    }
    Log(L"开机自启：删除计划任务失败 0x%08X", (unsigned)hr);
    return false;
}

// 旧方案在注册表 Run 项和启动文件夹留下入口，开启或关闭自启时一并清理。
// 保留它们会让未提权的实例随登录启动，与计划任务的实例抢同一个互斥量。
void RemoveRunValue() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (RegDeleteValueW(k, kRunValue) == ERROR_SUCCESS) Log(L"开机自启：已删除旧的 Run 项");
    RegCloseKey(k);
}

void RemoveStartupLnk() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) return;
    std::wstring lnk = std::wstring(p) + L"\\" + kLnkName;
    CoTaskMemFree(p);
    if (DeleteFileW(lnk.c_str())) Log(L"开机自启：已删除旧的启动文件夹快捷方式");
}

// 自启状态缓存，避免每次打开托盘菜单都访问任务计划服务。
bool g_cache = false;
ULONGLONG g_cacheAt = 0;

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

bool startup::IsEnabled() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_cacheAt < 2000) return g_cache;
    g_cache = TaskExists();
    g_cacheAt = now;
    return g_cache;
}

bool startup::SetEnabled(bool on) {
    const bool ok = on ? CreateTask() : DeleteTask();
    if (ok) {
        RemoveRunValue();
        RemoveStartupLnk();
    }
    g_cacheAt = 0;
    return ok;
}
