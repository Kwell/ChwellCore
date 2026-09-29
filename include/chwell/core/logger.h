#pragma once

#include <string>
#include <mutex>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <atomic>
#include <unordered_map>
#include <fstream>

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

    // ===== 结构化日志 =====
    // 输出格式：text（默认，原样）| json（每行一条 JSON）
    void set_format_json(bool enable) { format_json_ = enable; }
    bool format_json() const { return format_json_; }

    // 写入文件（追加）；空字符串表示仅终端
    void set_file(const std::string& path);

    // 分类级别：category -> level（未设置的分类用全局 level）
    void set_category_level(const std::string& category, LogLevel level);
    void clear_category_level(const std::string& category);
    bool category_enabled(const std::string& category, LogLevel level) const;

    void log(LogLevel level, const std::string& msg);
    void log(LogLevel level, const std::string& category, const std::string& msg);

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

    std::atomic<LogLevel> current_level_;
    std::atomic<bool> use_color_;
    std::atomic<bool> format_json_;
    std::mutex mutex_;
    std::string file_path_;
    void* file_stream_ = nullptr;  // std::ofstream*，避免头文件引入
    std::unordered_map<std::string, LogLevel> category_levels_;
    mutable std::mutex category_mutex_;
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

// 带分类的结构化日志（category 便于过滤；字段建议 key=value 空格分隔）
#define CHWELL_LOG_DEBUG_C(cat, x) do { \
    std::ostringstream _chwell_ss; _chwell_ss << x; \
    ::chwell::core::Logger::instance().log(::chwell::core::LogLevel::Debug, cat, _chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_INFO_C(cat, x) do { \
    std::ostringstream _chwell_ss; _chwell_ss << x; \
    ::chwell::core::Logger::instance().log(::chwell::core::LogLevel::Info, cat, _chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_WARN_C(cat, x) do { \
    std::ostringstream _chwell_ss; _chwell_ss << x; \
    ::chwell::core::Logger::instance().log(::chwell::core::LogLevel::Warn, cat, _chwell_ss.str()); \
} while (0)
#define CHWELL_LOG_ERROR_C(cat, x) do { \
    std::ostringstream _chwell_ss; _chwell_ss << x; \
    ::chwell::core::Logger::instance().log(::chwell::core::LogLevel::Error, cat, _chwell_ss.str()); \
} while (0)
