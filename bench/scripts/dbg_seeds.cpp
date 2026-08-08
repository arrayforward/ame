// 临时调试：LoCoMo case1 摄入后，打印指定问题的种子构成与目标事件能量
// 用法: dbg_seeds.sh "问题" "D1:3" [llm]（第三参数 llm 时用 LLM 提取摄入，走缓存）
#include <cstdio>
#include <cstdlib>
#include "ame/bench/bench.h"
#include "ame/engine/engine.h"
#include "ame/llm/components.h"

using namespace ame;
int main(int argc, char** argv) {
  auto cases = load_locomo("/mnt/d/agent/memory/ame/bench/data/locomo10.json");
  Engine e;
  bool use_llm = argc > 3 && std::string(argv[3]) == "llm";
  if (use_llm) {
    auto client = DeepSeekClient::from_config(
        "/mnt/d/agent/memory/ame/bench/config/deepseek.local.json", "deepseek-v4-flash",
        "/mnt/d/agent/memory/ame/bench/cache");
    RuleExtractor rule;
    LlmExtractor ext(client.get(), &rule);
    const TimePoint kBase = 1700000000;
    for (size_t si = 0; si < cases[0].sessions.size(); ++si) {
      const BenchSession& bs = cases[0].sessions[si];
      TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
      std::vector<std::pair<std::string, std::string>> turns;
      for (auto& t : bs.turns) turns.emplace_back(t.speaker, t.text);
      auto exts = ext.extract_session(turns);
      for (size_t ti = 0; ti < bs.turns.size(); ++ti)
        e.remember_turn_ex(std::move(exts[ti]), bs.turns[ti].speaker, base + (TimePoint)ti * 60,
                           bs.turns[ti].dia_id, bs.id);
    }
  } else {
    ingest(e, cases[0]);  // rule 摄入（调试用）
  }
  std::string q = argc > 1 ? argv[1] : "When did Caroline go to the LGBTQ support group?";
  std::string want = argc > 2 ? argv[2] : "D1:3";

  SeedQuery sq;
  sq.text = q;
  sq.soft_min_sim = 0.15;
  // 直接调 M5/M6 看种子与能量
  SeedGenerator sg(e.storage(), e.vectors(), e.keywords(), &e.extractor());
  auto seeds = sg.gen_seeds(sq);
  std::printf("Q: %s\nseeds(%zu):\n", q.c_str(), seeds.size());
  for (auto& s : seeds) {
    const Node* n = e.storage().get_node(s.node_uid);
    std::printf("  %-5s w=%.2f %s %.50s\n", s.source.c_str(), s.weight,
                n ? kind_name(n->kind) : "?", n ? (n->name.empty() ? n->text : n->name).c_str() : s.node_uid.c_str());
  }
  std::vector<DiffusionSeed> ds;
  for (auto& s : seeds) ds.push_back({s.node_uid, s.weight});
  DiffusionEngine de(e.storage(), &e.keywords());
  auto dr = de.diffuse(ds, 2);
  double maxe = 0;
  std::string maxuid;
  for (auto& [uid, ne] : dr)
    if (ne.energy > maxe) { maxe = ne.energy; maxuid = uid; }
  const Node* mn = e.storage().get_node(maxuid);
  std::printf("max energy=%.2f at %s %.40s\n", maxe, mn ? kind_name(mn->kind) : "?",
              mn ? (mn->name.empty() ? mn->text : mn->name).c_str() : "");
  for (auto* n : e.storage().nodes_of_kind(NodeKind::Event)) {
    auto it = n->attrs.find("dia_id");
    if (it != n->attrs.end() && it->second == want) {
      auto dit = dr.find(n->uid);
      std::printf("target %s energy=%.3f text=%.60s\n", want.c_str(),
                  dit != dr.end() ? dit->second.energy : -1.0, n->text.c_str());
    }
  }
  return 0;
}
