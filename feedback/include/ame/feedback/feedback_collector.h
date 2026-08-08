#pragma once
/*
 * 模块：M11 反馈采集器（feedback）
 * 定位：采集用户对检索结果的隐式反馈信号，为 M14 学习引擎提供训练数据。
 *
 * 本文件主体思路：
 *   声明反馈采集的对外接口。检索发生时通过 log_query 埋点记录一次查询及其
 *   返回结果；用户对结果产生行为后通过 log_feedback 记录信号；export_triples
 *   将日志汇总为 (query, 正样本, 负样本) 三元组，供学习引擎训练打分模型。
 *   所有记录以 JSONL（每行一个 JSON 对象）形式追加落盘。
 *
 * 关键算法/数据结构：
 *   无复杂算法。核心数据结构为 FeedbackTriple 三元组与 SignalType 信号分级
 *   枚举（选中=正、忽略=弱负、追问=强负+新约束、纠正=结构信号、会话共现=路径加强）。
 *
 * 依赖关系：
 *   依赖 core 公共层 ame/core/types.h（Err 错误码）。
 *   本模块被 M12 API 网关（engine）调用以记录埋点，产出的日志与三元组被
 *   M14 学习引擎（learning）消费。
 */
#include "ame/core/types.h"

#include <string>
#include <vector>

namespace ame {

// 隐式信号分级：选中=正、忽略=弱负、追问=强负+新约束、纠正=结构信号、会话共现=路径加强
enum class SignalType { Select, Ignore, FollowUp, Correct, CoOccur };
const char* signal_name(SignalType s);
std::string signal_from_name(const std::string& s);

struct FeedbackTriple {
  std::string query;
  std::string pos_uid;
  std::string neg_uid;
};

class FeedbackCollector {
 public:
  explicit FeedbackCollector(const std::string& log_path);

  long log_query(const std::string& query, const std::vector<std::string>& result_uids);
  Err log_feedback(long query_id, SignalType signal, const std::string& target_uid = "",
                   const std::string& note = "");

  // 导出 query_id >= since 的训练三元组
  std::vector<FeedbackTriple> export_triples(long since = 0) const;

 private:
  std::string path_;
  long next_id_ = 1;
};

}  // namespace ame
