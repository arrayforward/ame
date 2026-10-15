// =============================================================================
// 模块定位：M17 索引层 —— 抽象层可重建缓存：词倒排、槽位目录、Q→A 邻接。
//           索引不是事实，删了不丢信息（drop + replay 必须能完全重建）。
//
// 本文件思路：对外声明 MemIndex 与命中结构。索引从 Observation/Fact 增量建：
//           观测/事实 append 时网关调 add_* 增量更新；drop() 清空 +
//           rebuild(ObsStore, FactStore) 从两店 replay 全量重建。
//           P0 为内存版（设计允许带校验的持久化缓存，本期不做）。
//
// 关键算法/数据结构：
//   - 分词 memindex_tokenize：内置 CJK 字符二元组 + ASCII 词（小写化），
//     不依赖 extractor（P0 简化，1.x M1 停用）；
//   - 倒排：term -> owner -> seq 列表（观测与事实两套 posting）；
//   - 槽位目录：fact.entity（"owner:名"）-> fact seqs；
//   - Q→A 邻接：owner 内 user 观测 seq -> 下一条 assistant 观测 seq，
//     依赖 seq 全局单调 + 增序到达（append 与 replay 均满足）。
//
// 依赖关系：依赖 2.0 同代模块 memobs/memfact（二者仅依赖 core），
//           不依赖 storage/extractor 等 1.x 模块；被 M18 view、M19 mem 依赖。
// =============================================================================
#pragma once
#include "ame/core/json.h"
#include "ame/core/types.h"
#include "ame/memfact/memfact.h"
#include "ame/memobs/memobs.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ame {

// P0 内置分词：CJK 连续段取二元组（单字取一元），ASCII 取 [a-z0-9_]+ 小写词
std::vector<std::string> memindex_tokenize(const std::string& text);

// 一次召回命中：候选 seq + 命中词列表（供视图层 IDF 加权打分）
struct IndexHit {
  uint64_t seq = 0;
  std::vector<std::string> terms;
};

class MemIndex {
 public:
  MemIndex() = default;

  // 增量更新（写路径：append 观测/事实后由网关调用）
  void add_observation(const Observation& o);
  void add_fact(const Fact& f);

  // 召回：词倒排查 owner 作用域候选，按命中词数 desc、seq asc 取 top-k
  std::vector<IndexHit> search(const std::string& query, const std::string& owner,
                               int k) const;
  // 事实召回：entity/attr/value 文本倒排（视图层事实命中用）
  std::vector<IndexHit> search_facts(const std::string& query, const std::string& owner,
                                     int k) const;

  // Q→A 邻接：owner 内该 user 观测的下一条 assistant 观测 seq（0 = 无）
  uint64_t next_assistant_seq(const std::string& owner, uint64_t user_seq) const;
  // 槽位目录：fact.entity -> fact seqs（nullptr = 槽位不存在）
  const std::vector<uint64_t>* slot_facts(const std::string& entity) const;
  // P3b 联想扩散：词面种子 seq → 共现关联边逐跳扩散（出度归一 ×0.5/跳），
  // 返回候选观测 seq（top_m 上限，权重 desc/seq asc 全序）；只贡献候选不直接给分。
  // 无共现边/无种子返回空；max_hop<=0 或 top_m<=0 返回空。
  std::vector<uint64_t> expand(const std::vector<uint64_t>& seed_seqs,
                               const std::string& owner, int max_hop = 2,
                               int top_m = 30) const;
  // IDF：ln(1 + N_docs/df)，owner 作用域（观测语料；df=0 时保守给 ln(N+1)+1）
  double idf(const std::string& term, const std::string& owner) const;
  // owner 观测文档数（IDF 的 N）
  size_t owner_doc_count(const std::string& owner) const { return obs_count_.count(owner) ? obs_count_.at(owner) : 0; }

  // 可重建纪律：drop 清空全部缓存；rebuild 从两店 replay 全量重建
  void drop();
  void rebuild(const ObsStore& obs, const FactStore& facts);
  size_t term_count() const { return obs_posting_.size(); }

 private:
  // 倒排 posting：term -> owner -> seq 列表
  using Posting = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<uint64_t>>>;
  // 伪代码：登记一条 posting
  //   步骤1：取 term/owner 对应的 seq 列表；
  //   步骤2：尾部追加（到达序即升序，不查重——seq 单调）。
  static void post(Posting& p, const std::string& term, const std::string& owner,
                   uint64_t seq);
  // 倒排召回公共流程：query 分词 → owner posting 合并 → 命中词数排序取 top-k
  static std::vector<IndexHit> search_posting(const Posting& p, const std::string& query,
                                              const std::string& owner, int k);

  Posting obs_posting_;   // 观测词倒排
  Posting fact_posting_;  // 事实词倒排（entity+attr+value 分词）
  std::unordered_map<std::string, std::vector<uint64_t>> slot_dir_;    // entity -> fact seqs
  std::unordered_map<std::string, uint64_t> last_user_;                // owner -> 最近 user seq
  std::unordered_map<std::string, std::unordered_map<uint64_t, uint64_t>> qa_next_;  // owner -> {user seq -> assistant seq}
  std::unordered_map<std::string, size_t> obs_count_;                  // owner -> 观测文档数
  // P3b 共现关联边：owner -> term -> 邻接 term -> 共现计数（观测内去重后两两，
  // 双向登记；df>50% 超高频词跳过，写入时按当前 owner 文档数近似判定）
  using Cooc = std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>>;
  std::unordered_map<std::string, Cooc> cooc_;
  // P3b 观测去重词表（seq -> tokens）：联想扩散的种子词来源
  std::unordered_map<uint64_t, std::vector<std::string>> obs_terms_;
};

}  // namespace ame
