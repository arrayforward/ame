/*
 * 所属模块：bench 基准适配层——双后端存储抽象实现（设计 §10.3）。
 * 本文件主体思路：
 *   GraphStore：包现有 1.x 调用——ingest_case=ingest()；answer_question=answer()
 *     后按 evidence_uids 逐条打包证据文本（日期前缀 + NEXT 链前后文，与 CLI graph
 *     路径同一逻辑），dia_id 从事件 attrs 取。
 *   LmemStore：2.0 Mem 网关——ingest 逐轮 remember（时间戳合成规则与 1.x 一致：
 *     会话日期解析 + 轮序×1分钟），kind 由 speaker 映射，dia_id 落观测 meta；
 *     recall 取大候选池（k_pool，与 1.x 对齐）后用同一内容词 BM25-lite 公式
 *     终排截 top-k，证据 dia_id 经 EvidenceItem.ref_obs → Mem::obs 回溯。
 * 关键算法/数据结构：终排公式复用 ingest_answer.cpp 的 content_scores_for_texts
 *   （df/idf 在候选集内统计，stable 排序，与 1.x answer() 步骤4 同机制）。
 * 依赖关系：依赖 bench.h/store.h、M12 engine、2.0 M19 mem（经 ame_bench 链接）。
 */
#include "ame/bench/store.h"

#include "ame/engine/engine.h"
#include "ame/mem/mem.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace ame {

// 日期前缀辅助：epoch 数字 LLM 无法解读，与 CLI 证据打包同格式
static std::string date_prefix(int64_t ts) {
  char buf[40] = "";
  std::time_t t = (std::time_t)ts;
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::snprintf(buf, sizeof(buf), "[%04d-%02d-%02d] ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

// ---- 1.x 适配器 ----

IngestStats GraphStore::ingest_case(const BenchCase& c) { return ingest(*engine_, c); }

StoreAnswer GraphStore::answer_question(const BenchQuestion& q, const BenchConfig& cfg) {
  StoreAnswer sa;
  sa.result = answer(*engine_, q, cfg);
  // 证据打包与 CLI graph 路径同一逻辑：日期前缀 + 沿 NEXT 链带前/后一轮上下文
  for (auto& uid : sa.result.evidence_uids) {
    const Node* n = engine_->storage().get_node(uid);
    if (!n) continue;
    StoreEvidence ev;
    ev.text = date_prefix(n->ts) + n->text;
    ev.ts = n->ts;
    std::string prev_text, next_text;
    for (auto& [ed, nb] : engine_->storage().neighbors(uid, {RelType::NEXT})) {
      if (nb->kind != NodeKind::Event) continue;
      if (nb->ts < n->ts && prev_text.empty()) prev_text = nb->text;
      if (nb->ts > n->ts && next_text.empty()) next_text = nb->text;
    }
    if (!prev_text.empty()) ev.text += "（前一轮: " + prev_text + "）";
    if (!next_text.empty()) ev.text += "（后一轮: " + next_text + "）";
    auto it = n->attrs.find("dia_id");
    if (it != n->attrs.end()) ev.dia_id = it->second;
    sa.evidence.push_back(std::move(ev));
  }
  return sa;
}

// ---- 2.0 适配器 ----

// 伪代码：逐轮 remember 入观测层
//   步骤1：会话日期解析（与 1.x ingest 同一合成规则：解析失败按会话序合成递增）；
//   步骤2：kind = speaker=="user" ? "user" : "assistant"（LoCoMo speaker 即角色）；
//   步骤3：dia_id 非空则落 meta（证据回溯通道，评测硬指标）；
//   步骤4：统计观测数；2.0 无实体/关键词节点概念，entities/keywords 记 0。
IngestStats LmemStore::ingest_case(const BenchCase& c) {
  IngestStats st;
  const TimePoint kBase = 1700000000;
  for (size_t si = 0; si < c.sessions.size(); ++si) {
    const BenchSession& bs = c.sessions[si];
    TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
    for (size_t ti = 0; ti < bs.turns.size(); ++ti) {
      const BenchTurn& t = bs.turns[ti];
      TimePoint ts = base + (TimePoint)ti * 60;
      Json meta = Json::object();
      if (!t.dia_id.empty()) meta.set("dia_id", Json::string(t.dia_id));
      auto r = mem_->remember(owner_, bs.id, t.speaker == "user" ? "user" : "assistant",
                              t.text, ts, std::move(meta));
      if (r.code == Err::Ok) {
        ++st.events;
        if (ts > last_ts_) last_ts_ = ts;
      }
    }
  }
  return st;
}

// 伪代码：取证
//   步骤1：now = question_date（可解析）否则该 case 最大观测时刻；
//   步骤2：Mem.recall 取大候选池（k_pool，与 1.x 候选池同量级）；
//   步骤3：与 1.x 同一内容词 BM25-lite 终排（stable，同分保持原序），截 top-k；
//   步骤4：逐条证据：ref_obs → Mem::obs 回溯 meta.dia_id 与会话（channel）；
//   步骤5：弃答仅在零证据时触发（2.0 打分尺度与 1.x 不同，阈值不可比，不套用）；
//          否则答案 = top3 证据按时间序拼接（与 1.x baseline 同口径）。
StoreAnswer LmemStore::answer_question(const BenchQuestion& q, const BenchConfig& cfg) {
  StoreAnswer sa;
  TimePoint qdate = parse_session_date(q.question_date, 0);
  // now 兜底：last_ts_ 为 0（如持久化恢复后新建的 store）时从观测层扫最大 ts，
  // 保证同一份数据任意构造路径下 recall 的 now 口径一致（P2b 双 pass 可比性）
  int64_t max_ts = last_ts_;
  if (max_ts <= 0) {
    mem_->obs_store().for_owner(owner_, [&](const Observation& o) {
      if (o.ts > max_ts) max_ts = o.ts;
    });
  }
  const int64_t now = qdate > 0 ? qdate : (max_ts > 0 ? max_ts : 1700000000);

  auto items = mem_->recall(q.question, owner_, cfg.k_pool, now);
  // 终排：与 1.x answer() 步骤4 同一公式（content_scores_for_texts 在候选集内统计 idf）
  std::vector<std::string> texts;
  for (auto& it : items) texts.push_back(it.text);
  std::vector<double> cs = content_scores_for_texts(q.question, texts);
  std::vector<size_t> ord(items.size());
  for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
  std::stable_sort(ord.begin(), ord.end(),
                   [&](size_t a, size_t b) { return cs[a] > cs[b]; });
  if ((int)ord.size() > cfg.k) ord.resize(cfg.k);

  AnswerResult& ar = sa.result;
  int n_fallback = 0;  // P2a 观测：兜底通道触发计数（归因用）
  for (size_t idx : ord) {
    const EvidenceItem& it = items[idx];
    if (it.via == "fallback") ++n_fallback;
    StoreEvidence ev;
    ev.text = date_prefix(it.ts) + it.text;
    ev.ts = it.ts;
    ev.score = it.score;
    ev.ref_obs = it.ref_obs;          // P2b：反馈闭环定位证据来源
    ev.source_kind = it.source_kind;
    ev.via = it.via;
    std::string session_mark;
    if (it.ref_obs != 0) {
      const Observation* o = mem_->obs(it.ref_obs);
      if (o) {
        const Json* d = o->meta.find("dia_id");
        if (d) ev.dia_id = d->as_str();
        if (!o->channel.empty()) session_mark = "S:" + o->channel;
      }
    }
    sa.evidence.push_back(ev);
    ar.evidence_uids.push_back("obs:" + std::to_string(it.ref_obs));
    if (!ev.dia_id.empty()) ar.evidence_dia_ids.push_back(ev.dia_id);
    if (!session_mark.empty()) ar.evidence_dia_ids.push_back(session_mark);
  }
  ar.score = ord.empty() ? 0.0 : items[ord.front()].score;
  ar.abstained = ord.empty();
  if (n_fallback > 0)  // P2a 归因：兜底通道触发即报（词面命中 < k_pool/2）
    std::fprintf(stderr, "  [fallback] %s: 兜底证据 %d/%zu 条\n", q.id.c_str(),
                 n_fallback, ord.size());
  if (ar.abstained) {
    ar.answer_text = "unanswerable";
  } else {
    std::vector<size_t> top(ord.begin(), ord.begin() + std::min<size_t>(ord.size(), 3));
    std::sort(top.begin(), top.end(),
              [&](size_t a, size_t b) { return items[a].ts < items[b].ts; });
    std::string text;
    for (size_t i : top) {
      if (!text.empty()) text += " ";
      text += items[i].text;
    }
    ar.answer_text = text;
  }
  return sa;
}

}  // namespace ame
