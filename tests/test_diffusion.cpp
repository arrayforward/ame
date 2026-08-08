/*
 * M6 diffusion 单元测试：能量传导数值、路径记录、休眠关键词隔离、budget 限制
 */
#include "ame/diffusion/diffusion_engine.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "test_util.hpp"

using namespace ame;

static void test_energy_and_paths() {
  Storage s;
  KeywordManager kw(s);
  DiffusionEngine de(s, &kw);
  // 手工建图：A -CAUSE-> B -CONTAINS(反向)-> EV
  Node a, b, ev;
  a.uid = "ka"; a.kind = NodeKind::Keyword; a.name = "压力"; a.status = 0;
  b.uid = "kb"; b.kind = NodeKind::Keyword; b.name = "失眠"; b.status = 0;
  ev.uid = "ev"; ev.kind = NodeKind::Event; ev.text = "整夜失眠";
  s.put_node(a); s.put_node(b); s.put_node(ev);
  s.put_edge(Edge{"ka", "kb", RelType::REL_CAUSE, 1.0, {}});
  s.put_edge(Edge{"ev", "kb", RelType::CONTAINS, 1.0, {}});

  auto dr = de.diffuse({{"ka", 1.0}}, 2);
  // 能量沿因果边传导：ka 出度 1 → kb = 1.0*0.9*1/2/1 = 0.45
  CHECK(dr.count("kb") && std::fabs(dr["kb"].energy - 0.45) < 1e-6);
  // 二跳到事件：kb 出度 2（CAUSE 反向 + CONTAINS 反向），出度归一 → 0.45*0.8*2/3/2 = 0.12
  CHECK(dr.count("ev") && std::fabs(dr["ev"].energy - 0.12) < 1e-6);
  // 路径记录
  CHECK(!dr["ev"].paths.empty());
  CHECK(dr["ev"].paths[0].steps.size() == 2);
  CHECK(dr["ev"].paths[0].steps[0].type == RelType::REL_CAUSE);
}

static void test_dormant_and_budget() {
  Storage s;
  KeywordManager kw(s);
  DiffusionEngine de(s, &kw);
  Node a, b, ev;
  a.uid = "ka"; a.kind = NodeKind::Keyword; a.name = "压力"; a.status = 0;
  b.uid = "kb"; b.kind = NodeKind::Keyword; b.name = "失眠"; b.status = 0;
  ev.uid = "ev"; ev.kind = NodeKind::Event; ev.text = "整夜失眠";
  s.put_node(a); s.put_node(b); s.put_node(ev);
  s.put_edge(Edge{"ka", "kb", RelType::REL_CAUSE, 1.0, {}});
  s.put_edge(Edge{"ev", "kb", RelType::CONTAINS, 1.0, {}});

  // 休眠关键词不参与扩散
  Node* mb = s.get_node_mut("kb");
  mb->status = 1;
  auto dr2 = de.diffuse({{"ka", 1.0}}, 2);
  CHECK(!dr2.count("ev"));

  // budget 限制
  mb->status = 0;
  auto dr3 = de.diffuse({{"ka", 1.0}}, 2, nullptr, nullptr, 1);
  CHECK(dr3.count("kb") == 0 || dr3.size() <= 2);
}

static void test_reltype_filter() {
  Storage s;
  DiffusionEngine de(s, nullptr);
  Node a, b, c;
  a.uid = "ka"; a.kind = NodeKind::Keyword; a.name = "A"; a.status = 0;
  b.uid = "kb"; b.kind = NodeKind::Keyword; b.name = "B"; b.status = 0;
  c.uid = "kc"; c.kind = NodeKind::Keyword; c.name = "C"; c.status = 0;
  s.put_node(a); s.put_node(b); s.put_node(c);
  s.put_edge(Edge{"ka", "kb", RelType::REL_CAUSE, 1.0, {}});
  s.put_edge(Edge{"ka", "kc", RelType::REL_OPPOSITE, 1.0, {}});
  // 只沿因果边扩散 → C 不可达
  std::vector<RelType> only_cause = {RelType::REL_CAUSE};
  auto dr = de.diffuse({{"ka", 1.0}}, 2, &only_cause);
  CHECK(dr.count("kb"));
  CHECK(!dr.count("kc"));
}

// 束搜索：beam>=波前宽度时与 BFS 一致；beam 小宽度时高能路径保留、低能剪枝
static void test_beam() {
  Storage s;
  DiffusionEngine de(s, nullptr);
  Node seed;
  seed.uid = "s"; seed.kind = NodeKind::Keyword; seed.name = "根"; seed.status = 0;
  s.put_node(seed);
  // 星形图：10 个子节点，边权 0.1×i（高能路径 = 大权重的后几个）
  for (int i = 1; i <= 10; ++i) {
    Node c;
    c.uid = "c" + std::to_string(i);
    c.kind = NodeKind::Keyword;
    c.name = "子" + std::to_string(i);
    c.status = 0;
    s.put_node(c);
    s.put_edge(Edge{"s", c.uid, RelType::REL_SIMILAR, 0.1 * i, {}});
  }
  auto bfs = de.diffuse({{"s", 1.0}}, 2);
  auto beam_full = de.diffuse_beam({{"s", 1.0}}, 2, nullptr, nullptr, 1000, 100, nullptr);
  // beam=100（>=波前）与 BFS 能量一致
  CHECK(bfs.size() == beam_full.size());
  for (auto& [uid, ne] : bfs)
    CHECK_NEAR(ne.energy, beam_full[uid].energy, 1e-9);
  // beam=3：波前只留 top3 高能子节点（i=10,9,8）
  auto beam3 = de.diffuse_beam({{"s", 1.0}}, 2, nullptr, nullptr, 1000, 3, nullptr);
  CHECK(beam3.count("c10") && beam3.count("c9") && beam3.count("c8"));
  // 路径记录不断链：top 节点路径含两步以内的边
  CHECK(!beam3["c10"].paths.empty());
  CHECK(beam3["c10"].paths[0].steps.size() == 1);
  CHECK(beam3["c10"].paths[0].steps[0].type == RelType::REL_SIMILAR);
}

int main() {
  test_energy_and_paths();
  test_dormant_and_budget();
  test_reltype_filter();
  test_beam();
  return AME_TEST_REPORT();
}
