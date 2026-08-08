/*
 * DAT 实现：Darts-clone 批量构建 + 多起点前缀走查。
 * 语义界定（重要）：DAT 是纯 Trie，没有失败指针——匹配语义为"从每个文本位置做
 *   前缀走查"（O(文本长×最大词长)），与 AC 自动机（单遍 O(文本长)，含 fail 回退的
 *   后缀命中）不同。对匹配集合而言两者一致（任何词典词在文本中某位置出现都会被
 *   多起点走查命中），差异在时间与实现复杂度，不在结果。
 */
#include "ame/extractor/dat_trie.h"

#include <algorithm>

namespace ame {

// UTF-8 解码为码点序列（非法字节 → U+FFFD）
static std::vector<uint32_t> dat_decode(const std::string& s) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x2) { cp = c & 0x07; len = 4; }
    else { cp = 0xFFFD; len = 1; }
    if (i + len > s.size()) { cp = 0xFFFD; len = 1; }
    for (size_t j = 1; j < len; ++j) cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
    cps.push_back(cp);
    i += len;
  }
  return cps;
}

void DatTrie::ensure(int idx) {
  if ((size_t)idx >= base_.size()) {
    size_t ns = std::max(base_.size() * 3 / 2, (size_t)idx + 1024);
    base_.resize(ns, 0);
    check_.resize(ns, 0);
    value_.resize(ns, -1);
  }
  if ((size_t)idx + 1 > used_) used_ = idx + 1;
}

// find_base 伪代码：从 free_hint_ 起线性探查最小 b，使所有子字符 b+c 空闲。
int DatTrie::find_base(const std::vector<uint32_t>& children) {
  int b = free_hint_;
  for (;; ++b) {
    bool ok = true;
    for (uint32_t c : children) {
      ensure(b + (int)c);
      if (check_[b + (int)c] != 0) { ok = false; break; }
    }
    if (ok) {
      free_hint_ = b;
      return b;
    }
  }
}

int DatTrie::next_of(int node, uint32_t cp) const {
  int b = base_[node];
  if (b <= 0) return -1;
  int t = b + (int)cp;
  return (t < (int)base_.size() && check_[t] == node) ? t : -1;
}

// build 伪代码：码点化+排序去重 → ensure(1) 为根 → 递归 build_recursive。
void DatTrie::build(const std::vector<std::string>& words) {
  std::vector<std::vector<uint32_t>> keys;
  keys.reserve(words.size());
  for (auto& w : words) {
    auto cps = dat_decode(w);
    if (!cps.empty()) keys.push_back(std::move(cps));
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  // 排序后词面（value_ 下标对齐）
  words_sorted_.clear();
  words_sorted_.reserve(keys.size());
  for (auto& k : keys) {
    std::string s;
    for (uint32_t cp : k) {
      if (cp < 0x80) s += (char)cp;
      else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 63));
      } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 63));
        s += (char)(0x80 | (cp & 63));
      } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 63));
        s += (char)(0x80 | ((cp >> 6) & 63));
        s += (char)(0x80 | (cp & 63));
      }
    }
    words_sorted_.push_back(std::move(s));
  }
  words_ = words;
  base_.assign(1024, 0);
  check_.assign(1024, 0);
  value_.assign(1024, -1);
  ensure(1);
  max_len_ = 0;
  for (auto& k : keys) max_len_ = std::max(max_len_, k.size());
  // 词索引：排序后的 keys 与原 words_ 不同序，value 存码点序列→词 的序号需重建
  // 简化：value 存排序后 keys 的下标，words_sorted_ 另行维护
  build_recursive(1, keys, 0, keys.size(), 0);
}

// build_recursive 伪代码（Darts-clone 标准批量构建）：
//   步骤1：若区间空直接返回；
//   步骤2：找出区间内在 depth 处结束（key 长度==depth）的 key → value_[node]=其序号；
//   步骤3：收集 depth 层的子字符集合；无子字符（叶子）返回；
//   步骤4：find_base 分配 base，逐子字符 check[b+c]=node 并递归子区间。
void DatTrie::build_recursive(int node, const std::vector<std::vector<uint32_t>>& keys,
                              size_t l, size_t r, size_t depth) {
  if (l >= r) return;
  // 终结：区间内第一个 key 恰好在 depth 结束（有序，结束 key 必在区间头部）
  size_t pos = l;
  if (keys[pos].size() == depth) {
    value_[node] = (int)pos;
    ++pos;
  }
  if (pos >= r) return;
  // 子字符集合（有序区间，连续分组）
  std::vector<uint32_t> children;
  std::vector<std::pair<size_t, size_t>> ranges;  // (start, end)
  size_t i = pos;
  while (i < r) {
    uint32_t c = keys[i][depth];
    size_t j = i;
    while (j < r && keys[j][depth] == c) ++j;
    children.push_back(c);
    ranges.emplace_back(i, j);
    i = j;
  }
  int b = find_base(children);
  base_[node] = b;
  // 先为全部子节点占位，再逐个递归（Darts-clone 关键顺序：
  // 否则子节点内部 find_base 会看到尚未占位的兄弟槽位，造成路径合并误配）
  for (size_t k = 0; k < children.size(); ++k) check_[b + (int)children[k]] = node;
  for (size_t k = 0; k < children.size(); ++k)
    build_recursive(b + (int)children[k], keys, ranges[k].first, ranges[k].second,
                    depth + 1);
}

// match 伪代码：从每个文本位置做前缀走查（深度 ≤ max_len_），
//   途经终结节点（value_>=0）即输出对应词。
std::unordered_set<std::string> DatTrie::match(const std::string& text) const {
  std::unordered_set<std::string> out;
  auto cps = dat_decode(text);
  for (size_t i = 0; i < cps.size(); ++i) {
    int node = 1;
    for (size_t j = i; j < cps.size() && j - i < max_len_; ++j) {
      node = next_of(node, cps[j]);
      if (node < 0) break;
      if (value_[node] >= 0) {
        // value_ 存的是排序后 keys 下标；词面需在 build 时同步保存
        out.insert(words_sorted_[value_[node]]);
      }
    }
  }
  return out;
}

}  // namespace ame
