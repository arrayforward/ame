// =============================================================================
// 模块定位：M19 外观网关实现 —— owner 作用域记忆动词的分发与 erase 级联。
//
// 本文件思路：实现 mem.h 声明的接口。写路径一律先落存储店再增量更新索引
//           （索引挂了可由 rebuild_index 整体重建，不影响事实源）；
//           读路径纯委托；eraseOwner 先拦截 public 域，再两店抹除 + 索引重建；
//           eraseChannel 观测剔除 → 事实 source_obs 级联 → 索引重建。
//
// 关键算法/数据结构：
//   - declare 批量：attrs 逐键一条 Declared fact（confidence 恒 1.0）；
//   - profile：FactStore::fold_all 遍历全槽位，JSON 装配按 entity→attr 嵌套；
//   - erase 级联的安全性来自"事实与解释分离"：索引/画像全是可重建缓存，
//     删事实即顺带清理解释（§8）。
//
// 依赖关系：memobs/memfact/memindex/memview（2.0 同代），不依赖 1.x 模块。
// =============================================================================
#include "ame/mem/mem.h"

namespace ame {

// 伪代码：构造网关
//   步骤1：按 root 构造观测店与事实店（空 root = 内存态）；
//   步骤2：视图层注入三店 const 引用；
//   步骤3：从两店 replay 建索引（持久化目录下恢复到最新状态）。
Mem::Mem(std::string root) : obs_(root), facts_(root), view_(obs_, facts_, index_) {
  rebuild_index();
}

// ---- 写路径 ----

Result<Observation> Mem::remember(const std::string& owner, const std::string& channel,
                                  const std::string& kind, const std::string& text,
                                  int64_t ts, Json meta) {
  if (owner.empty()) return Result<Observation>::fail(Err::InvalidArg, "owner 为空");
  Observation o;
  o.ts = ts;
  o.owner = owner;
  o.channel = channel;
  o.kind = kind;
  o.text = text;
  o.meta = std::move(meta);
  Observation stored = obs_.append(std::move(o));
  index_.add_observation(stored);  // 索引增量更新（可重建缓存，挂了可 rebuild）
  return Result<Observation>::success(std::move(stored));
}

Err Mem::declare(const std::string& owner, const std::string& entity,
                 const std::map<std::string, std::string>& attrs, int64_t ts) {
  if (owner.empty() || entity.empty()) return Err::InvalidArg;
  for (const auto& kv : attrs) {
    Fact f;
    f.ts = ts;
    f.owner = owner;
    f.entity = entity;
    f.attr = kv.first;
    f.value = kv.second;
    f.reality = Reality::Declared;  // declare 权威，不衰减，可被新声明撤销
    f.confidence = 1.0;
    const Fact stored = facts_.append(std::move(f));
    index_.add_fact(stored);
  }
  return Err::Ok;
}

Result<Fact> Mem::rememberFact(const std::string& owner, const std::string& entity,
                               const std::string& attr, const std::string& value,
                               int64_t ts, double confidence, uint64_t source_obs) {
  if (owner.empty() || entity.empty() || attr.empty())
    return Result<Fact>::fail(Err::InvalidArg, "owner/entity/attr 为空");
  Fact f;
  f.ts = ts;
  f.owner = owner;
  f.entity = entity;
  f.attr = attr;
  f.value = value;
  f.reality = Reality::Observed;
  f.confidence = confidence;
  f.source_obs = source_obs;
  const Fact stored = facts_.append(std::move(f));
  index_.add_fact(stored);
  return Result<Fact>::success(stored);
}

// ---- 读路径 ----

std::vector<EvidenceItem> Mem::recall(const std::string& query, const std::string& owner,
                                      int k, int64_t now) {
  return view_.synthesize(query, owner, k, now);  // 视图层纯只读，零副作用
}

// 伪代码：profile —— fold 物化画像
//   步骤1：fold_all 遍历 owner 全部槽位的 latest；
//   步骤2：JSON 装配 entity → attr → {value, reality, base, effective, ts, seq}。
Json Mem::profile(const std::string& owner, int64_t now) {
  Json root = Json::object();
  facts_.fold_all(owner, now, [&](const Folded& fd) {
    Json& ent = root[fd.fact.entity];  // 不存在则创建 Null，set 后转 Obj
    if (ent.kind != Json::Obj) ent = Json::object();
    Json slot = Json::object();
    slot.set("value", Json::string(fd.fact.value));
    slot.set("reality", Json::string(reality_name(fd.fact.reality)));
    slot.set("base", Json::number(fd.base));
    slot.set("effective", Json::number(fd.effective));
    slot.set("ts", Json::number((double)fd.fact.ts));
    slot.set("seq", Json::number((double)fd.fact.seq));
    ent.set(fd.fact.attr, std::move(slot));
  });
  return root;
}

// ---- erase ----

// 伪代码：eraseOwner（抹除语义）
//   步骤1：public 域拦截（§8 公共域不可被个人 erase）；
//   步骤2：观测店删分片 + 写 tombstone；事实店删分片；
//   步骤3：索引重建（级联清理解释：索引条目全来自两店 replay，删事实即清解释）。
bool Mem::eraseOwner(const std::string& owner) {
  if (owner.empty() || owner == "public") return false;
  const bool r1 = obs_.eraseOwner(owner);
  const bool r2 = facts_.eraseOwner(owner);
  rebuild_index();
  return r1 || r2;
}

// 伪代码：eraseChannel（删除这个会话）
//   步骤1：观测店剔除该 channel 观测，拿被剔除 seq 列表；
//   步骤2：事实店按 source_obs 级联剔除（含冲销红字）；
//   步骤3：索引重建，owner 其他 channel 的数据不受影响。
size_t Mem::eraseChannel(const std::string& owner, const std::string& channel) {
  const std::vector<uint64_t> removed = obs_.eraseChannel(owner, channel);
  facts_.erase_sources(owner, removed);
  if (!removed.empty()) rebuild_index();
  return removed.size();
}

void Mem::rebuild_index() {
  index_.rebuild(obs_, facts_);
}

}  // namespace ame
