/*
 * 4.0：遗忘能力 E2E 场景验证器（内存后端，无 LLM，可直接运行）。
 * 场景：灌入两轮私密记忆 → 验证 recall 可见 → 条目屏蔽（不可见但数据在）→
 *       解除屏蔽（恢复）→ 条目删除（节点+边消失）→ session 清空（全部归零）。
 * 打印逐项 PASS/FAIL；全部通过输出 "E2E FORGET: ALL PASS"，退出码 0。
 */
#include "ame/engine/engine.h"

#include <cstdio>
#include <string>

using namespace ame;

static int failures = 0;

static void check(bool ok, const char* name) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}

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
  check(r1.code == Err::Ok && r2.code == Err::Ok, "S0 两轮记忆写入");
  check(recall_has(e, "password", "zephyrus"), "S1 私密记忆 recall 可见");
  check(recall_has(e, "drink", "quokka"), "S2 其它记忆 recall 可见");

  check(e.block_event(r1.event_id) == Err::Ok, "S3 block_event 返回成功");
  check(!recall_has(e, "password", "zephyrus"), "S4 屏蔽后 recall 不可见");
  check(recall_has(e, "drink", "quokka"), "S5 屏蔽不影响其它条目");
  check(e.storage().get_node(r1.event_id) != nullptr, "S6 屏蔽≠删除（数据保留）");

  check(e.unblock_event(r1.event_id) == Err::Ok, "S7 unblock_event 返回成功");
  check(recall_has(e, "password", "zephyrus"), "S8 解除屏蔽后恢复可见");

  check(e.delete_event(r1.event_id) == Err::Ok, "S9 delete_event 返回成功");
  check(!recall_has(e, "password", "zephyrus"), "S10 删除后 recall 不可见");
  check(e.storage().get_node(r1.event_id) == nullptr, "S11 节点已删");
  check(e.delete_event(r1.event_id) == Err::NotFound, "S12 重复删除 NotFound");

  e.clear_memory();
  check(e.stats().turns == 0, "S13 session 清空后事件数=0");
  check(!recall_has(e, "drink", "quokka"), "S14 session 清空后 recall 全空");
  check(e.remember_turn("user", "new day new memory", 300).code == Err::Ok,
        "S15 清空后可重新写入（新 session 语义）");

  if (failures == 0) std::printf("E2E FORGET: ALL PASS\n");
  return failures == 0 ? 0 : 1;
}
