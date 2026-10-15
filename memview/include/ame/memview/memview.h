// =============================================================================
// 模块定位：M18 视图层 —— 抽象层"看"：查询时把证据取证 → Q→A 打包 →
//           三路打分 → 文本级去重 → top-k 装配成 EvidenceItem。
//           永不落盘：synthesize 只读任何存储状态（设计 §4.4 硬纪律）。
//
// 本文件思路：对外声明 EvidenceItem 与 View。View 持有三店引用（观测/事实/
//           索引）与来源可靠性表只读引用，synthesize 全程只调用其 const 接口，
//           零写路径触发；打分 α·词面(IDF 加权命中词重合) + β·来源可靠性
//           （P2b 起读网关维护的 ReliabilityTable）+ γ·半衰期时间分，
//           排序键全序（tier, score desc, seq asc），输出确定性。
//
// 关键算法/数据结构：
//   - 打包：命中 user 观测自动附其下一条 assistant 观测（索引 Q→A 邻接），
//     文本拼为 "问题\n→ 回答"；fact 命中走 fold 当前值，格式 entity.attr=value；
//   - 三路打分：词面 = Σ idf(命中词)；可靠性 = kind_reliability / reality 级；
//     时间分 = FactStore::time_decay（观测按 Observed 半衰期）；
//   - 去重：截断前按文本精确去重（保序，先高分者留）。
//
// 依赖关系：依赖 2.0 同代模块 memobs/memfact/memindex（均不依赖 1.x）；
//           被 M19 mem 外观层依赖。
// =============================================================================
#pragma once
#include "ame/core/json.h"
#include "ame/core/types.h"
#include "ame/memfact/memfact.h"
#include "ame/memindex/memindex.h"
#include "ame/memobs/memobs.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ame {

// 来源可靠性表（设计 §6 闭环）：kind -> reliability ∈ [0.05, 1.0]。
// 由 M19 网关持有并持久化（root/shards/reliability.json），视图层查询时只读；
// 默认值同 P0 内置表（user 1.0 / assistant 0.7 / vision 0.9 / derived 0.5，
// declared 1.0 / observed 1.0，未命中 kind 保守 0.5），feedback 未落过表时
// 打分行为与 P0 硬编码表逐比特一致。线程安全：读写均加锁（JNI 单 io 线程
// 之外的多线程调用兜底）。
class ReliabilityTable {
 public:
  ReliabilityTable();

  // 查询（未命中返回 0.5 默认）
  double get(const std::string& kind) const;
  // 反馈调整：正 +0.02（封顶 1.0）/ 负 −0.05（封底 0.05），返回调整后的值
  double adjust(const std::string& kind, bool positive);
  bool has(const std::string& kind) const;

  // 持久化：扁平 JSON {kind: value}；load 在默认值上合并（缺 key 不覆盖默认）
  Json to_json() const;
  void load_json(const Json& j);

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, double> t_;
};

// 证据条目：装配给上层的召回结果（视图抽象层，不落盘）
struct EvidenceItem {
  std::string text;        // 原文 / "问题\n→ 回答" 打包 / "entity.attr=value"
  int64_t ts = 0;          // 证据时刻
  double score = 0;        // 三路加权总分
  std::string source_kind; // user|assistant|vision|declared|observed|derived
  std::string via;         // 来源通道：lex（词面倒排）| fallback（P2a 兜底）| fact（事实命中）
                           // | prox（P3b seq 邻近加成）| expand（P3b 共现联想扩散）
  uint64_t ref_obs = 0;    // 来源观测 seq（观测证据=其 seq；事实证据=source_obs，可无）；
                           // 上层经 Mem::obs(seq) 回溯 meta（如 dia_id），0=无法回溯
};

class View {
 public:
  // 三店引用由外部（M19 网关）注入，View 不拥有、不修改；
  // reliab 为网关持有的来源可靠性表（P2b 替代 P0 硬编码表）
  View(const ObsStore& obs, const FactStore& facts, const MemIndex& index,
       const ReliabilityTable& reliab);

  // 打分权重（α 词面 / β 来源可靠性 / γ 半衰期时间分），可调
  void set_weights(double alpha, double beta, double gamma);

  // P2a 兜底候选池大小（owner 最近 N 条观测，默认 20）；0 = 禁用兜底
  void set_fallback_pool(int n) { fallback_pool_ = n; }

  // P3b seq 邻近加成窗口：词面命中观测的 seq±window 邻居（同 owner）作为
  // proximity 候选，词面分按与命中观测的 seq 距离 1/d 衰减；tier 同正常层
  // （不抢兜底地板）。0 = 禁用（默认 2）
  void set_proximity_window(int w) { proximity_window_ = w; }
  // P3b 共现联想扩散开关（默认开）：词面种子命中 → MemIndex::expand 扩候选 →
  // 统一进打分管线；扩散候选只贡献候选不直接给分（lex=0，走可靠性/时间分）
  void set_expand_enabled(bool on) { expand_enabled_ = on; }

  // 查询时合成（设计 §4.4 evidence()）：
  //   ① 索引召回：观测全文 ∪ 事实命中（fold 当前值，尊重冲销）
  //   ② 打包：user 提问自动附下一条 assistant 回答
  //   ③ 三路打分；④ 文本级去重（截断前）；⑤ top-k
  // 纯只读：调用前后对观测/事实店 replay 的结果逐比特一致（测试验证）。
  std::vector<EvidenceItem> synthesize(const std::string& query, const std::string& owner,
                                       int k, int64_t now) const;

 private:
  const ObsStore& obs_;
  const FactStore& facts_;
  const MemIndex& index_;
  const ReliabilityTable& reliab_;
  double alpha_ = 1.0, beta_ = 1.0, gamma_ = 1.0;
  int fallback_pool_ = 20;     // P2a 兜底：owner 最近 N 条观测（0=禁用）
  int proximity_window_ = 2;   // P3b 邻近加成窗口（0=禁用）
  bool expand_enabled_ = true; // P3b 共现联想扩散开关
};

}  // namespace ame
