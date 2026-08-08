/*
 * 所属模块：core 公共基础层——极简日志实现（log.h 的配对实现）。
 * 本文件主体思路：维护全局日志级别阈值，log_write 按阈值过滤后
 *   以 "[级别] 文件:行号 消息" 格式输出到 stderr。
 * 关键算法/数据结构：无复杂算法。
 * 依赖关系：依赖本模块 log.h 与 <cstdio>；被全部模块经 AME_LOG* 宏间接调用。
 */
#include "ame/core/log.h"

namespace ame {

static LogLevel g_level = LogLevel::Info;

// 设置全局日志级别阈值。
void log_set_level(LogLevel lv) { g_level = lv; }

// 伪代码：
//   步骤1：消息级别低于全局阈值则直接丢弃；
//   步骤2：按 "[级别] 文件:行号 消息" 格式输出到 stderr。
void log_write(LogLevel lv, const char* file, int line, const char* msg) {
  if ((int)lv < (int)g_level) return;
  static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  std::fprintf(stderr, "[%s] %s:%d %s\n", names[(int)lv], file, line, msg);
}

}  // namespace ame
