#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <ctime>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace FrostMP {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

template<typename T>
using Atomic = std::atomic<T>;

struct ScopedHandle {
    HANDLE handle = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }
    ~ScopedHandle() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    operator HANDLE() const { return handle; }
    explicit operator bool() const { return handle && handle != INVALID_HANDLE_VALUE; }
    HANDLE* operator&() { return &handle; }
};

// Raw file trace that bypasses Log::Write entirely — safe to call from hooks,
// DllMain, and remote threads (no C++ standard library allocations).
inline void TraceToFile(const char* msg) {
    wchar_t tempPath[32768];
    DWORD len = GetTempPathW(32768, tempPath);
    if (!len) return;

    wchar_t logFile[32768];
    wcscpy_s(logFile, tempPath);
    wcscat_s(logFile, L"FrostMP_Trace.log");

    HANDLE hFile = CreateFileW(logFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hFile, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(hFile, msg, (DWORD)strlen(msg), &written, nullptr);
    const char* newline = "\n";
    WriteFile(hFile, newline, 1, &written, nullptr);
    CloseHandle(hFile);
    OutputDebugStringA("[FrostMP-Trace] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

inline std::string GetWin32Error(DWORD error = GetLastError()) {
    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string msg = size && buffer ? std::string(buffer, size) : "Win32 error " + std::to_string(error);
    if (buffer) LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
    return msg;
}

inline std::wstring Widen(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), len);
    return result;
}

inline std::string Narrow(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), len, nullptr, nullptr);
    return result;
}

inline std::filesystem::path GetModuleDirectory(HMODULE module = nullptr) {
    std::wstring path(32768, L'\0');
    DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!len || len == path.size()) throw std::runtime_error("Failed to get module path");
    path.resize(len);
    return std::filesystem::path(path).parent_path();
}

inline u64 GetTimestampMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

[[noreturn]] inline void Fail(const std::string& msg) {
    throw std::runtime_error(msg);
}

inline bool ParseSteamID64(const std::wstring& text, u64& out) {
    if (text.size() != 17) return false;
    try {
        size_t pos = 0;
        unsigned long long val = std::stoull(text, &pos, 10);
        if (pos != text.size()) return false;
        if (val < 76561197960265728ULL) return false;
        out = static_cast<u64>(val);
        return true;
    } catch (...) {
        return false;
    }
}

namespace Log {
    inline std::mutex g_mutex;
    inline std::ofstream g_file;
    inline bool g_initialized = false;

    inline void Init() {
        if (g_initialized) return;

        // Try module directory first (works for launcher), fall back to
        // current directory (works for injected DLL where exe dir is read-only).
        std::wstring candidates[2];
        candidates[0] = (GetModuleDirectory() / L"FrostMP.log").wstring();
        candidates[1] = L".\\FrostMP.log";

        for (const auto& cand : candidates) {
            g_file.open(cand, std::ios::out | std::ios::app);
            if (g_file.is_open()) break;
        }

        g_initialized = true;

        if (g_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            g_file << "\n=== FrostMP Log Started: " << std::ctime(&time);
            g_file.flush();
        }
    }

    // Reinitialize logging with a specific path (used by injected DLL mod)
    inline void Reinit(const std::wstring& logPath) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initialized = false;
        if (g_file.is_open()) g_file.close();
        g_file.open(logPath, std::ios::out | std::ios::app);
        g_initialized = true;
        if (g_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            g_file << "\n=== FrostMP Log Started: " << std::ctime(&time);
            g_file.flush();
        }
    }

    inline void Write(const char* fmt, ...) {
        // Lock covers both Init() (shared state: g_initialized, g_file) and the file write,
        // preventing a data race when multiple threads call Write() concurrently.
        std::lock_guard<std::mutex> lock(g_mutex);
        Init();
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsprintf_s(buf, fmt, args);
        va_end(args);

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        auto time = std::chrono::system_clock::to_time_t(now);
        tm tm;
        localtime_s(&tm, &time);
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tm);

        std::string line = "[" + std::string(timeStr) + "." + std::to_string(ms).substr(0,3) + "] " + buf + "\n";

        OutputDebugStringA("[FrostMP] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");

        if (g_file.is_open()) {
            g_file << line;
            g_file.flush();
        }
    }
}

inline void DebugLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    Log::Write("%s", buf);
}

} // namespace FrostMP