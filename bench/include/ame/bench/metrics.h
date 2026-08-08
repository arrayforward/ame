/*
 * 所属模块：bench 基准适配层——离线评测指标。
 * 本文件主体思路：声明全部离线可算的指标函数与分组汇总器：
 *   token F1（LoCoMo 口径）、evidence recall@k（LongMemEval 口径）、
 *   abstention 准确率、nugget 词重叠代理分（BEAM 官方 LLM judge 的离线代理）、
 *   Kendall tau-b（event_ordering 用）。
 * 关键算法/数据结构：tokenize 为 ASCII 词 + CJK 单字混合切词；
 *   tau-b 按 nugget 在回答中的首次出现位置与参考顺序做逆序对统计。
 *   IJudge 接口预留：接入真实 LLM judge 时替换 nugget 词重叠代理。
 * 依赖关系：依赖 core(types.h) 与 bench.h（AnswerResult/BenchQuestion）；
 *   被 bench CLI 与 tests/test_bench 依赖。
 */
#pragma once
#include "ame/bench/bench.h"

#include <map>
#include <string>
#include <vector>

namespace ame {

// 切词：小写化；ASCII 字母数字连续段为一个词；非 ASCII（CJK 等）每个字符一个词
std::vector<std::string> tokenize(const std::string& text);

// token bag-of-words F1（LoCoMo 口径）：多重集交并计算 precision/recall
double token_f1(const std::string& pred, const std::string& gold);

// nugget 词重叠代理分：每个 nugget 与回答算 token F1，
// ≥0.6 记 1、≥0.3 记 0.5、否则 0，取平均。
// 注意：这是 BEAM 官方 LLM judge（逐 nugget 打 0/0.5/1）的离线代理，
// 接真实 judge 时实现 IJudge 接口替换。
double nugget_proxy_score(const std::string& answer, const std::vector<std::string>& nuggets);

// event_ordering 用 Kendall tau-b：nugget 参考顺序 vs 其在回答中出现的先后顺序。
// 未在回答中出现的 nugget 不参与配对；无有效配对返回 0。
double order_tau_b(const std::string& answer, const std::vector<std::string>& nuggets);

// 真实 LLM judge 预留接口（离线环境不实现，仅定义扩展点）
class IJudge {
 public:
  virtual ~IJudge() = default;
  // 返回逐 nugget 的 0/0.5/1 评分
  virtual std::vector<double> judge(const std::string& question, const std::string& answer,
                                    const std::vector<std::string>& nuggets) = 0;
};

// 分组指标汇总器：按 category/qtype 分桶累计，输出文本报表
struct GroupStats {
  int n = 0;
  double f1_sum = 0;          // token F1（LoCoMo 口径，通用）
  int recall_hits = 0;        // evidence 命中数（dia_id 或 session_id 任一命中）
  int recall_total = 0;       // 有证据标注的题数
  int abstain_correct = 0;    // 该弃答的弃了 / 该答的没弃
  double nugget_sum = 0;      // nugget 代理分（有 nugget 的题）
  int nugget_n = 0;
  double tau_sum = 0;         // Kendall tau-b（event_ordering 题）
  int tau_n = 0;
  double judge_sum = 0;       // LLM judge 得分（correctness 或 nugget 均分）
  int judge_n = 0;
};

class MetricAccumulator {
 public:
  // 记录一题：group 为分桶键（如 "category_1" / "multi-session"）；
  // judge >= 0 时计入 LLM judge 均分（正确性 1/0 或 nugget 均分）
  void add(const std::string& group, const BenchQuestion& q, const AnswerResult& r,
           double judge = -1);
  // 输出分组报表 + 总计行
  std::string report() const;
  // 输出 JSON 格式报告（供 --out 落盘与断点续跑存档）
  std::string report_json() const;

 private:
  std::map<std::string, GroupStats> groups_;
};

}  // namespace ame
