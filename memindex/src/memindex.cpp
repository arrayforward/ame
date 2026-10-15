// =============================================================================
// 模块定位：M17 索引层实现 —— 分词、增量倒排、槽位目录、Q→A 邻接与
//           drop/rebuild 重建纪律。
//
// 本文件思路：实现 memindex.h 声明的全部接口。add_observation/add_fact 为
//           纯增量登记；search 系列先分词再合并 posting 计命中词数；
//           rebuild = drop + 两店 replay（观测 replay_all + 事实 replay_all），
//           与增量路径走同一套 add_*，天然保证两条路径结果逐比特一致。
//
// 关键算法/数据结构：
//   - tokenize：ASCII [a-z0-9_]+ 小写词；CJK 连续段滑窗二元组（单字退一元）；
//   - search：query 分词 → 逐词取 owner posting → seq 累加命中词集合 →
//     按 (命中词数 desc, seq asc) 排序取 top-k（排序全序，确定性输出）；
//   - Q→A：user 观测更新 last_user_[owner]；assistant 观测把
//     last_user_[owner] → 本 seq 写进 qa_next_[owner]（P0 简化，不判 channel）。
//
// 依赖关系：memobs/memfact（2.0 同代，均只依赖 core），不依赖 1.x 模块。
// =============================================================================
#include "ame/memindex/memindex.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ame {

// ---------------- 分词 ----------------

namespace {
inline bool is_ascii_word(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
inline bool is_cjk(unsigned char a) {
  // UTF-8 三字节 E4-E9 头：覆盖常用 CJK 统一汉字区
  return a >= 0xE4 && a <= 0xE9;
}
}  // namespace

// 伪代码：P0 内置分词（CJK 二元组 + ASCII 词，不依赖 extractor）
//   步骤1：扫描字节流，ASCII 词字符累积成词（小写），遇其它 flush；
//   步骤2：CJK UTF-8 三字节序列按字符切成连续段，段内滑窗取二元组；
//   步骤3：段长 1 退化为单字一元。
std::vector<std::string> memindex_tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::string word;
  auto flush_word = [&]() {
    if (!word.empty()) { out.push_back(word); word.clear(); }
  };
  // CJK 段缓冲（按 UTF-8 三字节字符存）
  std::vector<std::string> cjk_run;
  auto flush_cjk = [&]() {
    if (cjk_run.empty()) return;
    for (size_t i = 0; i + 1 < cjk_run.size(); ++i)
      out.push_back(cjk_run[i] + cjk_run[i + 1]);  // 二元组
    if (cjk_run.size() == 1) out.push_back(cjk_run[0]);
    cjk_run.clear();
  };
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = (unsigned char)text[i];
    if (c < 0x80) {
      flush_cjk();
      const unsigned char lc = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
      if (is_ascii_word(lc)) { word += (char)lc; ++i; continue; }
      flush_word();
      ++i;
      continue;
    }
    flush_word();
    if (i + 2 < text.size() && is_cjk(c)) {
      cjk_run.push_back(text.substr(i, 3));  // 取一个 CJK 字符（UTF-8 3 字节）
      i += 3;
    } else {
      flush_cjk();
      ++i;  // 其它字节（标点/扩展区）跳过
    }
  }
  flush_word();
  flush_cjk();
  return out;
}

// ---------------- 增量更新 ----------------

void MemIndex::post(Posting& p, const std::string& term, const std::string& owner,
                    uint64_t seq) {
  p[term][owner].push_back(seq);
}

// 伪代码：登记一条观测
//   步骤1：obs_count_[owner]++（IDF 的 N）；
//   步骤2：text 分词去重，登记 obs_terms_[seq]（P3b 扩散种子词表）；
//   步骤3：P3b 共现关联边——去重词两两计数（双向），df>50% 超高频词跳过
//          （df 按当前 posting 长度、N 含本条近似判定，防超高频词炸内存/带偏扩散）；
//   步骤4：分词逐词进 obs_posting_；
//   步骤5：kind==user 记 last_user_；kind==assistant 且 last_user_ 非空，
//          写 Q→A 邻接（last_user_ → 本 seq）。
void MemIndex::add_observation(const Observation& o) {
  ++obs_count_[o.owner];
  std::vector<std::string> uniq;
  {
    std::unordered_set<std::string> seen;
    for (const auto& t : memindex_tokenize(o.text))
      if (seen.insert(t).second) uniq.push_back(t);
  }
  obs_terms_[o.seq] = uniq;
  const size_t n = obs_count_[o.owner];
  std::vector<size_t> co_idx;  // 参与共现统计的词下标（df ≤ 50%）
  for (size_t i = 0; i < uniq.size(); ++i) {
    size_t df = 0;
    auto it = obs_posting_.find(uniq[i]);
    if (it != obs_posting_.end()) {
      auto oit = it->second.find(o.owner);
      if (oit != it->second.end()) df = oit->second.size();
    }
    if (df * 2 <= n) co_idx.push_back(i);
  }
  auto& co = cooc_[o.owner];
  for (size_t a = 0; a < co_idx.size(); ++a)
    for (size_t b = a + 1; b < co_idx.size(); ++b) {
      ++co[uniq[co_idx[a]]][uniq[co_idx[b]]];
      ++co[uniq[co_idx[b]]][uniq[co_idx[a]]];
    }
  for (const auto& t : uniq) post(obs_posting_, t, o.owner, o.seq);
  if (o.kind == "user") {
    last_user_[o.owner] = o.seq;
  } else if (o.kind == "assistant") {
    const auto it = last_user_.find(o.owner);
    if (it != last_user_.end() && it->second != 0)
      qa_next_[o.owner][it->second] = o.seq;
  }
}

// 伪代码：登记一条事实
//   步骤1：entity+attr+value 拼接分词进 fact_posting_；
//   步骤2：槽位目录 slot_dir_[entity] 追加 fact seq。
void MemIndex::add_fact(const Fact& f) {
  for (const auto& t : memindex_tokenize(f.entity + " " + f.attr + " " + f.value))
    post(fact_posting_, t, f.owner, f.seq);
  slot_dir_[f.entity].push_back(f.seq);
}

// ---------------- 召回 ----------------

// 伪代码：倒排召回公共流程
//   步骤1：query 分词去重；
//   步骤2：逐词取 owner posting，seq → 命中词集合累加；
//   步骤3：按 (命中词数 desc, seq asc) 全序排序取 top-k。
std::vector<IndexHit> MemIndex::search_posting(const Posting& p, const std::string& query,
                                               const std::string& owner, int k) {
  std::vector<std::string> terms = memindex_tokenize(query);
  std::unordered_set<std::string> seen;
  std::unordered_map<uint64_t, std::vector<std::string>> acc;
  for (const auto& t : terms) {
    if (!seen.insert(t).second) continue;  // 重复词只计一次
    auto pit = p.find(t);
    if (pit == p.end()) continue;
    auto oit = pit->second.find(owner);
    if (oit == pit->second.end()) continue;
    for (uint64_t s : oit->second) acc[s].push_back(t);
  }
  std::vector<IndexHit> hits;
  hits.reserve(acc.size());
  for (auto& kv : acc) hits.push_back(IndexHit{kv.first, std::move(kv.second)});
  std::sort(hits.begin(), hits.end(), [](const IndexHit& a, const IndexHit& b) {
    if (a.terms.size() != b.terms.size()) return a.terms.size() > b.terms.size();
    return a.seq < b.seq;
  });
  if ((int)hits.size() > k) hits.resize(k);
  return hits;
}

std::vector<IndexHit> MemIndex::search(const std::string& query, const std::string& owner,
                                       int k) const {
  return search_posting(obs_posting_, query, owner, k);
}

std::vector<IndexHit> MemIndex::search_facts(const std::string& query,
                                             const std::string& owner, int k) const {
  return search_posting(fact_posting_, query, owner, k);
}

// ---------------- 邻接 / 槽位 / IDF ----------------

uint64_t MemIndex::next_assistant_seq(const std::string& owner, uint64_t user_seq) const {
  auto it = qa_next_.find(owner);
  if (it == qa_next_.end()) return 0;
  const auto jt = it->second.find(user_seq);
  return jt == it->second.end() ? 0 : jt->second;
}

const std::vector<uint64_t>* MemIndex::slot_facts(const std::string& entity) const {
  auto it = slot_dir_.find(entity);
  return it == slot_dir_.end() ? nullptr : &it->second;
}

// ---------------- P3b 联想扩散（共现关联边） ----------------

// 伪代码：联想扩散（仿 1.x conduction 的简化版）
//   步骤1：种子 seq → 观测去重词表，种子词权 1.0（多种子同词取 1.0）；
//   步骤2：逐跳沿共现边扩散：nw = w × (共现计数/out_deg(t)) × 0.5，
//          每词每跳最多取共现计数 top 64 邻居（爆炸控制），到达权累加进 acc；
//   步骤3：到达词（剔除种子词）经观测倒排映射回 owner 内 seq（剔除种子 seq），
//          多词命中权重累加；
//   步骤4：按 (权重 desc, seq asc) 全序取 top_m。
std::vector<uint64_t> MemIndex::expand(const std::vector<uint64_t>& seed_seqs,
                                       const std::string& owner, int max_hop,
                                       int top_m) const {
  if (max_hop <= 0 || top_m <= 0) return {};
  auto cit = cooc_.find(owner);
  if (cit == cooc_.end()) return {};
  std::unordered_map<std::string, double> acc, frontier;
  std::unordered_set<std::string> seed_terms;
  std::unordered_set<uint64_t> seed_set(seed_seqs.begin(), seed_seqs.end());
  for (uint64_t s : seed_seqs) {
    auto it = obs_terms_.find(s);
    if (it == obs_terms_.end()) continue;
    for (const auto& t : it->second) {
      seed_terms.insert(t);
      acc[t] = 1.0;
      frontier[t] = 1.0;
    }
  }
  if (frontier.empty()) return {};
  for (int hop = 0; hop < max_hop; ++hop) {
    std::unordered_map<std::string, double> next;
    for (const auto& [t, w] : frontier) {
      auto ait = cit->second.find(t);
      if (ait == cit->second.end()) continue;
      const auto& adj = ait->second;
      uint64_t out_deg = 0;
      for (const auto& [u, c] : adj) out_deg += (uint64_t)c;
      if (out_deg == 0) continue;
      // 爆炸控制：邻居按共现计数取 top 64（超高频词已被共现统计的 df 门槛弱化，
      // 这里是第二道闸，防单个词的邻接表过长拖慢扩散）
      std::vector<std::pair<const std::string*, uint32_t>> nb;
      nb.reserve(adj.size());
      for (const auto& [u, c] : adj) nb.emplace_back(&u, c);
      if (nb.size() > 64) {
        std::partial_sort(nb.begin(), nb.begin() + 64, nb.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
        nb.resize(64);
      }
      for (const auto& [up, c] : nb)
        next[*up] += w * ((double)c / (double)out_deg) * 0.5;
    }
    for (auto& [u, w] : next) {
      auto it = acc.find(u);
      if (it == acc.end()) acc.emplace(u, w);
      else it->second += w;
    }
    frontier = std::move(next);
    if (frontier.empty()) break;
  }
  std::unordered_map<uint64_t, double> seq_w;
  for (const auto& [t, w] : acc) {
    if (w <= 0.0 || seed_terms.count(t)) continue;
    auto it = obs_posting_.find(t);
    if (it == obs_posting_.end()) continue;
    auto oit = it->second.find(owner);
    if (oit == it->second.end()) continue;
    for (uint64_t s : oit->second) {
      if (seed_set.count(s)) continue;
      seq_w[s] += w;
    }
  }
  std::vector<std::pair<uint64_t, double>> v(seq_w.begin(), seq_w.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  if ((int)v.size() > top_m) v.resize(top_m);
  std::vector<uint64_t> out;
  out.reserve(v.size());
  for (const auto& [s, w] : v) out.push_back(s);
  return out;
}

// 伪代码：IDF = ln(1 + N/df)，N=owner 观测文档数，df=该词在该 owner 的 df
double MemIndex::idf(const std::string& term, const std::string& owner) const {
  const double n = (double)owner_doc_count(owner);
  auto it = obs_posting_.find(term);
  if (it == obs_posting_.end()) return std::log(2.0 + n);
  auto oit = it->second.find(owner);
  const double df = oit == it->second.end() ? 0.0 : (double)oit->second.size();
  if (df <= 0) return std::log(2.0 + n);
  return std::log(1.0 + n / df);
}

// ---------------- drop / rebuild ----------------

// 索引不是事实：drop 后一切可由 replay 重建（设计 §4.3）
void MemIndex::drop() {
  obs_posting_.clear();
  fact_posting_.clear();
  slot_dir_.clear();
  last_user_.clear();
  qa_next_.clear();
  obs_count_.clear();
  cooc_.clear();       // P3b 共现关联边（可重建缓存的一部分）
  obs_terms_.clear();  // P3b 扩散种子词表
}

// 伪代码：全量重建
//   步骤1：drop 清空；
//   步骤2：观测店 replay_all 逐条 add_observation；
//   步骤3：事实店 replay_all 逐条 add_fact。
//   与增量写路径同一套 add_*，重建结果与增量结果必然逐比特一致。
void MemIndex::rebuild(const ObsStore& obs, const FactStore& facts) {
  drop();
  obs.replay_all([this](const Observation& o) { add_observation(o); });
  facts.replay_all([this](const Fact& f) { add_fact(f); });
}

}  // namespace ame
