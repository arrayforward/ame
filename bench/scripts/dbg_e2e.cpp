// 临时调试：复现 test_e2e 完整 seed_data 场景的 recall
#include <cstdio>
#include "ame/engine/engine.h"
using namespace ame;
int main() {
  Engine e;
  e.remember("老张和老王昨天在朝阳公园跑步，压力很大");
  e.remember("老张今天在公司加班到深夜，压力大");
  e.remember("老张最近总是睡不着，整夜失眠");
  e.remember("老王今天在咖啡馆谈项目");
  e.remember("老张的车在望京爆胎了，去维修");
  e.keywords().link_related("压力", "失眠", RelType::REL_CAUSE, 1.0);
  e.keywords().link_related("加班", "压力", RelType::REL_CAUSE, 1.0);
  SeedQuery sq;
  sq.text = "压力";
  auto rr = e.recall(sq, {}, 2, 20, true);
  printf("code=%d n=%zu\n", (int)rr.code, rr.events.size());
  for (auto& se : rr.events)
    printf("score=%.3f graph=%.3f ctx=%.3f text=%.40s\n", se.score, se.graph_score,
           se.context_score, se.event->text.c_str());
  // 失眠关键词状态
  const Node* ins = e.keywords().find("失眠");
  printf("失眠 keyword: %s status=%d\n", ins ? "found" : "MISSING", ins ? ins->status : -1);
  const Node* press = e.keywords().find("压力");
  printf("压力 keyword: %s status=%d\n", press ? "found" : "MISSING", press ? press->status : -1);
  // 直接看扩散结果
  SeedGenerator sg(e.storage(), e.vectors(), e.keywords(), &e.extractor());
  auto seeds = sg.gen_seeds(sq);
  printf("seeds:\n");
  for (auto& s : seeds) {
    const Node* n = e.storage().get_node(s.node_uid);
    printf("  %s w=%.2f %s %.30s\n", s.source.c_str(), s.weight, n ? kind_name(n->kind) : "?",
           n ? (n->name.empty() ? n->text : n->name).c_str() : "");
  }
  std::vector<DiffusionSeed> ds;
  for (auto& s : seeds) ds.push_back({s.node_uid, s.weight});
  DiffusionEngine de(e.storage(), &e.keywords());
  auto dr = de.diffuse(ds, 2);
  printf("diffusion nodes=%zu\n", dr.size());
  for (auto& [uid, ne] : dr) {
    const Node* n = e.storage().get_node(uid);
    printf("  e=%.3f %s %.30s\n", ne.energy, n ? kind_name(n->kind) : "?",
           n ? (n->name.empty() ? n->text : n->name).c_str() : "");
  }
  // 失眠节点的邻边明细
  printf("失眠 neighbors:\n");
  for (auto& [ed, n] : e.storage().neighbors(ins->uid))
    printf("  %s -> %s %s w=%.2f\n", rel_name(ed.type), kind_name(n->kind),
           (n->name.empty() ? n->text : n->name).substr(0, 20).c_str(), ed.weight);
  return 0;
}
