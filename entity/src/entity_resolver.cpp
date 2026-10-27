/*
 * 模块定位：M2 实体解析器（Entity Resolver）——Mention → Entity 的消歧归并
 *           与实体 uid 生命周期管理（注册、别名、合并、拆分、旧号跳转）的实现。
 *
 * 本文件主体思路：实现 EntityResolver 的全部接口。核心入口 resolve() 按三级
 *   定位策略逐级降级完成消歧：第 1 级 identifiers 含 uid 时经跳转表直连，置信
 *   度 1.0；第 2 级有强标识符时按 "id:键" 属性精确反查，查不到则以该标识符建档
 *   新实体；第 3 级仅有裸名字时，先按实体名 / 别名节点召回候选，再用上下文证据
 *   （共同经历、空间一致性、唯一候选）加权投票，分数 >0.9 直连，0.5~0.9 取最优
 *   候选但标记待确认，<0.5 新建实体并标记疑似重复。每次提及均落一个 Mention
 *   节点并以 REFERS_TO 边挂到实体上。merge/split/redirect 维护合并后的跳转
 *   关系，保证旧 uid 永不复用。
 *
 * 关键算法/数据结构：
 *   - 三级消歧流水线 + 证据加权投票（基础 0.4，共同经历 +0.3，空间一致 +0.2，
 *     唯一候选 +0.1，上限截断 1.0）；
 *   - redirect_ 跳转表（unordered_map），redirect() 沿链跳转且限 16 跳防环；
 *   - 存储层基于邻接表遍历（neighbors / find_edge / find_by_attr），实体与别名
 *     均以图节点表示、以关系边连接；无其他复杂算法。
 *
 * 依赖关系：依赖 core 公共层（types.h 基础类型、uid.h 的 gen_uid）；依赖 M9
 *   存储引擎 Storage 读写节点/边/属性索引；依赖 M13 向量服务 VectorService
 *   为新实体生成 embedding。本模块被 M12 API 网关、M8 思考编排器等调用。
 */
#include "ame/entity/entity_resolver.h"

#include "ame/core/uid.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"

#include <algorithm>

namespace ame {

EntityResolver::EntityResolver(Storage& storage, VectorService& vec)
    : storage_(storage), vec_(vec) {}

// 旧 uid 跳转：沿 redirect_ 链逐级查新 uid，最多 16 跳防环；无跳转则原样返回
std::string EntityResolver::redirect(const std::string& uid) const {
  std::string u = uid;
  for (int i = 0; i < 16; ++i) {  // 防环
    auto it = redirect_.find(u);
    if (it == redirect_.end()) break;
    u = it->second;
  }
  return u;
}

// 注册新实体
// 伪代码：
//   步骤1：生成新 uid，构造 kind=Entity 的节点，写入名字与类型
//   步骤2：调用向量服务对 "名字+类型" 生成 embedding 存入节点
//   步骤3：把强标识符逐条以 "id:键" 前缀写入节点 attrs（供精确反查）
//   步骤4：节点落库；随后逐个注册别名（Alias 节点 + ALIAS_OF 边）
//   步骤5：返回新实体 uid
std::string EntityResolver::register_entity(const std::string& type, const std::string& name,
                                            const Attrs& identifiers,
                                            const std::vector<std::string>& aliases) {
  Node n;
  n.uid = gen_uid();
  n.kind = NodeKind::Entity;
  n.name = name;
  n.type = type;
  n.embedding = vec_.embed(name + " " + type);
  for (auto& kv : identifiers) n.attrs["id:" + kv.first] = kv.second;
  storage_.put_node(n);
  for (auto& a : aliases) add_alias(n.uid, a);
  return n.uid;
}

// 为实体添加别名
// 伪代码：
//   步骤1：先经跳转表解析真实 uid；若实体不存在返回 NotFound
//   步骤2：新建 kind=Alias 节点，name 为别名表面形式，attrs["target"] 记录目标实体
//   步骤3：Alias 节点落库，并建一条 Alias -> Entity 的 ALIAS_OF 边（权重 1.0）
Err EntityResolver::add_alias(const std::string& uid, const std::string& surface) {
  std::string real = redirect(uid);
  if (!storage_.get_node(real)) return Err::NotFound;
  Node a;
  a.uid = gen_uid();
  a.kind = NodeKind::Alias;
  a.name = surface;
  a.attrs["target"] = real;
  storage_.put_node(a);
  return storage_.put_edge(Edge{a.uid, real, RelType::ALIAS_OF, 1.0, {}});
}

// 按强标识符反查实体
// 伪代码：
//   步骤1：逐个遍历标识符键值对
//   步骤2：若键为 "uid"，经跳转表解析后校验节点存在，存在则直接返回该 uid
//   步骤3：否则按 "id:键" 在存储层做属性精确查找，命中则返回跳转后的真实 uid
//   步骤4：全部标识符均未命中时返回空串
std::string EntityResolver::find_entity(const Attrs& identifiers) const {
  for (auto& kv : identifiers) {
    if (kv.first == "uid") {
      std::string u = redirect(kv.second);
      if (storage_.get_node(u)) return u;
      continue;
    }
    const Node* n = storage_.find_by_attr(NodeKind::Entity, "id:" + kv.first, kv.second);
    if (n) return redirect(n->uid);
  }
  return "";
}

// 创建一次提及（Mention 节点）并挂到实体上
// 伪代码：
//   步骤1：新建 kind=Mention 节点，name 为提及表面形式，落库
//   步骤2：建 Mention -> Entity 的 REFERS_TO 边，权重为置信度，
//          边 attrs 里冗余存一份 confidence 字符串
//   步骤3：返回 Mention 节点 uid
std::string EntityResolver::create_mention(const std::string& surface,
                                           const std::string& entity_uid, double confidence) {
  Node m;
  m.uid = gen_uid();
  m.kind = NodeKind::Mention;
  m.name = surface;
  storage_.put_node(m);
  storage_.put_edge(Edge{m.uid, entity_uid, RelType::REFERS_TO, confidence,
                         {{"confidence", std::to_string(confidence)}}});
  return m.uid;
}

// 消歧主入口：三级定位，把一次表面提及归并到某个实体
// 伪代码：
//   步骤1【第 1 级 uid 直连】：若 identifiers 含 "uid"，经跳转表解析并校验节点
//         存在；存在则置信度 1.0 直连，创建 Mention 后返回；uid 无效则降级
//   步骤2【第 2 级 强标识符匹配】：若 identifiers 非空，按强标识符反查：
//         命中则置信度 1.0 直连并返回；未命中则以该标识符直接建档新实体
//         （is_new=true，置信度 1.0）并返回
//   步骤3【第 3 级 候选召回】：仅有裸名字时，先遍历全部 Entity 节点按名字精确
//         匹配收集候选（跳过已合并的旧节点）；若无候选再扫 Alias 节点，按
//         target 找回目标实体作为候选
//   步骤4【无候选】：候选为空则直接新建实体，置信度 0.3，is_new=true，返回
//   步骤5【证据投票】：每个候选基础分 0.4；唯一候选 +0.1；与上下文同事件实体
//         存在 EXPERIENCED_TOGETHER 边（双向检查）每条 +0.3；候选历史事件与
//         上下文地点存在 HAPPENED_AT 边则 +0.2（命中一次即停）；记录最优候选，
//         分数截断到 1.0，候选 uid 列表写入结果
//   步骤6【按分数决策】：>0.9 直连最优候选；0.5~0.9 取最优候选但 pending=true
//         标记待确认；<0.5 新建实体，is_new=true、suspected_dup=true，并在新
//         实体 attrs 里记录 suspected_dup_of 指向最优候选
//   步骤7：以最终置信度创建 Mention 并返回结果
ResolveResult EntityResolver::resolve(const std::string& surface,
                                      const ResolveContext& context,
                                      const Attrs& identifiers) {
  ResolveResult rr;

  // 第 1 级：uid 直连
  auto uid_it = identifiers.find("uid");
  if (uid_it != identifiers.end()) {
    std::string u = redirect(uid_it->second);
    if (storage_.get_node(u)) {
      rr.uid = u;
      rr.confidence = 1.0;
      rr.mention_uid = create_mention(surface, u, 1.0);
      return rr;
    }
    // uid 无效则落入后续流程
  }

  // 第 2 级：强标识符精确匹配，无则建档
  if (!identifiers.empty()) {
    std::string u = find_entity(identifiers);
    if (!u.empty()) {
      rr.uid = u;
      rr.confidence = 1.0;
      rr.mention_uid = create_mention(surface, u, 1.0);
      return rr;
    }
    rr.uid = register_entity("person", surface, identifiers);
    rr.confidence = 1.0;
    rr.is_new = true;
    rr.mention_uid = create_mention(surface, rr.uid, 1.0);
    return rr;
  }

  // 第 3 级：裸名字 + 上下文证据投票（名字索引 O(1) 取候选，不再全表扫描）
  std::vector<const Node*> cands;
  for (auto* n : storage_.find_all_by_name(NodeKind::Entity, surface)) {
    if (redirect(n->uid) != n->uid) continue;  // 已合并的旧节点跳过
    cands.push_back(n);
  }
  // 别名匹配：扫 Alias 节点（名字索引同样适用）
  if (cands.empty()) {
    for (auto* a : storage_.find_all_by_name(NodeKind::Alias, surface)) {
      auto it = a->attrs.find("target");
      if (it != a->attrs.end()) {
        const Node* n = storage_.get_node(redirect(it->second));
        if (n) cands.push_back(n);
      }
    }
  }

  if (cands.empty()) {
    // 无候选：直接新建
    rr.uid = register_entity("person", surface);
    rr.confidence = 0.3;
    rr.is_new = true;
    rr.mention_uid = create_mention(surface, rr.uid, 1.0);
    return rr;
  }

  // 证据投票：基础分 0.4；共同经历 +0.3；空间一致 +0.2；唯一候选 +0.1
  const Node* best = nullptr;
  double best_score = -1;
  for (auto* c : cands) {
    double score = 0.4;
    if (cands.size() == 1) score += 0.1;
    for (auto& co : context.co_entity_uids) {
      if (storage_.find_edge(c->uid, co, RelType::EXPERIENCED_TOGETHER) ||
          storage_.find_edge(co, c->uid, RelType::EXPERIENCED_TOGETHER))
        score += 0.3;
    }
    if (!context.place_uid.empty()) {
      // 空间一致性：候选实体历史事件是否发生在同地点
      for (auto& [e, n] : storage_.neighbors(c->uid)) {
        if (e.type != RelType::INVOLVES || n->kind != NodeKind::Event) continue;
        if (storage_.find_edge(n->uid, context.place_uid, RelType::HAPPENED_AT)) {
          score += 0.2;
          break;
        }
      }
    }
    if (score > best_score) { best_score = score; best = c; }
    rr.candidates.push_back(c->uid);
  }
  if (best_score > 1.0) best_score = 1.0;

  if (best_score > 0.9) {
    rr.uid = best->uid;
    rr.confidence = best_score;
  } else if (best_score >= 0.5) {
    rr.uid = best->uid;
    rr.confidence = best_score;
    rr.pending = true;  // 标记待确认
    Node* m = storage_.get_node_mut(best->uid);
    (void)m;
  } else {
    rr.uid = register_entity("person", surface);
    rr.confidence = best_score;
    rr.is_new = true;
    rr.suspected_dup = true;
    Node* m = storage_.get_node_mut(rr.uid);
    m->attrs["suspected_dup_of"] = best->uid;
    storage_.put_node(*m);  // 4.0：PG 写穿
  }
  rr.mention_uid = create_mention(surface, rr.uid, rr.confidence);
  return rr;
}

// 取实体名片（uid 不存在或不是实体时返回空 card）
// 伪代码：
//   步骤1：经跳转表解析真实 uid，取节点；节点缺失或 kind 非 Entity 则返回空 card
//   步骤2：填入 uid/name/type；attrs 中 "id:" 前缀的条目剥前缀后回填 identifiers
//   步骤3：遍历邻接边：ALIAS_OF 收集别名；INVOLVES 且邻居为 Event 则累计
//          event_count 并刷新 latest_event_ts
EntityCard EntityResolver::get_entity(const std::string& uid) const {
  EntityCard c;
  std::string real = redirect(uid);
  const Node* n = storage_.get_node(real);
  if (!n || n->kind != NodeKind::Entity) return c;
  c.uid = real;
  c.name = n->name;
  c.type = n->type;
  for (auto& kv : n->attrs)
    if (kv.first.rfind("id:", 0) == 0) c.identifiers[kv.first.substr(3)] = kv.second;
  for (auto& [e, nb] : storage_.neighbors(real)) {
    if (e.type == RelType::ALIAS_OF) c.aliases.push_back(nb->name);
    if (e.type == RelType::INVOLVES && nb->kind == NodeKind::Event) {
      c.event_count++;
      if (nb->ts > c.latest_event_ts) c.latest_event_ts = nb->ts;
    }
  }
  return c;
}

// 合并实体：把 a 并入 b，旧 uid 保留跳转
// 伪代码：
//   步骤1：两个 uid 先经跳转表解析；任一节点不存在返回 NotFound，相同返回 InvalidArg
//   步骤2：遍历 a 的全部邻接边（出/入两方向），去掉 "_reversed" 标记后暂存
//   步骤3：在 a 的 attrs 里写 merged_into=b；a 的 "id:" 标识符并入 b（b 已有则保留）
//   步骤4：逐条重挂暂存边：端点为 a 的改成 b，自环边跳过，其余重新落库
//   步骤5：redirect_[a]=b 登记跳转，旧号永不复用；返回 Ok
Err EntityResolver::merge_entities(const std::string& uid_a, const std::string& into_b) {
  std::string a = redirect(uid_a), b = redirect(into_b);
  const Node* na = storage_.get_node(a);
  const Node* nb = storage_.get_node(b);
  if (!na || !nb) return Err::NotFound;
  if (a == b) return Err::InvalidArg;

  // 收集并重指向 a 的所有边（出/入两方向）
  std::vector<Edge> moved;
  for (auto& [e, other] : storage_.neighbors(a)) {
    Edge ne = e;
    ne.attrs.erase("_reversed");
    moved.push_back(ne);
  }
  Node* ma = storage_.get_node_mut(a);
  ma->attrs["merged_into"] = b;
  storage_.put_node(*ma);  // 4.0：PG 写穿
  for (auto& kv : na->attrs) {  // 标识符并入 b
    if (kv.first.rfind("id:", 0) == 0) {
      Node* mb = storage_.get_node_mut(b);
      if (!mb->attrs.count(kv.first)) {
        mb->attrs[kv.first] = kv.second;
        storage_.put_node(*mb);  // 4.0：PG 写穿
      }
    }
  }
  for (auto& e : moved) {
    Edge ne = e;
    if (ne.from == a) ne.from = b;
    if (ne.to == a) ne.to = b;
    if (ne.from == ne.to) continue;
    storage_.put_edge(ne);
  }
  redirect_[a] = b;  // 旧 uid 保留跳转
  return Err::Ok;
}

// 拆分实体：把指定的若干 Mention 从原实体拆出，挂到新建实体上
// 伪代码：
//   步骤1：经跳转表解析真实 uid；节点不存在返回空串
//   步骤2：以原实体的 type/name 注册一个新实体
//   步骤3：逐个校验 mention_ids：存在且 kind 为 Mention 的，补一条指向新实体的
//          REFERS_TO 边（权重 1.0），边 attrs 记录 split_from 来源 uid
//   步骤4：返回新实体 uid
std::string EntityResolver::split_entity(const std::string& uid,
                                         const std::vector<std::string>& mention_ids) {
  std::string real = redirect(uid);
  const Node* n = storage_.get_node(real);
  if (!n) return "";
  std::string new_uid = register_entity(n->type, n->name);
  for (auto& mid : mention_ids) {
    const Node* m = storage_.get_node(mid);
    if (!m || m->kind != NodeKind::Mention) continue;
    storage_.put_edge(Edge{mid, new_uid, RelType::REFERS_TO, 1.0, {{"split_from", real}}});
  }
  return new_uid;
}

// put_slot 伪代码：
//   步骤1：跳转解析 + 存在性校验（仅 Entity 接受槽位）；
//   步骤2：查当前有效槽位（同 slot 且 valid_to==0）：值相同 → 幂等更新来源后返回；
//          值不同 → 旧记录标 valid_to=ts（知识更新，与图层 SUPERSEDES 同语义）；
//   步骤3：追加新记录（valid_from=ts，valid_to=0，记录来源事件）。
Err EntityResolver::put_slot(const std::string& uid, const std::string& slot,
                             const std::string& value, TimePoint ts,
                             const std::string& source_event) {
  std::string real = redirect(uid);
  const Node* n = storage_.get_node(real);
  if (!n || n->kind != NodeKind::Entity) return Err::NotFound;
  if (slot.empty() || value.empty()) return Err::InvalidArg;
  auto& v = slots_[real];
  for (auto& s : v) {
    if (s.slot == slot && s.valid_to == 0) {
      if (s.value == value) {  // 幂等：同值只更新来源
        s.source_event = source_event;
        return Err::Ok;
      }
      s.valid_to = ts;  // 旧值失效
    }
  }
  v.push_back({slot, value, ts, 0, source_event});
  return Err::Ok;
}

// get_profile 伪代码：
//   步骤1：身份层复用 get_entity；
//   步骤2：槽位层取 slots_（merge 跳转后的真实 uid）；
//   步骤3：时间线层——INVOLVES 邻边取事件，按 ts 升序（NEXT 链与写入序在 ts 上一致）。
EntityProfile EntityResolver::get_profile(const std::string& uid) const {
  EntityProfile p;
  std::string real = redirect(uid);
  p.card = get_entity(real);
  auto it = slots_.find(real);
  if (it != slots_.end()) p.slots = it->second;
  std::vector<const Node*> evs;
  for (auto& [e, n] : storage_.neighbors(real, {RelType::INVOLVES}))
    if (n->kind == NodeKind::Event) evs.push_back(n);
  std::sort(evs.begin(), evs.end(), [](const Node* a, const Node* b) { return a->ts < b->ts; });
  for (auto* n : evs) p.timeline.push_back(n->uid);
  return p;
}

// slot_as_of 伪代码：找 valid_from<=as_of 且（valid_to==0 或 valid_to>as_of）的记录。
std::string EntityResolver::slot_as_of(const std::string& uid, const std::string& slot,
                                       TimePoint as_of) const {
  auto it = slots_.find(redirect(uid));
  if (it == slots_.end()) return "";
  for (auto& s : it->second)
    if (s.slot == slot && s.valid_from <= as_of && (s.valid_to == 0 || s.valid_to > as_of))
      return s.value;
  return "";
}

// slots_to_json 伪代码：uid → 槽位记录数组（slot/value/valid_from/valid_to/source_event）。
Json EntityResolver::slots_to_json() const {
  Json root = Json::object();
  for (auto& [uid, recs] : slots_) {
    Json arr = Json::array();
    for (auto& s : recs) {
      Json j = Json::object();
      j["slot"] = Json::string(s.slot);
      j["value"] = Json::string(s.value);
      j["valid_from"] = Json::number((double)s.valid_from);
      j["valid_to"] = Json::number((double)s.valid_to);
      j["src"] = Json::string(s.source_event);
      arr.push(std::move(j));
    }
    root[uid] = std::move(arr);
  }
  return root;
}

// slots_from_json 伪代码：清空后按 uid 逐条还原（宽容：缺字段用默认值）。
void EntityResolver::slots_from_json(const Json& j) {
  slots_.clear();
  if (j.kind != Json::Obj) return;
  for (auto& [uid, arr] : j.obj) {
    if (arr.kind != Json::Arr) continue;
    for (auto& r : arr.arr) {
      SlotRecord s;
      s.slot = r.find("slot") ? r.find("slot")->as_str() : "";
      s.value = r.find("value") ? r.find("value")->as_str() : "";
      s.valid_from = (TimePoint)(r.find("valid_from") ? r.find("valid_from")->as_num() : 0);
      s.valid_to = (TimePoint)(r.find("valid_to") ? r.find("valid_to")->as_num() : 0);
      s.source_event = r.find("src") ? r.find("src")->as_str() : "";
      slots_[uid].push_back(std::move(s));
    }
  }
}

}  // namespace ame
