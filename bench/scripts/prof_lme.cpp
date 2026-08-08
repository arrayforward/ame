// 临时微基准：分解单轮摄入各阶段耗时（extract/resolve/write/supersede）
#include <chrono>
#include <cstdio>
#include "ame/bench/bench.h"
#include "ame/engine/engine.h"

using namespace ame;
static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main() {
  auto cases = load_longmemeval("/mnt/d/agent/memory/ame/bench/data/longmemeval_s_cleaned.json");
  const BenchCase& c = cases[0];
  Engine e;
  double t_extract = 0, t_write = 0;
  const TimePoint kBase = 1700000000;
  int turns = 0;
  for (size_t si = 0; si < c.sessions.size(); ++si) {
    const BenchSession& bs = c.sessions[si];
    TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
    for (size_t ti = 0; ti < bs.turns.size(); ++ti) {
      const BenchTurn& t = bs.turns[ti];
      TimePoint ts = base + (TimePoint)ti * 60;
      double a = now();
      auto r = e.remember_turn(t.speaker, t.text, ts, t.dia_id, bs.id);
      t_write += now() - a;
      ++turns;
      if (turns % 100 == 0)
        printf("turns=%d cum_write=%.1fs (avg %.0fms)\n", turns, t_write,
               t_write / turns * 1000);
    }
  }
  return 0;
}
