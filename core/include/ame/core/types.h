/*
 * 所属模块：core 公共基础层（无 M 编号）——全系统统一的公共类型定义。
 * 本文件主体思路：集中定义所有模块共享的图数据模型（Node/Edge/NodeKind/RelType）、
 *   时间类型（TimePoint/Precision）、错误码（Err）与 Result<T> 返回包装，
 *   避免各模块各自定义造成的类型不一致。
 * 关键算法/数据结构：无复杂算法；核心是枚举 + POD 结构体，
 *   扩展属性统一用 Attrs（unordered_map<string,string>）承载。
 * 依赖关系：仅依赖标准库；被 M1~M14 全部模块依赖
 *   （storage M9 存取、diffusion M6 读取传导系数、graph_writer M4 建边等）。
 */
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ame {

using TimePoint = int64_t;               // UTC epoch 秒（唯一事实源）
using Vec = std::vector<float>;          // embedding 向量
using Attrs = std::unordered_map<std::string, std::string>;

enum class Precision { Year, Month, Day, Second };

enum class NodeKind { Event, Entity, Place, Region, Keyword, Mention, Alias, Phase };

// 关系类型（设计文档 3.2）
enum class RelType {
  INVOLVES,          // Event -> Entity {role}
  HAPPENED_AT,       // Event -> Place
  CONTAINS,          // Event -> Keyword
  NEXT,              // Event -> Event 叙事时间链
  REFERS_TO,         // Mention -> Entity {confidence}
  ALIAS_OF,          // Alias -> Entity|Keyword
  IN,                // Place -> Region
  NEAR,              // Place <-> Place (<=500m)
  REL_CAUSE,         // 因果 0.9
  REL_INFER,         // 推理 0.9
  REL_PART,          // 组成 0.8
  REL_SIMILAR,       // 相似 0.7
  REL_COOCCUR,       // 共现 0.6
  REL_OPPOSITE,      // 对立 0.5
  EXPERIENCED_TOGETHER, // Entity <-> Entity {count}
  OWNED_BY,          // Entity -> Entity {valid_from, valid_to}
  LIFE_PHASE,        // Entity -> Phase {start, end}
  SUPERSEDES         // Event -> Event 知识更新：新事实取代旧事实（旧事件标 valid_to）
};

// RELATED 六类 + 其余类型的默认传导系数（系数存 core，全系统唯一来源）
double conduction_coeff(RelType t);
const char* rel_name(RelType t);
std::optional<RelType> rel_from_name(const std::string& s);
bool rel_undirected(RelType t);  // NEAR / 共现 / 相似 / 共同经历 等双向边
const char* kind_name(NodeKind k);

// 图节点：八类节点共用同一结构，按 kind 取用相应字段
struct Node {
  std::string uid;
  NodeKind kind = NodeKind::Event;
  std::string name;      // Entity/Place/Keyword/Phase 的名称；Event 存摘要
  std::string text;      // Event 完整文本
  std::string type;      // 子类型：event type / entity type(person|object|org) / keyword category
  TimePoint ts = 0;      // 仅 Event 携带绝对时间(UTC)
  Precision precision = Precision::Day;
  double importance = 0.5;
  double lat = 0, lng = 0;   // Place
  std::string geohash;       // Place
  Vec embedding;             // 语义向量（可为空）
  int status = 0;            // Keyword: 0=active 1=休眠
  int mention_count = 0;     // Keyword 出现计数（低频休眠用）
  Attrs attrs;               // identifiers / tz_offset / open_hours / role / phase ...
};

// 图边：from/to 为节点 uid，attrs 承载 role/count/confidence/start/end 等边属性
struct Edge {
  std::string from, to;
  RelType type = RelType::REL_COOCCUR;
  double weight = 1.0;
  Attrs attrs;               // role / count / confidence / start / end ...
};

enum class Err { Ok = 0, NotFound, InvalidArg, AlreadyExists, IoError, ParseError, Internal };

const char* err_name(Err e);

// 统一返回包装：code 错误码 + value 值 + msg 错误消息；ok() 判成功，
// success/fail 为构造便捷函数（trivial 一行实现，不另加伪代码）。
template <typename T>
struct Result {
  Err code = Err::Ok;
  T value{};
  std::string msg;
  bool ok() const { return code == Err::Ok; }
  static Result success(T v) { Result r; r.value = std::move(v); return r; }
  static Result fail(Err c, std::string m) { Result r; r.code = c; r.msg = std::move(m); return r; }
};

}  // namespace ame
