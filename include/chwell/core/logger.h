#pragma once

#include <string>
#include <mutex>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>

namespace chwell {
namespace core {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const { return current_level_; }

    // 是否启用终端颜色（默认自动检测 isatty）
    void set_use_color(bool use) { use_color_ = use; }
    bool use_color() const { return use_color_; }

    void log(LogLevel level, const std::string& msg);

    void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
    void info(const std::string& msg)  { log(LogLevel::Info, msg); }
    void warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }

private:
    Logger();

    std::string level_to_string(LogLevel level);
    std::string now_string();
    std::string level_color(LogLevel level) const;
    std::string color_reset() const;
    std::ostream& stream_for(LogLevel level);

    LogLevel current_level_;
    bool use_color_;
    std::mutex mutex_;
};

}  // namespace core
}  // namespace chwell

// 简短日志宏，避免写 Logger::instance().info(...)
// 用法：CHWELL_LOG_INFO("hello"); 或 CHWELL_LOG_INFO("port=" << port << " ok");
#define CHWELL_LOG_DEBUG(x) do { \
    std::ostringstream _chwell_ss; \
    _chwell_ss << x; \
    ::chwell::core::Logger::instance().debug(_chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_INFO(x) do { \
    std::ostringstream _chwell_ss; \
    _chwell_ss << x; \
    ::chwell::core::Logger::instance().info(_chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_WARN(x) do { \
    std::ostringstream _chwell_ss; \
    _chwell_ss << x; \
    ::chwell::core::Logger::instance().warn(_chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_ERROR(x) do { \
    std::ostringstream _chwell_ss; \
    _chwell_ss << x; \
    ::chwell::core::Logger::instance().error(_chwell_ss.str()); \
} while (0)

