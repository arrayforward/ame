/*
 * M5 seed 单元测试：硬种子（uid/关键词）、软种子（ANN 相似度阈值与赋权）、
 * 坐标种子、自由文本种子、合并去重
 */
#include <fstream>

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

// 第六轮终局：统一 graded 边表（WordEdgeTable）——蒸馏边 + 反义弱边 + 现场铸词形边。
// q19 型：kid 直接命中 + like→love 蒸馏边 0.8 → m=2 进池；无表历史行为；噪声零新增。
static void test_word_edges() {
  const char* sim_path = "/tmp/ame_test_wordsim.json";
  {
    std::ofstream f(sim_path);
    f << "{\"source\":\"distilled-sim\",\"sims\":{\"like\":{\"love\":0.8}}}";
  }
  WordEdgeTable sim;
  CHECK(sim.load(sim_path));
  CHECK(sim.word_count() == 1 && sim.edge_count() == 1);
  CHECK(sim.sim("like", "love") == 0.8f);
  CHECK(sim.sim("love", "like") == 0.8f);   // 反向退化查找
  CHECK(sim.sim("like", "hate") == 0.0f);
  // 现场铸词形边（R2 词干改写的 graded 替代物）：双向可铸、分档 0.95/0.9
  CHECK(sim.sim("love", "loves") == 0.95f);   // 复数档
  CHECK(sim.sim("loves", "love") == 0.95f);   // 未登录词现场铸边再查
  CHECK(sim.sim("hop", "hopped") == 0.9f);    // 过去档（双写还原）
  CHECK(sim.sim("make", "making") == 0.9f);   // 进行档
  CHECK(sim.sim("make", "maked") == 0.9f);    // 不规则不处理也不误铸为 1.0
  CHECK(sim.sim("love", "loving") == 0.9f);
  // 反义弱边：antonyms 流 ×0.35
  const char* lex_path = "/tmp/ame_test_lex_ant.json";
  {
    std::ofstream f(lex_path);
    f << "{\"entries\":[{\"word\":\"like\",\"syn_strong\":[],\"syn_near\":[],"
         "\"antonyms\":[\"hate\"]}]}";
  }
  CHECK(sim.load_antonyms(lex_path));
  CHECK(sim.sim("like", "hate") == 0.35f);    // 反义弱边
  CHECK(sim.sim("like", "love") == 0.8f);     // 撞词取 max：蒸馏边不被反义边覆盖

  Fixture f;
  Node ev1, ev2;
  ev1.uid = "ev1"; ev1.kind = NodeKind::Event;
  ev1.text = "The kid love dinosaurs";      // kid 直接 + love→蒸馏边 0.8
  ev2.uid = "ev2"; ev2.kind = NodeKind::Event;
  ev2.text = "The weather is nice today";   // 零命中噪声
  f.s.put_node(ev1); f.s.put_node(ev2);

  // 无表（默认）：历史行为——单直接词非罕见 → 不进池
  SeedQuery q0;
  q0.text = "What did the kid like?";
  auto s0 = f.sg.gen_seeds(q0);
  bool f1 = false, f2 = false;
  for (auto& sd : s0) {
    if (sd.node_uid == "ev1") f1 = true;
    if (sd.node_uid == "ev2") f2 = true;
  }
  CHECK(!f1 && !f2);

  // 有表：软匹配命中（0.8 ≥0.5 计一个内容词命中）→ m=2 进池，噪声仍排除
  SeedQuery q1;
  q1.text = "What did the kid like?";
  q1.word_edges = &sim;
  auto s1 = f.sg.gen_seeds(q1);
  bool b1 = false, b2 = false;
  for (auto& sd : s1) {
    if (sd.node_uid == "ev1") { b1 = true; CHECK(sd.source == "lex"); }
    if (sd.node_uid == "ev2") b2 = true;
  }
  CHECK(b1);
  CHECK(!b2);
}

  // 人名/专名归一（--name-alias 域）：称呼提取出的昵称（老X/小X）经别名表归到
  // canonical——查询侧实体词面 alias_of 命中时：canonical 实体/关键词入硬种子，
  // 且 canonical 词面注入 CJK 词法通道，全变体轮次（含称呼轮）直接事件级种子化；
  // 未加载别名表时行为与历史一致（英文人名 Alice 不经归一）。
static void test_name_alias_bridge() {
  Fixture f;
  // M3 别名表 + M2 实体（canonical + Alias 节点）+ M3 canonical 关键词节点
  f.kw.add_alias("小李", "李建国");
  Node ent;
  ent.uid = "ent1"; ent.kind = NodeKind::Entity; ent.name = "李建国"; ent.type = "person";
  f.s.put_node(ent);
  Node al;
  al.uid = "al1"; al.kind = NodeKind::Alias; al.name = "小李";
  al.attrs["target"] = "ent1";
  f.s.put_node(al);
  std::string kuid = f.kw.get_or_create("李建国", "concept");
  // 事件：一个含 canonical 词面、一个含称呼变体
  Node ev1, ev2;
  ev1.uid = "ev1"; ev1.kind = NodeKind::Event; ev1.text = "李建国上周出差了";
  ev2.uid = "ev2"; ev2.kind = NodeKind::Event; ev2.text = "小李来过电话";
  f.s.put_node(ev1); f.s.put_node(ev2);

  SeedQuery q;
  q.text = "小李说过什么";  // M1 称呼模式提取实体 surface "小李"
  auto seeds = f.sg.gen_seeds(q);
  bool has_ent = false, has_kw = false, has_ev1 = false, has_ev2 = false;
  for (auto& sd : seeds) {
    if (sd.node_uid == "ent1") has_ent = true;
    if (sd.node_uid == kuid) has_kw = true;
    if (sd.node_uid == "ev1") has_ev1 = true;  // canonical 词面子串命中
    if (sd.node_uid == "ev2") has_ev2 = true;  // 变体（小李）经 variants 扩展命中
  }
  CHECK(has_ent);   // canonical 实体种子（INVOLVES 通道）
  CHECK(has_kw);    // canonical 关键词种子（CONTAINS 通道）
  CHECK(has_ev1);   // canonical 词面事件级种子（CJK 词法通道）
  CHECK(has_ev2);   // 称呼变体事件级种子（variants 扩展）

  // 零回归：无别名映射的英文人名行为不变（仅实体自身命中，无额外关键词种子）
  Node alice;
  alice.uid = "ent2"; alice.kind = NodeKind::Entity; alice.name = "Alice";
  alice.type = "person";
  f.s.put_node(alice);
  SeedQuery q2;
  q2.text = "did Alice say anything";
  auto seeds2 = f.sg.gen_seeds(q2);
  bool has_alice = false, extra_kw = false;
  for (auto& sd : seeds2) {
    if (sd.node_uid == "ent2") has_alice = true;
    if (sd.node_uid == kuid) extra_kw = true;  // Alice→alice 是小写化不是映射
  }
  CHECK(has_alice);
  CHECK(!extra_kw);
}

int main() {
  test_hard_seeds();
  test_soft_seeds();
  test_geo_and_text_seeds();
  test_merge_dedup();
  test_lex_seeds();
  test_conditional_normalize();
  test_word_edges();
  test_name_alias_bridge();
  return AME_TEST_REPORT();
}
