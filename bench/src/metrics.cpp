/*
 * 所属模块：bench 基准适配层——离线指标实现。
 * 本文件主体思路：实现 metrics.h 声明的全部离线指标与分组汇总。
 *   全部为纯函数，无 LLM、无网络依赖。
 * 关键算法/数据结构：
 *   tokenize：ASCII 字母数字连续段小写化为词，非 ASCII 逐字符成词；
 *   token F1：多重集交集 / LoCoMo 口径；
 *   nugget 代理分：逐 nugget F1 映射到 {0, 0.5, 1} 取平均（官方 LLM judge 离线代理）；
 *   Kendall tau-b：nugget 参考序 vs 回答中首次出现位置序的逆序对统计。
 * 依赖关系：依赖 bench.h/metrics.h 与标准库；被 CLI 与 tests/test_bench 依赖。
 */
#include "ame/bench/metrics.h"

#include "ame/core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace ame {

// tokenize 伪代码：
//   步骤1：线性扫描；ASCII 字母数字累积成词（小写化）；
//   步骤2：非 ASCII 字节按 UTF-8 序列整段取出作为一个词（CJK 单字成词）；
//   步骤3：其余字符视为分隔符。
std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) { out.push_back(cur); cur.clear(); }
  };
  for (size_t i = 0; i < text.size();) {
    unsigned char c = (unsigned char)text[i];
    if (c < 128 && std::isalnum(c)) {
      cur += (char)std::tolower(c);
      ++i;
    } else if (c >= 128) {
      flush();
      size_t len = 1;
      if ((c >> 5) == 0x6) len = 2;
      else if ((c >> 4) == 0xE) len = 3;
      else if ((c >> 3) == 0x2) len = 4;
      out.push_back(text.substr(i, len));
      i += len;
    } else {
      flush();
      ++i;
    }
  }
  flush();
  return out;
}

// token F1 伪代码：
//   步骤1：双方切词；任一方为空则返回 0（双方皆空返回 1 无意义，按 0 处理）；
//   步骤2：多重集交集计数 common（逐 pred 词匹配未消耗 gold 词）；
//   步骤3：F1 = 2*common/(|pred|+|gold|)。
double token_f1(const std::string& pred, const std::string& gold) {
  auto pt = tokenize(pred);
  auto gt = tokenize(gold);
  if (pt.empty() || gt.empty()) return 0;
  std::vector<bool> used(gt.size(), false);
  int common = 0;
  for (auto& w : pt)
    for (size_t i = 0; i < gt.size(); ++i)
      if (!used[i] && gt[i] == w) { used[i] = true; ++common; break; }
  return 2.0 * common / ((double)pt.size() + gt.size());
}

// nugget 代理分伪代码：逐 nugget 与回答算 token F1，
//   ≥0.6 记 1、≥0.3 记 0.5、否则 0；无 nugget 返回 0。
double nugget_proxy_score(const std::string& answer, const std::vector<std::string>& nuggets) {
  if (nuggets.empty()) return 0;
  double sum = 0;
  for (auto& n : nuggets) {
    double f1 = token_f1(answer, n);
    sum += f1 >= 0.6 ? 1.0 : (f1 >= 0.3 ? 0.5 : 0.0);
  }
  return sum / nuggets.size();
}

// order_tau_b 伪代码：
//   步骤1：把回答与 nugget 都小写化；逐 nugget 在回答中找首次出现位置
//          （子串查找），未找到的 nugget 跳过；
//   步骤2：对找到的 nugget 两两配对：参考顺序与位置顺序一致 → 和谐对，
//          相反 → 不和谐对（位置相同视为并列，不计入两者）；
//   步骤3：无有效配对返回 0；否则 tau = (C-D)/(C+D)（无并列时即 tau-b）。
double order_tau_b(const std::string& answer, const std::vector<std::string>& nuggets) {
  std::string low;
  for (char c : answer) low += (char)std::tolower((unsigned char)c);
  std::vector<size_t> pos;
  for (auto& n : nuggets) {
    std::string ln;
    for (char c : n) ln += (char)std::tolower((unsigned char)c);
    size_t p = low.find(ln);
    if (p != std::string::npos) pos.push_back(p);
  }
  int c = 0, d = 0;
  for (size_t i = 0; i < pos.size(); ++i)
    for (size_t j = i + 1; j < pos.size(); ++j) {
      if (pos[i] < pos[j]) ++c;        // 参考序 i<j 与出现位置同向
      else if (pos[i] > pos[j]) ++d;   // 逆序
    }
  if (c + d == 0) return 0;
  return (double)(c - d) / (c + d);
}

// MetricAccumulator::add 伪代码：
//   步骤1：累加题数与 token F1（弃答题标准答案无意义时仍计入，答非所问自然低分）；
//   步骤2：证据题（dia_id 或 session_id 非空）：命中检索证据即 recall_hits+1；
//   步骤3：弃答判定与标准答案一致 → abstain_correct+1；
//   步骤4：有 nugget 的题累计代理分；event_ordering 题累计 tau-b；
//   步骤5：judge >= 0 时累计 LLM judge 得分。
void MetricAccumulator::add(const std::string& group, const BenchQuestion& q,
                            const AnswerResult& r, double judge) {
  GroupStats& g = groups_[group];
  ++g.n;
  if (!r.abstained) g.f1_sum += token_f1(r.answer_text, q.answer);

  std::set<std::string> evid(r.evidence_dia_ids.begin(), r.evidence_dia_ids.end());
  bool has_gold = !q.evidence_dia_ids.empty() || !q.answer_session_ids.empty();
  if (has_gold) {
    ++g.recall_total;
    bool hit = false;
    for (auto& e : q.evidence_dia_ids)
      if (evid.count(e)) { hit = true; break; }
    for (auto& s : q.answer_session_ids)
      if (evid.count("S:" + s)) { hit = true; break; }
    if (hit) ++g.recall_hits;
  }

  if (r.abstained == q.unanswerable) ++g.abstain_correct;

  if (!q.nuggets.empty()) {
    g.nugget_sum += r.abstained ? 0 : nugget_proxy_score(r.answer_text, q.nuggets);
    ++g.nugget_n;
  }
  if (q.qtype == "event_ordering") {
    g.tau_sum += r.abstained ? 0 : order_tau_b(r.answer_text, q.nuggets);
    ++g.tau_n;
  }
  if (judge >= 0) {
    g.judge_sum += judge;
    ++g.judge_n;
  }
}

// report 伪代码：逐分组输出一行（题数/平均F1/recall@k/弃答准确率/nugget分/tau-b/judge），
//   最后输出 OVERALL 汇总行。
std::string MetricAccumulator::report() const {
  std::string out;
  char buf[320];
  GroupStats total;
  for (auto& [name, g] : groups_) {
    std::snprintf(buf, sizeof(buf),
                  "%-24s n=%3d  F1=%.3f  recall@k=%s  abstain=%.3f  nugget=%s  tau=%s  judge=%s\n",
                  name.c_str(), g.n, g.n ? g.f1_sum / g.n : 0,
                  g.recall_total
                      ? (std::to_string((double)g.recall_hits / g.recall_total)).c_str()
                      : "-",
                  g.n ? (double)g.abstain_correct / g.n : 0,
                  g.nugget_n
                      ? (std::to_string(g.nugget_sum / g.nugget_n)).c_str()
                      : "-",
                  g.tau_n ? (std::to_string(g.tau_sum / g.tau_n)).c_str() : "-",
                  g.judge_n ? (std::to_string(g.judge_sum / g.judge_n)).c_str() : "-");
    out += buf;
    total.n += g.n;
    total.f1_sum += g.f1_sum;
    total.recall_hits += g.recall_hits;
    total.recall_total += g.recall_total;
    total.abstain_correct += g.abstain_correct;
    total.nugget_sum += g.nugget_sum;
    total.nugget_n += g.nugget_n;
    total.tau_sum += g.tau_sum;
    total.tau_n += g.tau_n;
    total.judge_sum += g.judge_sum;
    total.judge_n += g.judge_n;
  }
  std::snprintf(buf, sizeof(buf),
                "%-24s n=%3d  F1=%.3f  recall@k=%.3f  abstain=%.3f  nugget=%s  tau=%s  judge=%s\n",
                "OVERALL", total.n, total.n ? total.f1_sum / total.n : 0,
                total.recall_total ? (double)total.recall_hits / total.recall_total : 0,
                total.n ? (double)total.abstain_correct / total.n : 0,
                total.nugget_n ? (std::to_string(total.nugget_sum / total.nugget_n)).c_str()
                               : "-",
                total.tau_n ? (std::to_string(total.tau_sum / total.tau_n)).c_str() : "-",
                total.judge_n ? (std::to_string(total.judge_sum / total.judge_n)).c_str()
                              : "-");
  out += buf;
  return out;
}

// report_json 伪代码：分组与总计序列化为 JSON 对象（每组含 n/f1/recall/abstain/nugget/tau/judge）。
std::string MetricAccumulator::report_json() const {
  auto grp = [](const GroupStats& g) {
    Json j = Json::object();
    j["n"] = Json::number(g.n);
    j["f1"] = Json::number(g.n ? g.f1_sum / g.n : 0);
    j["recall_hits"] = Json::number(g.recall_hits);
    j["recall_total"] = Json::number(g.recall_total);
    j["abstain_acc"] = Json::number(g.n ? (double)g.abstain_correct / g.n : 0);
    if (g.nugget_n) j["nugget"] = Json::number(g.nugget_sum / g.nugget_n);
    if (g.tau_n) j["tau_b"] = Json::number(g.tau_sum / g.tau_n);
    if (g.judge_n) j["judge"] = Json::number(g.judge_sum / g.judge_n);
    return j;
  };
  Json root = Json::object();
  Json groups = Json::object();
  GroupStats total;
  for (auto& [name, g] : groups_) {
    groups[name] = grp(g);
    total.n += g.n;
    total.f1_sum += g.f1_sum;
    total.recall_hits += g.recall_hits;
    total.recall_total += g.recall_total;
    total.abstain_correct += g.abstain_correct;
    total.nugget_sum += g.nugget_sum;
    total.nugget_n += g.nugget_n;
    total.tau_sum += g.tau_sum;
    total.tau_n += g.tau_n;
    total.judge_sum += g.judge_sum;
    total.judge_n += g.judge_n;
  }
  root["groups"] = std::move(groups);
  root["overall"] = grp(total);
  return root.dump();
}

}  // namespace ame
