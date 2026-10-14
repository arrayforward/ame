// =============================================================================
// 模块定位：M18 视图层实现 —— evidence 合成管线：召回 → 打包 → 打分 →
//           去重 → top-k。全程只读，零落盘副作用。
//
// 本文件思路：实现 memview.h 声明的接口。synthesize 从索引取观测候选与
//           事实候选；观测候选中的 user 提问经 Q→A 邻接打包成"问题\n→ 回答"；
//           事实候选按 (entity,attr) 走 fold 取当前值（冲销留痕不进证据）；
//           三路加权打分后按文本去重，全序排序取 top-k。
//
// 关键算法/数据结构：
//   - 打分：score = α·Σidf(命中词) + β·reliability + γ·time_decay；
//     观测可靠性用 kind 表（user 1.0/assistant 0.7/vision 0.9），事实用
//     reality 级（declared 1.0/observed 1.0/derived 0.5），与 fold 口径一致；
//   - 事实时间分用该 fact reality 的半衰期；观测时间分按 Observed 默认
//     半衰期（言语行为按观测对待）；
//   - 排序全序：(score desc, seq asc)，两次调用输出逐字段一致。
//
// 依赖关系：memobs/memfact/memindex（2.0 同代），不依赖 1.x 模块。
// =============================================================================
#include "ame/memview/memview.h"

#include <algorithm>
#include <unordered_set>

namespace ame {

View::View(const ObsStore& obs, const FactStore& facts, const MemIndex& index)
    : obs_(obs), facts_(facts), index_(index) {}

void View::set_weights(double alpha, double beta, double gamma) {
  alpha_ = alpha;
  beta_ = beta;
  gamma_ = gamma;
}

// 伪代码：evidence 合成（设计 §4.4）
//   步骤1：索引召回观测候选（pool = max(k×4, 16)）与事实候选；
//   步骤2：观测候选逐项打分；user 提问查 Q→A 邻接打包 "问题\n→ 回答"；
//   步骤3：事实候选按 (entity,attr) 分组，逐组 fold latest（尊重冲销），
//          命中格式化为 "entity.attr=value"，以 fold 的 effective 打时间分；
//   步骤4：全部证据按 (score desc, seq asc) 排序；
//   步骤5：文本级去重（保序），截断 top-k。
std::vector<EvidenceItem> View::synthesize(const std::string& query,
                                           const std::string& owner, int k,
                                           int64_t now) const {
  struct Cand {
    EvidenceItem item;
    uint64_t seq = 0;
  };
  std::vector<Cand> cands;

  // ①+② 观测证据与 Q→A 打包
  const int pool = std::max(k * 4, 16);
  for (const IndexHit& h : index_.search(query, owner, pool)) {
    const Observation* o = obs_.get(h.seq);
    if (!o) continue;
    double lex = 0;
    for (const auto& t : h.terms) lex += index_.idf(t, owner);
    const double decay = FactStore::time_decay(
        facts_.half_life_days(Reality::Observed), o->ts, now);
    Cand c;
    c.item.ts = o->ts;
    c.item.source_kind = o->kind;
    c.item.score = alpha_ * lex + beta_ * kind_reliability(o->kind) + gamma_ * decay;
    c.seq = o->seq;
    if (o->kind == "user") {
      // 打包：提问自动附 owner 内其下一条 assistant 观测（视图时打包，零记忆污染）
      const uint64_t aseq = index_.next_assistant_seq(owner, o->seq);
      const Observation* a = aseq ? obs_.get(aseq) : nullptr;
      if (a) c.item.text = o->text + "\n→ " + a->text;
      else c.item.text = o->text;
    } else {
      c.item.text = o->text;
    }
    cands.push_back(std::move(c));
  }

  // ③ 事实证据（fold 当前值：冲销留痕不进证据）
  const std::vector<IndexHit> fhits = index_.search_facts(query, owner, pool);
  std::unordered_set<std::string> folded_keys;  // "entity|attr" 已 fold 去重
  for (const IndexHit& h : fhits) {
    const Fact* f = facts_.get(h.seq);
    if (!f) continue;
    const std::string key = f->entity + "|" + f->attr;
    if (!folded_keys.insert(key).second) continue;
    Folded fd;
    if (!facts_.latest(owner, f->entity, f->attr, now, fd)) continue;  // 全被冲销
    double lex = 0;
    for (const auto& t : h.terms) lex += index_.idf(t, owner);
    const double decay = FactStore::time_decay(
        facts_.half_life_days(fd.fact.reality), fd.fact.ts, now);
    Cand c;
    c.seq = fd.fact.seq;
    c.item.ts = fd.fact.ts;
    c.item.source_kind = reality_name(fd.fact.reality);
    c.item.score = alpha_ * lex + beta_ * reality_reliability(fd.fact.reality) +
                   gamma_ * decay;
    c.item.text = fd.fact.entity + "." + fd.fact.attr + "=" + fd.fact.value;
    cands.push_back(std::move(c));
  }

  // ④ 全序排序（score desc, seq asc）
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.item.score != b.item.score) return a.item.score > b.item.score;
    return a.seq < b.seq;
  });

  // ⑤ 文本级去重（保序，先高分者留），再截断 top-k
  std::vector<EvidenceItem> out;
  std::unordered_set<std::string> seen_text;
  for (const Cand& c : cands) {
    if (!seen_text.insert(c.item.text).second) continue;
    out.push_back(c.item);
    if ((int)out.size() >= k) break;
  }
  return out;
}

}  // namespace ame
