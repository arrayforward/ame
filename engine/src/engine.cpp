/*
 * 所属模块与定位：M12 API 网关/外观（engine）——Engine 类各成员函数的实现，
 *                 负责把外部调用路由到对应子模块，并串联写入与检索两条主管道。
 * 本文件主体思路：构造函数按依赖顺序装配全部子模块（M9 存储为根，其余模块持有其引用/指针）；
 *                 remember 走 M1 提取 → M2 实体解析 → M3/M4 归一写图的写入管道；
 *                 recall 走 M5 软入口 → M6 硬扩散 → M7 软重排的三层检索管道；
 *                 其余动词（lookup / think / merge / split / feedback / train）基本是
 *                 对子模块的直通转发，仅做参数校验与结果包装。
 * 关键算法/数据结构说明：lookup 为全量事件扫描 + 多条件过滤（时间区间、INVOLVES 实体、
 *                        HAPPENED_AT 地点、CONTAINS 关键词）+ 排序截断，无复杂算法；
 *                        其余函数无复杂算法。
 * 依赖关系：依赖 engine.h 引入的全部子模块（M1~M11、M13、M14）及 core 公共类型；
 *           本实现被上层应用通过 Engine 外观直接调用。
 */
#include "ame/engine/engine.h"

#include "ame/core/json.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <fstream>
#include <sstream>

namespace ame {

// 步骤1：按依赖顺序构造子模块——storage_ 为根；vector_/time_ 持有其指针，keyword_ 持有其引用；
//        entity_ 依赖 storage+vector；writer_ 依赖 storage/keyword/time/vector；
//        seed_ 依赖 storage/vector/keyword/extractor；diffusion_ 依赖 storage/keyword；
Engine::Engine(const std::string& data_dir)
    : storage_(),
      vector_(&storage_),
      time_(&storage_),
      keyword_(storage_),
      entity_(storage_, vector_),
      extractor_(),
      writer_(storage_, keyword_, time_, vector_),
      seed_(storage_, vector_, keyword_, &extractor_),
      diffusion_(storage_, &keyword_),
      rerank_(storage_, time_, vector_) {
  if (!data_dir.empty()) {
    std::filesystem::create_directories(data_dir);
  }
}

// 直通 M2 实体解析器的注册接口，返回新实体 uid
std::string Engine::register_entity(const std::string& type, const std::string& name,
                                    const Attrs& identifiers,
                                    const std::vector<std::string>& aliases) {
  return entity_.register_entity(type, name, identifiers, aliases);
}

// 步骤1：若文本为空，返回 InvalidArg 错误结果
// 步骤2：调用 M1 规则提取器把自由文本解析为结构化 Extraction
// 步骤3：委托 remember_event 完成实体解析与写图
RememberResult Engine::remember(const std::string& text) {
  if (text.empty()) {
    RememberResult r;
    r.code = Err::InvalidArg;
    r.msg = "text 为空";
    return r;
  }
  Extraction ex = extractor_.extract(text);
  return remember_event(ex);
}

// 步骤1：若提取结果带地点名，先在 M9 存储中按名查找 Place 节点，作为后续解析的地点提示
// 步骤2：逐个解析提及实体（M2）：构造 ResolveContext，把先解析出的实体 uid 作为共现证据、
//        连同地点提示一并传入；记录 surface→uid 映射，并把新 uid 追加进共现列表
// 步骤3：调用 M4 图写入器落库（M3 关键词归一在 writer 内部完成）
// 步骤4：把 WriteResult 转换为 RememberResult 返回（错误码、事件 id、实体映射）
RememberResult Engine::remember_event(const Extraction& ex_in) {
  // 值副本：人名/专名归一域（--name-alias）需为命中别名表的称呼/昵称 surface 增补
  // canonical 关键词——否则称呼轮（"老X/小X"提取出的实体）只有 INVOLVES 实体锚点、
  // 不经实体种子不可达；alias_of 未命中时 ex 与 ex_in 内容一致，行为不变。
  Extraction ex = ex_in;
  // M1 → M2：实体解析（带上下文证据：同事项其他实体候选）
  std::vector<std::pair<std::string, std::string>> resolved;
  std::vector<std::string> co_uids;
  std::string place_uid_hint;
  if (!ex.place.name.empty()) {
    const Node* p = storage_.find_by_name(NodeKind::Place, ex.place.name);
    if (p) place_uid_hint = p->uid;
  }
  for (auto& e : ex.entities) {
    // 人名归一：实体 surface 命中别名表 → 事件补挂 canonical 关键词（与 M4 写入的
    // M3 normalize 归并到同一词节点，CONTAINS 通道对称呼轮/非称呼轮一视同仁）
    std::string canon = keyword_.alias_of(e.surface);
    if (!canon.empty()) {
      bool dup = false;
      for (auto& k : ex.keywords)
        if (k.word == canon) { dup = true; break; }
      if (!dup) ex.keywords.push_back({canon, "concept"});
    }
    ResolveContext ctx;
    ctx.co_entity_uids = co_uids;  // 先解析的实体作为后解析的共现证据
    ctx.place_uid = place_uid_hint;
    ResolveResult rr = entity_.resolve(e.surface, ctx, e.identifiers);
    resolved.emplace_back(e.surface, rr.uid);
    co_uids.push_back(rr.uid);
  }
  // M3 → M4：关键词归一在 writer 内完成
  WriteResult wr = writer_.write_event(ex, resolved);
  // 属性槽位落档（实体档案·槽位层，M2）：surface 命中本次解析结果的三元组写入档案，
  // 时间取落库后事件节点的 ts（与 SUPERSEDES 有效期语义一致）
  if (wr.code == Err::Ok && !ex.attributes.empty()) {
    const Node* ev = storage_.get_node(wr.event_id);
    TimePoint ts = ev ? ev->ts : 0;
    for (auto& a : ex.attributes) {
      for (auto& [surface, uid] : wr.resolved) {
        if (surface == a.entity) {
          entity_.put_slot(uid, a.slot, a.value, ts, wr.event_id);
          break;
        }
      }
    }
  }
  RememberResult r;
  r.code = wr.code;
  r.event_id = wr.event_id;
  r.resolved_entities = wr.resolved;
  return r;
}

// 对话轮写入伪代码：
//   步骤1：空文本返回 InvalidArg；调 M1 提取关键词/地点等要素；
//   步骤2：把 speaker 作为"主体"角色实体插入提取结果首位（同名 speaker 跨轮经 M2 投票复用同一 uid）；
//   步骤3：ts 非 0 时覆盖提取时间（对话轮以会话时间为准，UTC 存储）；
//   步骤4：走 remember_event 主管道落库；
//   步骤5：dia_id/session_id 非空则写入事件节点 attrs（证据命中统计与分组用）。
RememberResult Engine::remember_turn(const std::string& speaker, const std::string& text,
                                     TimePoint ts, const std::string& dia_id,
                                     const std::string& session_id) {
  if (text.empty()) {
    RememberResult r;
    r.code = Err::InvalidArg;
    r.msg = "text 为空";
    return r;
  }
  return remember_turn_ex(extractor_.extract(text), speaker, ts, dia_id, session_id);
}

// 预提取版对话轮写入伪代码：
//   步骤1：event.text 为空返回 InvalidArg；
//   步骤2：speaker 非空则作为"主体"角色实体插入提取结果首位（跨轮经 M2 投票复用同一 uid）；
//   步骤3：ts 非 0 时覆盖提取时间（对话轮以会话时间为准，UTC 存储）；
//   步骤4：走 remember_event 主管道落库；
//   步骤5：dia_id/session_id/speaker 写入事件节点 attrs（证据命中统计与分组用）。
RememberResult Engine::remember_turn_ex(Extraction ex, const std::string& speaker, TimePoint ts,
                                        const std::string& dia_id,
                                        const std::string& session_id) {
  if (ex.event.text.empty()) {
    RememberResult r;
    r.code = Err::InvalidArg;
    r.msg = "text 为空";
    return r;
  }
  if (!speaker.empty()) {
    ExtractedEntity sp;
    sp.surface = speaker;
    sp.role = "主体";
    sp.entity_type = "person";
    ex.entities.insert(ex.entities.begin(), sp);
  }
  if (ts > 0) ex.event.time = ts;
  RememberResult r = remember_event(ex);
  if (r.code == Err::Ok) {
    Node* ev = storage_.get_node_mut(r.event_id);
    if (ev) {
      if (!dia_id.empty()) ev->attrs["dia_id"] = dia_id;
      if (!session_id.empty()) ev->attrs["session_id"] = session_id;
      if (!speaker.empty()) ev->attrs["speaker"] = speaker;
      storage_.put_node(*ev);  // 4.0：PG 写穿（内存模式为同值覆盖，无副作用）
    }
  }
  return r;
}

// 步骤1：遍历 M9 存储中全部 Event 节点，逐个做多条件过滤：
//        若指定 time_from/time_to，则事件时间戳须落在区间内；
//        若指定 entity_uids，则事件的 INVOLVES 邻居须命中任一给定 uid；
//        若指定 place_uid，则须存在指向该地点的 HAPPENED_AT 边；
//        若指定 keywords，则事件的 CONTAINS 邻居名（与归一化后的关键词比较）须命中任一词；
//        若 current_only 为 true，则跳过已被 SUPERSEDES 取代（attrs 带 valid_to）的过时事件
// 步骤2：按 order_by 排序——"time" 按时间戳倒序，"importance" 按重要度倒序
// 步骤3：截断到 limit 条后返回
std::vector<const Node*> Engine::lookup(const LookupQuery& q) const {
  std::vector<const Node*> out;
  for (auto* n : storage_.nodes_of_kind(NodeKind::Event)) {
    bool ok = true;
    if (q.time_from > 0 && n->ts < q.time_from) ok = false;
    if (q.time_to > 0 && n->ts > q.time_to) ok = false;
    if (q.current_only && n->attrs.count("valid_to")) ok = false;  // 过时事实默认不取
    if (ok && !q.entity_uids.empty()) {
      bool hit = false;
      for (auto& [e, nb] : storage_.neighbors(n->uid, {RelType::INVOLVES}))
        for (auto& u : q.entity_uids)
          if (nb->uid == u) hit = true;
      ok = hit;
    }
    if (ok && !q.place_uid.empty())
      ok = storage_.find_edge(n->uid, q.place_uid, RelType::HAPPENED_AT) != nullptr;
    if (ok && !q.keywords.empty()) {
      bool hit = false;
      for (auto& [e, nb] : storage_.neighbors(n->uid, {RelType::CONTAINS}))
        for (auto& w : q.keywords)
          if (nb->name == keyword_.normalize(w)) hit = true;
      ok = hit;
    }
    if (ok) out.push_back(n);
  }
  if (q.order_by == "time")
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->ts > b->ts; });
  else if (q.order_by == "importance")
    std::sort(out.begin(), out.end(),
              [](auto* a, auto* b) { return a->importance > b->importance; });
  if ((int)out.size() > q.limit) out.resize(q.limit);
  return out;
}

// lookup_as_of 伪代码（双时态历史视角）：
//   步骤1：复用 lookup 拿到候选集合；
//   步骤2：过滤——仅保留 ts <= as_of 的事件，且若事件已被取代（valid_to 非空），
//          仅当 valid_to > as_of（as_of 时刻仍有效）时保留；
//   步骤3：保持原排序返回。
std::vector<const Node*> Engine::lookup_as_of(LookupQuery q, TimePoint as_of) const {
  q.current_only = false;  // as_of 语义自行判定有效性，不走 current_only 捷径
  std::vector<const Node*> base = lookup(q);
  std::vector<const Node*> out;
  for (auto* n : base) {
    if (n->ts > as_of) continue;
    auto it = n->attrs.find("valid_to");
    if (it != n->attrs.end() && std::stoll(it->second) <= as_of) continue;
    out.push_back(n);
  }
  return out;
}

// order_events 伪代码：
//   步骤1：过滤出存在的 Event 节点 uid；
//   步骤2：按时间戳升序排序（NEXT 链与写入顺序在 ts 上天然一致，ts 相同按 uid 稳定次序）；
//   步骤3：返回全局先后序列。
std::vector<std::string> Engine::order_events(const std::vector<std::string>& uids) const {
  std::vector<const Node*> evs;
  for (auto& u : uids) {
    const Node* n = storage_.get_node(u);
    if (n && n->kind == NodeKind::Event) evs.push_back(n);
  }
  std::sort(evs.begin(), evs.end(), [](const Node* a, const Node* b) {
    if (a->ts != b->ts) return a->ts < b->ts;
    return a->uid < b->uid;
  });
  std::vector<std::string> out;
  for (auto* n : evs) out.push_back(n->uid);
  return out;
}

// 步骤1：软入口——M5 种子生成器把查询转换为种子集合；若无命中，返回 NotFound
// 步骤2：硬扩散——把种子（uid+权重）交给 M6 扩散引擎按 depth 做图扩散
// 步骤3：软重排——若查询带文本则用 M13 生成查询向量，连同过滤条件交给 M7 重排并截断到 limit
// 步骤4：若 explain 为假，清空每条结果的可解释信息
// 步骤5：若 M11 反馈采集器已启用，记录本次查询及结果 uid 列表，回填 query_id 埋点
RecallResult Engine::recall(const SeedQuery& sq, const RecallFilters& filters, int depth,
                            int limit, bool explain) {
  RecallResult rr;
  // ① 软入口 M5 → ② 硬扩散 M6 → ③ 软重排 M7（无种子时跳过，仍可走租户知识库）
  SeedSet seeds = seed_.gen_seeds(sq);
  if (seeds.empty()) {
    rr.code = Err::NotFound;
    rr.msg = "无种子命中";
  } else {
    // （3.0：束搜索与 M14 门控随反馈/学习一并裁剪，恒 BFS 规则传导）
    std::vector<DiffusionSeed> ds;
    for (auto& s : seeds) ds.push_back({s.node_uid, s.weight});
    DiffusionResult dr = diffusion_.diffuse(ds, depth);
    Vec qemb = sq.text.empty() ? Vec{} : vector_.embed(sq.text);
    rr.events = rerank_.rerank(dr, qemb, filters, limit);
    if (!explain)
      for (auto& e : rr.events) e.explain.clear();
  }
  // ④ 租户知识库合并（4.0）：session 私有证据 + 租户共享证据按分融合去重
  if (tenant_) {
    RecallResult tr = tenant_->recall(sq, filters, depth, limit, explain);
    if (!tr.events.empty()) {
      std::unordered_set<std::string> seen;
      std::vector<ScoredEvent> merged;
      merged.reserve(rr.events.size() + tr.events.size());
      for (auto& e : rr.events)
        if (e.event && seen.insert(e.event->uid).second) merged.push_back(e);
      for (auto& e : tr.events)
        if (e.event && seen.insert(e.event->uid).second) merged.push_back(e);
      std::stable_sort(merged.begin(), merged.end(),
                       [](const ScoredEvent& a, const ScoredEvent& b) { return a.score > b.score; });
      if (limit > 0 && merged.size() > (size_t)limit) merged.resize((size_t)limit);
      rr.events = std::move(merged);
      rr.code = Err::Ok;
      rr.msg.clear();
    }
  }
  return rr;
}

// 直通 M2 实体解析器：把 uid_a 合并进 into_b
Err Engine::merge_entities(const std::string& uid_a, const std::string& into_b) {
  return entity_.merge_entities(uid_a, into_b);
}

// 直通 M2 实体解析器：把指定提及从实体中拆出，返回新实体 uid
std::string Engine::split_entity(const std::string& uid,
                                 const std::vector<std::string>& mention_ids) {
  return entity_.split_entity(uid, mention_ids);
}

// save 伪代码（组合快照）：
//   步骤1：{"graph": M9 to_json(), "slots": M2 槽位 JSON, "aliases": M3 别名表} 组装；
//   步骤2：trunc 写文件。
Err Engine::save(const std::string& path) const {
  Json root = Json::object();
  root["graph"] = storage_.to_json();
  root["slots"] = entity_.slots_to_json();
  root["aliases"] = keyword_.aliases_to_json();
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return Err::IoError;
  f << root.dump();
  return f.good() ? Err::Ok : Err::IoError;
}

// load 伪代码：
//   步骤1：读文件解析 JSON；步骤2：有 "graph" 键按组合格式还原图+槽位；
//          否则按裸 M9 图快照兼容旧格式；步骤3：还原槽位（无则空）。
Err Engine::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return Err::IoError;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return Err::ParseError;
  if (const Json* g = root.find("graph")) {
    Err r = storage_.from_json(*g);
    if (r != Err::Ok) return r;
    if (const Json* s = root.find("slots")) entity_.slots_from_json(*s);
    if (const Json* a = root.find("aliases")) keyword_.aliases_from_json(*a);
    return Err::Ok;
  }
  return storage_.from_json(root);  // 兼容裸图快照
}

}  // namespace ame
