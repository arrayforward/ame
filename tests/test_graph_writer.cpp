/*
 * M4 graph_writer 单元测试：写入即生长——共现边/共同经历边/NEAR 边/IN 边/推导标签/关键词归一
 * （经 M12 Engine 写入，断言落在 M4 的自动生长产物上）
 */
#include "ame/engine/engine.h"
#include "ame/graph_writer/graph_writer.h"
#include "test_util.hpp"

using namespace ame;

static void test_autogrow() {
  Engine e;
  auto r1 = e.remember("老张和老王今天在朝阳公园跑步，压力很大");
  CHECK(r1.code == Err::Ok);
  auto r2 = e.remember("老张今天在公司加班，压力让人焦虑");
  CHECK(r2.code == Err::Ok);
  auto r3 = e.remember("老王今天在咖啡馆谈项目");
  CHECK(r3.code == Err::Ok);

  Storage& s = e.storage();
  // ① 共现边：压力↔焦虑 同事件共现 weight>=1
  const Node* kPress = e.keywords().find("压力");
  const Node* kAnx = e.keywords().find("焦虑");
  CHECK(kPress && kAnx);
  const Edge* co = s.find_edge(kPress->uid, kAnx->uid, RelType::REL_COOCCUR);
  CHECK(co && co->weight >= 1.0);

  // ② 共同经历边：老张↔老王 count>=1
  std::string lz = r1.resolved_entities[0].second;
  std::string lw;
  for (auto& [surf, uid] : r1.resolved_entities)
    if (surf == "老王") lw = uid;
  CHECK(!lz.empty() && !lw.empty());
  const Edge* et = s.find_edge(lz, lw, RelType::EXPERIENCED_TOGETHER);
  CHECK(et && et->weight >= 1.0);

  // ③ NEAR 边：朝阳公园 ↔ 咖啡馆 (<500m)
  const Node* p1 = s.find_by_name(NodeKind::Place, "朝阳公园");
  const Node* p2 = s.find_by_name(NodeKind::Place, "咖啡馆");
  CHECK(p1 && p2);
  CHECK(s.find_edge(p1->uid, p2->uid, RelType::NEAR) != nullptr);
  // IN 边：Place → Region(geohash 前缀)
  CHECK(!s.neighbors(p1->uid, {RelType::IN}).empty());

  // ④ 推导标签：事件带 local_period
  const Node* ev1 = s.get_node(r1.event_id);
  CHECK(ev1 && ev1->attrs.count("local_period"));

  // 关键词归一写入：「睡不着」→「失眠」
  auto r4 = e.remember("老张最近总是睡不着");
  CHECK(r4.code == Err::Ok);
  const Node* kIns = e.keywords().find("失眠");
  CHECK(kIns != nullptr);
  CHECK(s.find_edge(r4.event_id, kIns->uid, RelType::CONTAINS) != nullptr);
}

static void test_summarize() {
  // 直接构造 M4 各依赖，验证摘要聚合接口
  Storage s;
  KeywordManager kw(s);
  TimeService ts(&s);
  VectorService vs(&s);
  GraphWriter gw(s, kw, ts, vs);

  Node ent;
  ent.uid = "ent1"; ent.kind = NodeKind::Entity; ent.name = "老张"; ent.type = "person";
  s.put_node(ent);

  Extraction ex1, ex2;
  ex1.event.text = "老张今天在朝阳公园跑步";
  ex1.event.time = TimeService::now_utc();
  ex2.event.text = "老张昨天在公司加班";
  ex2.event.time = TimeService::now_utc() - 86400;
  gw.write_event(ex1, {{"老张", "ent1"}});
  gw.write_event(ex2, {{"老张", "ent1"}});

  std::string sum_id = gw.summarize_entity_events("ent1", 0);  // 0 = 不限窗口
  CHECK(!sum_id.empty());
  const Node* sum = s.get_node(sum_id);
  CHECK(sum && sum->type == "summary");
  auto cnt = sum->attrs.find("summary_count");
  CHECK(cnt != sum->attrs.end() && cnt->second == "2");
  CHECK(s.find_edge(sum_id, "ent1", RelType::INVOLVES) != nullptr);
}

int main() {
  test_autogrow();
  test_summarize();
  return AME_TEST_REPORT();
}
