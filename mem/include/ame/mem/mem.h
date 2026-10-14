// =============================================================================
// 模块定位：M19 外观网关 —— AME 2.0 对 App 的唯一入口（owner 作用域 API）。
//           聚合 M15 观测层 / M16 事实层 / M17 索引层 / M18 视图层，
//           1.x 引擎（M12）经外观层切换（设计 §3、§10 P1）。
//
// 本文件思路：对外声明 Mem 网关。写路径：remember 入观测层并增量更新索引；
//           declare 写 Declared facts（权威，confidence 恒 1.0）；
//           rememberFact 直写 Observed fact（P0 无自动提取，§5 写路径的
//           M1 提取器与 G1–G3 校验门为 P2 插件）；读路径 recall/profile
//           委托视图层与 fold，零写副作用；erase 两级语义走观测+事实两店
//           并重建索引（索引是可重建缓存，erase 后无残留）。
//
// 关键算法/数据结构：
//   - 持久化根目录可配：默认空 = 内存态（测试用临时目录）；
//   - eraseOwner：public 域拒绝抹除（§8 公共域不可被个人 erase）；
//   - eraseChannel：观测店剔除会话 → 事实店按 source_obs 级联 → 索引重建。
//
// 依赖关系：依赖 2.0 同代四模块 memobs/memfact/memindex/memview
//           （均不依赖 1.x 模块）；不反向依赖任何 1.x 模块。
// =============================================================================
#pragma once
#include "ame/core/json.h"
#include "ame/core/types.h"
#include "ame/memfact/memfact.h"
#include "ame/memindex/memindex.h"
#include "ame/memobs/memobs.h"
#include "ame/memview/memview.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ame {

// AME 2.0 外观网关：owner 作用域的记忆动词（declare/allocate 经由写路径，
// maintain 经由 fold，reclaim 为 P2 重认领扩展口）
class Mem {
 public:
  // root 为空 = 内存态；否则 root/shards/{obs,fact}/ 持久化
  explicit Mem(std::string root = "");

  // ---- 写路径 ----
  // remember：入观测层（append-only 立即落盘）并增量更新索引；
  // kind ∈ user|assistant|vision|declare|note
  Result<Observation> remember(const std::string& owner, const std::string& channel,
                               const std::string& kind, const std::string& text,
                               int64_t ts, Json meta = Json());
  // declare：显式声明属性（权威，confidence 恒 1.0，reality=declared）；
  // entity 为槽位引用（"owner:self" / "owner:名" / "public:名"）
  Err declare(const std::string& owner, const std::string& entity,
              const std::map<std::string, std::string>& attrs, int64_t ts);
  // rememberFact：Observed fact 直写（P0 无自动提取）；source_obs 溯源可传 0
  Result<Fact> rememberFact(const std::string& owner, const std::string& entity,
                            const std::string& attr, const std::string& value,
                            int64_t ts, double confidence = 1.0,
                            uint64_t source_obs = 0);

  // ---- 读路径 ----
  // recall：视图层合成证据（永不落盘，零存储副作用）
  std::vector<EvidenceItem> recall(const std::string& query, const std::string& owner,
                                   int k, int64_t now);
  // 证据回溯：按 seq 取观测（EvidenceItem.ref_obs → meta 如 dia_id）；未找到返回 nullptr
  const Observation* obs(uint64_t seq) const { return obs_.get(seq); }
  // profile：fold 物化最新视图（Declared+latest facts 统一走三序裁决），
  // 返回 {entity: {attr: {value, reality, base, effective, ts, seq}}}
  Json profile(const std::string& owner, int64_t now);

  // ---- erase（抹除语义，§8 隐私一等公民）----
  // eraseOwner：删两店分片 + 写 tombstone + 重建索引；public 域拒绝
  bool eraseOwner(const std::string& owner);
  // eraseChannel：删该会话观测并级联 source_obs 指向它们的事实；返回剔除的观测数
  size_t eraseChannel(const std::string& owner, const std::string& channel);

  // 索引重建（drop + replay 观测+事实）；正常无需调用，erase 后自动触发
  void rebuild_index();

  // 子模块访问（测试与进阶视图用）
  ObsStore& obs_store() { return obs_; }
  FactStore& fact_store() { return facts_; }
  MemIndex& index() { return index_; }
  View& view() { return view_; }
  const ObsStore& obs_store() const { return obs_; }
  const FactStore& fact_store() const { return facts_; }
  const MemIndex& index() const { return index_; }
  const View& view() const { return view_; }

 private:
  ObsStore obs_;
  FactStore facts_;
  MemIndex index_;
  View view_;
};

}  // namespace ame
