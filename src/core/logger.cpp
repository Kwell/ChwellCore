#include "chwell/core/logger.h"

namespace chwell {
namespace core {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
}

void Logger::log(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(current_level_)) {
        return;
    }

    std::cout << "[" << now_string() << "]"
              << "[" << level_to_string(level) << "] "
              << msg << std::endl;
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        default:              return "UNKNOWN";
    }
}

std::string Logger::now_string() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    char buf[32];
#if defined(_MSC_VER)
    tm timeinfo;
    localtime_s(&timeinfo, &t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
#else
    std::tm* timeinfo = std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
#endif
    return std::string(buf);
}

} // namespace core
} // namespace chwell

