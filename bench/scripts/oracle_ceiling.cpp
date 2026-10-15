// oracle_ceiling.cpp —— 句级语义真实上限 oracle 测量（纯 bench 侧脚本，不动引擎代码）
//   三个实验的数据面：
//     pools : 导出 taxonomy 目标题（A 类 5 + B 类 6 + q104 近失）的标准管道候选池
//             （k_pool=60，融合序 + BM25 终排序 + dia_id + 全文，NDJSON）
//     e1    : B 类整池 LLM top-10 重选，判金标是否入选（池级语义排序天花板）
//     e2    : A 类 LLM 生成 3 条改写检索查询 → 逐查询跑同一词面 recall → 判金标进池
//             （查询扩展/扩池天花板）+ 进池非金标噪声计数
//   池构造逐行镜像 answer() 标准管道（SeedQuery 参数 / recall / 知识更新过滤），
//   LLM 走 DeepSeekClient 磁盘缓存（bench/cache，prompt 含题 id+池内容天然防污染）。
//   用法: oracle_ceiling <pools|e1|e2> <out.ndjson> [--config bench/config/deepseek.local.json]
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "ame/bench/bench.h"
#include "ame/bench/store.h"
#include "ame/core/json.h"
#include "ame/engine/engine.h"
#include "ame/llm/llm.h"
#include "ame/time/time_service.h"

using namespace ame;

namespace {

const char* kBClass[] = {"conv-26:q10", "conv-26:q101", "conv-26:q154",
                         "conv-26:q155", "conv-26:q34",  "conv-26:q50"};
const char* kAClass[] = {"conv-26:q3", "conv-26:q18", "conv-26:q64",
                         "conv-26:q105", "conv-26:q156"};
const char* kNearMiss[] = {"conv-26:q104"};

// ---- 池构造：逐行镜像 ingest_answer.cpp answer() 步骤1-3 ----
// 知识更新过滤辅助（answer() 内 static 函数的脚本侧副本，语义保持同步）
bool replaced_by_candidate(Engine& engine, const Node* ev, TimePoint qdate,
                           const std::unordered_set<std::string>& present) {
  if (qdate <= 0) return false;
  auto it = ev->attrs.find("valid_to");
  if (it == ev->attrs.end() || std::stoll(it->second) > qdate) return false;
  for (auto& [ed, nb] : engine.storage().neighbors(ev->uid, {RelType::SUPERSEDES}))
    if (nb->ts > ev->ts && present.count(nb->uid)) return true;
  return false;
}

// build_pool：复刻 answer() 的大候选池（融合排序输出序；不做 BM25 终排截断）。
// 同时返回 BM25 终排序名次（content_scores_for_texts，store.h 导出，与终排同公式）
// 与引擎现成信号特征（融合/图/语境/时间分、事件 ts）——便宜近似监督用。
struct PoolItem {
  std::string dia_id;
  std::string text;
  int bm25_rank = 0;    // 1 基；0 = 未参与（不可能，池非空时全参与）
  double bm25 = 0;      // 内容词 BM25-lite 得分（终排键）
  double fusion = 0;    // ScoredEvent.score（M7 融合分）
  double graph = 0, ctx = 0, time = 0;  // 融合分量
  double ts = 0;        // 事件时间戳（recency 特征原料）
};

std::vector<PoolItem> build_pool(Engine& e, const BenchQuestion& q, const BenchConfig& cfg) {
  std::vector<PoolItem> out;
  TimePoint qdate = parse_session_date(q.question_date, 0);
  SeedQuery sq;
  sq.text = q.question;
  sq.soft_min_sim = cfg.soft_min_sim;
  sq.soft_topk = cfg.k > 5 ? cfg.k : 8;
  sq.lex_topk = cfg.k_pool / 2;
  sq.conditional_normalize = cfg.conditional_normalize;
  sq.word_edges = cfg.word_edges;
  RecallFilters filters;
  if (qdate > 0) {
    TimePoint rel = TimeService::parse_relative(q.question, qdate);
    if (rel > 0) {
      TimePoint half = std::max<TimePoint>(4 * 86400, std::llabs(qdate - rel));
      filters.time_from = rel - half;
      filters.time_to = rel + half;
    }
  }
  auto rr = e.recall(sq, filters, 2, cfg.k_pool, false);
  std::vector<const ScoredEvent*> evs;
  std::unordered_set<std::string> present;
  for (auto& se : rr.events) present.insert(se.event->uid);
  for (auto& se : rr.events) {
    if (cfg.filter_superseded && qdate > 0 &&
        replaced_by_candidate(e, se.event, qdate, present))
      continue;
    evs.push_back(&se);
  }
  std::vector<std::string> texts(evs.size());
  std::vector<const Node*> nodes(evs.size());
  for (size_t i = 0; i < evs.size(); ++i) {
    nodes[i] = evs[i]->event;
    texts[i] = evs[i]->event->text;
  }
  std::vector<double> cs = content_scores_for_texts(q.question, texts);
  std::vector<size_t> ord(evs.size());
  for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
  std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return cs[a] > cs[b]; });
  std::vector<int> bm25_rank(evs.size(), 0);
  for (size_t r = 0; r < ord.size(); ++r) bm25_rank[ord[r]] = (int)r + 1;
  for (size_t i = 0; i < evs.size(); ++i) {
    PoolItem it;
    auto dit = evs[i]->event->attrs.find("dia_id");
    it.dia_id = dit != evs[i]->event->attrs.end() ? dit->second : "";
    it.text = evs[i]->event->text;
    it.bm25_rank = bm25_rank[i];
    it.bm25 = cs[i];
    it.fusion = evs[i]->score;
    it.graph = evs[i]->graph_score;
    it.ctx = evs[i]->context_score;
    it.time = evs[i]->time_score;
    it.ts = (double)evs[i]->event->ts;
    out.push_back(std::move(it));
  }
  return out;
}

// 用任意查询文本跑词面召回（E2 查询扩展用）：同 SeedQuery 参数，不做知识更新过滤
// （扩展查询的时间词不继承原题锚点——蓝方项：可能引入时间窗外事件，报告注明）。
std::unordered_set<std::string> recall_dias(Engine& e, const std::string& query_text,
                                            const BenchConfig& cfg) {
  SeedQuery sq;
  sq.text = query_text;
  sq.soft_min_sim = cfg.soft_min_sim;
  sq.soft_topk = cfg.k > 5 ? cfg.k : 8;
  sq.lex_topk = cfg.k_pool / 2;
  auto rr = e.recall(sq, {}, 2, cfg.k_pool, false);
  std::unordered_set<std::string> out;
  for (auto& se : rr.events) {
    auto it = se.event->attrs.find("dia_id");
    if (it != se.event->attrs.end()) out.insert(it->second);
  }
  return out;
}

std::vector<std::string> load_qids(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json j = Json::parse(ss.str(), &ok);
  std::vector<std::string> out;
  if (ok && j.kind == Json::Arr)
    for (auto& v : j.arr) out.push_back(v.as_str());
  return out;
}

std::string trunc200(const std::string& s) { return s.size() > 200 ? s.substr(0, 200) : s; }

// 装载 sample50 相关 case 并逐 case 独立引擎摄入（镜像 CLI graph 路径 rule 摄入）。
// cb(case_idx, engine, case) 对每个目标题回调。
template <typename F>
void for_each_target(const std::vector<std::string>& targets, F&& cb) {
  std::unordered_set<std::string> want(targets.begin(), targets.end());
  auto cases = load_locomo("bench/data/locomo10.json");
  auto qids = load_qids("bench/data/locomo_sample50.json");
  std::unordered_set<std::string> only(qids.begin(), qids.end());
  for (auto& c : cases) {
    bool any = false;
    for (auto& q : c.questions)
      if (want.count(q.id) && (only.empty() || only.count(q.id))) any = true;
    if (!any) continue;
    Engine e;
    ingest(e, c);
    for (auto& q : c.questions)
      if (want.count(q.id)) cb(e, q);
  }
}

Json pool_json(const std::vector<PoolItem>& pool) {
  Json arr = Json::array();
  for (auto& it : pool) {
    Json j = Json::object();
    j["dia_id"] = Json::string(it.dia_id);
    j["bm25_rank"] = Json::number((double)it.bm25_rank);
    j["bm25"] = Json::number(it.bm25);
    j["fusion"] = Json::number(it.fusion);
    j["graph"] = Json::number(it.graph);
    j["ctx"] = Json::number(it.ctx);
    j["time"] = Json::number(it.time);
    j["ts"] = Json::number(it.ts);
    j["text"] = Json::string(it.text);
    arr.push(std::move(j));
  }
  return arr;
}

std::string q_gold_key(const BenchQuestion& q) { return q.id; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "用法: oracle_ceiling <pools|e1|e2> <out.ndjson>\n");
    return 1;
  }
  std::string mode = argv[1];
  std::string out_path = argv[2];
  BenchConfig cfg;  // 默认 k=10/k_pool=60/soft_min_sim=0.15（与 off 基线同配置）

  std::vector<std::string> targets;
  if (mode == "pools" || mode == "e1")
    for (auto* s : kBClass) targets.push_back(s);
  if (mode == "pools")
    for (auto* s : kAClass) targets.push_back(s);
  if (mode == "pools")
    for (auto* s : kNearMiss) targets.push_back(s);
  if (mode == "pools50") {  // 便宜近似监督：sample50 全部 50 题的标准池特征导出
    auto all = load_qids("bench/data/locomo_sample50.json");
    targets.insert(targets.end(), all.begin(), all.end());
  }
  if (mode == "e2")
    for (auto* s : kAClass) targets.push_back(s);

  std::unique_ptr<DeepSeekClient> client;
  if (mode == "e1" || mode == "e2") {
    client = DeepSeekClient::from_config("bench/config/deepseek.local.json",
                                         "deepseek-v4-flash", "bench/cache");
    if (!client) {
      std::fprintf(stderr, "未找到 API key\n");
      return 1;
    }
  }

  FILE* out = std::fopen(out_path.c_str(), "w");
  if (!out) {
    std::fprintf(stderr, "打不开 %s\n", out_path.c_str());
    return 1;
  }

  for_each_target(targets, [&](Engine& e, const BenchQuestion& q) {
    Json rec = Json::object();
    rec["question_id"] = Json::string(q.id);
    rec["question"] = Json::string(q.question);
    Json gold = Json::array();
    for (auto& g : q.evidence_dia_ids) gold.push(Json::string(g));
    rec["gold"] = std::move(gold);

    if (mode == "pools" || mode == "pools50") {
      rec["pool"] = pool_json(build_pool(e, q, cfg));
    } else if (mode == "e1") {
      auto pool = build_pool(e, q, cfg);
      rec["pool"] = pool_json(pool);
      std::string list;
      for (size_t i = 0; i < pool.size(); ++i)
        list += "E" + std::to_string(i + 1) + " [" + pool[i].dia_id + "] " +
                trunc200(pool[i].text) + "\n";
      const std::string prompt =
          "你是检索重排器。给定一个问题（题号 " + q.id + "）和 " +
          std::to_string(pool.size()) +
          " 条候选证据（编号 E1..E" + std::to_string(pool.size()) +
          "，方括号内为来源标记，文本可能被截断）。\n"
          "任务：选出对回答该问题最有用的前 10 条证据，按相关度从高到低给出编号。\n\n"
          "问题: " +
          q.question + "\n\n" + list +
          "\n只输出编号列表（形如: E3 E1 E7），恰好 10 个或少于 10 个（如果不够相关），"
          "按相关度排序；若都不相关只输出 NONE。不要输出任何解释。";
      std::string resp = client->chat({{"user", prompt}}, {0.0, 1024, false});
      auto sel = parse_llm_rerank_ids(resp, pool.size(), 10);
      Json sel_arr = Json::array();
      std::unordered_set<std::string> sel_dias;
      for (size_t s : sel) {
        sel_arr.push(Json::number((double)s + 1));
        sel_dias.insert(pool[s].dia_id);
      }
      rec["llm_raw"] = Json::string(resp);
      rec["llm_pick_e"] = std::move(sel_arr);
      Json hit = Json::array();  // 入选且是金标的 dia_id
      for (auto& g : q.evidence_dia_ids)
        if (sel_dias.count(g)) hit.push(Json::string(g));
      rec["gold_picked"] = std::move(hit);
      std::fprintf(stderr, "[e1] %s: 池=%zu LLM选=%zu 金标入选=%zu\n", q.id.c_str(),
                   pool.size(), sel.size(), rec["gold_picked"].arr.size());
    } else if (mode == "e2") {
      auto orig = build_pool(e, q, cfg);
      std::unordered_set<std::string> orig_dias, gold(q.evidence_dia_ids.begin(),
                                                      q.evidence_dia_ids.end());
      for (auto& it : orig) orig_dias.insert(it.dia_id);
      std::unordered_set<std::string> orig_gold;
      for (auto& g : gold)
        if (orig_dias.count(g)) orig_gold.insert(g);
      const std::string prompt =
          "给定问题（题号 " + q.id +
          "），生成 3 条改写检索查询（paraphrase / expansion，用于关键词式召回，"
          "覆盖问题的不同语义侧面，可以换近义词、具体化或补充上下文）。\n"
          "问题: " +
          q.question +
          "\n\n只输出 3 行，格式：\nQ1: <查询>\nQ2: <查询>\nQ3: <查询>\n不要输出其他内容。";
      std::string resp = client->chat({{"user", prompt}}, {0.0, 1024, false});
      rec["llm_raw"] = Json::string(resp);
      std::vector<std::string> queries;
      {
        std::stringstream ss(resp);
        std::string line;
        while (std::getline(ss, line)) {
          auto p = line.find(':');
          if (p == std::string::npos) p = line.find("：");
          if (p == std::string::npos) continue;
          std::string head = line.substr(0, p), body = line.substr(p + 1);
          std::transform(head.begin(), head.end(), head.begin(),
                         [](unsigned char c) { return std::toupper(c); });
          if (head.find('Q') != std::string::npos && !body.empty()) {
            while (!body.empty() && (body.front() == ' ' || body.front() == '\t'))
              body.erase(body.begin());
            if (!body.empty()) queries.push_back(body);
          }
        }
      }
      Json qs = Json::array();
      Json per_q = Json::array();
      std::unordered_set<std::string> union_dias = orig_dias;
      int gold_hit_qs = 0;
      for (auto& qt : queries) {
        auto dias = recall_dias(e, qt, cfg);
        int gh = 0;
        for (auto& g : gold)
          if (dias.count(g)) ++gh;
        gold_hit_qs += gh > 0 ? 1 : 0;
        union_dias.insert(dias.begin(), dias.end());
        qs.push(Json::string(qt));
        Json pj = Json::object();
        pj["query"] = Json::string(qt);
        pj["pool_size"] = Json::number((double)dias.size());
        pj["gold_in_pool"] = Json::number((double)gh);
        per_q.push(std::move(pj));
      }
      // 假阳性噪声：扩展后进池的非金标 dia_id 数（相对原池的新增）
      int new_non_gold = 0;
      for (auto& d : union_dias)
        if (!orig_dias.count(d) && !gold.count(d)) ++new_non_gold;
      Json newg = Json::array();
      for (auto& g : gold)
        if (union_dias.count(g) && !orig_gold.count(g)) newg.push(Json::string(g));
      rec["queries"] = std::move(qs);
      rec["per_query"] = std::move(per_q);
      rec["orig_pool_size"] = Json::number((double)orig_dias.size());
      rec["orig_gold_in_pool"] = Json::number((double)orig_gold.size());
      rec["gold_hit_queries"] = Json::number((double)gold_hit_qs);
      rec["gold_new_in_union"] = std::move(newg);
      rec["new_non_gold_count"] = Json::number((double)new_non_gold);
      rec["union_pool_size"] = Json::number((double)union_dias.size());
      std::fprintf(stderr, "[e2] %s: 原池金标=%zu/扩展命中=%d 查询数=%zu 新噪声=%d\n",
                   q.id.c_str(), orig_gold.size(), gold_hit_qs, queries.size(), new_non_gold);
    }
    std::string line = rec.dump();
    std::fwrite(line.data(), 1, line.size(), out);
    std::fputc('\n', out);
  });
  std::fclose(out);
  if (client)
    std::fprintf(stderr, "api_calls=%d cache_hits=%d\n", client->api_calls(),
                 client->cache_hits());
  return 0;
}
