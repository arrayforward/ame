// IRCoT 调试：fake LLM 回调驱动 think_ircot
#include <cstdio>
#include "ame/engine/engine.h"
using namespace ame;
int main() {
  Engine e;
  e.remember("老张昨天在朝阳公园跑步");
  e.remember("老张晚上总是睡不着");
  int calls = 0;
  auto llm = [&](const std::string& p) -> std::string {
    ++calls;
    printf("LLM call %d prompt[:60]=%.60s\n", calls, p.c_str());
    if (calls == 1) return "{\"reasoning\":\"查睡眠\",\"next_query\":\"睡不着\"}";
    return "{\"done\":true}";
  };
  auto chain = e.think_ircot("老张为什么睡不着", 5, llm);
  printf("subqueries=%zu stop=%s\n", chain.subqueries.size(), chain.stop_reason.c_str());
  for (auto& s : chain.subqueries) printf("  sub: %s\n", s.c_str());
  return 0;
}
