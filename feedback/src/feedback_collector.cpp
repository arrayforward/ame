/*
 * 模块：M11 反馈采集器（feedback）
 * 定位：采集用户对检索结果的隐式反馈信号，为 M14 学习引擎提供训练数据。
 *
 * 本文件主体思路：
 *   实现 FeedbackCollector 的埋点记录与三元组导出。log_query / log_feedback
 *   把查询事件和反馈信号各自序列化为一个 JSON 对象，以 JSONL 形式追加写入
 *   日志文件；export_triples 重新扫描整个日志，按 query_id 把反馈信号归并到
 *   对应查询上，按信号类型划分正/负样本，再两两配对生成训练三元组。
 *
 * 关键算法/数据结构：
 *   无复杂算法。导出阶段用 unordered_map<long, Q> 以 query_id 为键聚合每次
 *   查询的结果列表与正/负样本集合，最后做正样本 × 负样本的笛卡尔配对；
 *   对未显式标注的返回结果，若该查询存在正样本则将其余结果视为弱负样本。
 *
 * 依赖关系：
 *   依赖 core 公共层 ame/core/json.h（Json 解析/序列化）与
 *   ame/core/types.h（Err 错误码，经头文件引入）。
 *   本模块被 M12 API 网关（engine）调用，产出供 M14 学习引擎（learning）消费。
 */
#include "ame/feedback/feedback_collector.h"

#include "ame/core/json.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace ame {

// 信号枚举 -> 日志中的字符串名；未知值返回 "?"
const char* signal_name(SignalType s) {
  switch (s) {
    case SignalType::Select: return "select";
    case SignalType::Ignore: return "ignore";
    case SignalType::FollowUp: return "followup";
    case SignalType::Correct: return "correct";
    case SignalType::CoOccur: return "cooccur";
  }
  return "?";
}

// 字符串名 -> 信号：当前实现原样透传，由调用方自行比对
std::string signal_from_name(const std::string& s) { return s; }

// 构造函数：恢复 query_id 计数，保证重启后 id 单调递增不复用
// 伪代码：
//   步骤1：以缺省值 next_id_=1 打开既有日志文件（不存在则跳过，视为全新日志）。
//   步骤2：逐行解析 JSON，解析失败的行直接跳过。
//   步骤3：若该行 type=="query" 且含 id 字段，则用 id+1 刷新 next_id_ 的最大值。
//   步骤4：扫描结束后 next_id_ 即为历史最大 query_id + 1。
FeedbackCollector::FeedbackCollector(const std::string& log_path) : path_(log_path) {
  // 恢复已有日志的最大 query_id
  std::ifstream f(path_);
  std::string line;
  while (std::getline(f, line)) {
    bool ok = false;
    Json j = Json::parse(line, &ok);
    if (!ok) continue;
    auto* t = j.find("type");
    auto* id = j.find("id");
    if (t && id && t->as_str() == "query")
      next_id_ = std::max(next_id_, (long)id->as_num() + 1);
  }
}

// 记录一次查询埋点，返回分配的 query_id
// 伪代码：
//   步骤1：取 next_id_ 作为本次 query_id 并自增。
//   步骤2：组装 JSON 对象 {type:"query", id, query, results:[uid...]}。
//   步骤3：以追加模式打开日志文件，写入一行序列化后的 JSON；
//          打开失败则静默丢弃（埋点不阻断主流程），仍返回已分配的 id。
long FeedbackCollector::log_query(const std::string& query,
                                  const std::vector<std::string>& result_uids) {
  Json j = Json::object();
  j["type"] = Json::string("query");
  long id = next_id_++;
  j["id"] = Json::number((double)id);
  j["query"] = Json::string(query);
  Json rs = Json::array();
  for (auto& u : result_uids) rs.push(Json::string(u));
  j["results"] = std::move(rs);
  std::ofstream f(path_, std::ios::app);
  if (f) f << j.dump() << "\n";
  return id;
}

// 记录一条针对某次查询结果的反馈信号
// 伪代码：
//   步骤1：组装 JSON 对象 {type:"feedback", query_id, signal, target, note}，
//          其中 signal 经 signal_name 转为字符串名。
//   步骤2：以追加模式打开日志文件；若打开失败返回 Err::IoError。
//   步骤3：写入一行序列化后的 JSON，返回 Err::Ok。
Err FeedbackCollector::log_feedback(long query_id, SignalType signal,
                                    const std::string& target_uid, const std::string& note) {
  Json j = Json::object();
  j["type"] = Json::string("feedback");
  j["query_id"] = Json::number((double)query_id);
  j["signal"] = Json::string(signal_name(signal));
  j["target"] = Json::string(target_uid);
  j["note"] = Json::string(note);
  std::ofstream f(path_, std::ios::app);
  if (!f) return Err::IoError;
  f << j.dump() << "\n";
  return Err::Ok;
}

// 导出 query_id >= since 的训练三元组
// 伪代码：
//   步骤1：打开日志文件，失败则返回空列表。
//   步骤2：逐行解析 JSON，按 type 分流：
//          - "query"：id < since 的跳过；否则记录查询文本与返回结果列表，
//            以 id 为键存入聚合表 qs。
//          - "feedback"：按 query_id 找到对应查询（找不到或 target 为空则跳过）；
//            信号为 select/cooccur 时 target 计入正样本，
//            为 ignore/followup 时计入负样本，correct 不参与配对。
//   步骤3：遍历聚合表生成三元组：对每个正样本 p，
//          先与所有显式负样本配对；再把该查询返回结果中除 p 自身外的
//          未标注结果当作弱负样本与 p 配对。
//   步骤4：返回全部 (query, 正, 负) 三元组。
std::vector<FeedbackTriple> FeedbackCollector::export_triples(long since) const {
  std::ifstream f(path_);
  if (!f) return {};
  struct Q {
    std::string query;
    std::vector<std::string> results;
    std::vector<std::string> pos, neg;
  };
  std::unordered_map<long, Q> qs;
  std::string line;
  while (std::getline(f, line)) {
    bool ok = false;
    Json j = Json::parse(line, &ok);
    if (!ok) continue;
    auto* t = j.find("type");
    if (!t) continue;
    if (t->as_str() == "query") {
      long id = (long)(j.find("id") ? j.find("id")->as_num() : 0);
      if (id < since) continue;
      Q q;
      q.query = j.find("query") ? j.find("query")->as_str() : "";
      if (auto* rs = j.find("results"))
        for (auto& u : rs->arr) q.results.push_back(u.as_str());
      qs[id] = std::move(q);
    } else if (t->as_str() == "feedback") {
      long qid = (long)(j.find("query_id") ? j.find("query_id")->as_num() : 0);
      auto it = qs.find(qid);
      if (it == qs.end()) continue;
      std::string sig = j.find("signal") ? j.find("signal")->as_str() : "";
      std::string target = j.find("target") ? j.find("target")->as_str() : "";
      if (target.empty()) continue;
      if (sig == "select" || sig == "cooccur") it->second.pos.push_back(target);
      else if (sig == "ignore" || sig == "followup") it->second.neg.push_back(target);
    }
  }
  std::vector<FeedbackTriple> triples;
  for (auto& [id, q] : qs) {
    // 未显式标注的返回结果：若该 query 有正样本，则其余视为弱负
    for (auto& p : q.pos) {
      for (auto& n : q.neg) triples.push_back({q.query, p, n});
      for (auto& r : q.results)
        if (r != p) triples.push_back({q.query, p, r});
    }
  }
  return triples;
}

}  // namespace ame
