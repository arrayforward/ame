/*
 * 所属模块：bench 基准适配层——双后端存储抽象实现（设计 §10.3）。
 * 本文件主体思路：
 *   GraphStore：包现有 1.x 调用——ingest_case=ingest()；answer_question=answer()
 *     后按 evidence_uids 逐条打包证据文本（日期前缀 + NEXT 链前后文，与 CLI graph
 *     路径同一逻辑），dia_id 从事件 attrs 取。
 */
#include "ame/bench/store.h"

#include "ame/engine/engine.h"

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

}  // namespace ame
