// =============================================================================
// 模块定位：M15 观测层实现 —— Observation 的 append-only 存取、per-owner
//           分片持久化与 erase/tombstone 语义。
//
// 本文件思路：实现 memobs.h 声明的全部接口。append 先分配 seq 进内存再
//           立即追加落盘分片；eraseOwner 删文件清内存写 tombstone；
//           eraseChannel 重建该 owner 的 deque 后全量重写分片。
//
// 关键算法/数据结构：
//   - sanitize：字符级替换 /\:*?"<>| 与 <0x20 控制字符为 _，防止路径注入；
//   - restore：遍历 shards/obs/*.jsonl 逐行 parse，next_seq = max(seq)+1；
//   - 持久化格式：JSON Lines，一行一条 Observation（Json dump 紧凑输出）。
//
// 依赖关系：仅依赖 core（types/json），不依赖 1.x 模块。
// =============================================================================
#include "ame/memobs/memobs.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace ame {

// ---------------- 构造 / 恢复 ----------------

// 伪代码：打开（或创建）存储
//   步骤1：记录 root_；若为空则纯内存态直接返回；
//   步骤2：创建 shards/obs 目录（已存在则忽略）；
//   步骤3：调 restore 扫描既有分片恢复 next_seq。
ObsStore::ObsStore(std::string root) : root_(std::move(root)) {
  if (root_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(shard_dir(), ec);
  restore();
}

// 伪代码：扫描分片恢复内存态
//   步骤1：清空 obs_/by_seq_，next_seq 置 1；遍历 shards/obs 下所有 .jsonl；
//   步骤2：逐行 parse 反序列化观测，回填 owner deque 与 by_seq_，seq 取 max；
//   步骤3：next_seq = max+1（无分片则保持 1）。
void ObsStore::restore() {
  obs_.clear();
  by_seq_.clear();
  std::error_code ec;
  if (!std::filesystem::exists(shard_dir(), ec)) return;
  uint64_t max_seq = 0;
  for (auto& e : std::filesystem::directory_iterator(shard_dir(), ec)) {
    if (ec || !e.is_regular_file()) continue;
    std::ifstream f(e.path(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty()) continue;
      bool ok = false;
      Json j = Json::parse(line, &ok);
      if (!ok) continue;
      Observation o = from_json(j);
      if (o.seq == 0) continue;  // 无 seq 的坏行跳过（seq 从 1 起，0 保留）
      if (o.seq > max_seq) max_seq = o.seq;
      by_seq_[o.seq] = o.owner;
      obs_[o.owner].push_back(std::move(o));
    }
  }
  next_seq_ = max_seq + 1;
}

// ---------------- 写路径 ----------------

// 伪代码：append 观测
//   步骤1：o.seq = next_seq_++；
//   步骤2：按 owner 进内存 deque，登记 by_seq_；
//   步骤3：持久化非空时立即追加一行落盘。
Observation ObsStore::append(Observation o) {
  o.seq = next_seq_++;
  const std::string owner = o.owner;
  obs_[owner].push_back(std::move(o));
  by_seq_[obs_[owner].back().seq] = owner;
  if (!root_.empty()) persist_append(obs_[owner].back());
  return obs_[owner].back();
}

const Observation* ObsStore::get(uint64_t seq) const {
  auto it = by_seq_.find(seq);
  if (it == by_seq_.end()) return nullptr;
  const auto& dq = obs_.at(it->second);
  for (const auto& o : dq)
    if (o.seq == seq) return &o;
  return nullptr;  // 已被 erase 移除但索引未清（防御，正常路径不会发生）
}

// ---------------- 遍历 ----------------

void ObsStore::for_owner(const std::string& owner,
                         const std::function<void(const Observation&)>& fn) const {
  auto it = obs_.find(owner);
  if (it == obs_.end()) return;
  for (const auto& o : it->second) fn(o);
}

void ObsStore::for_channel(const std::string& owner, const std::string& channel,
                           const std::function<void(const Observation&)>& fn) const {
  auto it = obs_.find(owner);
  if (it == obs_.end()) return;
  for (const auto& o : it->second)
    if (o.channel == channel) fn(o);
}

void ObsStore::replay(const std::string& owner,
                      const std::function<void(const Observation&)>& fn) const {
  if (owner.empty()) { replay_all(fn); return; }
  for_owner(owner, fn);
}

void ObsStore::replay_all(const std::function<void(const Observation&)>& fn) const {
  for (const auto& kv : obs_)  // 跨 owner 顺序不确定，owner 内 seq 升序
    for (const auto& o : kv.second) fn(o);
}

// ---------------- erase / tombstone ----------------

// 伪代码：eraseOwner（抹除语义）
//   步骤1：清内存（obs_ 与 by_seq_ 中该 owner 的条目）；
//   步骤2：删除分片文件 shards/obs/<owner>.jsonl（存在才删）；
//   步骤3：向公共 tombstone 文件追加 {owner, erased_at} 空壳记录；
//   步骤4：返回内存或文件曾存在数据。
bool ObsStore::eraseOwner(const std::string& owner) {
  const bool had_mem = obs_.erase(owner) > 0;
  // by_seq_ 清理：扫描代价 O(N)，erase 低频可接受
  for (auto it = by_seq_.begin(); it != by_seq_.end();) {
    if (it->second == owner) it = by_seq_.erase(it);
    else ++it;
  }
  bool had_file = false;
  if (!root_.empty()) {
    std::error_code ec;
    const std::string p = shard_path(owner);
    had_file = std::filesystem::exists(p, ec);
    if (had_file) std::filesystem::remove(p, ec);
    // tombstone：空壳无内容，只证明"该 owner 被抹除过"
    std::error_code ec2;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path().parent_path(), ec2);
    std::ofstream t(tombstone_path(), std::ios::binary | std::ios::app);
    Json rec = Json::object();
    rec.set("owner", Json::string(owner));
    rec.set("erased_at", Json::number((double)std::time(nullptr)));
    t << rec.dump() << "\n";
  }
  return had_mem || had_file;
}

// 伪代码：eraseChannel（删除这个会话）
//   步骤1：遍历该 owner deque，挑出非本 channel 的观测保留；
//   步骤2：被剔除的 seq 记入返回列表，并从 by_seq_ 摘除；
//   步骤3：全量重写 owner 分片（剩余观测按 seq 升序一行一条）。
std::vector<uint64_t> ObsStore::eraseChannel(const std::string& owner,
                                             const std::string& channel) {
  std::vector<uint64_t> removed;
  auto it = obs_.find(owner);
  if (it == obs_.end()) return removed;
  std::deque<Observation> kept;
  for (const auto& o : it->second) {
    if (o.channel == channel) {
      removed.push_back(o.seq);
      by_seq_.erase(o.seq);
    } else {
      kept.push_back(o);
    }
  }
  if (removed.empty()) return removed;
  it->second = std::move(kept);
  if (!root_.empty()) persist_rewrite(owner);
  return removed;
}

// ---------------- 序列化 ----------------

Json ObsStore::to_json(const Observation& o) const {
  Json j = Json::object();
  j.set("seq", Json::number((double)o.seq));
  j.set("ts", Json::number((double)o.ts));
  j.set("owner", Json::string(o.owner));
  j.set("channel", Json::string(o.channel));
  j.set("kind", Json::string(o.kind));
  j.set("text", Json::string(o.text));
  j.set("meta", o.meta);
  return j;
}

Observation ObsStore::from_json(const Json& j) const {
  // const 访问：find 不到返回共享 Null 兜底（operator[] 非 const，此处只读）
  static const Json kNull;
  auto get = [&](const char* k) -> const Json& {
    const Json* v = j.find(k);
    return v ? *v : kNull;
  };
  Observation o;
  o.seq = (uint64_t)get("seq").as_num();
  o.ts = (int64_t)get("ts").as_num();
  o.owner = get("owner").as_str();
  o.channel = get("channel").as_str();
  o.kind = get("kind").as_str();
  o.text = get("text").as_str();
  if (const Json* m = j.find("meta")) o.meta = *m;
  return o;
}

// ---------------- 私有：持久化 ----------------

std::string ObsStore::shard_dir() const {
  return root_ + "/shards/obs";
}

std::string ObsStore::shard_path(const std::string& owner) const {
  return shard_dir() + "/" + sanitize(owner) + ".jsonl";
}

std::string ObsStore::tombstone_path() const {
  return root_ + "/shards/tombstones.jsonl";
}

void ObsStore::persist_append(const Observation& o) {
  std::ofstream f(shard_path(o.owner), std::ios::binary | std::ios::app);
  f << to_json(o).dump() << "\n";
}

void ObsStore::persist_rewrite(const std::string& owner) {
  std::ofstream f(shard_path(owner), std::ios::binary | std::ios::trunc);
  auto it = obs_.find(owner);
  if (it == obs_.end()) return;
  for (const auto& o : it->second) f << to_json(o).dump() << "\n";
}

// 伪代码：文件名清洗
//   步骤1：遍历每个字符；
//   步骤2：若是 /\:*?"<>| 之一或控制字符（<0x20），替换为 _。
std::string ObsStore::sanitize(std::string s) {
  for (auto& c : s) {
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
        c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  return s;
}

size_t ObsStore::owner_count(const std::string& owner) const {
  auto it = obs_.find(owner);
  return it == obs_.end() ? 0 : it->second.size();
}

}  // namespace ame
