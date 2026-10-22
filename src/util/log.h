#ifndef LOG_H
#define LOG_H

#include <string>
#include <deque>
#include <mutex>
#include <cstdarg>
#include <cstdio>

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

class Logger {
public:
    static Logger& instance();

    void log(LogLevel level, const char* file, int line, const std::string& msg);

    /* 获取最近 N 条日志（供终端显示） */
    std::deque<std::string> getRecentLogs(int n);

    /* 设置最小输出级别 */
    void setLevel(LogLevel level) { min_level_ = level; }

private:
    Logger() : min_level_(LOG_INFO) {}
    std::mutex mutex_;
    std::deque<std::string> logs_;
    LogLevel min_level_;
};

/* printf 风格格式化辅助函数 */
inline std::string log_format(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    return std::string(b);
}

#define LOG_DBG(fmt, ...)  Logger::instance().log(LOG_DEBUG, __FILE__, __LINE__, log_format(fmt, ##__VA_ARGS__))
#define LOG_INFO(fmt, ...) Logger::instance().log(LOG_INFO,  __FILE__, __LINE__, log_format(fmt, ##__VA_ARGS__))
#define LOG_WARN(fmt, ...) Logger::instance().log(LOG_WARN,  __FILE__, __LINE__, log_format(fmt, ##__VA_ARGS__))
#define LOG_ERR(fmt, ...)  Logger::instance().log(LOG_ERROR, __FILE__, __LINE__, log_format(fmt, ##__VA_ARGS__))

#endif /* LOG_H */
