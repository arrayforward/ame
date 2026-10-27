/*
 * 4.0：PostgreSQL 后端对照测试（AME_WITH_POSTGRES 时才编译注册）。
 * 环境变量 AME_PG_CONN 提供连接串（如 host=db dbname=ame user=postgres password=ame）；
 * 未设置时打印 SKIP 并返回 0（无库环境不阻断 ctest）。
 * 覆盖：节点/边写读、名字/属性索引、邻居反向视角、权重累加、ann_search（pgvector）、
 *       词法候选下推、快照导出→另一 ns 导入往返、clear_ns。
 */
#include "ame/storage/storage.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ame;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                \
    }                                                            \
  } while (0)

static Vec vec128(float a, float b) {
  Vec v(128, 0.0f);
  v[0] = a;
  v[1] = b;
  return v;
}

int main() {
  const char* conn = std::getenv("AME_PG_CONN");
  if (!conn || !*conn) {
    std::printf("SKIP test_pg_storage：未设置 AME_PG_CONN\n");
    return 0;
  }
  Storage st;
  CHECK(st.pg_active() == false);  // 默认内存模式
  CHECK(st.connect_pg(conn, "test_pg") == Err::Ok);
  CHECK(st.pg_active());
  CHECK(st.from_json(Json::object()) == Err::Ok);  // 清 ns

  // 节点：2 事件（含向量/文本）+ 1 实体（强标识符）
  Node e1; e1.uid = "ev1"; e1.kind = NodeKind::Event; e1.text = "caroline moved to Paris";
  e1.name = "e1"; e1.ts = 100; e1.embedding = vec128(1, 0);
  Node e2; e2.uid = "ev2"; e2.kind = NodeKind::Event; e2.text = "jon adopted a dog";
  e2.name = "e2"; e2.ts = 200; e2.embedding = vec128(0, 1);
  Node p1; p1.uid = "p1"; p1.kind = NodeKind::Entity; p1.type = "person"; p1.name = "Caroline";
  p1.attrs["id:passport"] = "X123";
  CHECK(st.put_node(e1) == Err::Ok);
  CHECK(st.put_node(e2) == Err::Ok);
  CHECK(st.put_node(p1) == Err::Ok);
  CHECK(st.node_count() == 3);

  const Node* g = st.get_node("ev1");
  CHECK(g && g->text == "caroline moved to Paris" && g->embedding.size() == 128);
  CHECK(st.find_by_name(NodeKind::Entity, "Caroline") != nullptr);
  CHECK(st.find_by_attr(NodeKind::Entity, "id:passport", "X123") != nullptr);
  CHECK(st.find_by_attr(NodeKind::Entity, "id:passport", "NOPE") == nullptr);
  CHECK(st.nodes_of_kind(NodeKind::Event).size() == 2);

  // 边：写读 + 权重累加 + 邻居反向视角
  CHECK(st.put_edge(Edge{"ev1", "p1", RelType::INVOLVES, 1.0, {{"role", "主体"}}}) == Err::Ok);
  CHECK(st.upsert_edge(Edge{"ev1", "p1", RelType::INVOLVES, 0.5, {}}, true) == Err::Ok);
  CHECK(st.put_edge(Edge{"ev1", "evX", RelType::NEXT, 1.0, {}}) == Err::NotFound);  // 端点不存在
  const Edge* fe = st.find_edge("ev1", "p1", RelType::INVOLVES);
  CHECK(fe && fe->weight > 1.49 && fe->weight < 1.51);
  CHECK(st.edge_count() == 1);
  auto nb = st.neighbors("p1");
  CHECK(nb.size() == 1);
  if (nb.size() == 1) {
    CHECK(nb[0].second->uid == std::string("ev1"));
    CHECK(nb[0].first.from == "p1" && nb[0].first.to == "ev1");       // 反向视角
    CHECK(nb[0].first.attrs.count("_reversed") == 1);
  }

  // ann_search：查询向量 [1,0] 最近 ev1（ev2 正交 sim=0 被 >0 过滤，仅 1 条）
  auto ann = st.ann_search(vec128(1, 0), 2);
  CHECK(ann.size() == 1 && ann[0].first == "ev1" && ann[0].second > 0.99);

  // 词法候选下推：OR ILIKE，含扩展词
  auto lex = st.lexical_event_candidates({"paris", "dog"}, 10);
  CHECK(lex.size() == 2);
  auto lex1 = st.lexical_event_candidates({"paris"}, 10);
  CHECK(lex1.size() == 1 && lex1[0]->uid == std::string("ev1"));
  CHECK(st.lexical_event_candidates({"zzz_nothing"}, 10).empty());

  // 快照导出 → 另一 ns 导入往返
  Json snap = st.to_json();
  Storage st2;
  CHECK(st2.connect_pg(conn, "test_pg2") == Err::Ok);
  CHECK(st2.from_json(snap) == Err::Ok);
  CHECK(st2.node_count() == 3 && st2.edge_count() == 1);
  CHECK(st2.find_by_name(NodeKind::Entity, "Caroline") != nullptr);
  CHECK(st2.from_json(Json::object()) == Err::Ok);  // clear_ns 同语义
  CHECK(st2.node_count() == 0);

  CHECK(st.from_json(Json::object()) == Err::Ok);
  CHECK(st.node_count() == 0);

  if (failures == 0) std::printf("PASS test_pg_storage（全部对照断言通过）\n");
  return failures == 0 ? 0 : 1;
}
