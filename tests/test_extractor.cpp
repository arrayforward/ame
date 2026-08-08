/*
 * M1 extractor 单元测试：规则提取的实体/地点/关键词/时间词解析
 */
#include <cstdlib>
#include <filesystem>
#include <unordered_set>

#include "ame/extractor/dat_trie.h"
#include "ame/extractor/extractor.h"
#include "ame/time/time_service.h"
#include "test_util.hpp"

using namespace ame;

static void test_extract_basic() {
  RuleExtractor ext;
  Extraction ex = ext.extract("老张昨天在朝阳公园跑步，压力很大");

  // 实体：称呼模式命中
  bool found_lz = false;
  for (auto& e : ex.entities)
    if (e.surface == "老张" && e.entity_type == "person") found_lz = true;
  CHECK(found_lz);

  // 地点：词典命中带坐标
  CHECK(ex.place.name == "朝阳公园");
  CHECK(ex.place.has_coord);

  // 关键词：action + concept
  bool has_run = false, has_press = false;
  for (auto& k : ex.keywords) {
    if (k.word == "跑步" && k.category == "action") has_run = true;
    if (k.word == "压力" && k.category == "concept") has_press = true;
  }
  CHECK(has_run && has_press);

  // 事件类型取首个 action 关键词
  CHECK(ex.event.type == "跑步");

  // 时间词：昨天 → now-86400，精度 Day
  TimePoint now = TimeService::now_utc();
  CHECK(std::llabs((long long)(ex.event.time - (now - 86400))) < 60);
  CHECK(ex.event.precision == Precision::Day);
}

static void test_extract_time_words() {
  RuleExtractor ext;
  TimePoint now = TimeService::now_utc();
  CHECK(std::llabs((long long)(ext.extract("老张前天聚餐").event.time - (now - 2 * 86400))) < 60);
  CHECK(ext.extract("老张去年体检").event.precision == Precision::Year);
  CHECK(ext.extract("老张上个月维修").event.precision == Precision::Month);
}

static void test_extract_multi_entity_and_object() {
  RuleExtractor ext;
  Extraction ex = ext.extract("老张和老王在公司谈项目");
  CHECK(ex.entities.size() == 2);  // 老张、老王
  Extraction ex2 = ext.extract("老张的车在望京爆胎了");
  bool has_car = false;
  for (auto& e : ex2.entities)
    if (e.surface == "车" && e.entity_type == "object") has_car = true;
  CHECK(has_car);
  CHECK(ex2.place.name == "望京");
}

static void test_extract_multiword_proper() {
  // 连续大写词合并为复合专名（"New York" 不拆两个实体）
  RuleExtractor ext;
  Extraction ex = ext.extract("I flew to New York yesterday");
  bool has_ny = false, has_new = false, has_york = false;
  for (auto& e : ex.entities) {
    if (e.surface == "New York") has_ny = true;
    if (e.surface == "New") has_new = true;
    if (e.surface == "York") has_york = true;
  }
  CHECK(has_ny && !has_new && !has_york);
}

static void test_load_keyword_dict() {
  // 运行时加载中文领域词典（小文件模拟 domain/segmentation 格式）
  FILE* f = std::fopen("/tmp/ame_test_domain.txt", "w");
  std::fputs("北京烤鸭\t9999\tfood\n糖葫芦\t8888\tfood\n不起眼的词\t1\tfood\n", f);
  std::fclose(f);
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict("/tmp/ame_test_domain.txt", "food", 100) == 3);
  Extraction ex = ext.extract("老张中午吃了北京烤鸭");
  bool hit = false;
  for (auto& k : ex.keywords)
    if (k.word == "北京烤鸭" && k.category == "food") hit = true;
  CHECK(hit);
  // cap 截断
  RuleExtractor ext2;
  CHECK(ext2.load_keyword_dict("/tmp/ame_test_domain.txt", "food", 1) == 1);
  // 文件不存在
  RuleExtractor ext3;
  CHECK(ext3.load_keyword_dict("/tmp/nonexistent.txt", "x", 10) == -1);
}

static void test_ac_automaton() {
  // AC 多模式/重叠/UTF-8 边界
  FILE* f = std::fopen("/tmp/ame_test_ac.txt", "w");
  std::fputs("失眠\t1\n失眠症\t2\n压力\t3\nsleep\t4\n", f);
  std::fclose(f);
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict("/tmp/ame_test_ac.txt", "concept", 100) == 4);
  auto hits = ext.ac_match("他最近失眠症状加重，压力也大，can't sleep");
  std::unordered_set<std::string> got;
  for (auto& [pos, w, cat] : hits) got.insert(w);
  // 重叠模式全部报告（策略：不取最长，全集输出，提取层按词去重）
  CHECK(got.count("失眠") && got.count("失眠症"));
  CHECK(got.count("压力"));
  CHECK(got.count("sleep"));           // ASCII 同样走码点转移
  CHECK(!got.count("失") && !got.count("眠症"));  // 边界：不拆词
  // 位置：失眠 起始于码点 3（他=0 最=1 近=2 失=3）
  bool pos_ok = false;
  for (auto& [pos, w, cat] : hits)
    if (w == "失眠") pos_ok = (pos == 3);
  CHECK(pos_ok);
}

// AC 二进制持久化：往返命中一致 + 校验不匹配自动重建
static void test_ac_persistence() {
  FILE* f = std::fopen("/tmp/ame_test_acp.txt", "w");
  std::fputs("失眠\t1\n失眠症\t2\n压力\t3\n", f);
  std::fclose(f);
  std::string cache = "/tmp/ame_test_ac.cache";
  std::filesystem::remove(cache);
  RuleExtractor ext;
  // 首次：cache 不存在 → 构建并覆写
  CHECK(ext.load_keyword_dict_cached("/tmp/ame_test_acp.txt", "concept", 100, cache) == 3);
  CHECK(std::filesystem::exists(cache));
  // 第二次：直接加载（命中一致）
  RuleExtractor ext2;
  CHECK(ext2.load_keyword_dict_cached("/tmp/ame_test_acp.txt", "concept", 100, cache) == 3);
  auto h1 = ext.ac_match("他失眠了，失眠症严重");
  auto h2 = ext2.ac_match("他失眠了，失眠症严重");
  std::unordered_set<std::string> s1, s2;
  for (auto& [p, w, c] : h1) s1.insert(w);
  for (auto& [p, w, c] : h2) s2.insert(w);
  CHECK(s1 == s2 && !s1.empty());
  // 词典变更（mtime/size 变化）→ 校验失败自动重建
  FILE* f2 = std::fopen("/tmp/ame_test_acp.txt", "a");
  std::fputs("焦虑\t4\n", f2);
  std::fclose(f2);
  RuleExtractor ext3;
  CHECK(ext3.load_keyword_dict_cached("/tmp/ame_test_acp.txt", "concept", 100, cache) == 4);
  CHECK(!ext3.ac_match("焦虑").empty());
}

// DAT 双数组 Trie：小词典正确性 + 与 AC 对拍
static void test_dat_trie() {
  DatTrie d;
  d.build({"回家的路", "回家路上", "心理保健", "心理健康", "失眠", "失眠症"});
  auto m1 = d.match("回家路上");
  CHECK(m1.count("回家路上") && !m1.count("回家的路"));  // 兄弟槽位不合并（顺序 bug 修复）
  auto m2 = d.match("他失眠症犯了");
  CHECK(m2.count("失眠") && m2.count("失眠症"));
  // 与 AC 对拍（同词表同文本）
  FILE* f = std::fopen("/tmp/ame_test_dat.txt", "w");
  std::fputs("回家的路\n回家路上\n心理保健\n心理健康\n失眠\n失眠症\n", f);
  std::fclose(f);
  RuleExtractor ext;
  ext.load_keyword_dict("/tmp/ame_test_dat.txt", "x", 100);
  std::unordered_set<std::string> ac;
  for (auto& [p, w, c] : ext.ac_match("回家路上，心理健康")) ac.insert(w);
  CHECK(ac == d.match("回家路上，心理健康"));
}

int main() {
  test_extract_basic();
  test_extract_time_words();
  test_extract_multi_entity_and_object();
  test_extract_multiword_proper();
  test_load_keyword_dict();
  test_ac_automaton();
  test_ac_persistence();
  test_dat_trie();
  return AME_TEST_REPORT();
}
