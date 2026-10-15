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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ame {

namespace {

// UTF-8 码点计数（G2 长度限制按"字"而非字节：CJK 一字一码点）
size_t utf8_len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

// G3 规则过滤（P2c 收窄版）：public 域槽位只允许 public owner 写入，
// 个人 owner 写 public:* 一律拦截（防个人域污染公共域，§8）；
// 个人域写入不拦（规则库留待后续域分治）。
bool public_write_blocked(const std::string& owner, const std::string& entity) {
  return entity.rfind("public:", 0) == 0 && owner != "public";
}

}  // namespace

// 可靠性表持久化路径（root 为空 = 内存态，不持久化）
static std::string reliability_path(const std::string& root) {
  return root + "/shards/reliability.json";
}

// 原子落盘：先写临时文件再 rename（中断时最多留下 .tmp，不损坏旧表）
static void reliability_save(const std::string& root, const ReliabilityTable& t) {
  if (root.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(root + "/shards", ec);
  const std::string tmp = reliability_path(root) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << t.to_json().dump();
  }
  std::filesystem::rename(tmp, reliability_path(root), ec);
  if (ec) {  // rename 失败（跨设备等）：退化为直接重写
    std::ofstream f(reliability_path(root), std::ios::binary | std::ios::trunc);
    if (f) f << t.to_json().dump();
  }
}

static void reliability_load(const std::string& root, ReliabilityTable& t) {
  if (root.empty()) return;
  std::ifstream f(reliability_path(root), std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  const Json j = Json::parse(ss.str(), &ok);
  if (ok) t.load_json(j);
}

// 伪代码：构造网关
//   步骤1：按 root 构造观测店与事实店（空 root = 内存态）；
//   步骤2：视图层注入三店 const 引用 + 来源可靠性表；
//   步骤3：加载 reliability.json（P2b 闭环重启保留，缺文件 = 默认表）；
//   步骤4：从两店 replay 建索引（持久化目录下恢复到最新状态）。
Mem::Mem(std::string root)
    : root_(std::move(root)), obs_(root_), facts_(root_), view_(obs_, facts_, index_, reliab_) {
  reliability_load(root_, reliab_);
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

// declare：G1 槽位"本次创建"恒成立（declare 即创建语义，§7 四动词）；
// G2 attr/value 长度硬校验；G3 public 域拦截。任一失败返回错误码、整批不写入。
Err Mem::declare(const std::string& owner, const std::string& entity,
                 const std::map<std::string, std::string>& attrs, int64_t ts) {
  if (owner.empty() || entity.empty()) return Err::InvalidArg;
  for (const auto& kv : attrs) {
    if (kv.first.empty() || utf8_len(kv.first) > 64)
      return Err::InvalidArg;  // G2：attr 非空且 ≤64 字
    if (utf8_len(kv.second) > 256) return Err::InvalidArg;  // G2：value ≤256 字
  }
  if (public_write_blocked(owner, entity)) return Err::InvalidArg;  // G3
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

// rememberFact（allocate）：G1 槽位须已存在（declare 或其他写入创建过），
// 不允许 allocate 凭空开槽（§7：开槽是 declare 的语义）；
// G2 长度硬校验；G3 public 域拦截。任一失败返回错误码、不写入。
Result<Fact> Mem::rememberFact(const std::string& owner, const std::string& entity,
                               const std::string& attr, const std::string& value,
                               int64_t ts, double confidence, uint64_t source_obs) {
  if (owner.empty() || entity.empty() || attr.empty())
    return Result<Fact>::fail(Err::InvalidArg, "owner/entity/attr 为空");
  if (utf8_len(attr) > 64)
    return Result<Fact>::fail(Err::InvalidArg, "G2: attr 超长(>64 字)");
  if (utf8_len(value) > 256)
    return Result<Fact>::fail(Err::InvalidArg, "G2: value 超长(>256 字)");
  if (public_write_blocked(owner, entity))
    return Result<Fact>::fail(Err::InvalidArg, "G3: 个人域不可写 public 槽位");
  if (!facts_.has_slot(owner, entity))
    return Result<Fact>::fail(Err::InvalidArg, "G1: 槽位不存在（先 declare 创建）");
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

// ---- 来源可靠性闭环（P2b，设计 §6）----

// feedback：evidence 被采纳 → 来源 kind +0.02（封顶 1.0）；被纠正 → −0.05
// （封底 0.05，罚>奖防刷）；调整即持久化（root 非空时），重启经 reliability.json 保留。
Err Mem::feedback(const std::string& owner, const EvidenceItem& evidence, bool positive) {
  if (owner.empty()) return Err::InvalidArg;
  std::string kind = evidence.source_kind;
  // 观测证据以观测实际 kind 为准（fact 证据 source_kind 已是 reality 名）
  if (evidence.via != "fact" && evidence.ref_obs != 0)
    if (const Observation* o = obs_.get(evidence.ref_obs)) kind = o->kind;
  reliab_.adjust(kind, positive);
  reliability_save(root_, reliab_);
  return Err::Ok;
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

// ---- 1.x → 2.0 迁移工具（P1）----

std::string migrate_flag_path(const std::string& lmem_root) {
  return lmem_root + "/migrated.flag";
}

bool migrate_done(const std::string& lmem_root) {
  std::error_code ec;
  return std::filesystem::exists(migrate_flag_path(lmem_root), ec);
}

// 伪代码：1.x graph.json → 观测层
//   步骤1：读文件解析（Engine 组合快照取 "graph".nodes，兼容裸图快照）；
//   步骤2：遍历 kind==Event 的节点，空 text 跳过；attrs.speaker 映射 kind
//         （"user"→user，其余含缺失→assistant），attrs.dia_id 进 meta；
//   步骤3：先扫同 owner+channel 已有观测建 (kind,ts,text) 去重集，命中跳过——
//         迁移中断重启重做时不重复（观测 seq 单调，无法靠 seq 判重）；
//   步骤4：remember 逐条入观测层（append-only 立即落盘），统计写入数返回。
size_t migrate_graph(Mem& mem, const std::string& graph_json_path,
                     const std::string& owner, const std::string& channel) {
  std::ifstream f(graph_json_path, std::ios::binary);
  if (!f) return 0;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  const Json root = Json::parse(ss.str(), &ok);
  if (!ok) return 0;
  const Json* nodes = nullptr;
  if (const Json* g = root.find("graph")) nodes = g->find("nodes");
  if (!nodes) nodes = root.find("nodes");  // 兼容裸图快照
  if (!nodes || nodes->kind != Json::Arr) return 0;

  const std::string sep = "\x1f";  // 字段分隔（控制字符，文本不会含）
  std::set<std::string> seen;
  mem.obs_store().for_channel(owner, channel, [&](const Observation& o) {
    seen.insert(o.kind + sep + std::to_string(o.ts) + sep + o.text);
  });

  size_t n = 0;
  for (const Json& j : nodes->arr) {
    if ((int)(j.find("kind") ? j.find("kind")->as_num(-1) : -1) != (int)NodeKind::Event)
      continue;
    const std::string text = j.find("text") ? j.find("text")->as_str() : "";
    if (text.empty()) continue;
    const int64_t ts = (int64_t)(j.find("ts") ? j.find("ts")->as_num() : 0);
    std::string dia_id;
    if (const Json* a = j.find("attrs"))
      if (const Json* d = a->find("dia_id")) dia_id = d->as_str();
    const std::string kind =
        (j.find("attrs") && j.find("attrs")->find("speaker") &&
         j.find("attrs")->find("speaker")->as_str() == "user")
            ? "user"
            : "assistant";
    const std::string key = kind + sep + std::to_string(ts) + sep + text;
    if (!seen.insert(key).second) continue;
    Json meta = Json::object();
    if (!dia_id.empty()) meta.set("dia_id", Json::string(dia_id));
    if (mem.remember(owner, channel, kind, text, ts, std::move(meta)).code == Err::Ok) ++n;
  }
  return n;
}

}  // namespace ame
