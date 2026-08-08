/*
 * 所属模块：core 公共基础层——极简日志接口与宏。
 * 本文件主体思路：提供全局日志级别设置（log_set_level）与带文件/行号的写日志函数
 *   （log_write），并以 AME_LOGD/I/W/E 宏封装 snprintf 格式化后调用 log_write。
 * 关键算法/数据结构：无复杂算法；固定 1024 字节栈缓冲格式化，输出到 stderr（实现见 log.cpp）。
 * 依赖关系：仅依赖 <cstdio>；被 M1~M14 全部模块用于日志输出。
 */
#pragma once
#include <cstdio>

namespace ame {
enum class LogLevel { Debug, Info, Warn, Error };
void log_set_level(LogLevel lv);
void log_write(LogLevel lv, const char* file, int line, const char* msg);
}  // namespace ame

#define AME_LOG(lv, ...)                                                  \
  do {                                                                    \
    char _ame_buf[1024];                                                  \
    std::snprintf(_ame_buf, sizeof(_ame_buf), __VA_ARGS__);               \
    ::ame::log_write(lv, __FILE__, __LINE__, _ame_buf);                   \
  } while (0)

#define AME_LOGD(...) AME_LOG(::ame::LogLevel::Debug, __VA_ARGS__)
#define AME_LOGI(...) AME_LOG(::ame::LogLevel::Info, __VA_ARGS__)
#define AME_LOGW(...) AME_LOG(::ame::LogLevel::Warn, __VA_ARGS__)
#define AME_LOGE(...) AME_LOG(::ame::LogLevel::Error, __VA_ARGS__)
