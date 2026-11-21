#pragma once
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <windows.h>

namespace DebugLog {

inline std::mutex& Mutex() { static std::mutex m; return m; }

inline FILE* Open() {
    static FILE* f = nullptr;
    if (f) return f;
    CreateDirectoryA("C:\\VeoxDma", nullptr);
    CreateDirectoryA("C:\\VeoxDma\\robrun", nullptr);
    CreateDirectoryA("C:\\VeoxDma\\robrun\\debug", nullptr);
    f = fopen("C:\\VeoxDma\\robrun\\debug\\debug.log", "a");
    if (f) {
        fprintf(f, "\n=== SESSION START (pid %lu) ===\n", GetCurrentProcessId());
        fflush(f);
    }
    return f;
}

inline void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(Mutex());
    FILE* f = Open();
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fflush(f);
}

} // namespace DebugLog

#ifdef _DEBUG
#define DLOG(...) DebugLog::Log(__VA_ARGS__)
#else
#define DLOG(...) ((void)0)
#endif
