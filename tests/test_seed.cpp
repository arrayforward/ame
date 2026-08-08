/*
 * M5 seed 单元测试：硬种子（uid/关键词）、软种子（ANN 相似度阈值与赋权）、
 * 坐标种子、自由文本种子、合并去重
 */
#include "ame/extractor/extractor.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/seed/seed_generator.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"
#include "test_util.hpp"

using namespace ame;

struct Fixture {
  Storage s;
  VectorService vs{&s};
  KeywordManager kw{s};
  RuleExtractor ext;
  SeedGenerator sg{s, vs, kw, &ext};
};

static void test_hard_seeds() {
  Fixture f;
  Node ev;
  ev.uid = "ev1"; ev.kind = NodeKind::Event; ev.text = "老张压力很大";
  f.s.put_node(ev);
  std::string kuid = f.kw.get_or_create("压力", "concept");

  // uid 硬种子
  SeedQuery q1;
  q1.uid = "ev1";
  auto seeds = f.sg.gen_seeds(q1);
  CHECK(seeds.size() == 1 && seeds[0].node_uid == "ev1");
  CHECK(seeds[0].weight == 1.0 && seeds[0].source == "hard");

  // 关键词硬种子（别名归一后精确命中）
  SeedQuery q2;
  q2.keywords = {"压力大"};  // 归一 → 压力
  auto seeds2 = f.sg.gen_seeds(q2);
  CHECK(seeds2.size() == 1 && seeds2[0].node_uid == kuid);
  CHECK(seeds2[0].weight == 1.0 && seeds2[0].source == "hard");
}

static void test_soft_seeds() {
  Fixture f;
  Node ev;
  ev.uid = "ev1"; ev.kind = NodeKind::Event; ev.text = "老张在朝阳公园跑步";
  ev.embedding = f.vs.embed(ev.text);
  f.s.put_node(ev);
  Node far;
  far.uid = "ev2"; far.kind = NodeKind::Event; far.text = "zzqqxx 完全不相关";
  far.embedding = f.vs.embed(far.text);
  f.s.put_node(far);

  SeedQuery q;
  q.text = "zzqqxx 完全不相关";  // 与 ev2 完全同文 → 软种子 sim≈1
  auto seeds = f.sg.gen_seeds(q);
  bool found = false;
  for (auto& sd : seeds)
    if (sd.node_uid == "ev2" && sd.source == "soft") {
      found = true;
      CHECK(sd.weight > 0.9);  // 按相似度赋权
    }
  CHECK(found);
  // ev1 相似度低于阈值 0.5 → 被丢弃或低权
  for (auto& sd : seeds)
    if (sd.node_uid == "ev1") CHECK(sd.weight >= 0.5);
}

static void test_geo_and_text_seeds() {
  Fixture f;
  Node pl;
  pl.uid = "pl1"; pl.kind = NodeKind::Place; pl.name = "朝阳公园";
  pl.lat = 39.9449; pl.lng = 116.4781;
  f.s.put_node(pl);

  // 坐标种子
  SeedQuery q;
  q.has_geo = true;
  q.lat = 39.9450; q.lng = 116.4782;
  auto seeds = f.sg.gen_seeds(q);
  CHECK(seeds.size() == 1 && seeds[0].node_uid == "pl1" && seeds[0].source == "geo");

  // 自由文本种子：先过 M1，地名命中 → text 硬种子
  SeedQuery q2;
  q2.text = "老张在朝阳公园跑步";
  auto seeds2 = f.sg.gen_seeds(q2);
  bool found = false;
  for (auto& sd : seeds2)
    if (sd.node_uid == "pl1" && sd.source == "text") found = true;
  CHECK(found);
}

static void test_merge_dedup() {
  Fixture f;
  Node ev;
  ev.uid = "ev1"; ev.kind = NodeKind::Event; ev.text = "老张在朝阳公园跑步";
  ev.embedding = f.vs.embed(ev.text);
  f.s.put_node(ev);
  // 同一节点同时被 uid 硬种子与文本软种子命中 → 去重取最大权重
  SeedQuery q;
  q.uid = "ev1";
  q.text = "老张在朝阳公园跑步";
  auto seeds = f.sg.gen_seeds(q);
  int count = 0;
  for (auto& sd : seeds)
    if (sd.node_uid == "ev1") {
      ++count;
      CHECK(sd.weight == 1.0);  // 硬种子权重胜出
    }
  CHECK(count == 1);
}

static void test_lex_seeds() {
  Fixture f;
  Node ev1, ev2;
  ev1.uid = "ev1"; ev1.kind = NodeKind::Event;
  ev1.text = "I went to the LGBTQ support group meeting yesterday";
  ev2.uid = "ev2"; ev2.kind = NodeKind::Event;
  ev2.text = "The weather is nice today";
  f.s.put_node(ev1); f.s.put_node(ev2);

  SeedQuery q;
  q.text = "When did Caroline go to the LGBTQ support group?";
  auto seeds = f.sg.gen_seeds(q);
  bool found1 = false;
  for (auto& sd : seeds) {
    if (sd.node_uid == "ev1" && sd.source == "lex") {
      found1 = true;
      CHECK(sd.weight >= 0.6);  // 命中 lgbtq/support/group 多词
    }
    CHECK(sd.node_uid != "ev2");  // 零重叠事件不得成为词法种子
  }
  CHECK(found1);
}

// 条件归一：有命中不扩展 / 无命中 strong 扩展 / strong 缺失降级 near / 人名不扩展
static void test_conditional_normalize() {
  Fixture f;
  f.kw.add_alias("painting", "paint");
  f.kw.add_alias("jogging", "run");
  f.kw.add_near("walk", "stroll", 0.9);
  Node ent;
  ent.uid = "ent1"; ent.kind = NodeKind::Entity; ent.name = "Alice"; ent.type = "person";
  f.s.put_node(ent);
  Node ev1, ev2, ev3;
  ev1.uid = "ev1"; ev1.kind = NodeKind::Event; ev1.text = "I paint walls every weekend";
  ev2.uid = "ev2"; ev2.kind = NodeKind::Event; ev2.text = "She went jogging this morning";
  ev3.uid = "ev3"; ev3.kind = NodeKind::Event; ev3.text = "They stroll around the park";
  f.s.put_node(ev1); f.s.put_node(ev2); f.s.put_node(ev3);
  f.kw.get_or_create("paint", "attr");  // paint 节点存在 = 有直接命中

  auto has_seed = [](const SeedSet& ss, const std::string& uid) {
    for (auto& s : ss)
      if (s.node_uid == uid) return true;
    return false;
  };
  // 有命中：paint 直接命中 → "painting" 所在事件不得经扩展进入（保持原词）
  SeedQuery q1;
  q1.text = "paint weekend";
  q1.conditional_normalize = true;
  auto s1 = f.sg.gen_seeds(q1);
  CHECK(has_seed(s1, "ev1"));
  CHECK(!has_seed(s1, "ev2") && !has_seed(s1, "ev3"));
  // 无命中：run 无节点无事件 → strong 扩展 jogging 命中 ev2
  SeedQuery q2;
  q2.text = "run";
  q2.conditional_normalize = true;
  auto s2 = f.sg.gen_seeds(q2);
  CHECK(has_seed(s2, "ev2"));
  // strong 缺失降级 near：walk 无别名 → near stroll 命中 ev3
  SeedQuery q3;
  q3.text = "walk";
  q3.conditional_normalize = true;
  auto s3 = f.sg.gen_seeds(q3);
  CHECK(has_seed(s3, "ev3"));
  // 人名不扩展：Alice（实体存在）不触发任何扩展
  SeedQuery q4;
  q4.text = "did Alice run";
  q4.conditional_normalize = true;
  auto s4 = f.sg.gen_seeds(q4);
  CHECK(has_seed(s4, "ent1"));   // 实体硬种子照常
  CHECK(!has_seed(s4, "ev1"));   // 不经扩展误入
}

int main() {
  test_hard_seeds();
  test_soft_seeds();
  test_geo_and_text_seeds();
  test_merge_dedup();
  test_lex_seeds();
  test_conditional_normalize();
  return AME_TEST_REPORT();
}
