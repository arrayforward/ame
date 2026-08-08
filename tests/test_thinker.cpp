/*
 * M8 thinker 单元测试：多轮 ReasoningChain、focus/reflection、终止条件
 */
#include "ame/engine/engine.h"
#include "test_util.hpp"

using namespace ame;

static void test_think_chain() {
  Engine e;
  e.remember("老张昨天在朝阳公园跑步");
  e.remember("老张今天在公司加班，压力很大");
  e.remember("老张晚上总是睡不着");

  auto chain = e.think("老张为什么睡不着", 3);
  CHECK(!chain.rounds.empty());
  CHECK(chain.rounds.size() <= 3);
  CHECK(!chain.stop_reason.empty());
  CHECK(chain.confidence >= 0);
  // ReasoningChain 带 focus/reflection
  CHECK(!chain.rounds[0].focus.empty());
  CHECK(!chain.rounds[0].reflection.empty());
}

static void test_think_empty_graph() {
  Engine e;
  auto chain = e.think("完全不存在的概念zzqq", 2);
  CHECK(chain.rounds.empty() || chain.rounds.size() <= 2);
  CHECK(!chain.stop_reason.empty());
}

// IRCoT 模式：fake LLM 回调预设子查询序列，验证多轮循环/去重/终止
static void test_think_ircot() {
  Engine e;
  e.remember("老张昨天在朝阳公园跑步");
  e.remember("老张今天在公司加班，压力很大");
  e.remember("老张晚上总是睡不着");
  int calls = 0;
  auto llm = [&](const std::string& prompt) -> std::string {
    ++calls;
    CHECK(prompt.find("老张为什么睡不着") != std::string::npos);  // prompt 含原问题
    if (calls == 1) return R"({"reasoning":"需要查睡眠相关","next_query":"睡不着"})";
    if (calls == 2) return R"({"reasoning":"还要查压力","next_query":"压力"})";
    return R"({"done":true})";
  };
  auto chain = e.think_ircot("老张为什么睡不着", 5, llm);
  CHECK(chain.subqueries.size() >= 1);
  CHECK(chain.subqueries[0] == "睡不着");
  CHECK(!chain.stop_reason.empty());  // done 或 无新证据 均为合法终止
  CHECK(chain.rounds.size() >= 2);    // 初始检索 + ≥1 轮子查询

  // done 立即终止
  auto llm_done = [](const std::string&) -> std::string { return R"({"done":true})"; };
  auto chain3 = e.think_ircot("老张跑步", 5, llm_done);
  CHECK(chain3.stop_reason == "LLM 判定证据充分（done）");
  CHECK(chain3.subqueries.empty());

  // 无新证据终止（子查询不产新证据时退出，不死循环）
  int calls2 = 0;
  auto llm2 = [&](const std::string&) -> std::string {
    ++calls2;
    return R"({"next_query":"跑步"})";  // 重复检索同一目标
  };
  auto chain2 = e.think_ircot("老张跑步", 5, llm2);
  CHECK(chain2.stop_reason == "无新证据");
  CHECK(calls2 <= 5);
}

int main() {
  test_think_chain();
  test_think_empty_graph();
  test_think_ircot();
  return AME_TEST_REPORT();
}
