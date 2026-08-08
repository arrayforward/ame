/*
 * M9 storage 单元测试：内存邻接表（出/入边）、按类型过滤、JSON 快照 save/load 往返一致
 */
#include "ame/storage/storage.h"
#include "test_util.hpp"

using namespace ame;

static void test_adjacency_and_snapshot() {
  Storage s;
  Node a, b, c;
  a.uid = "n1"; a.kind = NodeKind::Entity; a.name = "老张"; a.type = "person";
  b.uid = "n2"; b.kind = NodeKind::Event; b.text = "爆胎了"; b.ts = 1700000000;
  b.embedding = {1, 2, 3};
  c.uid = "n3"; c.kind = NodeKind::Place; c.name = "望京";
  c.lat = 39.9965; c.lng = 116.4730; c.geohash = geohash_encode(c.lat, c.lng, 6);
  s.put_node(a); s.put_node(b); s.put_node(c);
  s.put_edge(Edge{"n2", "n1", RelType::INVOLVES, 1.0, {{"role", "人物"}}});
  s.put_edge(Edge{"n2", "n3", RelType::HAPPENED_AT, 1.0, {}});

  // 邻接表：正向 + 反向
  CHECK(s.neighbors("n2").size() == 2);
  CHECK(s.neighbors("n1").size() == 1);
  CHECK(s.neighbors("n1", {RelType::INVOLVES}).size() == 1);
  CHECK(s.neighbors("n1", {RelType::NEXT}).empty());
  CHECK(s.get_node("n1")->name == "老张");
  CHECK(s.get_node("nope") == nullptr);

  // 快照 save/load 往返一致
  std::string path = "/tmp/ame_test_snapshot.json";
  CHECK(s.save(path) == Err::Ok);
  Storage s2;
  CHECK(s2.load(path) == Err::Ok);
  CHECK(s2.node_count() == s.node_count());
  CHECK(s2.edge_count() == s.edge_count());
  const Node* b2 = s2.get_node("n2");
  CHECK(b2 && b2->text == "爆胎了" && b2->ts == 1700000000);
  CHECK(b2->embedding.size() == 3 && b2->embedding[2] == 3.0f);
  CHECK(s2.find_edge("n2", "n1", RelType::INVOLVES) != nullptr);
  const Node* c2 = s2.get_node("n3");
  CHECK(c2 && c2->geohash == c.geohash);
}

static void test_upsert_and_geo() {
  Storage s;
  Node p1, p2;
  p1.uid = "p1"; p1.kind = NodeKind::Place; p1.name = "朝阳公园";
  p1.lat = 39.9449; p1.lng = 116.4781;
  p2.uid = "p2"; p2.kind = NodeKind::Place; p2.name = "咖啡馆";
  p2.lat = 39.9455; p2.lng = 116.4790;
  s.put_node(p1); s.put_node(p2);

  // upsert 累加权重
  s.upsert_edge(Edge{"p1", "p2", RelType::NEAR, 1.0, {}}, true);
  s.upsert_edge(Edge{"p1", "p2", RelType::NEAR, 1.0, {}}, true);
  CHECK_NEAR(s.find_edge("p1", "p2", RelType::NEAR)->weight, 2.0, 1e-9);
  CHECK(s.edge_count() == 1);  // 同键边合并，不重复

  // geo_lookup 暴力距离
  CHECK(s.geo_lookup(39.945, 116.478, 500.0).size() == 2);
  CHECK(s.geo_lookup(40.1, 116.5, 50.0).empty());  // 远处无结果
  // geohash 前缀一致（同区域）
  CHECK(geohash_encode(p1.lat, p1.lng, 6).substr(0, 4) ==
        geohash_encode(p2.lat, p2.lng, 6).substr(0, 4));
}

int main() {
  test_adjacency_and_snapshot();
  test_upsert_and_geo();
  return AME_TEST_REPORT();
}
