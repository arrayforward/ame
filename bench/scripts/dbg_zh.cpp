// 临时调试：中文词林灌库后 recall("失眠") 的种子与结果
#include <cstdio>
#include "ame/engine/engine.h"
#include "ame/bench/bench.h"
using namespace ame;
int main() {
  Engine e;
  auto r1 = e.remember("我昨晚又睡不着了");
  auto r2 = e.remember("老张今天很愉快");
  auto r3 = e.remember("老王昨天很悲伤");
  int applied = 0;
  apply_lexicon(e, "/mnt/d/agent/memory/ame/bench/data/lexicon_zh.json", &applied);
  printf("applied=%d normalize(睡不着)=%s normalize(失眠)=%s\n", applied,
         e.keywords().normalize("睡不着").c_str(), e.keywords().normalize("失眠").c_str());
  const Node* ins = e.keywords().find("失眠");
  printf("失眠 node: %s status=%d\n", ins ? "yes" : "no", ins ? ins->status : -1);
  printf("normalize(开心)=%s variants(开心):", e.keywords().normalize("开心").c_str());
  for (auto& v : e.keywords().variants("开心", 8)) printf(" %s", v.c_str());
  printf("\n");
  SeedQuery sq2;
  sq2.text = "开心";
  auto rr2 = e.recall(sq2, {}, 2, 10, true);
  printf("开心: code=%d n=%zu\n", (int)rr2.code, rr2.events.size());
  for (auto& se : rr2.events)
    printf("  %.3f %s %.25s\n", se.score, se.event->uid.c_str(), se.event->text.c_str());
  SeedQuery sq;
  sq.text = "失眠";
  auto rr = e.recall(sq, {}, 2, 10, true);
  printf("code=%d n=%zu target=%s\n", (int)rr.code, rr.events.size(), r1.event_id.c_str());
  for (auto& se : rr.events)
    printf("  %.3f g=%.2f c=%.2f %s %.30s\n", se.score, se.graph_score, se.context_score,
           se.event->uid.c_str(), se.event->text.c_str());
  return 0;
}
