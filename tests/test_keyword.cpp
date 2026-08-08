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

int main() {
  test_normalize_and_stopword();
  test_dormancy();
  test_link_related();
  return AME_TEST_REPORT();
}
