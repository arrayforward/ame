/*
 * 所属模块：core 公共基础层——公共类型辅助函数实现（types.h 的配对实现）。
 * 本文件主体思路：为 RelType/NodeKind/Err 提供名称互转与属性查询，
 *   并以查表方式给出每种关系类型的扩散传导系数（系数全系统唯一定义于此，
 *   M6 扩散引擎直接查询，不再各自硬编码）。
 * 关键算法/数据结构：无复杂算法；switch 查表，rel_from_name 用枚举区间线性反查。
 * 依赖关系：依赖本模块 types.h；被 storage M9（边类型序列化）、diffusion M6（传导系数）等依赖。
 */
#include "ame/core/types.h"

namespace ame {

// 伪代码：按关系类型查表返回扩散传导系数
//   步骤1：RELATED 六类——因果/推理 0.9、组成 0.8、相似 0.7、共现 0.6、
//          对立 0.5（单列，防止反义被当强联想）；
//   步骤2：结构类边按语义强度返回——INVOLVES/REFERS_TO/ALIAS_OF 0.9，
//          HAPPENED_AT/CONTAINS 0.8，NEXT/EXPERIENCED_TOGETHER 0.7，
//          NEAR/LIFE_PHASE/OWNED_BY 0.6，IN 0.5；
//   步骤3：switch 已穷尽全部枚举，末尾 0.5 仅为理论兜底。
double conduction_coeff(RelType t) {
  switch (t) {
    case RelType::REL_CAUSE:     return 0.9;  // 因果
    case RelType::REL_INFER:     return 0.9;  // 推理
    case RelType::REL_PART:      return 0.8;  // 组成
    case RelType::REL_SIMILAR:   return 0.7;  // 相似
    case RelType::REL_COOCCUR:   return 0.6;  // 共现
    case RelType::REL_OPPOSITE:  return 0.5;  // 对立（必须单列，防反义被当强联想）
    case RelType::INVOLVES:      return 0.9;
    case RelType::HAPPENED_AT:   return 0.8;
    case RelType::CONTAINS:      return 0.8;
    case RelType::NEXT:          return 0.7;
    case RelType::NEAR:          return 0.6;
    case RelType::IN:            return 0.5;
    case RelType::EXPERIENCED_TOGETHER: return 0.7;
    case RelType::LIFE_PHASE:    return 0.6;
    case RelType::OWNED_BY:      return 0.6;
    case RelType::REFERS_TO:     return 0.9;
    case RelType::ALIAS_OF:      return 0.9;
    case RelType::SUPERSEDES:    return 0.8;  // 知识更新链（旧→新可追溯）
  }
  return 0.5;
}

// 返回关系类型的字符串名（switch 查表，未知返回 "UNKNOWN"）。
const char* rel_name(RelType t) {
  switch (t) {
    case RelType::INVOLVES: return "INVOLVES";
    case RelType::HAPPENED_AT: return "HAPPENED_AT";
    case RelType::CONTAINS: return "CONTAINS";
    case RelType::NEXT: return "NEXT";
    case RelType::REFERS_TO: return "REFERS_TO";
    case RelType::ALIAS_OF: return "ALIAS_OF";
    case RelType::IN: return "IN";
    case RelType::NEAR: return "NEAR";
    case RelType::REL_CAUSE: return "REL_CAUSE";
    case RelType::REL_INFER: return "REL_INFER";
    case RelType::REL_PART: return "REL_PART";
    case RelType::REL_SIMILAR: return "REL_SIMILAR";
    case RelType::REL_COOCCUR: return "REL_COOCCUR";
    case RelType::REL_OPPOSITE: return "REL_OPPOSITE";
    case RelType::EXPERIENCED_TOGETHER: return "EXPERIENCED_TOGETHER";
    case RelType::OWNED_BY: return "OWNED_BY";
    case RelType::LIFE_PHASE: return "LIFE_PHASE";
    case RelType::SUPERSEDES: return "SUPERSEDES";
  }
  return "UNKNOWN";
}

// 伪代码：
//   步骤1：在枚举区间 [0, SUPERSEDES] 内逐个构造 RelType；
//   步骤2：与 rel_name(t) 结果比对，命中即返回该类型；
//   步骤3：全部不命中返回 std::nullopt。
std::optional<RelType> rel_from_name(const std::string& s) {
  for (int i = 0; i <= (int)RelType::SUPERSEDES; ++i) {
    RelType t = (RelType)i;
    if (s == rel_name(t)) return t;
  }
  return std::nullopt;
}

// 双向边判定：NEAR/共现/相似/对立/共同经历返回 true，其余有向边返回 false。
bool rel_undirected(RelType t) {
  switch (t) {
    case RelType::NEAR:
    case RelType::REL_COOCCUR:
    case RelType::REL_SIMILAR:
    case RelType::REL_OPPOSITE:
    case RelType::EXPERIENCED_TOGETHER:
      return true;
    default:
      return false;
  }
}

// 返回节点类型的字符串名（switch 查表，未知返回 "Unknown"）。
const char* kind_name(NodeKind k) {
  switch (k) {
    case NodeKind::Event: return "Event";
    case NodeKind::Entity: return "Entity";
    case NodeKind::Place: return "Place";
    case NodeKind::Region: return "Region";
    case NodeKind::Keyword: return "Keyword";
    case NodeKind::Mention: return "Mention";
    case NodeKind::Alias: return "Alias";
    case NodeKind::Phase: return "Phase";
  }
  return "Unknown";
}

// 返回错误码的字符串名（switch 查表，未知返回 "?"）。
const char* err_name(Err e) {
  switch (e) {
    case Err::Ok: return "Ok";
    case Err::NotFound: return "NotFound";
    case Err::InvalidArg: return "InvalidArg";
    case Err::AlreadyExists: return "AlreadyExists";
    case Err::IoError: return "IoError";
    case Err::ParseError: return "ParseError";
    case Err::Internal: return "Internal";
  }
  return "?";
}

}  // namespace ame
