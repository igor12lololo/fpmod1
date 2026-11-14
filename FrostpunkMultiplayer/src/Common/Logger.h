#pragma once
#include "Common.h"
#include <fstream>
#include <filesystem>

namespace FrostMP {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLevel(LogLevel level) { minLevel_ = level; }
    void SetFile(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) file_.close();
        file_.open(path, std::ios::app | std::ios::out);
    }

    template<typename... Args>
    void Log(LogLevel level, const char* file, int line, const char* func, const char* fmt, Args... args) {
        if (level < minLevel_) return;

        char buffer[2048];
        int len = snprintf(buffer, sizeof(buffer), fmt, args...);
        if (len < 0) return;

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", std::localtime(&time));

        const char* levelStr = "";
        switch (level) {
            case LogLevel::Debug: levelStr = "DBG"; break;
            case LogLevel::Info: levelStr = "INF"; break;
            case LogLevel::Warning: levelStr = "WRN"; break;
            case LogLevel::Error: levelStr = "ERR"; break;
        }

        char finalMsg[4096];
        snprintf(finalMsg, sizeof(finalMsg), "[%s.%03lld] [%s] %s:%d %s() - %s",
                 timeStr, ms.count(), levelStr, file, line, func, buffer);

        OutputDebugStringA(finalMsg);
        OutputDebugStringA("\n");

        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_ << finalMsg << "\n";
            file_.flush();
        }
    }

private:
    Logger() : minLevel_(LogLevel::Info) {}
    ~Logger() { if (file_.is_open()) file_.close(); }

    LogLevel minLevel_;
    std::ofstream file_;
    std::mutex mutex_;
};

#define LOG_DEBUG(fmt, ...) FrostMP::Logger::Instance().Log(FrostMP::LogLevel::Debug, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) FrostMP::Logger::Instance().Log(FrostMP::LogLevel::Info, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) FrostMP::Logger::Instance().Log(FrostMP::LogLevel::Warning, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) FrostMP::Logger::Instance().Log(FrostMP::LogLevel::Error, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

} // namespace FrostMP