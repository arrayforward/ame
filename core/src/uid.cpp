/*
 * 所属模块：core 公共基础层——uid 生成实现（uid.h 的配对实现）。
 * 本文件主体思路：进程内以随机种子 + 原子计数器经 splitmix64 混合产生随机值，
 *   取低 40bit 编码为 8 位 crockford base32，加 "E" 前缀输出短码。
 * 关键算法/数据结构：splitmix64 位混合；counter 保证同进程唯一，seed 保证跨进程随机。
 * 依赖关系：依赖本模块 uid.h 与标准库（atomic/chrono/random）；
 *   被需要新建节点标识的模块（graph_writer M4、storage M9 等）使用。
 */
#include "ame/core/uid.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>

namespace ame {

static const char* kB32 = "0123456789abcdefghjkmnpqrstvwxyz";  // crockford 无歧义

// 伪代码：
//   步骤1：预填 8 个 '0'；
//   步骤2：自右向左循环 8 轮——取 v 低 5bit 查 crockford 字母表填入，再右移 5 位。
std::string base32_encode40(uint64_t v) {
  std::string s(8, '0');
  for (int i = 7; i >= 0; --i) {
    s[i] = kB32[v & 31];
    v >>= 5;
  }
  return s;
}

// 伪代码：
//   步骤1：惰性初始化进程级种子 seed（random_device 两次取值拼成 64bit，再异或高精度时钟）；
//   步骤2：原子计数器自增，乘黄金比例常数后加 seed 得 z
//          （counter 保证同进程唯一，seed 保证跨进程随机）；
//   步骤3：splitmix64 三轮异或-移位-乘法混合打散位分布；
//   步骤4：取 z 低 40bit 编码为 8 位 base32，加 "E" 前缀返回。
std::string gen_uid() {
  static std::atomic<uint64_t> counter{0};
  static uint64_t seed = [] {
    std::random_device rd;
    uint64_t s = ((uint64_t)rd() << 32) ^ rd();
    s ^= (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return s;
  }();
  // splitmix64：counter 保证同进程内唯一，seed 保证跨进程随机
  uint64_t z = seed + 0x9e3779b97f4a7c15ULL * (++counter);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return "E" + base32_encode40(z & 0xFFFFFFFFFFULL);  // 40bit -> 8 字符
}

}  // namespace ame
