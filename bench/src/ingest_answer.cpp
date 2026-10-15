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

#include "ame/engine/engine.h"
#include "ame/extractor/extractor.h"
#include "ame/time/time_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

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
//   （store.h 导出：lmem 后端终排与 1.x answer() 步骤4 共用同一公式；
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
  // 步骤0：实体档案通道（检索角色修正）
  if (cfg.use_entity_profile) {
    AnswerResult ent = answer_entity_channel(engine, q, cfg);
    // 通道弃权（证据分低于阈值）时不采用其弱结果，继续走标准管道——弱通道结果
    // 不应遮蔽标准召回（LME 偏好题实测：通道错选助手实体返回弱证据弃答，
    // 而标准管道本可召回金标，recall 0.233→1.0 单题验证）。
    if (!ent.abstained && (!ent.evidence_uids.empty() || !ent.answer_text.empty())) {
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
      return ent;
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
  ar.query_id = rr.query_id;  // M11 埋点回流（未启用反馈采集时为 0）
  std::vector<const ScoredEvent*> evs;
  std::unordered_set<std::string> present;  // 候选集（新旧都在选新用）
  for (auto& se : rr.events) present.insert(se.event->uid);
  for (auto& se : rr.events) {
    if (cfg.filter_superseded && qdate > 0 &&
        replaced_by_candidate(engine, se.event, qdate, present))
      continue;  // 有新弃旧；新值未召回则旧证据保留
    evs.push_back(&se);
  }
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
    if ((int)evs.size() > cfg.k) evs.resize(cfg.k);
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
