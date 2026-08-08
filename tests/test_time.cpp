/*
 * M10 time 单元测试：UTC 校验、地方时投影（时段/打烊后）、人生阶段匹配、指数衰减
 */
#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "test_util.hpp"

using namespace ame;

static void test_project_local() {
  // 1700000000 = 2023-11-14 22:13:20 UTC
  Node place;
  place.kind = NodeKind::Place;
  place.attrs["tz_offset"] = "8";
  place.attrs["open_hours"] = "08:00-22:00";
  TimeService ts(nullptr);
  auto lp = ts.project_local(1700000000, &place);
  CHECK(lp.local_hour == 6);            // UTC+8 → 次日 06:13
  CHECK(lp.period == "清晨");
  CHECK(lp.after_hours == true);        // 打烊后
}

static void test_decay() {
  TimePoint now = TimeService::now_utc();
  CHECK_NEAR(TimeService::decay_factor(now, 90, now), 1.0, 1e-6);
  CHECK_NEAR(TimeService::decay_factor(now - 90LL * 86400, 90, now), 0.5, 1e-6);
  CHECK(TimeService::decay_factor(now - 180LL * 86400, 90, now) <
        TimeService::decay_factor(now - 90LL * 86400, 90, now));
  CHECK(TimeService::validate_utc(now));
  CHECK(!TimeService::validate_utc(-5));
}

static void test_match_phase() {
  Storage s;
  Node ent, ph;
  ent.uid = "e1"; ent.kind = NodeKind::Entity; ent.name = "老张";
  ph.uid = "p1"; ph.kind = NodeKind::Phase; ph.name = "大学";
  s.put_node(ent); s.put_node(ph);
  s.put_edge(Edge{"e1", "p1", RelType::LIFE_PHASE, 1.0,
                  {{"start", "1000000000"}, {"end", "1100000000"}}});
  TimeService ts(&s);
  CHECK(ts.match_phase(1050000000, "e1") == "大学");
  CHECK(ts.match_phase(1200000000, "e1").empty());
}

int main() {
  test_project_local();
  test_decay();
  test_match_phase();
  return AME_TEST_REPORT();
}
