/*
 * M11 feedback 单元测试：埋点落 JSONL、信号分级、三元组导出
 */
#include <filesystem>

#include "ame/engine/engine.h"
#include "ame/feedback/feedback_collector.h"
#include "test_util.hpp"

using namespace ame;

static void test_collector_direct() {
  std::string path = "/tmp/ame_test_fbdirect.jsonl";
  std::filesystem::remove(path);
  {
    FeedbackCollector fb(path);
    long qid = fb.log_query("压力", {"e_pos", "e_neg", "e_neg2"});
    CHECK(qid > 0);
    CHECK(fb.log_feedback(qid, SignalType::Select, "e_pos") == Err::Ok);
    CHECK(fb.log_feedback(qid, SignalType::Ignore, "e_neg") == Err::Ok);

    auto triples = fb.export_triples(0);
    CHECK(!triples.empty());
    // 正样本 = e_pos；负样本含显式 ignore 与未标注弱负
    bool has_pos = false, has_weak_neg = false;
    for (auto& t : triples) {
      CHECK(t.pos_uid == "e_pos");
      CHECK(t.query == "压力");
      if (t.neg_uid == "e_neg") has_pos = true;
      if (t.neg_uid == "e_neg2") has_weak_neg = true;
    }
    CHECK(has_pos && has_weak_neg);
  }
  // 重新打开：query_id 单调延续
  FeedbackCollector fb2(path);
  CHECK(fb2.log_query("失眠", {}) == 2);
}

static void test_engine_integration() {
  std::string dir = "/tmp/ame_test_fb";
  std::filesystem::remove_all(dir);
  Engine e(dir);
  e.remember("老张在朝阳公园跑步");
  e.remember("老张失眠睡不着");
  e.keywords().link_related("跑步", "失眠", RelType::REL_OPPOSITE, 0.5);

  SeedQuery sq;
  sq.text = "跑步";
  auto rr = e.recall(sq);
  CHECK(rr.query_id > 0);
  if (!rr.events.empty()) {
    CHECK(e.feedback(rr.query_id, SignalType::Select, rr.events[0].event->uid) == Err::Ok);
    if (rr.events.size() > 1)
      CHECK(e.feedback(rr.query_id, SignalType::Ignore, rr.events[1].event->uid) == Err::Ok);
  }
  CHECK(std::filesystem::exists(dir + "/feedback.jsonl"));
}

int main() {
  test_collector_direct();
  test_engine_integration();
  return AME_TEST_REPORT();
}
