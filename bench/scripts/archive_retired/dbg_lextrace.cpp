/*
 * 临时调查工具（根因调查用，调查完可删）：词库激活四层证据链的第 1/2/3 层采集。
 *   第1层：灌库后的图结构存量（别名表/Keyword 节点/REL_SIMILAR/REL_OPPOSITE 边）
 *   第2层：sample50 问题词的同义扩展触发率（normalize 别名命中 / variants / near_words）
 *   第3层：指定问题的 lex 通道逐站 trace（扩展词→df→种子权重→recall 池内 rank→终排 BM25 rank）
 * 用法: dbg_lextrace <locomo10.json> <sample50_ids.json> <lexicon.json> [extractor_dict] [qid ...]
 * 编译: 见 bench/scripts/dbg_seeds.sh 同款 g++ 命令（libs 取 build-win）。
 */
#include <ame/bench/bench.h>
#include <ame/bench/store.h>
#include <ame/core/json.h>
#include <ame/engine/engine.h>
#include <ame/extractor/extractor.h>
#include <ame/keyword/keyword_manager.h>
#include <ame/storage/storage.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ame;

// 与 seed_generator.cpp 完全一致的 lex 通道切词（ASCII 小写 len≥3 去停用词）
static const std::unordered_set<std::string> kLexStop = {
    "the", "and", "for", "are", "was", "were", "has", "have", "had", "did",
    "does", "what", "when", "where", "which", "who", "whom", "whose", "how",
    "with", "from", "that", "this", "these", "those", "they", "them", "their",
    "would", "could", "should", "about", "into", "your", "you", "his", "her",
    "she", "him", "not", "but", "all", "any", "can", "will", "just"};

static std::unordered_set<std::string> tokenize(const std::string& text) {
  std::unordered_set<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (cur.size() >= 3) out.insert(cur);
    cur.clear();
  };
  for (unsigned char c : text) {
    if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
    else flush();
  }
  flush();
  return out;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: dbg_lextrace locomo10 sample50 lexicon [dict] [qids...]\n");
    return 1;
  }
  std::vector<BenchCase> cases = load_locomo(argv[1]);
  if (cases.empty()) { std::fprintf(stderr, "no cases\n"); return 1; }
  // sample50 题单
  std::unordered_set<std::string> only;
  {
    std::ifstream f(argv[2]);
    std::stringstream ss; ss << f.rdbuf();
    bool ok = false; Json j = Json::parse(ss.str(), &ok);
    if (ok && j.kind == Json::Arr) for (auto& v : j.arr) only.insert(v.as_str());
  }
  Engine e("");
  const BenchCase* c = nullptr;
  for (auto& cc : cases) {
    bool any = false;
    for (auto& q : cc.questions) if (only.count(q.id)) any = true;
    if (any) { c = &cc; break; }
  }
  if (!c) { std::fprintf(stderr, "no case matched\n"); return 1; }
  IngestStats st = ingest(e, *c);
  std::fprintf(stderr, "[ingest] events=%d entities=%d keywords=%d\n", st.events,
               st.entities, st.keywords);

  // ---- 第1层：灌库前后结构存量 ----
  auto kw_nodes = [&] { return e.storage().nodes_of_kind(NodeKind::Keyword).size(); };
  auto alias_cnt = [&] { return e.keywords().aliases_to_json().obj.size(); };
  size_t kw_before = kw_nodes();
  size_t alias_before = alias_cnt();
  int applied = 0;
  if (apply_lexicon(e, argv[3], &applied) != Err::Ok) {
    std::fprintf(stderr, "apply_lexicon failed\n"); return 1;
  }
  size_t kw_after = kw_nodes();
  size_t alias_after = alias_cnt();
  // RELATED 边统计（Keyword 出边去重）
  std::set<std::string> sim_edges, opp_edges;
  for (auto* kn : e.storage().nodes_of_kind(NodeKind::Keyword)) {
    for (auto& [ed, nb] : e.storage().neighbors(kn->uid, {RelType::REL_SIMILAR}))
      sim_edges.insert(kn->name + "|" + nb->name);
    for (auto& [ed, nb] : e.storage().neighbors(kn->uid, {RelType::REL_OPPOSITE}))
      opp_edges.insert(kn->name + "|" + nb->name);
  }
  std::printf("== L1层 结构存量 ==\n");
  std::printf("Keyword 节点: 灌库前 %zu -> 灌库后 %zu\n", kw_before, kw_after);
  std::printf("别名表: 灌库前 %zu -> 灌库后 %zu（灌入 applied=%d）\n", alias_before,
              alias_after, applied);
  std::printf("REL_SIMILAR 边 %zu，REL_OPPOSITE 边 %zu\n", sim_edges.size(),
              opp_edges.size());

  // 可选：加载查询侧大词表（复现 L1 配置）
  bool dict_on = argc > 4 && std::string(argv[4]) != "-";
  if (dict_on) {
    int n = e.extractor().load_keyword_dict_cached(argv[4], "seg", 700000,
                                                   std::string(argv[4]) + ".ac");
    std::fprintf(stderr, "[dict] loaded %d\n", n);
  }
  // R2：调查工具默认开启英文词干（灌库后开，词干 df 护栏需要图已存在）
  e.keywords().set_stem_en(true);

  // 蓝方自检模式：--stemcheck FILE 逐词打印 w -> normalize(w) 后退出
  if (argc > 5 && std::string(argv[5]) == "--stemcheck") {
    std::ifstream wf(argv[6]);
    std::string w;
    while (wf >> w) std::printf("%s -> %s\n", w.c_str(), e.keywords().normalize(w).c_str());
    return 0;
  }

  // ---- 第2层：问题词激活率 ----
  std::printf("\n== L2层 查询词激活率（lex 通道口径，baseline 无条件归一）==\n");
  int n_q = 0, n_expand_q = 0, n_hardseed_q = 0;
  int tot_words = 0, tot_alias_hit = 0, tot_variants = 0, tot_variant_df = 0;
  std::vector<const BenchQuestion*> trace_qs;
  for (auto& q : c->questions) {
    if (!only.count(q.id)) continue;
    ++n_q;
    auto toks = tokenize(q.question);
    std::unordered_set<std::string> qcontent;
    for (auto& w : toks) if (!kLexStop.count(w)) qcontent.insert(w);
    int alias_hit = 0, with_var = 0, var_df = 0;
    std::vector<std::string> ex_vars;
    for (auto& w : qcontent) {
      ++tot_words;
      std::string nw = e.keywords().normalize(w);
      if (nw != w) { ++alias_hit; ++tot_alias_hit; }
      auto vars = e.keywords().variants(w, 3);
      if (!vars.empty()) {
        ++with_var; ++tot_variants;
        for (auto& v : vars) {
          if (ex_vars.size() < 6) ex_vars.push_back(w + "->" + v);
          // 扩展词在事件文本中的 df（快速子串扫，近似 lex 通道 tokenize 命中）
          int df = 0;
          for (auto* n : e.storage().nodes_of_kind(NodeKind::Event)) {
            auto et = tokenize(n->text);
            if (et.count(v)) ++df;
          }
          if (df > 0) { ++var_df; ++tot_variant_df; break; }
        }
      }
    }
    // gen_seeds 自由文本硬种子通道（extractor 关键词 -> normalize -> find）
    Extraction ex = e.extractor().extract(q.question);
    int hard_kw = 0;
    for (auto& k : ex.keywords)
      if (e.keywords().find(e.keywords().normalize(k.word))) ++hard_kw;
    if (hard_kw > 0) ++n_hardseed_q;
    bool miss = q.evidence_dia_ids.empty() ? false : true;  // 占位（金标在 dump 侧判）
    (void)miss;
    std::printf("题 %s | 内容词 %zu | 别名命中 %d | 有variants %d | 扩展df>0 %d | "
                "关键词硬种子 %d | 例:",
                q.id.c_str(), qcontent.size(), alias_hit, with_var, var_df, hard_kw);
    for (auto& s : ex_vars) std::printf(" %s;", s.c_str());
    std::printf("\n");
    if (with_var > 0) ++n_expand_q;
    if (argc > 5) {
      for (int i = 5; i < argc; ++i)
        if (q.id == argv[i]) trace_qs.push_back(&q);
    }
  }
  std::printf("汇总: %d 题 | 内容词 %d | 别名命中 %d | 有variants词 %d | 扩展df>0 %d | "
              "有硬种子题 %d/%d | 有扩展题 %d/%d\n",
              n_q, tot_words, tot_alias_hit, tot_variants, tot_variant_df, n_hardseed_q,
              n_q, n_expand_q, n_q);

  // ---- 第3层：逐站 trace ----
  for (auto* q : trace_qs) {
    std::printf("\n== L3层 trace %s ==\n", q->id.c_str());
    std::printf("Q: %s\n", q->question.c_str());
    auto toks = tokenize(q->question);
    std::unordered_set<std::string> qcontent;
    for (auto& w : toks) if (!kLexStop.count(w)) qcontent.insert(w);
    // 站(a)：复算 lex 通道（与 seed_generator 同公式）
    auto events = e.storage().nodes_of_kind(NodeKind::Event);
    std::vector<std::unordered_set<std::string>> etoks(events.size());
    std::unordered_map<std::string, int> df;
    for (size_t i = 0; i < events.size(); ++i) {
      etoks[i] = tokenize(events[i]->text);
      for (auto& w : qcontent) if (etoks[i].count(w)) ++df[w];
    }
    std::unordered_map<std::string, std::pair<std::string, double>> expand_of;
    for (auto& w : qcontent)
      for (auto& v : e.keywords().variants(w, 3))
        if (!qcontent.count(v)) expand_of[v] = {w, 0.6};
    for (auto& [v, w] : expand_of)
      for (auto& t : etoks) if (t.count(v)) ++df[v];
    double idf_sum = 0;
    std::unordered_map<std::string, double> idf;
    for (auto& w : qcontent) { idf[w] = std::log(1.0 + events.size() / (1.0 + df[w])); idf_sum += idf[w]; }
    for (auto& [v, w] : expand_of) idf[v] = std::log(1.0 + events.size() / (1.0 + df[v]));
    std::printf("[站a lex通道] 内容词:");
    for (auto& w : qcontent) std::printf(" %s(df=%d,idf=%.2f)", w.c_str(), df[w], idf[w]);
    std::printf("\n  扩展词:");
    for (auto& [v, ws] : expand_of)
      std::printf(" %s<= %s(df=%d,idf=%.2f)", v.c_str(), ws.first.c_str(), df[v], idf[v]);
    // lex 种子会收录的事件（m>=2 或 单罕见词）
    std::vector<std::pair<double, std::string>> lex_hits;
    std::set<std::string> variant_only_uids;
    for (size_t i = 0; i < events.size(); ++i) {
      int m = 0; bool rare = false; double s = 0;
      for (auto& w : qcontent) if (etoks[i].count(w)) { ++m; s += idf[w]; if (w.size() >= 6) rare = true; }
      bool direct = m > 0;
      int mv = 0; double sv = 0;
      for (auto& [v, ws] : expand_of)
        if (etoks[i].count(v)) { ++mv; s += ws.second * idf[v]; if (v.size() >= 6) rare = true; sv += ws.second * idf[v]; }
      if (m >= 2 || (m + mv >= 1 && (m + mv) >= 2) || (m + mv == 1 && rare)) {
        double wt = 0.3 + 0.7 * (idf_sum > 0 ? s / idf_sum : 0);
        lex_hits.emplace_back(wt, events[i]->uid);
        if (!direct && mv > 0) variant_only_uids.insert(events[i]->uid);
      }
      (void)sv;
    }
    std::sort(lex_hits.begin(), lex_hits.end(), std::greater<>());
    std::printf("\n  lex种子收录 %zu 条（其中仅靠扩展词命中 %zu 条）:",
                lex_hits.size(), variant_only_uids.size());
    for (size_t i = 0; i < lex_hits.size() && i < 5; ++i)
      std::printf(" %s(w=%.2f)", lex_hits[i].second.c_str(), lex_hits[i].first);
    std::printf("\n");

    // 站(b)：recall 池（explain）——查这些事件是否进池及池内 rank/融合分
    SeedQuery sq;
    sq.text = q->question;
    sq.soft_min_sim = 0.15;
    sq.soft_topk = 8;
    sq.lex_topk = 30;
    auto rr = e.recall(sq, {}, 2, 60, true);
    std::printf("[站b recall池] code=%d pool=%zu\n", (int)rr.code, rr.events.size());
    std::map<std::string, int> pool_rank;
    for (size_t i = 0; i < rr.events.size(); ++i) pool_rank[rr.events[i].event->uid] = (int)i + 1;
    for (auto& u : variant_only_uids) {
      auto it = pool_rank.find(u);
      const Node* n = e.storage().get_node(u);
      std::string dia = n && n->attrs.count("dia_id") ? n->attrs.at("dia_id") : "?";
      if (it == pool_rank.end())
        std::printf("  仅靠扩展命中 %s(dia=%s): 未进池\n", u.c_str(), dia.c_str());
      else {
        auto& se = rr.events[it->second - 1];
        std::printf("  仅靠扩展命中 %s(dia=%s): 池rank=%d 融合分=%.4f ctx=%.4f\n",
                    u.c_str(), dia.c_str(), it->second, se.score, se.context_score);
      }
    }
    // 站(c)：终排 BM25 复算（answer() 步骤4 同公式）
    std::vector<const Node*> pool_nodes;
    for (auto& se : rr.events) pool_nodes.push_back(se.event);
    std::vector<std::string> texts(pool_nodes.size());
    for (size_t i = 0; i < pool_nodes.size(); ++i) texts[i] = pool_nodes[i]->text;
    std::vector<double> cs = content_scores_for_texts(q->question, texts);
    std::vector<size_t> ord(cs.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return cs[a] > cs[b]; });
    std::printf("[站c 终排BM25] 池内按内容分排序后 top10:");
    for (size_t i = 0; i < ord.size() && i < 10; ++i) {
      auto* n = pool_nodes[ord[i]];
      std::string dia = n->attrs.count("dia_id") ? n->attrs.at("dia_id") : "?";
      std::printf(" %s:%.3f", dia.c_str(), cs[ord[i]]);
    }
    std::printf("\n  金标 dia:");
    for (auto& g : q->evidence_dia_ids) std::printf(" %s", g.c_str());
    std::printf("\n  仅靠扩展命中事件的终排位次:");
    for (auto& u : variant_only_uids) {
      int pos = -1;
      for (size_t i = 0; i < ord.size(); ++i)
        if (pool_nodes[ord[i]]->uid == u) { pos = (int)i + 1; break; }
      auto* n = e.storage().get_node(u);
      std::string dia = n && n->attrs.count("dia_id") ? n->attrs.at("dia_id") : "?";
      double c = 0;
      for (size_t i = 0; i < pool_nodes.size(); ++i)
        if (pool_nodes[i]->uid == u) c = cs[i];
      std::printf(" %s(dia=%s): BM25rank=%d/得分%.4f;", u.c_str(), dia.c_str(), pos, c);
    }
    std::printf("\n");
  }
  return 0;
}
