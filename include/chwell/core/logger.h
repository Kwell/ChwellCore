#pragma once

#include <string>
#include <mutex>
#include <iostream>
#include <chrono>
#include <ctime>

namespace chwell {
namespace core {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);

    void log(LogLevel level, const std::string& msg);

    void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
    void info(const std::string& msg)  { log(LogLevel::Info, msg); }
    void warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }

private:
    Logger() : current_level_(LogLevel::Debug) {}

    std::string level_to_string(LogLevel level);
    std::string now_string();

    LogLevel current_level_;
    std::mutex mutex_;
};

} // namespace core
} // namespace chwell

