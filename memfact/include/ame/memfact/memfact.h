// =============================================================================
// 模块定位：M16 事实层 —— 属性变化事件流（存在层，append-only + 冲销），
//           fold 三序裁决物化 latest(owner, entity, attr, now) 当前值视图。
//
// 本文件思路：对外声明 Reality 三值、Fact 事件模型与 FactStore 接口。
//           fact 是"属性变成了某值"的红字/黑字流水：append 落盘即定稿，
//           记错不改写，追加 corrects 指向旧 seq 的冲销 fact（留痕）；
//           fold 在查询时从未冲销候选中按 (权威序, 可靠性, ts, seq) 取最后。
//
// 关键算法/数据结构：
//   - 半衰期纪律：effective = base × 0.5^((now−ts)/half_life)，基准字段
//     （confidence/ts）永不改写，衰减纯查询时计算；Declared 不衰减，
//     Observed 默认 90 天，Derived 默认 7 天，均可 set_half_life_days 配置；
//   - P0 来源可靠性内置表 {user:1.0, assistant:0.7, vision:0.9, derived:0.5}
//     （观测 kind 用表；fact 无 kind 字段，fold 按 reality 取：Declared=1.0、
//      Observed=1.0(user 级)、Derived=0.5，同 rank 内 reliability 高者优先）；
//   - 持久化 per-owner 分片 shards/fact/<owner>.jsonl（JSON Lines）。
//
// 依赖关系：仅依赖 core 公共层，不依赖任何 1.x 模块；被 M17 index、
//           M18 view、M19 mem 外观层依赖。
// =============================================================================
#pragma once
#include "ame/core/json.h"
#include "ame/core/types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ame {

// Reality 三值（设计 §6）：一个标记，行为差异收敛于可信度/消亡/修改三处
enum class Reality { Observed, Declared, Derived };

const char* reality_name(Reality r);
bool reality_from_name(const std::string& s, Reality* out);

// P0 来源可靠性内置表：观测 kind -> reliability（§6 闭环的静态初始值）
double kind_reliability(const std::string& kind);

// fold 排序用的权威序：Declared > Observed > Derived
int reality_authority(Reality r);
// fold 用的 reality 级可靠性（fact 无 kind 字段，P0 按 reality 取值）
double reality_reliability(Reality r);

// 事实：一条属性变化事件（红字冲销用 corrects 指向被冲销 fact，原记录保留）
struct Fact {
  uint64_t seq = 0;          // 全局单调（per-store uint64）
  int64_t ts = 0;            // 事件时刻（epoch 秒，裁决用事件时刻的权威值）
  std::string owner;
  std::string entity;        // 槽位引用："owner:self" / "owner:实体名" / "public:景点X"
  std::string attr;
  std::string value;
  Reality reality = Reality::Observed;
  double confidence = 1.0;   // 提取置信度（declare 恒 1.0）
  uint64_t source_obs = 0;   // 溯源：来自哪条观测（derived 可空=0）
  uint64_t corrects = 0;     // 非 0 = 红字冲销哪条 fact（原记录不被改写）
};

// fold 查询结果：命中 fact 的副本 + 衰减后的有效置信度（基准字段只读不改写）
struct Folded {
  bool found = false;
  Fact fact{};               // latest 命中 fact 的副本（留痕可查）
  double base = 0;           // base = confidence × 来源可靠性（不衰减）
  double effective = 0;      // effective = base × 0.5^((now−ts)/half_life)
};

class FactStore {
 public:
  explicit FactStore(std::string root = "");  // 空 = 纯内存态

  // append fact：分配 seq，立即落盘 owner 分片；返回带 seq 的副本
  Fact append(Fact f);
  const Fact* get(uint64_t seq) const;

  // replay：owner 为空 = 全量（owner 内 seq 升序）
  void replay(const std::string& owner, const std::function<void(const Fact&)>& fn) const;
  void replay_all(const std::function<void(const Fact&)>& fn) const;

  // fold 三序裁决（设计 §6）：
  //   候选 = 未冲销（seq 未被任何 corrects 引用）且匹配 (owner, entity, attr) 的 fact；
  //   排序键 = (authority_rank(reality, 来源可靠性)@事件时刻, ts, seq)，取最后；
  //   纯查询，不修改任何存储状态。
  bool latest(const std::string& owner, const std::string& entity, const std::string& attr,
              int64_t now, Folded& out) const;
  // profile 用：fold owner 全部 (entity, attr) 槽位的当前值
  void fold_all(const std::string& owner, int64_t now,
                const std::function<void(const Folded&)>& fn) const;

  // 半衰期配置（天）：Declared 默认不衰减（huge），Observed 90 天，Derived 7 天
  void set_half_life_days(Reality r, double days);
  double half_life_days(Reality r) const;
  // 时间衰减因子 ∈ (0,1]（纯函数；half_life_days<=0 视为不衰减）
  static double time_decay(double half_life_days, int64_t ts, int64_t now);
  // fact 的有效置信度 = confidence × reality_reliability × 时间衰减
  double effective_confidence(const Fact& f, int64_t now) const;

  // erase（抹除语义）：物理删除 owner 分片 + 清内存（解释级联由索引层重建承担）
  bool eraseOwner(const std::string& owner);
  // channel 级抹除的级联：剔除 source_obs 属于被删观测集合的 fact，重写分片；
  // 返回剔除条数
  size_t erase_sources(const std::string& owner, const std::vector<uint64_t>& obs_seqs);

  Json to_json(const Fact& f) const;
  Fact from_json(const Json& j) const;

  uint64_t next_seq() const { return next_seq_; }
  size_t owner_count(const std::string& owner) const;

 private:
  std::string shard_dir() const;
  std::string shard_path(const std::string& owner) const;
  void persist_append(const Fact& f);
  void persist_rewrite(const std::string& owner);
  void restore();
  static std::string sanitize(std::string s);

  std::string root_;
  uint64_t next_seq_ = 1;
  double half_life_[3] = {90.0, 0.0, 7.0};  // Observed / Declared(0=不衰减) / Derived
  std::unordered_map<std::string, std::deque<Fact>> facts_;  // owner -> 事件流
  std::unordered_map<uint64_t, std::string> by_seq_;
};

}  // namespace ame
