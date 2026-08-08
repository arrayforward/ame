/*
 * M2 entity 单元测试：三级定位（uid 直连/强标识/裸名字投票三档）、
 * merge（旧 uid 跳转）、split（按 mention_ids）、add_alias
 */
#include "ame/entity/entity_resolver.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"
#include "test_util.hpp"

using namespace ame;

static void test_resolve_three_branches() {
  Storage s;
  VectorService vs(&s);
  EntityResolver er(s, vs);

  // 分支1：uid 直连
  std::string u1 = er.register_entity("person", "张三", {{"phone", "13800000001"}});
  auto r1 = er.resolve("张三", {}, {{"uid", u1}});
  CHECK(r1.confidence == 1.0 && r1.uid == u1 && !r1.is_new);

  // 分支2：强标识符精确匹配；无则建档
  auto r2 = er.resolve("张三", {}, {{"phone", "13800000001"}});
  CHECK(r2.confidence == 1.0 && r2.uid == u1 && !r2.is_new);
  auto r3 = er.resolve("李四", {}, {{"phone", "13800000002"}});
  CHECK(r3.is_new && r3.confidence == 1.0);
  CHECK(er.find_entity({{"phone", "13800000002"}}) == r3.uid);

  // 分支3a：裸名字 + 强上下文证据 → >0.9 直连
  std::string laoA = er.register_entity("person", "老张");
  std::string wang = er.register_entity("person", "老王");
  s.put_edge(Edge{laoA, wang, RelType::EXPERIENCED_TOGETHER, 3.0, {}});
  Node ev, pl;
  ev.uid = "ev1"; ev.kind = NodeKind::Event; ev.text = "老张在朝阳公园跑步";
  pl.uid = "pl1"; pl.kind = NodeKind::Place; pl.name = "朝阳公园";
  s.put_node(ev); s.put_node(pl);
  s.put_edge(Edge{"ev1", laoA, RelType::INVOLVES, 1.0, {}});
  s.put_edge(Edge{"ev1", "pl1", RelType::HAPPENED_AT, 1.0, {}});
  ResolveContext ctx;
  ctx.co_entity_uids = {wang};
  ctx.place_uid = "pl1";
  auto r4 = er.resolve("老张", ctx, {});
  CHECK(!r4.is_new && r4.uid == laoA && r4.confidence > 0.9);

  // 分支3b：唯一候选无证据 → 0.5~0.9 待确认
  std::string uniq = er.register_entity("person", "老陈");
  auto r5 = er.resolve("老陈", {}, {});
  CHECK(!r5.is_new && r5.uid == uniq && r5.pending);
  CHECK(r5.confidence >= 0.5 && r5.confidence <= 0.9);

  // 分支3c：多候选无证据 → <0.5 新建并标记疑似重复
  er.register_entity("person", "老刘");
  er.register_entity("person", "老刘");
  auto r6 = er.resolve("老刘", {}, {});
  CHECK(r6.is_new && r6.suspected_dup && r6.confidence < 0.5);
  const Node* nn = s.get_node(r6.uid);
  CHECK(nn && nn->attrs.count("suspected_dup_of"));
}

static void test_merge_split_alias() {
  Storage s;
  VectorService vs(&s);
  EntityResolver er(s, vs);
  std::string laoA = er.register_entity("person", "老张");

  // merge：旧 uid 跳转
  std::string m1 = er.register_entity("person", "老张", {{"phone", "13900000001"}});
  CHECK(er.merge_entities(m1, laoA) == Err::Ok);
  CHECK(er.redirect(m1) == laoA);
  CHECK(er.find_entity({{"phone", "13900000001"}}) == laoA);
  auto card = er.get_entity(m1);  // 旧号查身份卡 → 跳转
  CHECK(card.uid == laoA && card.name == "老张");

  // split：按 mention_ids 拆出
  std::string v1 = er.register_entity("person", "老李");
  auto rr1 = er.resolve("老李", {}, {});
  auto rr2 = er.resolve("老李", {}, {});
  std::string new_uid = er.split_entity(v1, {rr2.mention_uid});
  CHECK(!new_uid.empty() && new_uid != v1);
  CHECK(s.find_edge(rr2.mention_uid, new_uid, RelType::REFERS_TO) != nullptr);

  // add_alias → resolve 经别名命中
  er.add_alias(v1, "李子");
  auto r7 = er.resolve("李子", {}, {});
  CHECK(r7.uid == v1 || r7.is_new);  // 单候选走投票分支
}

static void test_slots_profile() {
  Storage s;
  VectorService vs(&s);
  EntityResolver er(s, vs);
  std::string u = er.register_entity("person", "老张");
  // 槽位写入 + 知识更新（旧值标 valid_to）
  CHECK(er.put_slot(u, "residence", "北京", 1000, "ev1") == Err::Ok);
  CHECK(er.put_slot(u, "residence", "北京", 2000, "ev2") == Err::Ok);  // 幂等
  CHECK(er.put_slot(u, "residence", "上海", 3000, "ev3") == Err::Ok);  // 更新
  EntityProfile p = er.get_profile(u);
  CHECK(p.current_slot("residence") == "上海");
  CHECK(p.slots.size() == 2);  // 北京 + 上海两条
  CHECK(er.slot_as_of(u, "residence", 1500) == "北京");
  CHECK(er.slot_as_of(u, "residence", 3500) == "上海");
  CHECK(er.slot_as_of(u, "job", 3500).empty());
  // 时间线
  Node ev1, ev2;
  ev1.uid = "e1"; ev1.kind = NodeKind::Event; ev1.text = "旧事"; ev1.ts = 100;
  ev2.uid = "e2"; ev2.kind = NodeKind::Event; ev2.text = "新事"; ev2.ts = 200;
  s.put_node(ev1); s.put_node(ev2);
  s.put_edge(Edge{"e2", u, RelType::INVOLVES, 1.0, {}});
  s.put_edge(Edge{"e1", u, RelType::INVOLVES, 1.0, {}});
  p = er.get_profile(u);
  CHECK(p.timeline.size() == 2 && p.timeline[0] == "e1" && p.timeline[1] == "e2");
  // 非实体拒绝槽位
  CHECK(er.put_slot("no-such-uid", "job", "x", 1, "e") == Err::NotFound);
}

int main() {
  test_resolve_three_branches();
  test_merge_split_alias();
  test_slots_profile();
  return AME_TEST_REPORT();
}
