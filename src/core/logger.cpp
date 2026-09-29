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

Logger::Logger() : current_level_(LogLevel::Info), use_color_(false), format_json_(false) {
#if defined(__linux__) || defined(__APPLE__)
    use_color_ = (isatty(STDOUT_FILENO) != 0);
#endif
}

void Logger::set_level(LogLevel level) {
    current_level_ = level;
}

void Logger::log(LogLevel level, const std::string& msg) {
    log(level, std::string(), msg);
}

void Logger::log(LogLevel level, const std::string& category, const std::string& msg) {
    if (!category.empty()) {
        if (!category_enabled(category, level)) return;
    } else if (static_cast<int>(level) < static_cast<int>(current_level_.load())) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto emit = [&](std::ostream& out, bool colorize) {
        if (format_json_.load()) {
            std::string escaped;
            escaped.reserve(msg.size());
            for (char c : msg) {
                if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
                else if (c == '\n') { escaped += "\\n"; }
                else escaped += c;
            }
            out << "{\"ts\":\"" << now_string() << "\",\"level\":\""
                << level_to_string(level) << "\",\"cat\":\""
                << (category.empty() ? "default" : category)
                << "\",\"msg\":\"" << escaped << "\"}\n";
        } else {
            out << now_string();
            if (colorize && use_color_.load()) out << " " << level_color(level);
            out << " [" << level_to_string(level) << "]";
            if (colorize && use_color_.load()) out << color_reset();
            if (!category.empty()) out << " [" << category << "]";
            out << " " << msg << "\n";
        }
    };

    emit(stream_for(level), true);
    if (file_stream_) {
        emit(*static_cast<std::ofstream*>(file_stream_), false);
    }
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
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
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


void Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_) {
        static_cast<std::ofstream*>(file_stream_)->close();
        delete static_cast<std::ofstream*>(file_stream_);
        file_stream_ = nullptr;
    }
    file_path_ = path;
    if (!path.empty()) {
        auto* fs = new std::ofstream(path, std::ios::app);
        if (fs->is_open()) {
            file_stream_ = fs;
        } else {
            delete fs;
            file_stream_ = nullptr;
        }
    }
}

void Logger::set_category_level(const std::string& category, LogLevel level) {
    std::lock_guard<std::mutex> lock(category_mutex_);
    category_levels_[category] = level;
}

void Logger::clear_category_level(const std::string& category) {
    std::lock_guard<std::mutex> lock(category_mutex_);
    category_levels_.erase(category);
}

bool Logger::category_enabled(const std::string& category, LogLevel level) const {
    LogLevel effective = current_level_.load();
    {
        std::lock_guard<std::mutex> lock(category_mutex_);
        auto it = category_levels_.find(category);
        if (it != category_levels_.end()) effective = it->second;
    }
    return static_cast<int>(level) >= static_cast<int>(effective);
}

}  // namespace core
}  // namespace chwell
