// =============================================================================
// 模块定位：M18 视图层 —— 抽象层"看"：查询时把证据取证 → Q→A 打包 →
//           三路打分 → 文本级去重 → top-k 装配成 EvidenceItem。
//           永不落盘：synthesize 只读任何存储状态（设计 §4.4 硬纪律）。
//
// 本文件思路：对外声明 EvidenceItem 与 View。View 持有三店引用（观测/事实/
//           索引），synthesize 全程只调用其 const 接口，零写路径触发；
//           打分 α·词面(IDF 加权命中词重合) + β·来源可靠性 + γ·半衰期
//           时间分，排序键全序（score desc, seq asc），输出确定性。
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
#include <string>
#include <vector>

namespace ame {

// 证据条目：装配给上层的召回结果（视图抽象层，不落盘）
struct EvidenceItem {
  std::string text;        // 原文 / "问题\n→ 回答" 打包 / "entity.attr=value"
  int64_t ts = 0;          // 证据时刻
  double score = 0;        // 三路加权总分
  std::string source_kind; // user|assistant|vision|declared|observed|derived
  uint64_t ref_obs = 0;    // 来源观测 seq（观测证据=其 seq；事实证据=source_obs，可无）；
                           // 上层经 Mem::obs(seq) 回溯 meta（如 dia_id），0=无法回溯
};

class View {
 public:
  // 三店引用由外部（M19 网关）注入，View 不拥有、不修改
  View(const ObsStore& obs, const FactStore& facts, const MemIndex& index);

  // 打分权重（α 词面 / β 来源可靠性 / γ 半衰期时间分），可调
  void set_weights(double alpha, double beta, double gamma);

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
  double alpha_ = 1.0, beta_ = 1.0, gamma_ = 1.0;
};

}  // namespace ame
