// =============================================================================
// 模块定位：M18 视图层实现 —— evidence 合成管线：召回 → 打包 → 打分 →
//           去重 → top-k。全程只读，零落盘副作用。
//
// 本文件思路：实现 memview.h 声明的接口。synthesize 从索引取观测候选与
//           事实候选；观测候选中的 user 提问经 Q→A 邻接打包成"问题\n→ 回答"；
//           事实候选按 (entity,attr) 走 fold 取当前值（冲销留痕不进证据）；
//           P2a 兜底：词面命中 < k/2 时以 owner 最近 N 条观测补候选池
//           （tier 置 0，排序恒低于正常候选——只兜"零证据弃答"，不与正常
//           召回混排竞争）；三路加权打分后按文本去重，全序排序取 top-k。
//
// 关键算法/数据结构：
//   - 打分：score = α·Σidf(命中词) + β·reliability + γ·time_decay；
//     可靠性读网关的 ReliabilityTable（P2b 闭环，默认表与 P0 硬编码逐比特一致）：
//     观测按 kind 取，事实按 reality 名取（declared/observed/derived）；
//   - 排序全序：(tier desc, score desc, seq asc)：fallback 候选 tier=0，
//     正常候选 tier=1——未启用兜底时所有候选同 tier，比较退化为 P0 的
//     (score desc, seq asc)，正常路径行为逐比特不变；
//   - 事实时间分用该 fact reality 的半衰期；观测时间分按 Observed 默认
//     半衰期（言语行为按观测对待）；
//   - 去重：截断前按文本精确去重（保序，先高分者留）；兜底候选另按 seq
//     跳过已在词面命中集中的观测，与 Q→A 打包文本的去重交给文本级去重。
//
// 依赖关系：memobs/memfact/memindex（2.0 同代），不依赖 1.x 模块。
// =============================================================================
#include "ame/memview/memview.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace ame {

// ---------------- ReliabilityTable（P2b 来源可靠性闭环） ----------------

ReliabilityTable::ReliabilityTable() {
  // 默认表同 P0 内置值（memfact kind_reliability / reality_reliability）：
  // 未 feedback 过时视图打分与 P0 逐比特一致
  t_["user"] = 1.0;
  t_["assistant"] = 0.7;
  t_["vision"] = 0.9;
  t_["declared"] = 1.0;
  t_["observed"] = 1.0;
  t_["derived"] = 0.5;
}

double ReliabilityTable::get(const std::string& kind) const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = t_.find(kind);
  return it == t_.end() ? 0.5 : it->second;
}

bool ReliabilityTable::has(const std::string& kind) const {
  std::lock_guard<std::mutex> lk(mu_);
  return t_.count(kind) != 0;
}

// 伪代码：反馈调整（罚 > 奖，防刷）
//   步骤1：正 +0.02 封顶 1.0；负 −0.05 封底 0.05；
//   步骤2：返回调整后的当前值（供调用方日志/断言）。
double ReliabilityTable::adjust(const std::string& kind, bool positive) {
  std::lock_guard<std::mutex> lk(mu_);
  double v = t_.count(kind) ? t_[kind] : 0.5;
  v += positive ? 0.02 : -0.05;
  if (v > 1.0) v = 1.0;
  if (v < 0.05) v = 0.05;
  t_[kind] = v;
  return v;
}

Json ReliabilityTable::to_json() const {
  std::lock_guard<std::mutex> lk(mu_);
  Json j = Json::object();
  for (const auto& [k, v] : t_) j[k] = Json::number(v);
  return j;
}

// load 在默认表上合并：文件里的 key 覆盖默认，缺 key 保持默认（P0 兼容）
void ReliabilityTable::load_json(const Json& j) {
  if (j.kind != Json::Obj) return;
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& [k, v] : j.obj)
    if (v.kind == Json::Num) t_[k] = v.as_num();
}

// ---------------- View ----------------

View::View(const ObsStore& obs, const FactStore& facts, const MemIndex& index,
           const ReliabilityTable& reliab)
    : obs_(obs), facts_(facts), index_(index), reliab_(reliab) {}

void View::set_weights(double alpha, double beta, double gamma) {
  alpha_ = alpha;
  beta_ = beta;
  gamma_ = gamma;
}

// 伪代码：evidence 合成（设计 §4.4 + P2a 兜底 + P3b 邻近加成/联想扩散）
//   步骤1：索引召回观测候选（pool = max(k×4, 16)）与事实候选；
//   步骤2：观测候选逐项打分；user 提问查 Q→A 邻接打包 "问题\n→ 回答"；
//   步骤2b：P3b seq 邻近加成——词面命中观测的 seq±window 邻居（同 owner）进池，
//           词面分按 1/d 衰减，tier 同正常层（不抢兜底地板）；
//   步骤2c：P3b 共现联想扩散——词面种子 → index.expand 扩候选（只贡献候选
//           不直接给分：lex=0，走可靠性/时间分），tier 同正常层；
//   步骤2d：词面命中 < k/2 → 兜底候选池（owner 最近 N 条，user 优先 + ts 新鲜）
//           以 tier=0 补入（排序恒低于正常候选，只兜零证据弃答）；
//           去重口径 = 全部正常层观测候选 seq（lex+prox+expand），
//           已进池的观测不再以兜底身份重复出现；
//   步骤3：事实候选按 (entity,attr) 分组，逐组 fold latest（尊重冲销），
//          命中格式化为 "entity.attr=value"，以 fold 的 effective 打时间分；
//   步骤4：全部证据按 (tier desc, score desc, seq asc) 排序；
//   步骤5：文本级去重（保序），截断 top-k。
std::vector<EvidenceItem> View::synthesize(const std::string& query,
                                           const std::string& owner, int k,
                                           int64_t now) const {
  struct Cand {
    EvidenceItem item;
    uint64_t seq = 0;
    int tier = 1;  // 1=正常召回（lex/prox/expand/fact）；0=P2a 兜底（恒排正常候选之后）
  };
  std::vector<Cand> cands;

  // ①+② 观测证据与 Q→A 打包
  const int pool = std::max(k * 4, 16);
  const std::vector<IndexHit> ohits = index_.search(query, owner, pool);
  std::unordered_set<uint64_t> obs_cand_seqs;  // 正常层观测候选 seq（lex/prox/expand），兜底去重用
  for (const IndexHit& h : ohits) {
    const Observation* o = obs_.get(h.seq);
    if (!o) continue;
    obs_cand_seqs.insert(o->seq);
    double lex = 0;
    for (const auto& t : h.terms) lex += index_.idf(t, owner);
    const double decay = FactStore::time_decay(
        facts_.half_life_days(Reality::Observed), o->ts, now);
    Cand c;
    c.item.ts = o->ts;
    c.item.source_kind = o->kind;
    c.item.via = "lex";
    c.item.ref_obs = o->seq;  // 证据可回溯到观测（meta 如 dia_id 经 Mem::obs 取）
    c.item.score = alpha_ * lex + beta_ * reliab_.get(o->kind) + gamma_ * decay;
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

  // ②b P3b seq 邻近加成：词面命中观测的 seq±d 邻居（同 owner）作为 proximity 候选。
  // 词面分 = 命中观测词面分 × 1/d（seq 距离衰减）；tier 同正常层，不抢兜底地板。
  // 一邻居多命中时先处理者先得（ohits 序确定 ⇒ 输出确定）。
  if (proximity_window_ > 0) {
    for (const IndexHit& h : ohits) {
      double base_lex = 0;
      for (const auto& t : h.terms) base_lex += index_.idf(t, owner);
      for (int d = 1; d <= proximity_window_; ++d) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
          const int64_t ns = (int64_t)h.seq + sgn * d;
          if (ns < 1) continue;
          const uint64_t nseq = (uint64_t)ns;
          if (!obs_cand_seqs.insert(nseq).second) continue;
          const Observation* po = obs_.get(nseq);
          if (!po || po->owner != owner) continue;
          const double decay = FactStore::time_decay(
              facts_.half_life_days(Reality::Observed), po->ts, now);
          Cand c;
          c.seq = po->seq;
          c.item.ts = po->ts;
          c.item.source_kind = po->kind;
          c.item.via = "prox";
          c.item.ref_obs = po->seq;
          c.item.score = alpha_ * base_lex / (double)d +
                         beta_ * reliab_.get(po->kind) + gamma_ * decay;
          if (po->kind == "user") {
            const uint64_t aseq = index_.next_assistant_seq(owner, po->seq);
            const Observation* a = aseq ? obs_.get(aseq) : nullptr;
            c.item.text = a ? po->text + "\n→ " + a->text : po->text;
          } else {
            c.item.text = po->text;
          }
          cands.push_back(std::move(c));
        }
      }
    }
  }

  // ②c P3b 共现联想扩散：词面种子 → expand 扩候选 → 统一进打分管线。
  // 扩散只贡献候选不直接给分：lex=0，按可靠性/时间分参与同层排序。
  if (expand_enabled_ && !ohits.empty()) {
    std::vector<uint64_t> seeds;
    seeds.reserve(ohits.size());
    for (const IndexHit& h : ohits) seeds.push_back(h.seq);
    for (uint64_t s : index_.expand(seeds, owner, 2, 30)) {
      if (!obs_cand_seqs.insert(s).second) continue;
      const Observation* o = obs_.get(s);
      if (!o || o->owner != owner) continue;
      const double decay = FactStore::time_decay(
          facts_.half_life_days(Reality::Observed), o->ts, now);
      Cand c;
      c.seq = o->seq;
      c.item.ts = o->ts;
      c.item.source_kind = o->kind;
      c.item.via = "expand";
      c.item.ref_obs = o->seq;
      c.item.score = beta_ * reliab_.get(o->kind) + gamma_ * decay;
      if (o->kind == "user") {
        const uint64_t aseq = index_.next_assistant_seq(owner, o->seq);
        const Observation* a = aseq ? obs_.get(aseq) : nullptr;
        c.item.text = a ? o->text + "\n→ " + a->text : o->text;
      } else {
        c.item.text = o->text;
      }
      cands.push_back(std::move(c));
    }
  }

  // ②d P2a 兜底：词面命中 < k/2 → owner 最近 N 条观测补候选池。
  // 触发含"零命中"（零证据弃答的对口修复）；0 < 命中 < k/2 时仅作尾部补足——
  // tier=0 保证兜底候选排序恒低于全部正常候选，不与正常召回混排竞争。
  if (fallback_pool_ > 0 && (int)ohits.size() < k / 2) {
    std::deque<const Observation*> recent;
    obs_.for_owner(owner, [&](const Observation& o) {
      recent.push_back(&o);
      if ((int)recent.size() > fallback_pool_) recent.pop_front();
    });
    std::vector<const Observation*> fpool(recent.begin(), recent.end());
    // 排序取 top-k：speaker=user 优先，同优先级按 ts 新鲜（新者前），全序 seq 决胜
    std::stable_sort(fpool.begin(), fpool.end(),
                     [](const Observation* a, const Observation* b) {
                       const bool ua = a->kind == "user", ub = b->kind == "user";
                       if (ua != ub) return ua > ub;
                       if (a->ts != b->ts) return a->ts > b->ts;
                       return a->seq > b->seq;
                     });
    int taken = 0;
    for (const Observation* o : fpool) {
      if (taken >= k) break;
      if (obs_cand_seqs.count(o->seq)) continue;  // 正常层（lex/prox/expand）已进池的不重复进池
      const double decay = FactStore::time_decay(
          facts_.half_life_days(Reality::Observed), o->ts, now);
      Cand c;
      c.tier = 0;
      c.seq = o->seq;
      c.item.ts = o->ts;
      c.item.source_kind = o->kind;
      c.item.via = "fallback";
      c.item.ref_obs = o->seq;
      // 词面分按命中计、兜底候选可为 0；可靠性/时间分照常
      c.item.score = beta_ * reliab_.get(o->kind) + gamma_ * decay;
      if (o->kind == "user") {
        const uint64_t aseq = index_.next_assistant_seq(owner, o->seq);
        const Observation* a = aseq ? obs_.get(aseq) : nullptr;
        c.item.text = a ? o->text + "\n→ " + a->text : o->text;
      } else {
        c.item.text = o->text;
      }
      cands.push_back(std::move(c));
      ++taken;
    }
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
    c.item.via = "fact";
    c.item.ref_obs = fd.fact.source_obs;  // 事实证据回溯其来源观测（0=直写无源）
    c.item.score = alpha_ * lex + beta_ * reliab_.get(reality_name(fd.fact.reality)) +
                   gamma_ * decay;
    c.item.text = fd.fact.entity + "." + fd.fact.attr + "=" + fd.fact.value;
    cands.push_back(std::move(c));
  }

  // ④ 全序排序（tier desc, score desc, seq asc）；无兜底候选时退化为 P0 排序键
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.tier != b.tier) return a.tier > b.tier;
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
