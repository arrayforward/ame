/*
 * M13 vector 单元测试：哈希 embedding 确定性与归一化、compose/edge_vec、
 * 以及 M9 暴力余弦 ANN 检索（embedding → ann_search topK）
 */
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"
#include "test_util.hpp"

using namespace ame;

static void test_embed_deterministic() {
  VectorService vs(nullptr);
  Vec a = vs.embed("老张在朝阳公园跑步");
  Vec b = vs.embed("老张在朝阳公园跑步");
  CHECK(a.size() == 128);
  CHECK(a == b);  // 确定性（含缓存路径一致）
  // L2 归一化
  double norm = 0;
  for (float f : a) norm += (double)f * f;
  CHECK_NEAR(norm, 1.0, 1e-4);
  // 中文支持：不同文本向量不同
  Vec c = vs.embed("整夜失眠无法入睡");
  CHECK(a != c);
  // 共享字符越多越相似
  Vec d = vs.embed("老张在朝阳公园散步");
  CHECK(Storage::cosine(a, d) > Storage::cosine(a, c));
}

static void test_compose_and_edge_vec() {
  VectorService vs(nullptr);
  Vec red = vs.embed("红色");
  Vec shop = vs.embed("店");
  Vec composed = vs.compose({red, shop}, "add");
  CHECK(composed.size() == 128);
  double norm = 0;
  for (float f : composed) norm += (double)f * f;
  CHECK_NEAR(norm, 1.0, 1e-4);
  Vec ev = vs.edge_vec(red, shop);
  CHECK(ev.size() == 128);
  CHECK_NEAR((double)ev[0], (double)(shop[0] - red[0]), 1e-6);
}

static void test_ann_cosine() {
  Storage s;
  VectorService vs(&s);
  Node a, b;
  a.uid = "a"; a.kind = NodeKind::Event; a.text = "老张在朝阳公园跑步";
  a.embedding = vs.embed(a.text);
  b.uid = "b"; b.kind = NodeKind::Event; b.text = "彻底无关的内容xyz";
  b.embedding = vs.embed(b.text);
  s.put_node(a); s.put_node(b);
  auto r = s.ann_search(vs.embed("老张在朝阳公园跑步"), 2);
  CHECK(!r.empty() && r[0].first == "a");
  CHECK_NEAR(r[0].second, 1.0, 1e-4);
  // 余弦值域
  for (auto& [uid, sim] : r) CHECK(sim > 0 && sim <= 1.0001);
}

int main() {
  test_embed_deterministic();
  test_compose_and_edge_vec();
  test_ann_cosine();
  return AME_TEST_REPORT();
}
