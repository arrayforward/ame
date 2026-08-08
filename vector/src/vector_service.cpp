/*
 * 所属模块：M13 向量服务（vector）—— embedding 计算/缓存/组合运算的实现。
 * 本文件主体思路：在无外部模型的约束下实现确定性向量服务：UTF-8 文本经
 *   字符 n-gram feature hashing 得到 128 维 L2 归一化向量；VectorService 在其上
 *   提供线程安全缓存、节点向量构造、向量合成、边向量差分与基于近邻的成分分解。
 * 关键算法/数据结构：UTF-8 码点解码；FNV-1a 64 位哈希；字符 n-gram(n=1,2,3)
 *   feature hashing（哈希取模定位维度、最高位定符号）；L2 归一化；
 *   unordered_map + mutex 的嵌入缓存。
 * 依赖关系：依赖 core 公共层的 Vec/Node 类型；依赖 storage(M9) 的
 *   get_node/ann_search 完成 decompose 近邻检索；被上层编排模块调用。
 */
#include "ame/vector/vector_service.h"

#include "ame/storage/storage.h"

#include <cmath>

namespace ame {

// UTF-8 解码为码点序列
// 伪代码：
//   步骤1：逐字节扫描字符串，按首字节高位模式判定码点长度：
//          0xxxxxxx 为 1 字节，110xxxxx 为 2 字节，1110xxxx 为 3 字节，
//          11110xxx 为 4 字节；其余非法首字节记为 U+FFFD 且长度按 1 处理。
//   步骤2：按长度取后续字节的低 6 位逐次拼入码点（越界则截断）。
//   步骤3：码点入列，前进 len 个字节，直至扫描完毕返回码点序列。
static std::vector<uint32_t> utf8_decode(const std::string& s) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp; size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x2) { cp = c & 0x07; len = 4; }
    else { cp = 0xFFFD; len = 1; }
    for (size_t j = 1; j < len && i + j < s.size(); ++j)
      cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
    cps.push_back(cp);
    i += len;
  }
  return cps;
}

// FNV-1a 64bit
// 伪代码：以 FNV 偏移基值初始化哈希；对每个码点按 4 个字节从低到高
//   逐字节"先异或、后乘 FNV 素数"，返回最终哈希值。
static uint64_t fnv1a(const uint32_t* data, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    for (int b = 0; b < 4; ++b) {
      h ^= (data[i] >> (b * 8)) & 0xFF;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

// 确定性哈希 embedding：对文本做字符 n-gram feature hashing
// 伪代码：
//   步骤1：初始化 kDim 维零向量；将文本 UTF-8 解码为码点序列，空序列直接返回零向量。
//   步骤2：对每个位置枚举 n=1,2,3 的字符 n-gram（i+n 越界则停止）；
//          对该 n-gram 计算 FNV-1a 哈希并异或 n 的黄金比例常数以区分不同 n；
//          哈希取模 kDim 定位维度，最高位定符号（正/负），各 n-gram 等权累加。
//   步骤3：L2 归一化后返回。
Vec HashEmbedder::embed_text(const std::string& text) {
  Vec v(kDim, 0.0f);
  auto cps = utf8_decode(text);
  if (cps.empty()) return v;
  for (size_t i = 0; i < cps.size(); ++i) {
    for (int n = 1; n <= 3; ++n) {  // 字符 n-gram
      if (i + n > cps.size()) break;
      uint64_t h = fnv1a(&cps[i], n) ^ (uint64_t)n * 0x9e3779b97f4a7c15ULL;
      int idx = (int)(h % kDim);
      float sign = (h & (1ULL << 63)) ? -1.0f : 1.0f;
      v[idx] += sign;  // 各 n-gram 等权
    }
  }
  VectorService::l2_normalize(v);
  return v;
}

// L2 归一化
// 伪代码：累加各分量平方和并开根得模长；若模长 > 1e-9 则逐分量除以模长，
//   近零向量保持不变（避免除零）。
void VectorService::l2_normalize(Vec& v) {
  double s = 0;
  for (float f : v) s += (double)f * f;
  s = std::sqrt(s);
  if (s > 1e-9)
    for (float& f : v) f = (float)(f / s);
}

// 构造函数
// 伪代码：保存 storage 指针；embedder 为空时回退到内置默认 HashEmbedder。
VectorService::VectorService(const Storage* storage, IEmbedder* embedder)
    : storage_(storage), embedder_(embedder ? embedder : &default_embedder_) {}

// 带缓存的文本 embedding
// 伪代码：
//   步骤1：加锁查缓存，命中则直接返回缓存向量。
//   步骤2：未命中则在锁外调用 embedder 计算向量（避免持锁做耗时计算）。
//   步骤3：再次加锁写入缓存并返回。
Vec VectorService::embed(const std::string& text) {
  {
    std::lock_guard lk(mu_);
    auto it = cache_.find(text);
    if (it != cache_.end()) return it->second;
  }
  Vec v = embedder_->embed_text(text);
  std::lock_guard lk(mu_);
  cache_[text] = v;
  return v;
}

// 节点 embedding
// 伪代码：以 name 为基底，text 非空则追加 text、type 非空则再追加 type，
//   拼成一段文本后走带缓存的 embed 计算向量。
Vec VectorService::embed_node(const Node& node) {
  std::string s = node.name;
  if (!node.text.empty()) s += " " + node.text;
  if (!node.type.empty()) s += " " + node.type;
  return embed(s);
}

// 多向量组合
// 伪代码：
//   步骤1：输入为空则返回空向量；否则以首个向量的维度建零向量。
//   步骤2：op 为 "add" 时，遍历所有向量，跳过维度不符者，其余逐维累加；
//          其它 op 暂不处理，保持零向量。
//   步骤3：L2 归一化后返回。
Vec VectorService::compose(const std::vector<Vec>& vectors, const std::string& op) {
  if (vectors.empty()) return {};
  Vec r(vectors[0].size(), 0.0f);
  if (op == "add") {
    for (auto& v : vectors) {
      if (v.size() != r.size()) continue;
      for (size_t i = 0; i < r.size(); ++i) r[i] += v[i];
    }
  }
  l2_normalize(r);
  return r;
}

// 边向量
// 伪代码：两向量维度不一致则返回空向量；否则逐维计算 b-a 差分，
//   得到 A 指向 B 的方向向量（未归一化）。
Vec VectorService::edge_vec(const Vec& a, const Vec& b) {
  if (a.size() != b.size()) return {};
  Vec r(a.size());
  for (size_t i = 0; i < a.size(); ++i) r[i] = b[i] - a[i];
  return r;
}

// 成分分解（简化版）
// 伪代码：
//   步骤1：storage 为空则返回空；按 uid 取节点，节点不存在或无 embedding 也返回空。
//   步骤2：以节点 embedding 调用 storage 的 ann_search 取 topk+1 个近邻
//          （多取 1 个用于跳过自身），返回 (近邻 uid, 余弦相似度) 列表作为成分方向。
std::vector<std::pair<std::string, double>> VectorService::decompose(const std::string& uid, int topk) {
  // 简化版：返回最邻近节点作为"成分方向"，weight=余弦相似度
  if (!storage_) return {};
  const Node* n = storage_->get_node(uid);
  if (!n || n->embedding.empty()) return {};
  return storage_->ann_search(n->embedding, topk + 1);  // +1 跳过自身（调用方可忽略）
}

}  // namespace ame
