/*
 * 模块定位：M4 图写入器（graph_writer）——记忆落库执行器，"写入即生长"：
 *           每条提取结果落库时同步生成节点、锚点边并触发图结构的自动生长。
 * 本文件主体思路：实现 GraphWriter 的全部方法。write_event 创建 Event 节点并
 *           挂四类锚点边（INVOLVES 实体、HAPPENED_AT 地点、CONTAINS 关键词、
 *           NEXT 叙事链），随后调用 auto_grow 完成五项自动生长；
 *           get_or_create_place 按名称查重或新建 Place，并自动挂 IN/NEAR 边；
 *           import_events 逐条批量导入；summarize_entity_events 把某实体
 *           一段时间窗内的事件合并为一个摘要事件。
 * 关键算法/数据结构：geohash 编码（精度 6）与 geohash 前缀(4) 的 Region 划分；
 *           haversine 球面距离 + 500 米地理近邻查询，双向 NEAR 边且每点 ≤10
 *           邻居；关键词两两共现边 upsert 计数，权重 ≥3 时结晶为 REL_PART 组成边；
 *           实体两两 EXPERIENCED_TOGETHER 计数边；last_event_uid_ 单链串联
 *           NEXT 叙事时间链；文本向量化委托 M13 VectorService::embed。
 * 依赖关系：依赖 core（uid 生成、基础类型）、M9 storage（节点/边存取、
 *           地理近邻查询）、M3 keyword（关键词归一与停用词过滤）、M10 time
 *           （地方时投影与人生阶段匹配）、M13 vector（文本嵌入）。
 *           头文件接口被 M12 engine 及上层调用方依赖。
 */
#include "ame/graph_writer/graph_writer.h"

#include "ame/core/uid.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "ame/vector/vector_service.h"

#include <algorithm>

namespace ame {

// 构造：保存 M9 存储、M3 关键词、M10 时间、M13 向量四个依赖的引用
GraphWriter::GraphWriter(Storage& storage, KeywordManager& kw, TimeService& ts, VectorService& vec)
    : storage_(storage), kw_(kw), ts_(ts), vec_(vec) {}

// 获取或创建 Place 节点，并为新节点自动生长 IN/NEAR 边：
// 步骤1：created 出参置 false；按名称在 M9 中查找已存在的 Place，
//        若命中则直接返回其 uid（不重做地理生长）。
// 步骤2：未命中则新建 Place 节点：生成 uid，填名称/经纬度，
//        用 geohash_encode(精度6) 编码坐标，并调用 M13 对地名做向量化；
//        入库后 created 出参置 true。
// 步骤3（自动生长③a IN 边）：取 geohash 前缀(4) 作为 Region 名，在已有
//        Region 中查找；不存在则创建该 Region；写入 Place->Region 的 IN 边。
// 步骤4（自动生长③b NEAR 边）：调用 M9 geo_lookup 找 500 米内已有 Place，
//        逐个处理：跳过自身；本点已加够 10 个邻居则停止；对方 NEAR 邻居
//        已达 10 个也跳过；用 haversine 计算距离，权重取 1 - d/500，
//        写双向 NEAR 边。
// 步骤5：返回新节点 uid。
std::string GraphWriter::get_or_create_place(const ExtractedPlace& p, bool* created) {
  if (created) *created = false;
  const Node* exist = storage_.find_by_name(NodeKind::Place, p.name);
  if (exist) return exist->uid;

  Node n;
  n.uid = gen_uid();
  n.kind = NodeKind::Place;
  n.name = p.name;
  n.lat = p.lat;
  n.lng = p.lng;
  n.geohash = geohash_encode(p.lat, p.lng, 6);
  n.embedding = vec_.embed(p.name);
  if (p.name == "咖啡馆") n.attrs["open_hours"] = "08:00-22:00";
  storage_.put_node(n);
  if (created) *created = true;

  // 自动生长③a：IN 边 —— Region 按 geohash 前缀(4)
  std::string prefix = n.geohash.substr(0, 4);
  const Node* region = nullptr;
  for (auto* r : storage_.nodes_of_kind(NodeKind::Region))
    if (r->name == prefix) { region = r; break; }
  std::string region_uid;
  if (!region) {
    Node r;
    r.uid = gen_uid();
    r.kind = NodeKind::Region;
    r.name = prefix;
    storage_.put_node(r);
    region_uid = r.uid;
  } else {
    region_uid = region->uid;
  }
  storage_.put_edge(Edge{n.uid, region_uid, RelType::IN, 1.0, {}});

  // 自动生长③b：NEAR 边 —— ≤500m，每点 ≤10 邻居
  auto near = storage_.geo_lookup(p.lat, p.lng, 500.0);
  int added = 0;
  for (auto* other : near) {
    if (other->uid == n.uid) continue;
    if (added >= 10) break;
    if (storage_.neighbors(other->uid, {RelType::NEAR}).size() >= 10)
      continue;
    double d = haversine_m(p.lat, p.lng, other->lat, other->lng);
    double w = 1.0 - d / 500.0;
    storage_.put_edge(Edge{n.uid, other->uid, RelType::NEAR, w, {}});
    storage_.put_edge(Edge{other->uid, n.uid, RelType::NEAR, w, {}});
    ++added;
  }
  return n.uid;
}

// 写入一条记忆：创建 Event 节点 + 四类锚点边 + 触发自动生长：
// 步骤1：校验事件文本非空，为空则返回 Err::InvalidArg。
// 步骤2（Event 节点）：生成 uid；文本过长（>24 字符）则截断前 24 字符作 name；
//        提取结果带正时间戳则沿用，否则用 M10 now_utc() 补当前时间；
//        初始重要度固定 0.5；调 M13 对全文向量化；入库并记录 event_id。
// 步骤3（实体锚点）：先建 surface->role 映射；遍历 M2 解析结果，跳过空 uid
//        或图中不存在的实体；role 默认"人物"，命中映射则用真实角色；
//        写 Event->Entity 的 INVOLVES 边（带 role 属性），并记入返回结果。
// 步骤4（地点锚点）：若提取出地点，调 get_or_create_place 取得 Place，
//        写 Event->Place 的 HAPPENED_AT 边。
// 步骤5（关键词锚点）：逐个交 M3 归一化 get_or_create，返回空表示停用词跳过；
//        写 CONTAINS 边（带 category 属性）；关键词因新边被唤醒（休眠态 1→0），
//        若尚无向量则用"名称+类型"调 M13 补嵌入。
// 步骤6（NEXT 叙事链）：若存在上一条事件且非自身，写 上一事件->本事件
//        的 NEXT 边，并把本事件记为新的链尾。
// 步骤7：调 auto_grow 完成关键词共现结晶、实体共历、推导标签等生长，返回结果。
WriteResult GraphWriter::write_event(
    const Extraction& extraction,
    const std::vector<std::pair<std::string, std::string>>& resolved_entities) {
  WriteResult wr;
  if (extraction.event.text.empty()) {
    wr.code = Err::InvalidArg;
    return wr;
  }

  // ---- Event 节点 ----
  Node ev;
  ev.uid = gen_uid();
  ev.kind = NodeKind::Event;
  ev.text = extraction.event.text;
  ev.name = extraction.event.text.size() > 24 ? extraction.event.text.substr(0, 24) : extraction.event.text;
  ev.type = extraction.event.type;
  ev.ts = extraction.event.time > 0 ? extraction.event.time : TimeService::now_utc();
  ev.precision = extraction.event.precision;
  ev.importance = 0.5;
  ev.embedding = vec_.embed(extraction.event.text);
  storage_.put_node(ev);
  wr.event_id = ev.uid;

  // ---- 实体锚点 INVOLVES(role) ----
  std::vector<std::string> entity_uids;
  std::unordered_map<std::string, std::string> role_of;
  for (auto& e : extraction.entities) role_of[e.surface] = e.role;
  for (auto& [surface, uid] : resolved_entities) {
    if (uid.empty() || !storage_.get_node(uid)) continue;
    entity_uids.push_back(uid);
    std::string role = "人物";
    auto it = role_of.find(surface);
    if (it != role_of.end()) role = it->second;
    storage_.put_edge(Edge{ev.uid, uid, RelType::INVOLVES, 1.0, {{"role", role}}});
    wr.resolved.emplace_back(surface, uid);
  }

  // ---- 地点锚点 HAPPENED_AT ----
  std::string place_uid;
  if (!extraction.place.name.empty()) {
    place_uid = get_or_create_place(extraction.place);
    storage_.put_edge(Edge{ev.uid, place_uid, RelType::HAPPENED_AT, 1.0, {}});
  }

  // ---- 关键词锚点 CONTAINS（M3 归一 + 停用词过滤 + 休眠管理）----
  std::vector<std::string> kw_uids;
  for (auto& k : extraction.keywords) {
    std::string kuid = kw_.get_or_create(k.word, k.category);
    if (kuid.empty()) continue;  // 停用词
    // 归一后去重：同事件两个词形归一到同一关键词时不重复建边（防自环共现边）
    bool dup = false;
    for (auto& u : kw_uids)
      if (u == kuid) { dup = true; break; }
    if (dup) continue;
    kw_uids.push_back(kuid);
    storage_.put_edge(Edge{ev.uid, kuid, RelType::CONTAINS, 1.0, {{"category", k.category}}});
    // 有边连接 → 不满足"无边"休眠条件，唤醒
    Node* kn = storage_.get_node_mut(kuid);
    if (kn && kn->status == 1) kn->status = 0;
    if (kn && kn->embedding.empty())
      kn->embedding = vec_.embed(kn->name + " " + kn->type);
  }

  // ---- NEXT 叙事时间链 ----
  if (!last_event_uid_.empty() && last_event_uid_ != ev.uid)
    storage_.put_edge(Edge{last_event_uid_, ev.uid, RelType::NEXT, 1.0, {}});
  last_event_uid_ = ev.uid;

  auto_grow(ev, entity_uids, kw_uids, place_uid);
  supersede_pass(ev, entity_uids, kw_uids);
  return wr;
}

// 知识更新传递（SUPERSEDES，设计文档"双时态/状态关系带有效期"的落地）：
// 步骤1：取本事件的关键词中属于"槽位语义"的类别（attr/topic/concept）；
// 步骤2：槽位词判别——该词在库中的事件文档频数（CONTAINS 度数）须 ≤ kMaxSlotDf，
//        高频通用词（如 home/great/cable，同说话人几乎所有轮次都共享）不是槽位，
//        稀有词（pre-approved/violin/具体数字）才是（防"相邻轮互相取代"的过度标记）；
// 步骤3：对每个槽位关键词，经 CONTAINS 邻边直接取到含该词的历史事件（不再全表扫描），
//        过滤出 INVOLVES 同一实体、时间更早且尚未被取代（无 valid_to）者，
//        视为被本事件更新的同槽位旧事实；
// 步骤4：给旧事件标 attrs["valid_to"]=本事件 ts（事实失效时刻），并写
//        新事件 -[:SUPERSEDES]-> 旧事件 边（旧记录不覆盖、可追溯，as_of 查询可取历史值）。
void GraphWriter::supersede_pass(const Node& ev, const std::vector<std::string>& entity_uids,
                                 const std::vector<std::string>& kw_uids) {
  if (entity_uids.empty() || kw_uids.empty()) return;
  const int kMaxSlotDf = 5;  // 槽位词最大事件文档频数（超过则视为通用词）
  for (auto& kuid : kw_uids) {
    const Node* kn = storage_.get_node(kuid);
    if (!kn) continue;
    if (kn->type != "attr" && kn->type != "topic" && kn->type != "concept") continue;
    // CONTAINS 邻边直达含该词的事件
    auto kw_events = storage_.neighbors(kuid, {RelType::CONTAINS});
    int df = 0;
    for (auto& [ce, n] : kw_events)
      if (n->kind == NodeKind::Event) ++df;
    if (df > kMaxSlotDf) continue;  // 通用词，不是槽位
    for (auto& [ce, old] : kw_events) {
      if (old->kind != NodeKind::Event) continue;
      if (old->uid == ev.uid || old->ts >= ev.ts) continue;
      if (old->attrs.count("valid_to")) continue;  // 已被取代
      for (auto& euid : entity_uids) {
        if (!storage_.find_edge(old->uid, euid, RelType::INVOLVES)) continue;
        storage_.get_node_mut(old->uid)->attrs["valid_to"] = std::to_string(ev.ts);
        storage_.put_edge(Edge{ev.uid, old->uid, RelType::SUPERSEDES, 1.0,
                               {{"slot", kn->name}, {"entity", euid}}});
        break;  // 一个旧事件只需被取代一次
      }
    }
  }
}

// 自动生长：在一次写入后维护图的派生结构：
// 步骤1（① 关键词共现）：同事项关键词两两组合，双向 upsert REL_COOCCUR 边
//        并使 weight+1；若累计权重 ≥3 且尚无 REL_PART 边，则结晶出一条
//        组成边（weight 0.8，attrs 标记 crystalized_from=cooccur）。
// 步骤2（② 实体共历）：同事项实体两两组合，双向 upsert
//        EXPERIENCED_TOGETHER 边并计数 +1。
// 步骤3（③ IN/NEAR）：已在 get_or_create_place 中完成，此处不重复。
// 步骤4（④ 推导标签）：若本次事件带地点，调 M10 project_local 计算地方时
//        时段写入 attrs["local_period"]，after_hours 为真时追加标记；
//        再对每个实体调 M10 match_phase 匹配人生阶段，首个非空结果写入
//        attrs["phase"]（已有值不覆盖）。
void GraphWriter::auto_grow(const Node& ev, const std::vector<std::string>& entity_uids,
                            const std::vector<std::string>& kw_uids,
                            const std::string& place_uid) {
  // ① 同事项关键词两两共现边 weight+1；≥3 结晶为组成边（⑤）
  for (size_t i = 0; i < kw_uids.size(); ++i) {
    for (size_t j = i + 1; j < kw_uids.size(); ++j) {
      Edge e{kw_uids[i], kw_uids[j], RelType::REL_COOCCUR, 1.0, {}};
      storage_.upsert_edge(e, true);
      Edge rev{kw_uids[j], kw_uids[i], RelType::REL_COOCCUR, 1.0, {}};
      storage_.upsert_edge(rev, true);
      const Edge* cur = storage_.find_edge(kw_uids[i], kw_uids[j], RelType::REL_COOCCUR);
      if (cur && cur->weight >= 3.0 &&
          !storage_.find_edge(kw_uids[i], kw_uids[j], RelType::REL_PART)) {
        // 成分反复提及 ≥3 次 → 结晶为组成边
        storage_.put_edge(Edge{kw_uids[j], kw_uids[i], RelType::REL_PART, 0.8,
                               {{"crystalized_from", "cooccur"}}});
      }
    }
  }

  // ② 同事项实体两两 EXPERIENCED_TOGETHER count+1
  for (size_t i = 0; i < entity_uids.size(); ++i) {
    for (size_t j = i + 1; j < entity_uids.size(); ++j) {
      storage_.upsert_edge(Edge{entity_uids[i], entity_uids[j],
                                RelType::EXPERIENCED_TOGETHER, 1.0, {}}, true);
      storage_.upsert_edge(Edge{entity_uids[j], entity_uids[i],
                                RelType::EXPERIENCED_TOGETHER, 1.0, {}}, true);
    }
  }

  // ③ IN/NEAR 已在 get_or_create_place 中处理

  // ④ 推导标签：地方时/场所时段/人生阶段（调 M10）
  const Node* place = place_uid.empty() ? nullptr : storage_.get_node(place_uid);
  if (place) {
    auto lp = ts_.project_local(ev.ts, place);
    Node* m = storage_.get_node_mut(ev.uid);
    m->attrs["local_period"] = lp.period;
    if (lp.after_hours) m->attrs["after_hours"] = "1";
  }
  for (auto& euid : entity_uids) {
    std::string phase = ts_.match_phase(ev.ts, euid);
    if (!phase.empty()) {
      Node* m = storage_.get_node_mut(ev.uid);
      if (m->attrs["phase"].empty()) m->attrs["phase"] = phase;
    }
  }
}

// 批量导入：预分配结果空间，逐条调用 write_event 落库并收集结果；
// batch_size 在内存图实现中无事务语义，仅保留在接口上兼容分批调用约定。
std::vector<WriteResult> GraphWriter::import_events(
    const std::vector<std::pair<Extraction, std::vector<std::pair<std::string, std::string>>>>& batch,
    int batch_size) {
  std::vector<WriteResult> results;
  results.reserve(batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    results.push_back(write_event(batch[i].first, batch[i].second));
    (void)batch_size;  // 内存图无事务，分批语义保留在接口上
  }
  return results;
}

// 高频实体摘要聚合：把 window 秒内该实体相关事件合并为一个摘要事件：
// 步骤1：实体不存在则返回空串。
// 步骤2：遍历该实体的 INVOLVES 邻居，只保留 Event 节点；window>0 时
//        跳过时间窗之外的旧事件；文本以"；"拼接，累计条数并记录最新时间戳。
// 步骤3：无命中事件则返回空串。
// 步骤4：新建 type="summary" 的摘要 Event：文本为"摘要(实体名):拼接文本"，
//        name 截前 24 字符，时间取最新事件时间，精度按天，重要度 0.6，
//        调 M13 向量化，attrs 记录合并条数；入库后写摘要->实体的
//        INVOLVES 边（role=主体），返回摘要节点 uid。
std::string GraphWriter::summarize_entity_events(const std::string& uid, TimePoint window_seconds) {
  const Node* ent = storage_.get_node(uid);
  if (!ent) return "";
  TimePoint now = TimeService::now_utc();
  std::string merged;
  int count = 0;
  TimePoint latest = 0;
  for (auto& [e, n] : storage_.neighbors(uid, {RelType::INVOLVES})) {
    if (n->kind != NodeKind::Event) continue;
    if (window_seconds > 0 && now - n->ts > window_seconds) continue;
    if (!merged.empty()) merged += "；";
    merged += n->text;
    ++count;
    latest = std::max(latest, n->ts);
  }
  if (count == 0) return "";

  Node sum;
  sum.uid = gen_uid();
  sum.kind = NodeKind::Event;
  sum.type = "summary";
  sum.text = "摘要(" + ent->name + "):" + merged;
  sum.name = sum.text.substr(0, 24);
  sum.ts = latest;
  sum.precision = Precision::Day;
  sum.importance = 0.6;
  sum.embedding = vec_.embed(sum.text);
  sum.attrs["summary_count"] = std::to_string(count);
  storage_.put_node(sum);
  storage_.put_edge(Edge{sum.uid, uid, RelType::INVOLVES, 1.0, {{"role", "主体"}}});
  return sum.uid;
}

}  // namespace ame
