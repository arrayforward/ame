/*
 * M3 keyword 单元测试：别名归一、停用词过滤、低频休眠/唤醒、RELATED 边、受控主题表
 */
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "test_util.hpp"

using namespace ame;

static void test_normalize_and_stopword() {
  Storage s;
  KeywordManager kw(s);
  CHECK(kw.normalize("睡不着") == "失眠");
  CHECK(kw.normalize("失眠症") == "失眠");
  CHECK(kw.normalize("咖啡") == "咖啡");
  CHECK(kw.is_stopword("的"));
  CHECK(!KeywordManager::root_topics().empty() &&
        KeywordManager::root_topics().size() >= 20);
}

static void test_dormancy() {
  Storage s;
  KeywordManager kw(s);
  // 停用词过滤
  CHECK(kw.get_or_create("的", "attr").empty());
  // 新词无双边 → 休眠
  bool created = false;
  std::string k1 = kw.get_or_create("拖延", "concept", &created);
  CHECK(created && !k1.empty());
  CHECK(kw.is_dormant(k1));
  // 唤醒
  kw.wake_keyword("拖延");
  CHECK(!kw.is_dormant(k1));
  kw.sleep_keyword("拖延");
  CHECK(kw.is_dormant(k1));
}

static void test_link_related() {
  Storage s;
  KeywordManager kw(s);
  kw.get_or_create("压力", "concept");
  kw.get_or_create("失眠", "concept");
  CHECK(kw.link_related("压力", "失眠", RelType::REL_CAUSE, 1.0) == Err::Ok);
  const Node* a = kw.find("压力");
  const Node* b = kw.find("失眠");
  CHECK(a && b);
  CHECK(s.find_edge(a->uid, b->uid, RelType::REL_CAUSE) != nullptr);
}

// 复活工程 R2：英文保守词干归一（--stem-en 开关，默认关闭 = 历史行为）
static void test_stem_en() {
  // 默认关闭：不归一
  {
    Storage s;
    KeywordManager kw(s);
    CHECK(kw.normalize("loves") == "loves");
  }
  Storage s;
  KeywordManager kw(s);
  // 造 stem_df 护栏需要的图状态：事件 -CONTAINS-> 词干关键词节点
  auto mk_kw_with_df = [&](const char* word) {
    std::string kuid = kw.get_or_create(word, "attr");
    Node ev;
    ev.uid = std::string("E") + word;
    ev.kind = NodeKind::Event;
    ev.text = "t";
    ev.ts = 1700000000;
    s.put_node(ev);
    s.put_edge(Edge{ev.uid, kuid, RelType::CONTAINS, 1.0, {}});
    return kuid;
  };
  mk_kw_with_df("love");
  mk_kw_with_df("sense");
  mk_kw_with_df("politic");
  // 别名：politic -> political（词干再查别名表，C 类形态全链贯通）
  kw.add_alias("politic", "political");
  kw.set_stem_en(true);
  CHECK(kw.normalize("loves") == "love");    // 复数 s
  CHECK(kw.normalize("LOVES") == "love");    // 大写小写化先行
  CHECK(kw.normalize("loved") == "love");    // e+d 特判（防 made→mak 过度截取）
  CHECK(kw.normalize("senses") == "sense");  // es 尾部
  CHECK(kw.normalize("sense") == "sense");   // 不过度：sens 无节点 → 拒收
  CHECK(kw.normalize("politics") == "political");  // s→politic→别名→political
  CHECK(kw.normalize("destress") == "destress");   // 无词干节点 → 原样
  // 中文不受词干影响
  CHECK(kw.normalize("感冒") == "感冒");
  // 全链贯通：loves → love →（蒸馏别名表）like
  kw.add_alias_distilled("love", "like");
  CHECK(kw.normalize("love") == "like");
  CHECK(kw.normalize("loves") == "like");
  // 防互换成环：love 已归一到 like，再登记 like->love 必须被拒
  CHECK(!kw.add_alias_distilled("like", "love"));
  CHECK(kw.normalize("like") == "like");
}

int main() {
  test_normalize_and_stopword();
  test_dormancy();
  test_link_related();
  test_stem_en();
  return AME_TEST_REPORT();
}
