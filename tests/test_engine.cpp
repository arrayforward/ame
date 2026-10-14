/*
 * M12 engine 单元测试（网关层）：register/remember/lookup/merge/split 动词与错误码
 */
#include "ame/engine/engine.h"
#include "test_util.hpp"

using namespace ame;

static void test_lookup() {
  Engine e;
  e.remember("老张昨天在朝阳公园跑步");
  e.remember("老张今天在公司加班，压力很大");
  e.remember("老张晚上总是睡不着");

  // lookup by keyword
  LookupQuery lq;
  lq.keywords = {"跑步"};
  auto r1 = e.lookup(lq);
  CHECK(r1.size() == 1);
  // lookup by entity
  const Node* ent = e.storage().find_by_name(NodeKind::Entity, "老张");
  CHECK(ent != nullptr);
  LookupQuery lq2;
  lq2.entity_uids = {ent->uid};
  CHECK(e.lookup(lq2).size() == 3);
  // lookup by time
  LookupQuery lq3;
  lq3.time_from = TimeService::now_utc() - 3600;
  CHECK(e.lookup(lq3).size() >= 2);
}

static void test_gateway_verbs() {
  Engine e;
  // register
  std::string uid = e.register_entity("person", "老张", {{"phone", "13800000009"}});
  CHECK(!uid.empty());
  // remember 空文本 → 统一错误码
  auto bad = e.remember("");
  CHECK(bad.code == Err::InvalidArg);
  // recall 无种子命中 → NotFound
  SeedQuery sq;
  sq.text = "";
  auto rr = e.recall(sq);
  CHECK(rr.code == Err::NotFound);
  // merge / split 经网关
  std::string a = e.register_entity("person", "老王");
  std::string b = e.register_entity("person", "老王");
  CHECK(e.merge_entities(a, b) == Err::Ok);
  auto card = e.entities().get_entity(a);
  CHECK(card.uid == b);
}

// 引擎统计接口：turns/events=事件数、total_chars=事件文本字符合计、nodes=图节点总数
static void test_engine_stats() {
  Engine e;
  auto s0 = e.stats();
  CHECK(s0.turns == 0 && s0.events == 0 && s0.total_chars == 0 && s0.nodes == 0);
  e.remember_turn("user", "我今天去爬山了", 1700000000, "D1:1", "s1");
  e.remember_turn("assistant", "天气不错吧", 1700000060, "D1:2", "s1");
  auto s1 = e.stats();
  CHECK(s1.turns == 2 && s1.events == 2);
  CHECK(s1.total_chars == (long)(std::string("我今天去爬山了").size() +
                                 std::string("天气不错吧").size()));
  CHECK(s1.nodes > 2);  // 事件 + speaker 实体 + 关键词节点
}

int main() {
  test_lookup();
  test_gateway_verbs();
  test_engine_stats();
  return AME_TEST_REPORT();
}
