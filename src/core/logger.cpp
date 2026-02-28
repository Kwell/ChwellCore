#include "chwell/core/logger.h"

#include <cstdio>
#include <iomanip>
#include <sstream>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace chwell {
namespace core {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() : current_level_(LogLevel::Info), use_color_(false) {
#if defined(__linux__) || defined(__APPLE__)
    use_color_ = (isatty(STDOUT_FILENO) != 0);
#endif
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

    std::ostream& out = stream_for(level);
    out << now_string();

    if (use_color_) {
        out << " " << level_color(level);
    }
    out << " [" << level_to_string(level) << "] ";
    if (use_color_) {
        out << color_reset();
    }
    out << msg << std::endl;
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

std::string Logger::now_string() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    char buf[32];
#if defined(_MSC_VER)
    tm timeinfo;
    localtime_s(&timeinfo, &t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
#else
    std::tm* timeinfo = std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
#endif

    std::ostringstream oss;
    oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::level_color(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return "\033[2m";      // dim
        case LogLevel::Info:  return "\033[0m";     // default
        case LogLevel::Warn:  return "\033[33m";     // yellow
        case LogLevel::Error: return "\033[31m";     // red
        default:              return "\033[0m";
    }
}

std::string Logger::color_reset() const {
    return "\033[0m";
}

std::ostream& Logger::stream_for(LogLevel level) {
    return (level == LogLevel::Error || level == LogLevel::Warn) ? std::cerr : std::cout;
}

}  // namespace core
}  // namespace chwell
