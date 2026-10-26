/*
 * 所属模块：bench 基准适配层——摄入器与问答器。
 * 本文件主体思路：
 *   ingest：把 BenchCase 的会话逐轮写入引擎（Engine::remember_turn），
 *     时间戳取会话日期解析值（失败则按顺序合成递增时间），同一 speaker 跨轮
 *     经 M2 投票复用同一实体 uid；相邻事件由 M4 自动建 NEXT 链。
 *   answer：离线基线问答——recall(text=question) 取证（含相对时间词锚定与
 *     过时事实过滤），证据最高分低于阈值则弃答。
 * 关键算法/数据结构：无复杂算法；合成时间基数 1700000000 + 会话序*1天 + 轮序*1分钟。
 * 依赖关系：依赖 bench.h、M12 engine、M10 time（parse_relative）。
 */
#include "ame/bench/bench.h"
#include "ame/bench/store.h"

#include "ame/core/json.h"
#include "ame/engine/engine.h"
#include "ame/extractor/extractor.h"
#include "ame/llm/llm.h"
#include "ame/time/time_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>  // mkstemp/close（popen 子进程传输先例同 llm/src/client.cpp）

namespace ame {

// ingest 伪代码：
//   步骤1：遍历会话；解析会话日期为基准时间，失败用合成时间
//          （1700000000 + 会话序*86400）兜底；
//   步骤2：逐轮调 Engine::remember_turn(speaker, text, ts, dia_id, session_id)，
//          轮内时间按轮序每分钟递增；统计写入事件数；
//   步骤3：返回图规模统计（事件/实体/关键词节点数）。
IngestStats ingest(Engine& engine, const BenchCase& c) {
  IngestStats st;
  const TimePoint kBase = 1700000000;
  for (size_t si = 0; si < c.sessions.size(); ++si) {
    const BenchSession& bs = c.sessions[si];
    TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
    for (size_t ti = 0; ti < bs.turns.size(); ++ti) {
      const BenchTurn& t = bs.turns[ti];
      TimePoint ts = base + (TimePoint)ti * 60;
      auto r = engine.remember_turn(t.speaker, t.text, ts, t.dia_id, bs.id);
      if (r.code == Err::Ok) ++st.events;
    }
  }
  st.entities = (int)engine.storage().nodes_of_kind(NodeKind::Entity).size();
  st.keywords = (int)engine.storage().nodes_of_kind(NodeKind::Keyword).size();
  return st;
}

// ingest(extractor 版) 伪代码：
//   步骤1：extractor 为空 → 走默认 rule 版 ingest；
//   步骤2：否则逐轮用给定提取器提取（LLM 批量提取由调用方预做后逐轮写入亦可），
//          再调 remember_turn_ex 落库；时间戳合成规则与默认版一致。
IngestStats ingest(Engine& engine, const BenchCase& c, IExtractor* extractor) {
  if (!extractor) return ingest(engine, c);
  IngestStats st;
  const TimePoint kBase = 1700000000;
  for (size_t si = 0; si < c.sessions.size(); ++si) {
    const BenchSession& bs = c.sessions[si];
    TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
    for (size_t ti = 0; ti < bs.turns.size(); ++ti) {
      const BenchTurn& t = bs.turns[ti];
      TimePoint ts = base + (TimePoint)ti * 60;
      Extraction ex = extractor->extract(t.text);
      ex.event.text = t.text;
      auto r = engine.remember_turn_ex(std::move(ex), t.speaker, ts, t.dia_id, bs.id);
      if (r.code == Err::Ok) ++st.events;
    }
  }
  st.entities = (int)engine.storage().nodes_of_kind(NodeKind::Entity).size();
  st.keywords = (int)engine.storage().nodes_of_kind(NodeKind::Keyword).size();
  return st;
}

// 知识更新过滤辅助：E 已被取代（valid_to<=qdate）且其取代者也在候选集中时才丢弃——
// "新旧都在选新"；若新值事件未被召回，旧证据仍保留兜底（防过度过滤导致全弃答）。
static bool replaced_by_candidate(Engine& engine, const Node* ev, TimePoint qdate,
                                  const std::unordered_set<std::string>& present) {
  if (qdate <= 0) return false;
  auto it = ev->attrs.find("valid_to");
  if (it == ev->attrs.end() || std::stoll(it->second) > qdate) return false;
  for (auto& [ed, nb] : engine.storage().neighbors(ev->uid, {RelType::SUPERSEDES}))
    if (nb->ts > ev->ts && present.count(nb->uid)) return true;
  return false;
}

// 证据终排（通用机制，locomo_v3 cat2 失败模式修复）：
//   think/IRCoT 通道只增候选不改序——其能量扩散带时近偏置（rerank γ·decay 项），
//   会把词面精确命中的金标证据顶到队尾（实测 "charity race" 金标 D2:1 被顶到第 9 位，
//   top-3 全是近期客套轮）。合并后统一按问题内容词 BM25-lite 精确命中重排：
//   内容词 = 问题 ASCII 小写切词 len≥3；df/idf 在候选集内统计（log(1+N/(1+df))）；
//   得分 = Σ idf·tf/(tf+1)（tf 饱和），按 idf_sum 归一；stable 排序，同分保持原序
//   （无内容信号时不扰动原通道排序）。
static std::vector<std::string> content_words(const std::string& question) {
  std::vector<std::string> words;
  std::string cur;
  auto flush = [&] {
    if (cur.size() >= 3) words.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : question) {
    if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
    else flush();
  }
  flush();
  return words;
}

// content_scores_for_texts：对一组候选文本返回每个的内容词 BM25-lite 得分
//   （store.h 导出：answer() 步骤4 与同公式复用方共用；
//   df/idf 在候选集内统计 log(1+N/(1+df))，得分 = Σ idf·tf/(tf+1) 按 idf_sum 归一）。
std::vector<double> content_scores_for_texts(const std::string& question,
                                             const std::vector<std::string>& texts) {
  std::vector<std::string> words = content_words(question);
  std::vector<double> scores(texts.size(), 0.0);
  if (words.empty() || texts.empty()) return scores;
  std::unordered_map<std::string, int> df;
  std::vector<std::unordered_map<std::string, int>> tfs(texts.size());
  for (size_t i = 0; i < texts.size(); ++i) {
    for (auto& w : content_words(texts[i])) {
      ++tfs[i][w];
    }
    for (auto& w : words)
      if (tfs[i].count(w)) ++df[w];
  }
  double idf_sum = 0;
  std::unordered_map<std::string, double> idf;
  for (auto& w : words) {
    idf[w] = std::log(1.0 + (double)texts.size() / (1.0 + df[w]));
    idf_sum += idf[w];
  }
  for (size_t i = 0; i < texts.size(); ++i) {
    double s = 0;
    for (auto& w : words) {
      auto it = tfs[i].find(w);
      if (it != tfs[i].end()) s += idf[w] * (double)it->second / (it->second + 1.0);
    }
    scores[i] = idf_sum > 0 ? s / idf_sum : 0;
  }
  return scores;
}

// content_scores：节点版委托（空指针按空文本计 0 分，与历史行为一致）
static std::vector<double> content_scores(const std::string& question,
                                          const std::vector<const Node*>& cands) {
  std::vector<std::string> texts(cands.size());
  for (size_t i = 0; i < cands.size(); ++i)
    if (cands[i]) texts[i] = cands[i]->text;
  return content_scores_for_texts(question, texts);
}

// ---- 便宜近似 v1：特征逻辑重排器（零 LLM 推理；训练见 cheap_rerank_train.py）----
// 模型 JSON：{"features": [名...], "weights": [w...], "bias": b}。特征口径与
// 训练脚本逐一对齐（bm25 / bm25_rank / fusion / graph / ctx / time /
// fusion_rank / is_entity / recency / bm25_share，minmax 均在题内池上归一）。
struct CheapModel {
  std::vector<std::string> features;
  std::vector<double> weights;
  double bias = 0;
  bool ok = false;
};

// load_cheap_model：按路径缓存加载（bench 单线程；失败返回 ok=false → 调用方降级）
static const CheapModel& load_cheap_model(const std::string& path) {
  static std::unordered_map<std::string, CheapModel> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  CheapModel m;
  std::ifstream f(path);
  if (f) {
    std::stringstream ss;
    ss << f.rdbuf();
    bool ok = false;
    Json j = Json::parse(ss.str(), &ok);
    if (ok && j.kind == Json::Obj) {
      const Json* feats = j.find("features");
      const Json* wts = j.find("weights");
      const Json* bs = j.find("bias");
      if (feats && feats->kind == Json::Arr && wts && wts->kind == Json::Arr) {
        for (auto& feat : feats->arr) m.features.push_back(feat.as_str());
        for (auto& wv : wts->arr) m.weights.push_back(wv.as_num());
        m.bias = bs ? bs->as_num() : 0.0;
        m.ok = m.features.size() == m.weights.size() && !m.features.empty();
      }
    }
  }
  return cache.emplace(path, std::move(m)).first->second;
}

// ---- svec 档外挂打分器（--rerank svec）----
// 每题候选池 spawn 一次 `python <scorer> --pool <tmp.json>`（popen 先例同 llm 的
// curl 子进程传输；引擎核心零依赖）。输入 {"question":..., "texts":[...]}，
// 期望 stdout 末行 JSON {"cos":[...]}（每候选一个余弦，长度 == texts）。
// 任一环节失败（spawn/解析/长度不符/非有限值）→ 返回空 vector，调用方降级
// cheap 原分（svec_cos 置 0，特征贡献归零）。临时文件用系统 temp + 进程内计数器
// 唯一名（bench 单线程，无并发竞争；崩溃残留由系统 temp 清理策略兜底）。
static std::vector<double> run_svec_scorer(const std::string& scorer_path,
                                           const std::string& question,
                                           const std::vector<const ScoredEvent*>& evs,
                                           double* elapsed_ms) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> out;
  char tmp_path[] = "/tmp/ame_svec_pool_XXXXXX";
  int fd = mkstemp(tmp_path);
  if (fd < 0) return out;
  {
    std::ofstream f(tmp_path);
    Json j = Json::object();
    j["question"] = Json::string(question);
    Json arr = Json::array();
    for (auto* se : evs) arr.push(Json::string(se->event->text));
    j["texts"] = arr;
    f << j.dump();
  }
  close(fd);
  std::string cmd = "python \"" + scorer_path + "\" --pool \"" + std::string(tmp_path) + "\"";
  std::string raw;
  FILE* fp = popen(cmd.c_str(), "r");
  int rc = -1;
  if (fp) {
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) raw.append(buf, n);
    rc = pclose(fp);
  }
  std::remove(tmp_path);
  *elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
  if (rc != 0 || raw.empty()) return out;
  // stdout 容忍前后噪声行：取最后一行非空行解析。
  size_t end = raw.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) return out;
  size_t begin = raw.rfind('\n', end);
  begin = (begin == std::string::npos) ? 0 : begin + 1;
  std::string last = raw.substr(begin, end - begin + 1);
  bool ok = false;
  Json j = Json::parse(last, &ok);
  if (!ok || j.kind != Json::Obj) return out;
  const Json* cos = j.find("cos");
  if (!cos || cos->kind != Json::Arr || cos->arr.size() != evs.size()) return out;
  out.reserve(evs.size());
  for (auto& v : cos->arr) {
    double x = v.as_num();
    if (!std::isfinite(x)) return {};
    out.push_back(x);
  }
  return out;
}

// ---- formula 档（--rerank formula）：七语义关系特征线性合成重排（零 LLM）----
// 特征口径与权重语义见 bench.h 声明注释；权重 CLI 可配/离线拟合注入。
// 逐候选贡献可落盘（--formula-dump），可解释性产物。
struct FormulaFeat {
  double bm25 = 0, involves = 0, related = 0, next = 0, cooccur = 0, time = 0,
         fusion = 0, opposite = 0;
};

static std::vector<std::string> question_entity_tokens(const std::string& question);

static std::unordered_set<std::string> text_words_set(const std::string& text) {
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

// query_keyword_rel：查询内容词 → 关键词节点的 RELATED 邻接表（预算一次/题）。
// rel_best[事件关键词uid] = 最大 边权×传导系数；cooccur_cnt[...] = 共现边数；
// opposite_hit[...] = 经 REL_OPPOSITE 连接（权取边权×0.5 传导）。
static void query_keyword_maps(Storage& st, const std::unordered_set<std::string>& words,
                               std::unordered_map<std::string, double>& rel_best,
                               std::unordered_map<std::string, int>& cooccur_cnt,
                               std::unordered_map<std::string, double>& opposite_hit) {
  static const std::vector<RelType> kRel = {RelType::REL_CAUSE, RelType::REL_INFER,
                                            RelType::REL_PART,  RelType::REL_SIMILAR,
                                            RelType::REL_COOCCUR};
  for (auto& w : words) {
    const Node* kw = st.find_by_name(NodeKind::Keyword, w);
    if (!kw) continue;
    for (auto& [e, other] : st.neighbors(kw->uid, &kRel)) {
      double v = e.weight * conduction_coeff(e.type);
      auto it = rel_best.find(other->uid);
      if (it == rel_best.end() || v > it->second) rel_best[other->uid] = v;
      if (e.type == RelType::REL_COOCCUR) ++cooccur_cnt[other->uid];
    }
    for (auto& [e, other] : st.neighbors(kw->uid, {RelType::REL_OPPOSITE})) {
      double v = e.weight * conduction_coeff(RelType::REL_OPPOSITE);
      auto it = opposite_hit.find(other->uid);
      if (it == opposite_hit.end() || v > it->second) opposite_hit[other->uid] = v;
    }
  }
}

// formula_score_pool：对 BM25 终排后全池逐候选算七特征并合成，返回 (总分, 特征)。
static std::pair<std::vector<double>, std::vector<FormulaFeat>> formula_score_pool(
    Engine& engine, const std::string& question, const std::string& question_date,
    const std::vector<const ScoredEvent*>& evs, const std::vector<double>& cs,
    const std::vector<double>& w) {
  Storage& st = engine.storage();
  const size_t n = evs.size();
  auto minmax = [&](const std::vector<double>& v, size_t i) {
    double lo = *std::min_element(v.begin(), v.end());
    double hi = *std::max_element(v.begin(), v.end());
    return hi > lo ? (v[i] - lo) / (hi - lo) : 0.0;
  };
  std::vector<double> fus(n);
  double bm_max = cs[0];
  for (size_t i = 0; i < n; ++i) {
    fus[i] = evs[i]->score;
    bm_max = std::max(bm_max, cs[i]);
  }
  if (bm_max <= 0) bm_max = 1.0;

  // 查询侧预算：内容词 / 实体节点 / 关键词关系图 / 时间窗
  std::unordered_set<std::string> words = text_words_set(question);
  auto ents = question_entity_tokens(question);
  std::vector<std::string> ent_uids;
  for (auto& t : ents) {
    std::string cap = t;
    if (!cap.empty()) cap[0] = (char)std::toupper((unsigned char)cap[0]);
    if (const Node* nd = st.find_by_name(NodeKind::Entity, cap)) ent_uids.push_back(nd->uid);
    else if (const Node* nd2 = st.find_by_name(NodeKind::Entity, t)) ent_uids.push_back(nd2->uid);
  }
  std::unordered_map<std::string, double> rel_best, opposite_hit;
  std::unordered_map<std::string, int> cooccur_cnt;
  query_keyword_maps(st, words, rel_best, cooccur_cnt, opposite_hit);

  // f_involves：实体 BFS（INVOLVES 双向，≤2 跳）→ 事件最小跳数
  std::unordered_map<std::string, int> hop;
  std::queue<std::pair<std::string, int>> bq;
  for (auto& u : ent_uids) bq.emplace(u, 0);
  while (!bq.empty()) {
    auto [u, d] = bq.front();
    bq.pop();
    if (d >= 2) continue;
    for (auto& [e, other] : st.neighbors(u, {RelType::INVOLVES})) {
      if (other->kind == NodeKind::Event) {
        if (!hop.count(other->uid)) hop[other->uid] = d + 1;
      }
      if (d + 1 < 2 && !hop.count(other->uid + "#n")) {
        // 实体→实体继续扩（事件已在 map 里，实体用临时标记防环）
        if (other->kind == NodeKind::Entity) {
          bq.emplace(other->uid, d + 1);
          hop[other->uid + "#n"] = 1;
        }
      }
    }
  }

  // 叙事序：按 ts 排序的位次；BM25 命中集 = cs>0
  std::vector<size_t> by_ts(n);
  for (size_t i = 0; i < n; ++i) by_ts[i] = i;
  std::stable_sort(by_ts.begin(), by_ts.end(), [&](size_t a, size_t b) {
    return evs[a]->event->ts < evs[b]->event->ts;
  });
  std::vector<int> seq(n, 0);
  for (size_t r = 0; r < n; ++r) seq[by_ts[r]] = (int)r;

  bool temporal = false;
  {
    std::string ql = question;
    for (auto& c : ql) c = (char)std::tolower((unsigned char)c);
    static const char* kTw[] = {"when", "what time", "what date", "how long", "date",
                                "before", "after",  "ago",        "recent",  "recently",
                                "order", "earlier", "later",      "以前",    "之后",  "最近"};
    for (auto* t : kTw)
      if (ql.find(t) != std::string::npos) { temporal = true; break; }
  }
  TimePoint qdate = parse_session_date(question_date, 0);
  const TimePoint win_lo = qdate > 0 ? qdate - 180LL * 86400 : 0;  // 窗=(qdate-180d, qdate]

  std::vector<double> total(n, 0.0);
  std::vector<FormulaFeat> ff(n);
  for (size_t i = 0; i < n; ++i) {
    auto& f = ff[i];
    const Node* e = evs[i]->event;
    f.bm25 = cs[i] / bm_max;
    f.fusion = minmax(fus, i);
    // f_involves
    auto hi = hop.find(e->uid);
    if (hi != hop.end()) f.involves = 1.0 / (1.0 + hi->second);
    // f_related / f_cooccur / f_opposite：事件关键词 × 查询词关系图
    for (auto& [ke, kwnode] : st.neighbors(e->uid, {RelType::CONTAINS})) {
      if (words.count(kwnode->name)) f.related = 1.0;  // 查询词直连事件关键词
      auto rb = rel_best.find(kwnode->uid);
      if (rb != rel_best.end()) f.related = std::max(f.related, rb->second);
      auto cc = cooccur_cnt.find(kwnode->uid);
      if (cc != cooccur_cnt.end()) f.cooccur += cc->second;
      auto oh = opposite_hit.find(kwnode->uid);
      if (oh != opposite_hit.end()) f.opposite = std::min(f.opposite, -oh->second);
    }
    f.cooccur = std::log1p(f.cooccur);
    // f_next：与最近 BM25 命中（cs>0）事件的叙事序距离倒数
    int best_d = 1 << 30;
    for (size_t j = 0; j < n; ++j) {
      if (cs[j] <= 0 || (int)j == (int)i) continue;
      best_d = std::min(best_d, std::abs(seq[i] - seq[j]));
    }
    if (best_d < (1 << 30)) f.next = 1.0 / (1.0 + best_d);
    // f_time：时间词题的时间窗相容（窗内 +1 / 窗外 -1；非时间题 0 不激活）
    if (temporal && qdate > 0) f.time = (e->ts > win_lo && e->ts <= qdate) ? 1.0 : -1.0;
    total[i] = w[0] * f.bm25 + w[1] * f.involves + w[2] * f.related + w[3] * f.next +
               w[4] * f.cooccur + w[5] * f.time + w[6] * f.fusion + w[7] * f.opposite;
  }
  return {total, ff};
}


// run_xenc_scorer：cross-encoder 精排外挂打分器（协议同 run_svec_scorer：
// 临时文件 JSON in / stdout 单行 JSON out / 末行容错解析），返回每候选得分；
// 失败（spawn/解析/长度/非有限）→ 空 vector，调用方降级 BM25 原序。
static std::vector<double> run_xenc_scorer(const std::string& scorer_path,
                                           const std::string& question,
                                           const std::vector<const ScoredEvent*>& evs,
                                           double* elapsed_ms) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> out;
  char tmp_path[] = "/tmp/ame_xenc_pool_XXXXXX";
  int fd = mkstemp(tmp_path);
  if (fd < 0) return out;
  {
    std::ofstream f(tmp_path);
    Json j = Json::object();
    j["question"] = Json::string(question);
    Json arr = Json::array();
    for (auto* se : evs) arr.push(Json::string(se->event->text));
    j["texts"] = arr;
    f << j.dump();
  }
  close(fd);
  std::string cmd = "python \"" + scorer_path + "\" --pool \"" + std::string(tmp_path) + "\"";
  std::string raw;
  FILE* fp = popen(cmd.c_str(), "r");
  int rc = -1;
  if (fp) {
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) raw.append(buf, n);
    rc = pclose(fp);
  }
  std::remove(tmp_path);
  *elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
  if (rc != 0 || raw.empty()) return out;
  size_t end = raw.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) return out;
  size_t begin = raw.rfind('\n', end);
  begin = (begin == std::string::npos) ? 0 : begin + 1;
  std::string last = raw.substr(begin, end - begin + 1);
  bool ok = false;
  Json j = Json::parse(last, &ok);
  if (!ok || j.kind != Json::Obj) return out;
  const Json* sc = j.find("scores");
  if (!sc || sc->kind != Json::Arr || sc->arr.size() != evs.size()) return out;
  out.reserve(evs.size());
  for (auto& v : sc->arr) {
    double x = v.as_num();
    if (!std::isfinite(x)) return {};
    out.push_back(x);
  }
  return out;
}

// question_entity_tokens：问题专名 token（大写非句首，len≥2，小写化）——
// 与 cheap_rerank_train.py / svec_eval.py 同口径。
static std::vector<std::string> question_entity_tokens(const std::string& question) {
  std::vector<std::string> out;
  std::string cur;
  bool sent_start = true;
  auto flush = [&] {
    if (cur.size() >= 2 && !sent_start && std::isupper((unsigned char)cur[0])) {
      std::string low;
      for (char c : cur) low += (char)std::tolower((unsigned char)c);
      out.push_back(low);
    }
    cur.clear();
  };
  for (size_t i = 0; i <= question.size(); ++i) {
    unsigned char c = i < question.size() ? (unsigned char)question[i] : 0;
    if (c < 128 && std::isalnum(c)) {
      cur += (char)c;
    } else {
      flush();
      sent_start = (c == '.' || c == '!' || c == '?' || c == 0);
    }
    if (c == ' ') sent_start = false;  // 句中词
  }
  return out;
}

// cheap_score_pool：对 BM25 终排后的全池逐候选算特征并打分，返回 0 基下标
// 降序（模型序）。特征归一全部题内 minmax（与训练脚本同口径）；is_entity 由
// 调用方传入的通道候选 uid 集合给出（训练数据该特征恒 0，模型权重≈0 安全外推）；
// ent_overlap = 问题专名 token（大写非句首，len≥2）出现在证据词集合中的比例
// （词面特征：实体重合，无泄漏——标签来自 LLM，特征不含任何 LLM 产物）。
// svec_cos（可空）：外挂打分器给出的原始余弦，题内 minmax 后入特征表——与
// svec_eval.py 修复后的口径一致；空指针/全同值 → 特征恒 0（权重项自然归零）。
static std::vector<size_t> cheap_score_pool(const CheapModel& m, const std::string& question,
                                            const std::vector<const ScoredEvent*>& evs,
                                            const std::vector<double>& cs,
                                            const std::unordered_set<std::string>& entity_uids,
                                            const std::vector<double>* svec_cos = nullptr) {
  size_t n = evs.size();
  auto minmax = [&](double v, double lo, double hi) { return hi > lo ? (v - lo) / (hi - lo) : 0.0; };
  std::vector<double> fus(n), gra(n), ctx(n), tim(n), rec(n);
  double f_lo = 0, f_hi = 0, g_lo = 0, g_hi = 0, c_lo = 0, c_hi = 0, t_lo = 0, t_hi = 0,
         r_lo = 0, r_hi = 0, bm_max = 0;
  for (size_t i = 0; i < n; ++i) {
    fus[i] = evs[i]->score;
    gra[i] = evs[i]->graph_score;
    ctx[i] = evs[i]->context_score;
    tim[i] = evs[i]->time_score;
    rec[i] = -(double)evs[i]->event->ts;
    bm_max = std::max(bm_max, cs[i]);
  }
  auto span = [&](const std::vector<double>& v, double& lo, double& hi) {
    lo = hi = v[0];
    for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
  };
  span(fus, f_lo, f_hi);
  span(gra, g_lo, g_hi);
  span(ctx, c_lo, c_hi);
  span(tim, t_lo, t_hi);
  span(rec, r_lo, r_hi);
  // 排名特征：BM25 序（cs 降序，stable 与训练一致）与融合分降序
  std::vector<size_t> ord_bm(n), ord_fu(n);
  for (size_t i = 0; i < n; ++i) ord_bm[i] = ord_fu[i] = i;
  std::stable_sort(ord_bm.begin(), ord_bm.end(), [&](size_t a, size_t b) { return cs[a] > cs[b]; });
  std::stable_sort(ord_fu.begin(), ord_fu.end(), [&](size_t a, size_t b) { return fus[a] > fus[b]; });
  std::vector<double> rank_bm(n), rank_fu(n);
  for (size_t r = 0; r < n; ++r) {
    rank_bm[ord_bm[r]] = 1.0 - (double)r / (double)(n - 1);
    rank_fu[ord_fu[r]] = 1.0 - (double)r / (double)(n - 1);
  }
  if (bm_max <= 0) bm_max = 1.0;
  const std::vector<std::string> q_ents = question_entity_tokens(question);
  std::vector<std::pair<double, size_t>> scored(n);
  for (size_t i = 0; i < n; ++i) {
    // 证据词集合（ASCII 小写切词，与 content_words 同口径）
    std::unordered_set<std::string> ew;
    {
      std::string cur;
      auto flush = [&] {
        if (cur.size() >= 3) ew.insert(cur);
        cur.clear();
      };
      for (unsigned char c : evs[i]->event->text) {
        if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
        else flush();
      }
      flush();
    }
    double ent_ov = 0.0;
    if (!q_ents.empty()) {
      int hit = 0;
      for (auto& t : q_ents)
        if (ew.count(t)) ++hit;
      ent_ov = (double)hit / (double)q_ents.size();
    }
    std::unordered_map<std::string, double> x = {
        {"bm25", cs[i]},
        {"bm25_rank", rank_bm[i]},
        {"fusion", minmax(fus[i], f_lo, f_hi)},
        {"graph", minmax(gra[i], g_lo, g_hi)},
        {"ctx", minmax(ctx[i], c_lo, c_hi)},
        {"time", minmax(tim[i], t_lo, t_hi)},
        {"fusion_rank", rank_fu[i]},
        {"is_entity", entity_uids.count(evs[i]->event->uid) ? 1.0 : 0.0},
        {"recency", minmax(rec[i], r_lo, r_hi)},
        {"bm25_share", cs[i] / bm_max},
        {"ent_overlap", ent_ov},
    };
    if (svec_cos != nullptr) {
      double lo = *std::min_element(svec_cos->begin(), svec_cos->end());
      double hi = *std::max_element(svec_cos->begin(), svec_cos->end());
      x["svec_cos"] = minmax((*svec_cos)[i], lo, hi);
    }
    double z = m.bias;
    for (size_t k = 0; k < m.features.size(); ++k) z += m.weights[k] * x[m.features[k]];
    double s = 1.0 / (1.0 + std::exp(-std::max(-30.0, std::min(30.0, z))));
    scored[i] = {s, i};
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<size_t> out;
  for (auto& [s, i] : scored) out.push_back(i);
  return out;
}

// parse_llm_rerank_ids 实现（容错解析，语义见 bench.h 声明）：
//   扫描 "E" + 十进制数字 token；数字越界（>n_cands）跳过；按首次出现去重；
//   凑满 k 个即停。模型输出 NONE / 空串 / 纯解释 → 空 vector。
std::vector<size_t> parse_llm_rerank_ids(const std::string& resp, size_t n_cands, size_t k) {
  std::vector<size_t> out;
  std::unordered_set<size_t> seen;
  for (size_t i = 0; i < resp.size() && out.size() < k; ++i) {
    if (resp[i] != 'E' && resp[i] != 'e') continue;
    if (i + 1 >= resp.size() || resp[i + 1] < '0' || resp[i + 1] > '9') continue;
    // 词边界：前一字符不得是字母/数字（防 "the" 里的 e 之类误配，E 前多为空白/行首）
    if (i > 0 && (std::isalnum((unsigned char)resp[i - 1]) || resp[i - 1] == '_')) continue;
    size_t j = i + 1, v = 0;
    while (j < resp.size() && resp[j] >= '0' && resp[j] <= '9') {
      v = v * 10 + (size_t)(resp[j] - '0');
      if (v > n_cands) break;  // 越界前缀不再消耗（如 E123 对 n=20 取 E12 仍会越界跳过）
      ++j;
    }
    if (v >= 1 && v <= n_cands && j == i + 1 + std::to_string(v).size() && seen.insert(v).second)
      out.push_back(v - 1);  // 转 0 基池内下标
  }
  return out;
}

// LLM 重排（句级语义 spike，--llm-rerank N）：
//   一次批量调用：给定问题与编号证据列表（dia_id + 首 200 字），让模型输出与问题
//   最相关的前 k 个编号（允许"都不相关"）。返回选中的池内 0 基下标（≤k，LLM 序）；
//   失败/空响应/无有效编号 → 空 vector，调用方降级为 BM25 原序。
// 缓存防污染：prompt 含 question_id + 逐条 dia_id + 证据截断文本——不同题、同一题
//   不同证据集（uid 噪声导致池序翻转）天然产生不同缓存键
//   （DeepSeekClient 缓存键 = FNV-1a(模型+消息+选项)），与 answer/judge 缓存不撞。
// llm_rerank_select：一次批量调用重选。prompt 含"词面精确命中必须保留靠前"
// 指令（T3 A/B 胜出并入正形：0.646→0.667、13 救 0 失）——与 --rerank-guard
// 执行侧兜底配套（指令防挤、guard 防漏）。
static std::vector<size_t> llm_rerank_select(ILlmClient& client, const BenchQuestion& q,
                                             const std::vector<const Node*>& cands, size_t k) {
  std::string list;
  for (size_t i = 0; i < cands.size(); ++i) {
    std::string snip = cands[i] ? cands[i]->text : "";
    if (snip.size() > 200) snip.resize(200);  // 蓝方项：截断丢信息风险见报告
    std::string dia;
    if (cands[i]) {
      auto it = cands[i]->attrs.find("dia_id");
      if (it != cands[i]->attrs.end()) dia = it->second;
    }
    list += "E" + std::to_string(i + 1) + " [" + dia + "] " + snip + "\n";
  }
  const std::string prompt =
      "你是检索重排器。给定一个问题（题号 " + q.id + "）和 " +
      std::to_string(cands.size()) +
      " 条编号证据（方括号内为来源标记，文本可能被截断）。\n"
      "任务：选出对回答该问题最有用的证据，按相关度从高到低给出编号；"
      "若证据都不相关，只输出 NONE。\n\n"
      "问题: " +
      q.question + "\n\n" + list +
      "\n注意：若某条证据与问题存在关键词的精确重合（词面直接命中），"
      "必须将其保留在靠前位置，不得因其他证据语义相近而挤掉它。"
      "\n只输出编号列表（形如: E3 E1 E7），最多 " +
      std::to_string(k) + " 个，按相关度排序。不要输出任何解释。";
  std::string resp = client.chat({{"user", prompt}}, {0.0, 1024, false});
  if (resp.empty()) return {};  // 无网/失败：优雅降级
  return parse_llm_rerank_ids(resp, cands.size(), k);
}

// ---- 两阶段检索（实体档案·检索角色修正，设计文档 8.3）----
// 混合查询（已知识别人名 + 内容词）：实体只做候选池圈定（lookup），内容词在池内
// 用 IDF 词面重叠驱动排序，实体不再当扩散能量源（防能量垄断）；
// 纯实体查询：直查档案槽位当前值。无命中返回空结果，调用方回退 recall 管道。
static AnswerResult answer_entity_channel(Engine& engine, const BenchQuestion& q,
                                          const BenchConfig& cfg) {
  AnswerResult ar;
  // 步骤1：问题中的大写词对齐已知实体（人名通道）
  std::vector<const Node*> persons;
  {
    std::string cur;
    bool sent_start = true;
    auto try_tok = [&](const std::string& tok) {
      if (tok.size() < 2 || !std::isupper((unsigned char)tok[0])) return;
      const Node* n = engine.storage().find_by_name(NodeKind::Entity, tok);
      if (n && n->type == "person") persons.push_back(n);
    };
    for (size_t i = 0; i <= q.question.size(); ++i) {
      unsigned char c = i < q.question.size() ? (unsigned char)q.question[i] : 0;
      if (c < 128 && std::isalnum(c)) {
        cur += (char)c;
      } else {
        if (!sent_start) try_tok(cur);  // 句首大写词跳过（歧义）
        cur.clear();
        sent_start = (c == '.' || c == '!' || c == '?' || c == 0);
      }
      if (c == ' ') sent_start = false;  // 句中词
    }
  }
  if (persons.empty()) {
    // 第一人称映射（通用规则）：无显式人名但含 I/my/me → haystack 主说话人实体
    //（对话记忆中第一人称即主角/库主）。角色感知选择：优先用户角色名
    //（user/me/我/用户），排除助手角色名（assistant/ai/bot/助手）——实测 LME 中
    // assistant 的 INVOLVES 度（258）高于 user（250），按度数盲选会错选助手，
    // 偏好题检索池随之错位（金标在 user 池，不在 assistant 池）。
    static const std::unordered_set<std::string> kUserRole = {"user", "me", "self",
                                                              "我", "用户"};
    static const std::unordered_set<std::string> kAssistantRole = {"assistant", "ai",
                                                                   "bot", "助手"};
    std::string ql;
    for (char c : q.question) ql += (char)std::tolower((unsigned char)c);
    bool first_person = ql.find(" i ") != std::string::npos ||
                        ql.rfind("i ", 0) == 0 || ql.find(" my ") != std::string::npos ||
                        ql.find(" me ") != std::string::npos ||
                        ql.find(" i'") != std::string::npos;
    if (first_person) {
      const Node* best = nullptr;   // 度数最高的非助手 person（兜底）
      size_t best_deg = 0;
      const Node* urole = nullptr;  // 度数最高的用户角色 person（优先）
      size_t urole_deg = 0;
      for (auto* n : engine.storage().nodes_of_kind(NodeKind::Entity)) {
        if (n->type != "person") continue;
        std::string low;
        for (char c : n->name) low += (char)std::tolower((unsigned char)c);
        if (kAssistantRole.count(low)) continue;
        size_t deg = engine.storage().neighbors(n->uid, {RelType::INVOLVES}).size();
        if (kUserRole.count(low)) {
          if (deg > urole_deg) { urole = n; urole_deg = deg; }
        }
        if (deg > best_deg) { best = n; best_deg = deg; }
      }
      if (urole) persons.push_back(urole);
      else if (best) persons.push_back(best);
    }
  }
  if (persons.empty()) return ar;  // 无实体锚点 → 回退

  // 步骤2：内容词 = 问题内容词去掉实体名（复用词法切词，ASCII 小写 len≥3）
  std::unordered_set<std::string> ent_low;
  for (auto* p : persons) {
    std::string low;
    for (char c : p->name) low += (char)std::tolower((unsigned char)c);
    ent_low.insert(low);
  }
  std::vector<std::string> content;
  {
    std::string cur;
    auto flush = [&] {
      if (cur.size() >= 3 && !ent_low.count(cur)) content.push_back(cur);
      cur.clear();
    };
    for (unsigned char c : q.question) {
      if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
      else flush();
    }
    flush();
  }

  // 步骤3：纯实体/属性查询 → 槽位直查（内容词命中槽位名或槽位映射词表）
  static const std::unordered_map<std::string, std::string> kSlotHint = {
      {"live", "residence"}, {"lives", "residence"}, {"living", "residence"},
      {"work", "job"}, {"works", "job"}, {"working", "job"}, {"job", "job"},
      {"like", "preference"}, {"likes", "preference"}, {"prefer", "preference"},
      {"favorite", "preference"}, {"love", "preference"}, {"loves", "preference"}};
  EntityProfile prof = engine.get_entity_profile(persons.front()->uid);
  if (!prof.slots.empty()) {
    for (auto& w : content) {
      std::string slot = w;
      auto hit = kSlotHint.find(w);
      if (hit != kSlotHint.end()) slot = hit->second;
      std::string val = prof.current_slot(slot);
      if (!val.empty()) {
        ar.answer_text = val;
        ar.score = 1.0;
        ar.abstained = false;
        if (!prof.slots.empty()) ar.evidence_uids.push_back(prof.slots.front().source_event);
        return ar;
      }
    }
  }
  if (content.empty()) return ar;  // 纯人名无内容词 → 不处理，回退

  // 步骤4：混合查询 → 候选池圈定 + 池内 BM25-lite 排序
  //   （tf 饱和 tf/(tf+1) × IDF，长度归一；同义词扩展命中按 0.6 打折）
  LookupQuery lq;
  lq.entity_uids = {persons.front()->uid};
  lq.limit = 1000;
  auto pool = engine.lookup(lq);
  if (pool.empty()) return ar;
  // 知识更新过滤（与标准 recall 管道同语义）：新旧都在选新——被取代旧事实仅当
  // 其取代者也在候选池中才丢弃（新值未召回时旧证据保留兜底）
  if (cfg.filter_superseded) {
    TimePoint qdate = parse_session_date(q.question_date, 0);
    if (qdate > 0) {
      std::unordered_set<std::string> present;
      for (auto* ev : pool) present.insert(ev->uid);
      std::vector<const Node*> kept;
      for (auto* ev : pool)
        if (!replaced_by_candidate(engine, ev, qdate, present)) kept.push_back(ev);
      pool = std::move(kept);
      if (pool.empty()) return ar;
    }
  }
  // 同义词扩展（M3 反向别名，每词 ≤3 个）
  std::unordered_map<std::string, std::string> expand_of;
  for (auto& w : content)
    for (auto& v : engine.keywords().variants(w, 3)) expand_of[v] = w;
  // df/tf 统计（池内）
  std::unordered_map<std::string, int> df;
  std::vector<std::unordered_map<std::string, int>> ptf;  // 每事件 词->tf
  std::vector<int> plen;
  double avg_len = 0;
  for (auto* ev : pool) {
    std::unordered_map<std::string, int> tf;
    std::string cur;
    auto flush = [&] {
      if (cur.size() >= 3) ++tf[cur];
      cur.clear();
    };
    for (unsigned char c : ev->text) {
      if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
      else flush();
    }
    flush();
    int len = 0;
    for (auto& [w, c2] : tf) len += c2;
    avg_len += len;
    plen.push_back(len);
    for (auto& w : content)
      if (tf.count(w)) ++df[w];
    for (auto& [v, w] : expand_of)
      if (tf.count(v)) ++df[v];
    ptf.push_back(std::move(tf));
  }
  avg_len = pool.empty() ? 1.0 : avg_len / pool.size();
  double idf_sum = 0;
  std::unordered_map<std::string, double> idf;
  for (auto& w : content) {
    idf[w] = std::log(1.0 + (double)pool.size() / (1.0 + df[w]));
    idf_sum += idf[w];
  }
  for (auto& [v, w] : expand_of)
    idf[v] = std::log(1.0 + (double)pool.size() / (1.0 + df[v]));
  std::vector<std::pair<double, const Node*>> scored;
  for (size_t i = 0; i < pool.size(); ++i) {
    double s = 0;
    double len_norm = std::sqrt(avg_len / std::max(1, plen[i]));
    (void)len_norm;
    for (auto& w : content) {
      auto it = ptf[i].find(w);
      if (it != ptf[i].end()) s += idf[w] * (double)it->second / (it->second + 1.0);
    }
    for (auto& [v, w] : expand_of) {
      auto it = ptf[i].find(v);
      if (it != ptf[i].end()) s += 0.6 * idf[v] * (double)it->second / (it->second + 1.0);
    }
    (void)len_norm;  // A/B 显示长度归一偏好短客套轮，已停用
    if (s > 0) scored.emplace_back(idf_sum > 0 ? s / idf_sum : 0, pool[i]);
  }
  if (scored.empty()) return ar;
  std::sort(scored.begin(), scored.end(),
            [](auto& a, auto& b) { return a.first > b.first; });
  // 排序类问题（通用模式：in what order / order of / chronological / 顺序/先后）：
  // 放宽取证上限并按时间戳全局排序组合（order_events 语义接入查询规划）
  bool is_order = false;
  {
    std::string ql;
    for (char c : q.question) ql += (char)std::tolower((unsigned char)c);
    is_order = ql.find("in what order") != std::string::npos ||
               ql.find("what order") != std::string::npos ||
               ql.find("order of") != std::string::npos ||
               ql.find("chronological") != std::string::npos ||
               ql.find("earliest to latest") != std::string::npos ||
               q.question.find("顺序") != std::string::npos ||
               q.question.find("先后") != std::string::npos;
  }
  if ((int)scored.size() > (is_order ? 12 : cfg.k)) scored.resize(is_order ? 12 : cfg.k);
  ar.score = scored.front().first;
  ar.abstained = ar.score < cfg.abstain_threshold;
  if (is_order) {
    std::sort(scored.begin(), scored.end(),
              [](auto& a, auto& b) { return a.second->ts < b.second->ts; });
  }
  for (auto& [w, ev] : scored) {
    ar.evidence_uids.push_back(ev->uid);
    auto it = ev->attrs.find("dia_id");
    if (it != ev->attrs.end()) ar.evidence_dia_ids.push_back(it->second);
    auto sit = ev->attrs.find("session_id");
    if (sit != ev->attrs.end() && !sit->second.empty())
      ar.evidence_dia_ids.push_back("S:" + sit->second);
  }
  if (ar.abstained) {
    ar.answer_text = "unanswerable";
  } else {
    // 按事件时间先后拼接（叙事序）；排序题放宽到 5 段（完整时序组合）
    std::sort(scored.begin(), scored.end(),
              [](auto& a, auto& b) { return a.second->ts < b.second->ts; });
    size_t cap = is_order ? 5 : 3;
    std::string text;
    for (size_t i = 0; i < scored.size() && i < cap; ++i) {
      if (!text.empty()) text += " ";
      text += scored[i].second->text;
    }
    ar.answer_text = text;
  }
  return ar;
}

// 时序/多跳题通用判定（中英常见时序词表）
static bool is_temporal_question(const std::string& question) {
  static const char* kTemporal[] = {"when",     "before", "after", "first",  "last",
                                    "how long", "how many", "recently", "order",
                                    "earlier",  "later",  "以前",   "之后",  "最近"};
  std::string ql;
  for (char c : question) ql += (char)std::tolower((unsigned char)c);
  for (auto* w : kTemporal)
    if (ql.find(w) != std::string::npos) return true;
  return false;
}

// think 证据合并辅助：跑 M8 多轮推理，把 ReasoningChain findings 事件按 uid 去重
// 追加到 ar 的证据列表（实体通道/recall 通道共用，避免两份逻辑）；
// qdate>0 时跳过已被 SUPERSEDES 取代（valid_to<=qdate）的过时事件（与取证过滤同语义）。
static void merge_think_evidence(Engine& engine, const std::string& question, int rounds,
                                 AnswerResult& ar, TimePoint qdate = 0) {
  auto chain = engine.think(question, rounds);
  std::unordered_set<std::string> have(ar.evidence_uids.begin(), ar.evidence_uids.end());
  // 先收全部 findings 建候选集（新旧都在选新语义需要判断取代者是否在场）
  std::unordered_set<std::string> present(have);
  for (auto& round : chain.rounds)
    for (auto& f : round.findings)
      if (f.event) present.insert(f.event->uid);
  for (auto& round : chain.rounds)
    for (auto& f : round.findings) {
      if (!f.event || have.count(f.event->uid)) continue;
      if (qdate > 0 && replaced_by_candidate(engine, f.event, qdate, present))
        continue;  // 有新弃旧；新值不在场则旧证据保留
      have.insert(f.event->uid);
      ar.evidence_uids.insert(ar.evidence_uids.begin(), f.event->uid);  // think 证据排前
      auto it = f.event->attrs.find("dia_id");
      if (it != f.event->attrs.end())
        ar.evidence_dia_ids.insert(ar.evidence_dia_ids.begin(), it->second);
    }
}


//   步骤0（实体通道，use_entity_profile 开启时）：问题命中已知识别人名实体 →
//          两阶段检索（候选池圈定+池内 IDF 排序）或槽位直查；时序题叠加 think 证据；
//          通道无产出才进入标准管道；
//   步骤1：构造 SeedQuery（text=question，软种子阈值取配置值）；若 question_date
//          可解析且问题文本含相对时间词（M10 parse_relative），以 question_date
//          为锚设置时间范围过滤（窗口半宽随短语粗粒度放宽）；
//   步骤2：调 Engine::recall 取大候选池（k_pool），知识更新过滤后不提前截断；
//   步骤3（知识更新过滤）：若开启 filter_superseded，丢弃 attrs.valid_to 非空
//          且 <= 提问时刻的过时事件（被 SUPERSEDES 取代的旧事实）；
//   步骤4：证据合并（recall + think）后按问题内容词 BM25-lite 终排并截 top-k，
//          证据最高分低于 abstain_threshold 或无证据 → abstained=true，
//          答案记 "unanswerable"；否则拼接 top3 证据事件文本为答案；
//   步骤5：收集证据 uid 与 dia_id（LoCoMo 证据命中统计用）返回。
AnswerResult answer(Engine& engine, const BenchQuestion& q, const BenchConfig& cfg) {
  TimePoint qdate = parse_session_date(q.question_date, 0);
  std::vector<std::string> entity_extra_uids;  // 低置信放行：通道候选并入标准池（去重后）
  // 步骤0：实体档案通道（检索角色修正）
  if (cfg.use_entity_profile) {
    AnswerResult ent = answer_entity_channel(engine, q, cfg);
    // 通道弃权（证据分低于阈值）时不采用其弱结果，继续走标准管道——弱通道结果
    // 不应遮蔽标准召回（LME 偏好题实测：通道错选助手实体返回弱证据弃答，
    // 而标准管道本可召回金标，recall 0.233→1.0 单题验证）。
    if (!ent.abstained && (!ent.evidence_uids.empty() || !ent.answer_text.empty())) {
      // 低置信放行（--entity-pass-threshold，默认 -1=关）：通道榜首内容分
      // （槽位直查恒 1.0）低于阈值 → 不整通道返回，候选并入标准管道池子继续流程；
      // 高置信 → 维持整通道返回（下方逐行现状）。
      bool pass_through =
          cfg.entity_pass_threshold >= 0.0 && ent.score < cfg.entity_pass_threshold;
      if (!pass_through) {
        // 时序/多跳题：实体通道结果上叠加 think 证据（与标准管道共用 merge_think_evidence，
        // 旧事实（valid_to<=qdate）在 think 证据中同样剔除）
        if (cfg.use_think && is_temporal_question(q.question))
          merge_think_evidence(engine, q.question, cfg.think_rounds, ent, qdate);
        // 证据终排（与标准管道同机制）：think 证据并入后按问题内容词 BM25-lite
        // 精确命中重排，防能量/时近偏置盖过词面精确命中（stable，同分保持原序）。
        if (ent.evidence_uids.size() > 1) {
          std::vector<const Node*> nodes;
          std::vector<size_t> ord(ent.evidence_uids.size());
          for (size_t i = 0; i < ord.size(); ++i) {
            ord[i] = i;
            nodes.push_back(engine.storage().get_node(ent.evidence_uids[i]));
          }
          std::vector<double> cs = content_scores(q.question, nodes);
          std::stable_sort(ord.begin(), ord.end(),
                           [&](size_t a, size_t b) { return cs[a] > cs[b]; });
          std::vector<std::string> reranked;
          for (size_t i : ord) reranked.push_back(ent.evidence_uids[i]);
          ent.evidence_uids = std::move(reranked);
        }
        if ((cfg.rerank_mode == "llm" && cfg.llm_client) || cfg.rerank_mode == "cheap" ||
            cfg.rerank_mode == "svec" || cfg.rerank_mode == "formula" ||
            cfg.rerank_mode == "xenc")
          std::fprintf(stderr, "  [llm-rerank] %s: 实体档案通道提前返回，未重排\n",
                       q.id.c_str());
        return ent;
      }
      entity_extra_uids = ent.evidence_uids;  // 不丢：并入标准池（下方按 uid 去重）
      std::fprintf(stderr, "  [entity-pass] %s: 通道低置信 %.3f < %.2f，%zu 条候选并入标准池\n",
                   q.id.c_str(), ent.score, cfg.entity_pass_threshold,
                   entity_extra_uids.size());
    }
  }
  AnswerResult ar;
  SeedQuery sq;
  sq.text = q.question;
  sq.soft_min_sim = cfg.soft_min_sim;
  sq.soft_topk = cfg.k > 5 ? cfg.k : 8;
  sq.lex_topk = cfg.k_pool / 2;  // 词法通道配额：候选池的一半
  sq.conditional_normalize = cfg.conditional_normalize;

  RecallFilters filters;
  if (qdate > 0) {
    TimePoint rel = TimeService::parse_relative(q.question, qdate);
    if (rel > 0) {
      // 窗口半宽：短语越粗略窗口越宽（"昨天"≈±4天，"几个月前"≈±90天）
      TimePoint half = std::max<TimePoint>(4 * 86400, std::llabs(qdate - rel));
      filters.time_from = rel - half;
      filters.time_to = rel + half;
    }
  }

  auto rr = engine.recall(sq, filters, 2, cfg.k_pool, false);  // 大候选池
  std::vector<const ScoredEvent*> evs;
  std::unordered_set<std::string> present;  // 候选集（新旧都在选新用）
  for (auto& se : rr.events) present.insert(se.event->uid);
  for (auto& se : rr.events) {
    if (cfg.filter_superseded && qdate > 0 &&
        replaced_by_candidate(engine, se.event, qdate, present))
      continue;  // 有新弃旧；新值未召回则旧证据保留
    evs.push_back(&se);
  }
  // 低置信放行合并：实体通道候选按 uid 去重并入池（来源标记 = 本段日志；
  // ScoredEvent 分数置 0——终排只看内容词 BM25，融合分仅 final_mix 开启时使用，
  // 该通道默认关闭）。通道候选已过其自身的知识更新过滤（语义与标准管道同）。
  // 注意：先完整构建 entity_extra 再统一取指针——边 push 边取 &back() 会在
  // realloc 时悬空（segfault 实测教训）。
  std::vector<ScoredEvent> entity_extra;  // 值持有，指针稳定
  for (auto& uid : entity_extra_uids) {
    if (present.count(uid)) continue;  // 与 recall 池重叠 → 去重，不重复占位
    const Node* n = engine.storage().get_node(uid);
    if (!n) continue;
    present.insert(uid);
    ScoredEvent se{};
    se.event = n;
    entity_extra.push_back(std::move(se));
  }
  for (auto& se : entity_extra) evs.push_back(&se);
  // 注意：此处不再提前截断——融合排序（能量/语境/时近）只决定候选池成员资格，
  // top-k 选择推迟到内容词终排之后（见步骤4），避免词面精确命中的金标在池内
  // 排名 11~k_pool 时被提前裁掉（LME 偏好题实测失败模式）。

  // think 通道（M8 接入答题）：时序/多跳题型（通用时序词表判定）或 recall 最高分
  // 低于弃答阈值时升级为多轮推理，ReasoningChain 各轮 findings 事件按 uid 去重并入
  // 证据（合并后统一内容词终排，见步骤4）。协作说明：think 的种子走 M5（词法/实体
  // 硬种子 + 软种子），实体能量垄断由 M6 出度归一与 M7 事件级归一约束，无需在答题层重复圈池。
  std::vector<ScoredEvent> think_extra;  // think 通道证据（值持有，指针稳定）
  if (cfg.use_think) {
    static const char* kTemporal[] = {"when",   "before", "after", "first",  "last",
                                      "how long", "how many", "recently", "order",
                                      "earlier", "later",   "以前",   "之后",  "最近"};
    std::string ql;
    for (char c : q.question) ql += (char)std::tolower((unsigned char)c);
    bool temporal = false;
    for (auto* w : kTemporal)
      if (ql.find(w) != std::string::npos) { temporal = true; break; }
    double top_score = evs.empty() ? 0.0 : evs.front()->score;
    if (temporal || top_score < cfg.abstain_threshold) {
      auto chain = engine.think(q.question, cfg.think_rounds);
      std::unordered_set<std::string> have;
      for (auto* se : evs) have.insert(se->event->uid);
      std::unordered_set<std::string> tpresent(have);  // 候选集含 think findings
      for (auto& round : chain.rounds)
        for (auto& f : round.findings)
          if (f.event) tpresent.insert(f.event->uid);
      for (auto& round : chain.rounds)
        for (auto& f : round.findings) {
          if (!f.event || have.count(f.event->uid)) continue;
          if (cfg.filter_superseded && qdate > 0 &&
              replaced_by_candidate(engine, f.event, qdate, tpresent))
            continue;  // think 证据同样"新旧都在选新"
          have.insert(f.event->uid);
          think_extra.push_back(f);
        }
    }
  }
  if (!think_extra.empty()) {
    // think 证据并入（终排前不区分来源），recall 证据按原序保留在后
    std::vector<const ScoredEvent*> merged;
    for (auto& x : think_extra) merged.push_back(&x);
    for (auto* se : evs) merged.push_back(se);
    evs = std::move(merged);
  }
  // 证据终排：对 recall 大候选池（k_pool，未提前截断）+ think 并入证据，统一按问题
  // 内容词 BM25-lite 精确命中重排（stable，同分保持原序）后才截 top-k——融合排序的
  // 能量/时近偏置只决定候选资格，不允许盖过词面精确命中。
  // L2 终排混合（--final-alpha/--final-beta 开启）：排序键从纯内容分改为
  //   final = α·BM25内容分 + β·minmax_norm(1.x 融合分)
  // 归一在本题候选集内 min-max（每题独立，无跨 case 泄漏）；默认关闭 = 历史行为。
  double top_content = 0.0;  // 终排后榜首的内容词得分（弃答门控用）
  if (evs.size() > 1) {
    std::vector<const Node*> nodes;
    for (auto* se : evs) nodes.push_back(se->event);
    std::vector<double> cs = content_scores(q.question, nodes);
    std::vector<double> key = cs;  // 排序键：默认即内容分
    if (cfg.final_mix) {
      double mn = evs.front()->score, mx = mn;
      for (auto* se : evs) {
        mn = std::min(mn, se->score);
        mx = std::max(mx, se->score);
      }
      for (size_t i = 0; i < evs.size(); ++i) {
        double norm = mx > mn ? (evs[i]->score - mn) / (mx - mn) : 0.0;
        key[i] = cfg.final_alpha * cs[i] + cfg.final_beta * norm;
      }
    }
    std::vector<size_t> ord(evs.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(),
                     [&](size_t a, size_t b) { return key[a] > key[b]; });
    std::vector<const ScoredEvent*> reranked;
    for (size_t i : ord) reranked.push_back(evs[i]);
    evs = std::move(reranked);
    top_content = cs[ord.front()];
    // cs 与重排后 evs 重新对齐（便宜近似特征按下标取 bm25；LLM 路径不用 cs 逐候选，无此问题）
    {
      std::vector<double> csr(evs.size());
      for (size_t r = 0; r < ord.size(); ++r) csr[r] = cs[ord[r]];
      cs = std::move(csr);
    }
    // BM25 强命中豁免（--rerank-guard，默认 -1=关）：榜首内容词分数 ≥ 阈值时
    // 记住榜首 uid，LLM 重排后保底首槽（q100 型假阳性防护）。
    const Node* guard_top = (cfg.rerank_guard >= 0.0 && top_content >= cfg.rerank_guard)
                                ? evs.front()->event
                                : nullptr;
    // 重排选中通用执行：sel = 池内 0 基下标（选中序，≤k 个）→ 置顶 + 其余 BM25
    // 原序补齐 + 强命中豁免保底首槽。LLM 与便宜近似两路共用。返回选中 dia 串（调用方记日志）。
    auto apply_selection = [&](const std::vector<size_t>& sel) -> std::string {
      std::vector<char> used(evs.size(), 0);
      std::vector<const ScoredEvent*> rr;
      std::string sel_dias;
      for (size_t s : sel) {
        rr.push_back(evs[s]);
        used[s] = 1;
        auto it = evs[s]->event->attrs.find("dia_id");
        sel_dias += (sel_dias.empty() ? "" : ",") +
                    (it != evs[s]->event->attrs.end() ? it->second : "-");
      }
      for (size_t i = 0; i < evs.size(); ++i)
        if (!used[i]) rr.push_back(evs[i]);
      evs = std::move(rr);
      // 豁免执行：BM25 强命中榜首未在首槽 → 前置保底（不丢选中项，仅整体
      // 后移一位；选中榜首时无操作）。
      if (guard_top && evs.front()->event != guard_top) {
        for (size_t i = 1; i < evs.size(); ++i) {
          if (evs[i]->event == guard_top) {
            const ScoredEvent* pin = evs[i];
            evs.erase(evs.begin() + i);
            evs.insert(evs.begin(), pin);
            break;
          }
        }
        auto it = guard_top->attrs.find("dia_id");
        std::fprintf(stderr, "  [rerank-guard] %s: BM25 榜首 %s（%.3f≥%.2f）豁免，保底首槽\n",
                     q.id.c_str(),
                     it != guard_top->attrs.end() ? it->second.c_str() : "-",
                     top_content, cfg.rerank_guard);
      }
      return sel_dias;
    };
    // 句级语义终排三档（--rerank bm25|cheap|llm，默认 bm25 = 本段不执行）：
    // llm = 全池/窗口 LLM 语义打分（-all 或 llm_rerank<=0 时全池）；cheap = 特征
    // 逻辑重排器（零 LLM）；失败/无有效选中/模型缺失 → 均优雅降级 BM25 原序。
    if (cfg.rerank_mode == "llm" && cfg.llm_client) {
      size_t n = (cfg.llm_rerank_all || cfg.llm_rerank <= 0)
                     ? evs.size()
                     : std::min<size_t>((size_t)cfg.llm_rerank, evs.size());
      std::vector<const Node*> cands;
      std::string cand_dias;  // 诊断：候选 dia_id 列表（金标是否进 top-N 的判定依据）
      for (size_t i = 0; i < n; ++i) {
        cands.push_back(evs[i]->event);
        auto it = evs[i]->event->attrs.find("dia_id");
        cand_dias += (i ? "," : "") + (it != evs[i]->event->attrs.end() ? it->second : "-");
      }
      std::vector<size_t> sel =
          llm_rerank_select(*cfg.llm_client, q, cands, (size_t)cfg.k);
      if (sel.empty()) {
        std::fprintf(stderr, "  [llm-rerank] %s: LLM 无有效输出，降级 BM25 原序 | 候选: %s\n",
                     q.id.c_str(), cand_dias.c_str());
      } else {
        std::string sel_dias = apply_selection(sel);
        std::fprintf(stderr,
                     "  [llm-rerank] %s: LLM 置顶 %zu/%zu 条 | 榜首内容分 %.3f | 选中: %s | 候选: %s\n",
                     q.id.c_str(), sel.size(), n, top_content, sel_dias.c_str(),
                     cand_dias.c_str());
      }
    } else if (cfg.rerank_mode == "cheap") {
      // 便宜近似 v1：特征逻辑重排（零 LLM 零网络；未配模型/加载失败 → 降级 BM25 原序）。
      if (cfg.cheap_rerank_path.empty()) {
        std::fprintf(stderr, "  [cheap-rerank] %s: cheap 档未配模型路径，降级 BM25 原序\n",
                     q.id.c_str());
      } else {
        const CheapModel& m = load_cheap_model(cfg.cheap_rerank_path);
        if (!m.ok) {
          std::fprintf(stderr, "  [cheap-rerank] %s: 模型加载失败，降级 BM25 原序\n",
                       q.id.c_str());
        } else {
          std::unordered_set<std::string> e_uids(entity_extra_uids.begin(),
                                                 entity_extra_uids.end());
          auto order_c = cheap_score_pool(m, q.question, evs, cs, e_uids);
          size_t kk = std::min<size_t>((size_t)cfg.k, order_c.size());
          std::string sel_dias =
              apply_selection(std::vector<size_t>(order_c.begin(), order_c.begin() + kk));
          std::fprintf(stderr,
                       "  [cheap-rerank] %s: 置顶 %zu/%zu 条 | 榜首内容分 %.3f | 选中: %s\n",
                       q.id.c_str(), kk, evs.size(), top_content, sel_dias.c_str());
        }
      }
    } else if (cfg.rerank_mode == "svec") {
      // 句向量档：cheap 特征 + svec_cos（外挂 python 打分器，每题一次 spawn）。
      // 模型缺失 → 降级 BM25 原序；scorer 失败 → 该题降级 cheap 原分
      //（svec_cos 全同值 → 题内 minmax 恒 0 → 该特征权重项自然归零，排序由
      // 其余 11 特征承担——与 cheap 档同级的可用性兜底，不降级 BM25 原序）。
      if (cfg.svec_rerank_path.empty()) {
        std::fprintf(stderr, "  [svec-rerank] %s: svec 档未配模型路径，降级 BM25 原序\n",
                     q.id.c_str());
      } else {
        const CheapModel& m = load_cheap_model(cfg.svec_rerank_path);
        if (!m.ok) {
          std::fprintf(stderr, "  [svec-rerank] %s: 模型加载失败，降级 BM25 原序\n",
                       q.id.c_str());
        } else {
          double scorer_ms = 0.0;
          std::vector<double> cos = run_svec_scorer(cfg.svec_scorer, q.question, evs, &scorer_ms);
          const bool scorer_ok = cos.size() == evs.size();
          if (!scorer_ok) {
            std::fprintf(stderr,
                         "  [svec-rerank] %s: scorer 不可用（%.0fms），降级 cheap 原分（svec_cos 置 0）\n",
                         q.id.c_str(), scorer_ms);
            cos.assign(evs.size(), 0.0);
          }
          std::unordered_set<std::string> e_uids(entity_extra_uids.begin(),
                                                 entity_extra_uids.end());
          auto order_c = cheap_score_pool(m, q.question, evs, cs, e_uids, &cos);
          size_t kk = std::min<size_t>((size_t)cfg.k, order_c.size());
          std::string sel_dias =
              apply_selection(std::vector<size_t>(order_c.begin(), order_c.begin() + kk));
          std::fprintf(stderr,
                       "  [svec-rerank] %s: 置顶 %zu/%zu 条 | scorer %.0fms%s | 榜首内容分 %.3f | 选中: %s\n",
                       q.id.c_str(), kk, evs.size(), scorer_ms,
                       scorer_ok ? "" : "(降级)", top_content, sel_dias.c_str());
        }
      }
    } else if (cfg.rerank_mode == "formula") {
      // 路由打分公式重排：七语义关系特征线性合成（零 LLM，公式可解释；
      // 权重先验=传导系数语义档，--formula-w 注入拟合值，逐候选贡献 --formula-dump）。
      const std::vector<double> w = cfg.formula_w.size() == 8
                                        ? cfg.formula_w
                                        : std::vector<double>{0.9, 0.7, 0.9, 0.5,
                                                              0.6, 0.6, 0.4, -0.5};
      const size_t n_pool = evs.size();
      auto t0 = std::chrono::steady_clock::now();
      auto scored = formula_score_pool(engine, q.question, q.question_date, evs, cs, w);
      auto& total = scored.first;
      auto& ff = scored.second;
      double feat_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
      std::vector<size_t> ord(n_pool);
      for (size_t i = 0; i < n_pool; ++i) ord[i] = i;
      std::stable_sort(ord.begin(), ord.end(),
                       [&](size_t a, size_t b) { return total[a] > total[b]; });
      size_t kk = std::min<size_t>((size_t)cfg.k, ord.size());
      std::string sel_dias = apply_selection(std::vector<size_t>(ord.begin(), ord.begin() + kk));
      std::fprintf(stderr,
                   "  [formula] %s: 置顶 %zu/%zu 条 | 特征 %.1fms | 榜首内容分 %.3f | 选中: %s\n",
                   q.id.c_str(), kk, n_pool, feat_ms, top_content, sel_dias.c_str());
      if (!cfg.formula_dump.empty()) {  // 逐候选特征贡献（可解释性产物，诊断向）
        std::vector<int> rank_of(n_pool, 0);
        for (size_t r = 0; r < n_pool; ++r) rank_of[ord[r]] = (int)r + 1;
        std::ofstream f(cfg.formula_dump, std::ios::app);
        for (size_t i = 0; i < n_pool; ++i) {
          auto dia = evs[i]->event->attrs.find("dia_id");
          Json j = Json::object();
          j["qid"] = Json::string(q.id);
          j["dia"] = Json::string(dia != evs[i]->event->attrs.end() ? dia->second : "-");
          j["rank"] = Json::number(rank_of[i]);
          j["total"] = Json::number(total[i]);
          j["bm25"] = Json::number(ff[i].bm25);
          j["involves"] = Json::number(ff[i].involves);
          j["related"] = Json::number(ff[i].related);
          j["next"] = Json::number(ff[i].next);
          j["cooccur"] = Json::number(ff[i].cooccur);
          j["time"] = Json::number(ff[i].time);
          j["fusion"] = Json::number(ff[i].fusion);
          j["opposite"] = Json::number(ff[i].opposite);
          j["y"] = Json::number(0);  // 拟合脚本用 m12 日志回填标签
          f << j.dump() << "\n";
        }
      }
    } else if (cfg.rerank_mode == "xenc") {
      // cross-encoder 精排：pair 打分直接定序（判别式，非特征合成）。
      // scorer 失败 → 降级 BM25 原序（零依赖兜底，日志明示）。
      double scorer_ms = 0.0;
      std::vector<double> xs = run_xenc_scorer(cfg.xenc_scorer, q.question, evs, &scorer_ms);
      const bool scorer_ok = xs.size() == evs.size();
      if (!scorer_ok) {
        std::fprintf(stderr,
                     "  [xenc-rerank] %s: scorer 不可用（%.0fms），降级 BM25 原序",
                     q.id.c_str(), scorer_ms);
      } else {
        std::vector<size_t> ord(evs.size());
        for (size_t i = 0; i < evs.size(); ++i) ord[i] = i;
        std::stable_sort(ord.begin(), ord.end(),
                         [&](size_t a, size_t b) { return xs[a] > xs[b]; });
        size_t kk = std::min<size_t>((size_t)cfg.k, ord.size());
        std::string sel_dias =
            apply_selection(std::vector<size_t>(ord.begin(), ord.begin() + kk));
        std::fprintf(stderr,
                     "  [xenc-rerank] %s: 置顶 %zu/%zu 条 | scorer %.0fms | 榜首内容分 %.3f | 选中: %s",
                     q.id.c_str(), kk, evs.size(), scorer_ms, top_content, sel_dias.c_str());
      }
    }
    if ((int)evs.size() > cfg.k) evs.resize(cfg.k);
  } else if ((cfg.rerank_mode == "llm" && cfg.llm_client) || cfg.rerank_mode == "cheap" ||
             cfg.rerank_mode == "svec" || cfg.rerank_mode == "formula") {
    std::fprintf(stderr, "  [llm-rerank] %s: 候选≤1，未重排\n", q.id.c_str());
  }

  ar.score = evs.empty() ? 0.0 : evs.front()->score;
  // 弃答门控：证据最高分低于阈值或无证据 → 弃答。
  // 另加语境相关性闸门：归一化后图扩散分恒偏高，真正的"毫不相关"体现在
  // 证据与提问的语境余弦上，两者任一触闸即弃答（阈值均为配置项）。
  // 例外：榜首内容词精确命中够强（≥阈值）时直接放行——哈希语境余弦是弱信号，
  // 不得否决词面精确命中（LME 偏好题误弃答 23/30 的修复）。
  double top_ctx = evs.empty() ? 0.0 : evs.front()->context_score;
  bool content_ok = top_content >= cfg.abstain_threshold;
  ar.abstained = evs.empty() ||
                 (!content_ok && (ar.score < cfg.abstain_threshold ||
                                  top_ctx < cfg.abstain_threshold));
  if (ar.abstained) {
    ar.answer_text = "unanswerable";
  } else {
    // 答案文本按事件时间先后拼接（叙事序，供 event_ordering / Kendall tau-b）
    std::vector<const ScoredEvent*> top(evs.begin(), evs.begin() + std::min<size_t>(evs.size(), 3));
    std::sort(top.begin(), top.end(),
              [](const ScoredEvent* a, const ScoredEvent* b) { return a->event->ts < b->event->ts; });
    std::string text;
    for (auto* se : top) {
      if (!text.empty()) text += " ";
      text += se->event->text;
    }
    ar.answer_text = text;
  }
  for (auto* se : evs) {
    ar.evidence_uids.push_back(se->event->uid);
    auto it = se->event->attrs.find("dia_id");
    if (it != se->event->attrs.end()) ar.evidence_dia_ids.push_back(it->second);
    auto sit = se->event->attrs.find("session_id");
    if (sit != se->event->attrs.end() && !sit->second.empty())
      ar.evidence_dia_ids.push_back("S:" + sit->second);  // 会话级证据带前缀区分
  }
  return ar;
}

}  // namespace ame
