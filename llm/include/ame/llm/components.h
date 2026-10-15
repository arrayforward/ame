/*
 * 所属模块：llm 接入层——三个 LLM 组件。
 * 本文件主体思路：
 *   LlmExtractor：实现 M1 IExtractor 接口，按会话批量提取（一次调用喂一个 session
 *     的若干轮，JSON 输出，解析失败回退规则提取器）；
 *   LlmAnswerer：question + question_date + recall top-k 证据 → 短答案，
 *     证据不足回答 NO INFORMATION（弃答）；
 *   LlmJudge：实现 bench IJudge 接口，支持 LoCoMo/LongMemEval 正确性判定（yes/no）
 *     与 BEAM 逐 nugget 0/0.5/1 评分。
 * 关键算法/数据结构：prompt 模板 + 宽容 JSON 解析（截取首个 [ 到末个 ] 再 parse）。
 * 依赖关系：依赖 llm.h（ILlmClient）、M1 extractor（IExtractor/规则回退）、
 *   bench metrics.h（IJudge）；被 bench CLI 依赖。
 */
#pragma once
#include "ame/bench/bench.h"
#include "ame/bench/metrics.h"
#include "ame/extractor/extractor.h"
#include "ame/llm/llm.h"

#include <memory>
#include <string>
#include <vector>

namespace ame {

class LlmExtractor : public IExtractor {
 public:
  // client 可为空（空则全部回退 fallback）；fallback 为规则提取器
  LlmExtractor(ILlmClient* client, IExtractor* fallback);

  // IExtractor 单条提取（内部走一次 API 调用）
  Extraction extract(const std::string& text) override;

  // 会话批量提取：turns 为 (speaker, text)；返回与输入等长的 Extraction 列表
  // （event.text 保留原文，时间字段由摄入侧填）
  std::vector<Extraction> extract_session(
      const std::vector<std::pair<std::string, std::string>>& turns);

 private:
  ILlmClient* client_;
  IExtractor* fallback_;
};

class LlmAnswerer {
 public:
  explicit LlmAnswerer(ILlmClient* client) : client_(client) {}

  // 构造答题请求（evidence 为 top-k 事件文本，已带时间戳前缀）
  ChatRequest build_request(const std::string& question, const std::string& question_date,
                            const std::vector<std::string>& evidence_texts) const;
  // 解析回答：含 "NO INFORMATION"（大小写不敏感）视为弃答
  static bool is_abstain(const std::string& resp);

  // ---- 两阶段聚合作答（通用机制）----
  // 聚合类问题识别：how many / number of / total / all the / list / every / 多少/几个/所有/哪些
  static bool is_aggregate_question(const std::string& question);
  // 阶段一：单条证据的逐项提取请求（相关事实或 NONE；关思考加速）
  static ChatRequest build_extract_request(const std::string& question,
                                           const std::string& evidence_text);
  // 批量提取：一次喂 n 条编号证据，逐条输出 [E#]: 事实 或 [E#]: NONE
  static ChatRequest build_extract_batch_request(
      const std::string& question, const std::vector<std::string>& evidence_texts);
  // 批量提取解析：{证据编号(0 起): 事实}；NONE/空/编号缺失宽容跳过
  static std::vector<std::pair<int, std::string>> parse_extract_batch(
      const std::string& resp, int n_evidence);
  // 逐项提取结果判定：NONE/空 视为无关
  static bool is_none(const std::string& resp);
  // 阶段二：基于逐项事实列表（不含原文长片段）的聚合作答请求
  ChatRequest build_aggregate_request(const std::string& question,
                                      const std::string& question_date,
                                      const std::vector<std::string>& facts) const;

 private:
  ILlmClient* client_;
};

class LlmJudge : public IJudge { public:
  explicit LlmJudge(ILlmClient* client) : client_(client) {}

  // IJudge：BEAM nugget 口径，逐 nugget 0/0.5/1
  std::vector<double> judge(const std::string& question, const std::string& answer,
                            const std::vector<std::string>& nuggets) override;

  // LoCoMo/LongMemEval 口径：判定回答是否正确（参考答案对比），返回 1.0/0.0
  double correctness(const std::string& question, const std::string& gold,
                     const std::string& pred);

  // yes/no 解析（宽容：前缀匹配 yes/no，默认 no）
  static bool parse_yes_no(const std::string& resp);

 private:
  ILlmClient* client_;
};

// 词典蒸馏（增量+多文件模式）：vocab_paths 支持多个词频文件（逗号分隔由调用方拆开，
// 合并词频），跳过 out_path 已有词条（断点续跑），批量 LLM 蒸馏 synonyms/antonyms/related，
// 每批合并落盘 out_path。返回本次新增词条数（失败返回 -1）。
int build_lexicon_json(ILlmClient& client, const std::vector<std::string>& vocab_paths,
                       const std::string& out_path, int top_n, int batch, int concurrency);

// 复活工程第四轮 M2：蒸馏词库 strong 流可互换性二次精筛。逐对问 LLM
// "大多数句子里 A 能否直接替换 B 且句意不变"，只保留答 yes 的对；
// in_path 为蒸馏产物（source=distilled），out_path 写新文件（不动原始产物），
// 根字段带 validated=true；断点续跑（按 word 跳过已完成），批量磁盘缓存。
// 返回处理的 pair 总数（失败返回 -1）。
int validate_lexicon_json(ILlmClient& client, const std::string& in_path,
                          const std::string& out_path, int batch, int concurrency);

// 复活工程第五轮 M3：词级近似连续打分（1-10 → /10），候选=输入词库 strong∪near
// 并集；产出 {"source":"distilled-sim","sims":{w:{v:score}}}；断点续跑。
int score_word_sims(ILlmClient& client, const std::string& in_path,
                    const std::string& out_path, int batch, int concurrency);

}  // namespace ame
