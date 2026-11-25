#include "common.h"

#include <share.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// 返回 exe 所在目录，运行时产生的文件都放在这里。
std::wstring ExeDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    auto i = d.find_last_of(L'\\');
    return i == std::wstring::npos ? std::wstring() : d.substr(0, i);
}

// 返回 exe 目录下指定文件名的完整路径。
std::wstring FilePath(const wchar_t* name) {
    std::wstring d = ExeDir();
    if (d.empty()) return name;
    return d + L"\\" + name;
}

FILE* g_log = nullptr;
std::atomic<bool> g_debug{ false };
std::mutex g_mx;
std::condition_variable g_cv;
std::wstring g_queue;  // 待写入磁盘的文本。
bool g_stop = false;
std::thread g_writer;  // 唯一持有日志文件句柄的线程。
std::mutex g_flushMx;

// 打开日志文件，位置为 exe 所在目录的 FlipRow.log。
FILE* OpenLog() {
    std::wstring p = FilePath(L"FlipRow.log");
    return _wfsopen(p.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

void WriterLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::wstring chunk;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
            if (g_queue.empty() && g_stop) break;
            chunk.swap(g_queue);
        }
        if (g_log && !chunk.empty()) {
            fputws(chunk.c_str(), g_log);
            fflush(g_log);
        }
    }
    if (g_log) {
        fflush(g_log);
        fclose(g_log);
        g_log = nullptr;
    }
}

}

void InitLog(bool debug) {
    g_debug = debug;
    g_log = OpenLog();
    if (!g_log) return;
    g_writer = std::thread(WriterLoop);
    SYSTEMTIME st;
    GetLocalTime(&st);
    Log(L"==== FlipRow %04d-%02d-%02d %02d:%02d:%02d%s ====", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        debug ? L"（调试日志）" : L"");
}

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t body[2048] = {};
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(body, _TRUNCATE, fmt, a);
    va_end(a);
    wchar_t line[2200] = {};
    _snwprintf_s(line, _TRUNCATE, L"%02d:%02d:%02d.%03d  %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_queue.size() < 256 * 1024) g_queue += line;  // 队列上限为 256 KB，超出时丢弃新日志以避免内存增长。
    }
    g_cv.notify_one();
}

void LogFlush() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_writer.joinable()) g_writer.join();
}

// 诊断输出，写入 exe 所在目录的 FlipRow.boot.log。
void BootLog(const wchar_t* fmt, ...) {
    std::wstring p = FilePath(L"FlipRow.boot.log");
    if (p.empty()) return;
    HANDLE f = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    wchar_t body[1024] = {};
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(body, _TRUNCATE, fmt, a);
    va_end(a);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200] = {};
    _snwprintf_s(line, _TRUNCATE, L"%02d:%02d:%02d.%03d  %ls\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    char utf8[4096] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD wr = 0;
    if (n > 1) WriteFile(f, utf8, (DWORD)(n - 1), &wr, nullptr);
    CloseHandle(f);
}

bool LogDebugOn() { return g_debug; }

namespace {
int g_langMode = 0;  // 语言模式，0 表示跟随系统，1 表示中文，2 表示英文。

// 返回配置文件路径，位置为 exe 所在目录的 FlipRow.ini。
std::wstring ConfigPath() { return FilePath(L"FlipRow.ini"); }
}

int LangMode() { return g_langMode; }

void SetLangMode(int mode) { g_langMode = (mode < 0 || mode > 2) ? 0 : mode; }

void LoadConfig() {
    std::wstring f = ConfigPath();
    if (f.empty()) return;
    SetLangMode((int)GetPrivateProfileIntW(L"ui", L"lang", 0, f.c_str()));
}

void SaveLangMode(int mode) {
    SetLangMode(mode);
    std::wstring f = ConfigPath();
    if (f.empty()) return;
    std::wstring v = std::to_wstring(g_langMode);
    WritePrivateProfileStringW(L"ui", L"lang", v.c_str(), f.c_str());
}

// 返回界面语言是否为中文，手动设置优先于系统语言。
bool IsChineseUi() {
    if (g_langMode == 1) return true;
    if (g_langMode == 2) return false;
    static int v = -1;
    if (v < 0) {
        wchar_t lang[16] = {};
        GetLocaleInfoW(LOCALE_SYSTEM_DEFAULT, LOCALE_SISO639LANGNAME, lang, 16);
        v = (!_wcsicmp(lang, L"zh")) ? 1 : 0;
    }
    return v == 1;
}
