/*
 * 模块定位：M1 要素提取器 —— 双数组 Trie（DAT，Darts-clone 风格批量构建）。
 * 本文件主体思路：以 base/check 双数组实现紧凑 Trie，作为 AC 自动机的内存优化备选后端。
 *   只支持"批量有序构建"（一次性从排序词表构建，Darts-clone 标准做法），
 *   不做增量插入（relocate 子树的高危路径在本场景无收益）。
 * 关键算法/数据结构：base/check/value 三数组；find_base 带 free-hint 线性探查；
 *   语义界定：DAT 是纯 Trie——多起点前缀走查（O(文本长×最大词长)），
 *   没有 AC 的失败指针/单遍扫描语义（详见实现注释与 README）。
 * 依赖关系：仅依赖标准库；被 bench/scripts/bench_ac.cpp 基准与测试使用。
 */
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace ame {

class DatTrie {
 public:
  // 批量构建（词表任意顺序，内部按码点排序去重）
  void build(const std::vector<std::string>& words);
  // 多起点前缀走查：返回文本中命中的全部词（集合语义，与 AC 输出集合对齐）
  std::unordered_set<std::string> match(const std::string& text) const;

  size_t nodes() const { return used_; }
  size_t approx_bytes() const { return used_ * 8 + value_.size() * 4; }
  // 调试访问器
  int base_at(int i) const { return i < (int)base_.size() ? base_[i] : 0; }
  int check_at(int i) const { return i < (int)check_.size() ? check_[i] : 0; }
  int value_at(int i) const { return i < (int)value_.size() ? value_[i] : -1; }

 private:
  void build_recursive(int node, const std::vector<std::vector<uint32_t>>& keys,
                       size_t l, size_t r, size_t depth);
  int find_base(const std::vector<uint32_t>& children);
  void ensure(int idx);
  int next_of(int node, uint32_t cp) const;

  std::vector<int32_t> base_, check_, value_;
  std::vector<std::string> words_;         // 原始词表（输入序）
  std::vector<std::string> words_sorted_;  // 排序去重后词表（value_ 下标对齐）
  size_t used_ = 0;      // 已分配节点数
  int free_hint_ = 1;    // find_base 线性探查起点提示
  size_t max_len_ = 0;   // 最长词码点数
};

}  // namespace ame
