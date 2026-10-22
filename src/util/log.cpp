#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>

static const char* level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static const char* level_color[] = { "\033[37m", "\033[32m", "\033[33m", "\033[31m" };

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& msg)
{
    if (level < min_level_) return;

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

    const char* fname = file;
    for (const char* p = file; *p; p++) {
        if (*p == '/' || *p == '\\') fname = p + 1;
    }

    std::ostringstream ss;
    ss << level_color[level] << "[" << timebuf << "] "
       << level_str[level] << " [" << fname << ":" << line << "]\033[0m " << msg;
    std::string line_str = ss.str();

    /* 同时输出到 stderr 和内存队列 */
    fprintf(stderr, "%s\n", line_str.c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    logs_.push_back(line_str);
    while ((int)logs_.size() > 200) logs_.pop_front();
}

std::deque<std::string> Logger::getRecentLogs(int n)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<std::string> result;
    int start = (int)logs_.size() - n;
    if (start < 0) start = 0;
    for (int i = start; i < (int)logs_.size(); i++) {
        result.push_back(logs_[i]);
    }
    return result;
}
