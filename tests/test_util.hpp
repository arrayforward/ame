#pragma once
/*
 * AME 测试共享工具（tests/test_util.hpp）
 * 主体思路：提供无 gtest 依赖的极简断言宏与计数器，每个测试可执行文件为单一翻译单元，
 *           内联静态计数器天然隔离，互不干扰。
 * 关键数据结构：ame_test::total()/failed() 两个函数级静态计数器。
 * 依赖关系：仅依赖标准库；被 tests/ 下全部测试文件包含。
 */
#include <cmath>
#include <cstdio>

namespace ame_test {
inline int& total() { static int v = 0; return v; }
inline int& failed() { static int v = 0; return v; }
}  // namespace ame_test

// 断言：失败打印位置与表达式，计数继续跑
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++::ame_test::total();                                                   \
    if (!(cond)) {                                                           \
      ++::ame_test::failed();                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
    }                                                                        \
  } while (0)

// 浮点近似断言
#define CHECK_NEAR(a, b, eps)                                                \
  do {                                                                       \
    ++::ame_test::total();                                                   \
    if (std::fabs((double)(a) - (double)(b)) > (eps)) {                      \
      ++::ame_test::failed();                                                \
      std::printf("FAIL %s:%d: %s(%f) != %s(%f)\n", __FILE__, __LINE__,     \
                  #a, (double)(a), #b, (double)(b));                         \
    }                                                                        \
  } while (0)

// main 末尾报告并返回退出码（全绿 = 0）
#define AME_TEST_REPORT()                                                    \
  (std::printf("== %d/%d passed ==\n",                                       \
               ::ame_test::total() - ::ame_test::failed(),                   \
               ::ame_test::total()),                                         \
   ::ame_test::failed() ? 1 : 0)
