/*
 * 所属模块：core 公共基础层——零依赖极简 JSON 值类型。
 * 本文件主体思路：定义 Json 变体类型（Null/Bool/Num/Str/Arr/Obj）及其构造、
 *   访问、序列化（dump）、解析（parse）接口，供快照/日志/配置等场景
 *   在不引入第三方库的前提下读写 JSON。
 * 关键算法/数据结构：Obj 用 vector<pair<string,Json>> 保序存储（线性查找）；
 *   序列化与递归下降解析的实现见 json.cpp。
 * 依赖关系：仅依赖标准库；被需要 JSON 读写的模块（storage M9 快照、engine M12 接口等）使用。
 */
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ame {

struct Json {
  enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;

  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;  // 保序

  // 工厂构造：创建对应 kind 的 Json 值
  static Json object() { Json j; j.kind = Obj; return j; }
  static Json array() { Json j; j.kind = Arr; return j; }
  static Json string(std::string s) { Json j; j.kind = Str; j.str = std::move(s); return j; }
  static Json number(double n) { Json j; j.kind = Num; j.num = n; return j; }
  static Json boolean(bool v) { Json j; j.kind = Bool; j.b = v; return j; }

  // Obj 访问（不存在则创建 Null）
  Json& operator[](const std::string& key);
  const Json* find(const std::string& key) const;
  // 追加数组元素（将自身置为 Arr 后 push_back）
  void push(Json v) { kind = Arr; arr.push_back(std::move(v)); }
  void set(const std::string& key, Json v);

  // 类型判断与带默认值的取值（类型不符时返回 def）
  bool is_null() const { return kind == Null; }
  double as_num(double def = 0) const { return kind == Num ? num : def; }
  std::string as_str(const std::string& def = "") const { return kind == Str ? str : def; }
  bool as_bool(bool def = false) const { return kind == Bool ? b : def; }

  std::string dump() const;
  static Json parse(const std::string& s, bool* ok = nullptr);
};

}  // namespace ame
