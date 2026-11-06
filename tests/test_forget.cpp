/*
 * 4.0：条目级遗忘（内存后端）——block/unblock/delete_event 语义测试。
 * 覆盖：屏蔽可逆（recall 不再返回但数据保留）、删除（节点+关联边清除）、
 *       删除后 recall 不再命中、重复删除返回 NotFound。
 */
#include "ame/engine/engine.h"

#include <cstdio>
#include <string>

using namespace ame;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static bool recall_has(Engine& e, const char* query, const char* needle) {
  SeedQuery q;
  q.text = query;
  for (auto& ev : e.recall(q, {}, 2, 5, false).events)
    if (ev.event && ev.event->text.find(needle) != std::string::npos) return true;
  return false;
}

int main() {
  Engine e;
  auto r1 = e.remember_turn("user", "my password is zephyrus", 100);
  auto r2 = e.remember_turn("user", "favorite drink is quokka", 200);
  CHECK(r1.code == Err::Ok && r2.code == Err::Ok);
  CHECK(recall_has(e, "zephyrus", "zephyrus"));
  CHECK(recall_has(e, "quokka", "quokka"));

  // 屏蔽：recall 不再返回，但数据保留（可逆）
  CHECK(e.block_event(r1.event_id) == Err::Ok);
  CHECK(!recall_has(e, "zephyrus", "zephyrus"));
  CHECK(recall_has(e, "quokka", "quokka"));  // 其它条目不受影响
  CHECK(e.storage().get_node(r1.event_id) != nullptr);  // 屏蔽≠删除
  CHECK(e.unblock_event(r1.event_id) == Err::Ok);
  CHECK(recall_has(e, "zephyrus", "zephyrus"));  // 解除后恢复

  // 删除：节点+关联边清除，不可恢复
  size_t before = e.storage().node_count();
  CHECK(e.delete_event(r1.event_id) == Err::Ok);
  CHECK(!recall_has(e, "zephyrus", "zephyrus"));
  CHECK(e.storage().get_node(r1.event_id) == nullptr);
  CHECK(e.storage().node_count() == before - 1);
  CHECK(e.delete_event(r1.event_id) == Err::NotFound);  // 重复删除
  CHECK(recall_has(e, "quokka", "quokka"));

  if (failures == 0) std::printf("PASS test_forget（条目屏蔽/删除 全断言通过）\n");
  return failures == 0 ? 0 : 1;
}
