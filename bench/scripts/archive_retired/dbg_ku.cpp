// 调试 852ce960 案例：SUPERSEDES 边是否形成、旧事实是否标 valid_to
#include <cstdio>
#include "ame/bench/bench.h"
#include "ame/engine/engine.h"
using namespace ame;
int main() {
  auto cases = load_longmemeval("/mnt/d/agent/memory/ame/bench/data/lme_type_sample.json");
  for (auto& c : cases) {
    if (c.conversation_id != "852ce960") continue;
    Engine e;
    ingest(e, c);
    int supersede_edges = 0, marked = 0;
    for (auto* n : e.storage().nodes_of_kind(NodeKind::Event)) {
      if (n->attrs.count("valid_to")) ++marked;
      if (n->text.find("pre-approved") != std::string::npos)
        printf("event: ts=%lld valid_to=%s | %.80s\n", (long long)n->ts,
               n->attrs.count("valid_to") ? n->attrs.at("valid_to").c_str() : "-",
               n->text.c_str());
      for (auto& [ed, nb] : e.storage().neighbors(n->uid, {RelType::SUPERSEDES}))
        ++supersede_edges;
    }
    printf("SUPERSEDES 边数=%d valid_to 标记事件数=%d\n", supersede_edges, marked);
  }
  return 0;
}
