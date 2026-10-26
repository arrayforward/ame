// AME 演示：中文样例数据走 remember → lookup → recall(带 explain)
#include <cstdio>

#include "ame/engine/engine.h"

using namespace ame;

static void hr(const char* title) { std::printf("\n========== %s ==========\n", title); }

int main() {
  Engine e("/tmp/ame_demo");

  hr("1. remember：写入一批中文记忆");
  const char* texts[] = {
      "老张和老王昨天在朝阳公园附近的咖啡馆谈项目，压力很大",
      "老张今天在公司加班到深夜，压力大",
      "老张最近总是睡不着，整夜失眠",
      "老张前天在朝阳公园跑步，心情舒畅",
      "老李上个月在三里屯聚餐，很开心",
      "老张的车在望京爆胎了，去维修花了两千块",
  };
  for (auto* t : texts) {
    auto r = e.remember(t);
    std::printf("[remember] %s\n  -> event=%s", t, r.event_id.c_str());
    for (auto& [surf, uid] : r.resolved_entities)
      std::printf("  %s=%s", surf.c_str(), uid.c_str());
    std::printf("\n");
  }
  // 找到「有事件的老张」（区别于后文注册的另一个手机号老张），供 lookup 演示
  const Node* lz_node = nullptr;
  for (auto* n : e.storage().nodes_of_kind(NodeKind::Entity)) {
    if (n->name == "老张" && !e.storage().neighbors(n->uid, {RelType::INVOLVES}).empty()) {
      lz_node = n;
      break;
    }
  }
  // 领域知识：因果链（冷启动灌库，生产中来自 ConceptNet/LLM）
  e.keywords().link_related("压力", "失眠", RelType::REL_CAUSE, 1.0);
  e.keywords().link_related("加班", "压力", RelType::REL_CAUSE, 1.0);
  e.keywords().link_related("爆胎", "维修", RelType::REL_INFER, 1.0);
  std::printf("[知识] 已灌入因果边：压力→失眠，加班→压力，爆胎→维修\n");

  // 重名消歧演示：另一个"老张"（不同手机号 → 强标识建档）
  std::string lz2 = e.entities().register_entity("person", "老张", {{"phone", "13811112222"}});
  std::printf("[消歧] 注册了另一个老张（手机号强标识）：%s\n", lz2.c_str());

  hr("2. lookup：精确查找");
  LookupQuery lq;
  lq.entity_uids = {lz_node->uid};
  lq.order_by = "time";
  std::printf("「老张」参与的事件（按时间倒序）：\n");
  for (auto* n : e.lookup(lq)) std::printf("  - %s\n", n->text.c_str());

  LookupQuery lq2;
  lq2.keywords = {"睡不着"};  // 归一 → 失眠
  std::printf("关键词「睡不着」（归一为失眠）的事件：\n");
  for (auto* n : e.lookup(lq2)) std::printf("  - %s\n", n->text.c_str());

  hr("3. recall：联想检索（软→硬→软，带 explain）");
  SeedQuery sq;
  sq.text = "压力";
  auto rr = e.recall(sq, {}, 2, 5, true);
  std::printf("查询「压力」的联想结果 top5：\n");
  for (auto& se : rr.events) {
    std::printf("  [%.3f] %s\n", se.score, se.event->text.c_str());
    std::printf("         (图%.2f 语境%.2f 时%.2f) 路径: %s\n", se.graph_score,
                se.context_score, se.time_score, se.explain.c_str());
  }

  std::printf("\n查询「车坏了怎么办」的联想结果 top5：\n");
  SeedQuery sq2;
  sq2.text = "车坏了怎么办";
  auto rr2 = e.recall(sq2, {}, 2, 5, true);
  for (auto& se : rr2.events)
    std::printf("  [%.3f] %s\n         路径: %s\n", se.score, se.event->text.c_str(),
                se.explain.c_str());

  hr("5. 快照 save/load");
  std::string snap = "/tmp/ame_demo_snapshot.json";
  if (e.save(snap) == Err::Ok) {
    Engine e2;
    if (e2.load(snap) == Err::Ok)
      std::printf("快照往返：节点 %zu → %zu，边 %zu → %zu\n",
                  e.storage().node_count(), e2.storage().node_count(),
                  e.storage().edge_count(), e2.storage().edge_count());
  }

  return 0;
}
