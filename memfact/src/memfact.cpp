// =============================================================================
// 模块定位：M16 事实层实现 —— Fact 事件流的 append-only 存取、fold 三序
//           裁决、半衰期查询时衰减与 erase 级联。
//
// 本文件思路：实现 memfact.h 声明的全部接口。append 分配 seq 进内存再落盘；
//           fold 纯查询：现场扫描 owner 事件流，先建冲销集合，再对候选
//           取 (权威, 可靠性, ts, seq) 最大者，effective 由 time_decay 现算；
//           任何路径都不回写 fact 基准字段（半衰期纪律）。
//
// 关键算法/数据结构：
//   - latest：O(N_owner) 单遍扫描（P0 数据量小；生产可换增量 fold 物化）；
//   - 冲销集合：扫描 corrects != 0 的 fact 收 seq，候选剔除命中者；
//   - time_decay：0.5^(Δt / (half_life_days×86400))，Δt 为负钳 0；
//   - 持久化：JSON Lines 一行一 fact，打开时扫描恢复 next_seq。
//
// 依赖关系：仅依赖 core（types/json），不依赖 1.x 模块。
// =============================================================================
#include "ame/memfact/memfact.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace ame {

// ---------------- Reality / 可靠性 ----------------

const char* reality_name(Reality r) {
  switch (r) {
    case Reality::Observed: return "observed";
    case Reality::Declared: return "declared";
    case Reality::Derived: return "derived";
  }
  return "observed";
}

bool reality_from_name(const std::string& s, Reality* out) {
  if (s == "declared") { *out = Reality::Declared; return true; }
  if (s == "observed") { *out = Reality::Observed; return true; }
  if (s == "derived") { *out = Reality::Derived; return true; }
  return false;
}

// 伪代码：P0 内置来源可靠性表（§6 静态初始值）
//   步骤1：查表 user:1.0 assistant:0.7 vision:0.9 derived:0.5；
//   步骤2：未命中（declare/note/未知）返回保守默认 0.5。
double kind_reliability(const std::string& kind) {
  if (kind == "user") return 1.0;
  if (kind == "assistant") return 0.7;
  if (kind == "vision") return 0.9;
  if (kind == "derived") return 0.5;
  return 0.5;
}

int reality_authority(Reality r) {
  switch (r) {
    case Reality::Declared: return 2;
    case Reality::Observed: return 1;
    case Reality::Derived: return 0;
  }
  return 0;
}

// fact 无 kind 字段（§4.2 数据模型固定），P0 fold 按 reality 取可靠性：
// Declared 权威恒 1.0（declare 恒 1.0）；Observed 取 user 级 1.0；
// Derived 取表内 derived 0.5。同 rank 内 reliability 高者优先（为 P1 来源
// 可靠性闭环预留位，P0 同 reality 内恒相等，退化为 (ts, seq) 裁决）。
double reality_reliability(Reality r) {
  switch (r) {
    case Reality::Declared: return 1.0;
    case Reality::Observed: return 1.0;
    case Reality::Derived: return 0.5;
  }
  return 0.5;
}

// ---------------- 构造 / 恢复 ----------------

FactStore::FactStore(std::string root) : root_(std::move(root)) {
  if (root_.empty()) return;
  half_life_[(int)Reality::Declared] = 0.0;  // 0 = 不衰减
  std::error_code ec;
  std::filesystem::create_directories(shard_dir(), ec);
  restore();
}

// 伪代码：扫描分片恢复 next_seq（同 ObsStore）
void FactStore::restore() {
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
      const Json* seq = j.find("seq");
      if (seq && seq->kind == Json::Num && (uint64_t)seq->num > max_seq)
        max_seq = (uint64_t)seq->num;
    }
  }
  next_seq_ = max_seq + 1;
}

// ---------------- 写路径 ----------------

Fact FactStore::append(Fact f) {
  f.seq = next_seq_++;
  const std::string owner = f.owner;
  facts_[owner].push_back(std::move(f));
  by_seq_[facts_[owner].back().seq] = owner;
  if (!root_.empty()) persist_append(facts_[owner].back());
  return facts_[owner].back();
}

const Fact* FactStore::get(uint64_t seq) const {
  auto it = by_seq_.find(seq);
  if (it == by_seq_.end()) return nullptr;
  const auto& dq = facts_.at(it->second);
  for (const auto& f : dq)
    if (f.seq == seq) return &f;
  return nullptr;
}

void FactStore::replay(const std::string& owner,
                       const std::function<void(const Fact&)>& fn) const {
  if (owner.empty()) { replay_all(fn); return; }
  auto it = facts_.find(owner);
  if (it == facts_.end()) return;
  for (const auto& f : it->second) fn(f);
}

void FactStore::replay_all(const std::function<void(const Fact&)>& fn) const {
  for (const auto& kv : facts_)
    for (const auto& f : kv.second) fn(f);
}

// ---------------- fold 三序裁决 ----------------

// 伪代码：latest(owner, entity, attr, now)
//   步骤1：冲销集合 corrected = {f.corrects | f.corrects != 0}；
//   步骤2：候选 = owner 事件流中 entity/attr 匹配且 seq 不在 corrected 的 fact；
//   步骤3：取排序键最大的候选：先比 (authority, reliability)，再比 ts，再比 seq；
//   步骤4：base = confidence × reality_reliability；effective = base × 时间衰减；
//   步骤5：全程只读，基准字段零改写（半衰期纪律）。
bool FactStore::latest(const std::string& owner, const std::string& entity,
                       const std::string& attr, int64_t now, Folded& out) const {
  auto it = facts_.find(owner);
  if (it == facts_.end()) return false;
  // 冲销集合（红字指向的 seq 均退出 latest-view，原记录保留留痕）
  std::unordered_map<uint64_t, bool> corrected;
  for (const auto& f : it->second)
    if (f.corrects != 0) corrected[f.corrects] = true;

  const Fact* best = nullptr;
  for (const auto& f : it->second) {
    if (f.entity != entity || f.attr != attr) continue;
    if (corrected.count(f.seq)) continue;  // 被冲销：留痕但不进 latest
    if (!best) { best = &f; continue; }
    // 三序比较：权威@事件时刻 → (ts, seq)
    const int ra = reality_authority(f.reality), rb = reality_authority(best->reality);
    if (ra != rb) { if (ra > rb) best = &f; continue; }
    const double la = reality_reliability(f.reality), lb = reality_reliability(best->reality);
    if (la != lb) { if (la > lb) best = &f; continue; }
    if (f.ts != best->ts) { if (f.ts > best->ts) best = &f; continue; }
    if (f.seq > best->seq) best = &f;
  }
  if (!best) return false;
  out.found = true;
  out.fact = *best;  // 副本：调用方拿到的是快照，存储侧永不被改写
  out.base = best->confidence * reality_reliability(best->reality);
  out.effective = out.base * time_decay(half_life_days(best->reality), best->ts, now);
  return true;
}

// 伪代码：fold_all —— 先收集 owner 全部槽位键，再逐键 latest
void FactStore::fold_all(const std::string& owner, int64_t now,
                         const std::function<void(const Folded&)>& fn) const {
  auto it = facts_.find(owner);
  if (it == facts_.end()) return;
  std::vector<std::pair<std::string, std::string>> keys;  // (entity, attr)，保序去重
  for (const auto& f : it->second) {
    const std::pair<std::string, std::string> k{f.entity, f.attr};
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  for (const auto& k : keys) {
    Folded fd;
    if (latest(owner, k.first, k.second, now, fd)) fn(fd);
  }
}

// ---------------- 半衰期 ----------------

void FactStore::set_half_life_days(Reality r, double days) {
  half_life_[(int)r] = days;
}

double FactStore::half_life_days(Reality r) const {
  return half_life_[(int)r];
}

// 伪代码：时间衰减因子
//   步骤1：half_life_days<=0（Declared 配置）→ 返回 1（不衰减）；
//   步骤2：Δt = max(0, now − ts)（未来事件不衰减）；
//   步骤3：返回 0.5^(Δt / (half_life_days×86400))。
double FactStore::time_decay(double half_life_days, int64_t ts, int64_t now) {
  if (half_life_days <= 0) return 1.0;
  const double dt = (double)(now > ts ? now - ts : 0);
  return std::pow(0.5, dt / (half_life_days * 86400.0));
}

double FactStore::effective_confidence(const Fact& f, int64_t now) const {
  return f.confidence * reality_reliability(f.reality) *
         time_decay(half_life_days(f.reality), f.ts, now);
}

// ---------------- erase / 级联 ----------------

bool FactStore::eraseOwner(const std::string& owner) {
  const bool had_mem = facts_.erase(owner) > 0;
  for (auto it = by_seq_.begin(); it != by_seq_.end();) {
    if (it->second == owner) it = by_seq_.erase(it);
    else ++it;
  }
  bool had_file = false;
  if (!root_.empty()) {
    std::error_code ec;
    had_file = std::filesystem::exists(shard_path(owner), ec);
    if (had_file) std::filesystem::remove(shard_path(owner), ec);
    // tombstone 由观测层统一写（eraseOwner 必经网关，两店共享一份空壳记录）
  }
  return had_mem || had_file;
}

// 伪代码：erase_sources —— channel 级抹除的事实级联
//   步骤1：将被剔除观测的 seq 集合成哈希；
//   步骤2：owner 事件流中 source_obs 命中的 fact 移除（含冲销红字），
//          同时清 by_seq_；
//   步骤3：有移除则全量重写 owner 分片；返回移除条数。
size_t FactStore::erase_sources(const std::string& owner,
                                const std::vector<uint64_t>& obs_seqs) {
  if (obs_seqs.empty()) return 0;
  std::unordered_map<uint64_t, bool> doomed;
  for (uint64_t s : obs_seqs) doomed[s] = true;
  auto it = facts_.find(owner);
  if (it == facts_.end()) return 0;
  std::deque<Fact> kept;
  size_t removed = 0;
  for (const auto& f : it->second) {
    if (doomed.count(f.source_obs)) {
      ++removed;
      by_seq_.erase(f.seq);
    } else {
      kept.push_back(f);
    }
  }
  if (removed == 0) return 0;
  it->second = std::move(kept);
  if (!root_.empty()) persist_rewrite(owner);
  return removed;
}

// ---------------- 序列化 ----------------

Json FactStore::to_json(const Fact& f) const {
  Json j = Json::object();
  j.set("seq", Json::number((double)f.seq));
  j.set("ts", Json::number((double)f.ts));
  j.set("owner", Json::string(f.owner));
  j.set("entity", Json::string(f.entity));
  j.set("attr", Json::string(f.attr));
  j.set("value", Json::string(f.value));
  j.set("reality", Json::string(reality_name(f.reality)));
  j.set("confidence", Json::number(f.confidence));
  j.set("source_obs", Json::number((double)f.source_obs));
  j.set("corrects", Json::number((double)f.corrects));
  return j;
}

Fact FactStore::from_json(const Json& j) const {
  // const 访问：find 不到返回共享 Null 兜底（operator[] 非 const，此处只读）
  static const Json kNull;
  auto get = [&](const char* k) -> const Json& {
    const Json* v = j.find(k);
    return v ? *v : kNull;
  };
  Fact f;
  f.seq = (uint64_t)get("seq").as_num();
  f.ts = (int64_t)get("ts").as_num();
  f.owner = get("owner").as_str();
  f.entity = get("entity").as_str();
  f.attr = get("attr").as_str();
  f.value = get("value").as_str();
  Reality r;
  if (reality_from_name(get("reality").as_str(), &r)) f.reality = r;
  f.confidence = get("confidence").as_num(1.0);
  f.source_obs = (uint64_t)get("source_obs").as_num();
  f.corrects = (uint64_t)get("corrects").as_num();
  return f;
}

// ---------------- 私有：持久化 ----------------

std::string FactStore::shard_dir() const { return root_ + "/shards/fact"; }

std::string FactStore::shard_path(const std::string& owner) const {
  return shard_dir() + "/" + sanitize(owner) + ".jsonl";
}

void FactStore::persist_append(const Fact& f) {
  std::ofstream fsh(shard_path(f.owner), std::ios::binary | std::ios::app);
  fsh << to_json(f).dump() << "\n";
}

void FactStore::persist_rewrite(const std::string& owner) {
  std::ofstream f(shard_path(owner), std::ios::binary | std::ios::trunc);
  auto it = facts_.find(owner);
  if (it == facts_.end()) return;
  for (const auto& x : it->second) f << to_json(x).dump() << "\n";
}

std::string FactStore::sanitize(std::string s) {
  for (auto& c : s) {
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
        c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  return s;
}

size_t FactStore::owner_count(const std::string& owner) const {
  auto it = facts_.find(owner);
  return it == facts_.end() ? 0 : it->second.size();
}

}  // namespace ame
