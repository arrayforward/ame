/*
 * 所属模块：bench 基准适配层——双后端存储抽象（设计 §10.3：bench 双后端开关）。
 * 本文件主体思路：定义窄接口 IStore（摄入一个 case + 逐题取证召回），
 *   1.x 适配器 GraphStore 包现有 Engine 调用（ingest()/answer()），
 *   2.0 适配器 LmemStore 走 M19 Mem 网关（owner=conversation_id，
 *   channel=session_id，dia_id 落观测 meta，证据经 EvidenceItem.ref_obs 回溯）。
 *   两个后端跑同一份数据/指标/LLM 缓存，evidence_dia_ids 覆盖率可一对一对比。
 * 关键算法/数据结构：无复杂算法；StoreAnswer 复用既有 AnswerResult（metrics 口径不变），
 *   StoreEvidence 携带逐条证据的 dia_id（graph 取自事件 attrs，lmem 取自观测 meta）。
 * 依赖关系：依赖 bench.h；实现依赖 M12 engine 与 2.0 M19 mem（store.cpp）。
 */
#pragma once
#include "ame/bench/bench.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ame {

class Engine;
class Mem;

// 一次取证召回的逐条证据（两后端统一口径）
struct StoreEvidence {
  std::string text;      // 证据文本（日期前缀 + 原文/打包，可直接喂 LLM）
  std::string dia_id;    // 证据 dia_id（graph 取自事件 attrs，lmem 取自观测 meta；无则空）
  int64_t ts = 0;        // 证据时刻
  double score = 0;      // 后端打分（口径不同，仅供观察，不进指标）
};

// 一题取证结果：metrics 口径 + 逐条证据
struct StoreAnswer {
  AnswerResult result;                 // answer_text/abstained/score/evidence_dia_ids 沿用既有口径
  std::vector<StoreEvidence> evidence;
};

// 窄接口：--store graph|lmem 两后端的公共面（摄入 + 取证）
class IStore {
 public:
  virtual ~IStore() = default;
  virtual IngestStats ingest_case(const BenchCase& c) = 0;
  virtual StoreAnswer answer_question(const BenchQuestion& q, const BenchConfig& cfg) = 0;
};

// 1.x 适配器：包现有 Engine 调用（recall 结果的 dia_id 从事件 attrs 取——
// 1.x 摄入时 dia_id 落在事件 attrs）
class GraphStore final : public IStore {
 public:
  explicit GraphStore(Engine* engine) : engine_(engine) {}
  IngestStats ingest_case(const BenchCase& c) override;
  StoreAnswer answer_question(const BenchQuestion& q, const BenchConfig& cfg) override;
  Engine& engine() { return *engine_; }

 private:
  Engine* engine_;
};

// 2.0 适配器：Mem 网关。ingest=remember(owner=conversation_id, channel=session_id,
// kind=user|assistant, meta={"dia_id":...})；recall=Mem.recall 后按 EvidenceItem.ref_obs
// 经 Mem::obs 取 meta.dia_id 回溯证据来源
class LmemStore final : public IStore {
 public:
  explicit LmemStore(Mem* mem, std::string owner) : mem_(mem), owner_(std::move(owner)) {}
  IngestStats ingest_case(const BenchCase& c) override;
  StoreAnswer answer_question(const BenchQuestion& q, const BenchConfig& cfg) override;

 private:
  Mem* mem_;
  std::string owner_;
  int64_t last_ts_ = 0;  // 该 case 最大观测时刻（question_date 缺失时作 recall 的 now）
};

// 问题内容词 BM25-lite 终排（两后端共用同一公式；定义在 ingest_answer.cpp，
// 1.x answer() 内部已含同机制，这里供 lmem 终排对齐口径，stable 同分保持原序）
std::vector<double> content_scores_for_texts(const std::string& question,
                                             const std::vector<std::string>& texts);

}  // namespace ame
