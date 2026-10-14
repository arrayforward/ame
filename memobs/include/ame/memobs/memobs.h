// =============================================================================
// 模块定位：M15 观测层 —— AME 2.0 分层记忆的存在层底座，原始言语行为日志
//           （谁 owner 在哪个会话 channel 于何时说了什么 kind/text）。
//
// 本文件思路：对外声明 Observation 数据模型与 ObsStore 存储接口。
//           观测 append-only：append 即分配全局单调 seq 并立即落盘
//           （per-owner 分片 shards/obs/<owner>.jsonl，JSON Lines）；
//           打开时扫描分片恢复 next_seq；eraseOwner 物理删分片并写
//           tombstone 空壳记录，eraseChannel 重写 owner 分片剔除该会话。
//
// 关键算法/数据结构：
//   - 内存：owner -> deque<Observation>（deque 保证元素地址稳定，供 seq 索引
//     悬空指针零风险）+ by_seq_ 哈希表（seq -> 所在 owner）；
//   - 分片文件名做非法字符清洗（/\:*?"<>| 与控制字符替换为 _）；
//   - tombstone 为公共追加文件 shards/tombstones.jsonl，记录 {owner, erased_at}
//     空壳，不含任何原始内容。
//
// 依赖关系：仅依赖 core 公共层（types/json/uid），不依赖任何 1.x 模块；
//           被 M16 fact（source_obs 溯源）、M17 index、M19 mem 外观层依赖。
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

// 观测：一次真实发生的言语行为（存在层，append-only，可抹除不可改写）
struct Observation {
  uint64_t seq = 0;        // 全局单调（per-store uint64）
  int64_t ts = 0;          // UTC epoch 秒
  std::string owner;       // 数据主体（分片键）
  std::string channel;     // 会话/渠道（观察面，非所有权）
  std::string kind;        // user | assistant | vision | declare | note
  std::string text;        // 原文
  Json meta;               // 可附：图片路径、坐标、置信度等
};

class ObsStore {
 public:
  // root 为空 = 纯内存态（不落盘、不写 tombstone）；否则 root/shards/obs/
  explicit ObsStore(std::string root = "");

  // append 观测：分配全局单调 seq，立即追加落盘 owner 分片；返回带 seq 的副本
  Observation append(Observation o);
  // 按 seq 查找（未找到返回 nullptr）
  const Observation* get(uint64_t seq) const;

  // 按 owner 范围遍历（seq 升序）
  void for_owner(const std::string& owner,
                 const std::function<void(const Observation&)>& fn) const;
  // 按 owner+channel 遍历（seq 升序）
  void for_channel(const std::string& owner, const std::string& channel,
                   const std::function<void(const Observation&)>& fn) const;
  // replay：owner 为空 = 全量（跨 owner 顺序不确定，owner 内升序）
  void replay(const std::string& owner,
              const std::function<void(const Observation&)>& fn) const;
  void replay_all(const std::function<void(const Observation&)>& fn) const;

  // erase（抹除语义，区别于冲销 corrects）：
  // eraseOwner —— 物理删除该 owner 分片文件 + 清内存 + 写 tombstone 空壳；
  // 返回 false 表示该 owner 无数据（public 域由网关层拦截，本层不判）
  bool eraseOwner(const std::string& owner);
  // eraseChannel —— 从内存/分片中剔除该会话观测并重写 owner 分片；
  // 返回被剔除的观测 seq 列表（供事实层级联 source_obs）
  std::vector<uint64_t> eraseChannel(const std::string& owner, const std::string& channel);

  // 序列化辅助（tombstone 校验、测试零副作用 dump 用）
  Json to_json(const Observation& o) const;
  Observation from_json(const Json& j) const;

  uint64_t next_seq() const { return next_seq_; }
  bool has_owner(const std::string& owner) const { return obs_.count(owner) != 0; }
  size_t owner_count(const std::string& owner) const;

 private:
  std::string shard_dir() const;
  std::string shard_path(const std::string& owner) const;
  std::string tombstone_path() const;
  void persist_append(const Observation& o);   // 追加一行到 owner 分片
  void persist_rewrite(const std::string& owner);  // 全量重写 owner 分片
  void restore();                              // 扫描分片目录恢复 next_seq
  static std::string sanitize(std::string s);  // 文件名非法字符清洗

  std::string root_;          // 空 = 内存态
  uint64_t next_seq_ = 1;     // 全局单调（从 1 起，0 保留给"无"）
  std::unordered_map<std::string, std::deque<Observation>> obs_;  // owner -> 观测
  std::unordered_map<uint64_t, std::string> by_seq_;              // seq -> owner
};

}  // namespace ame
