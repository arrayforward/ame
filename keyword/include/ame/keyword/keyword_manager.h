/*
 * 模块：M3 关键词管理器（keyword）
 * 定位：负责关键词的归一化、停用词过滤、Keyword 节点生命周期（提及计数、低频休眠/唤醒）、
 *       RELATED 关联边维护，以及受控根主题表，是记忆图谱中关键词节点的唯一入口。
 *
 * 本文件主体思路：
 *   声明 KeywordManager 的对外接口。所有关键词先经别名表归一为标准词，
 *   再经停用词黑名单过滤，最后落到存储层的 Keyword 节点上；通过 mention_count
 *   与 status 字段实现"低频休眠、活跃唤醒"的生命周期管理。
 *
 * 关键算法/数据结构：
 *   - 别名表：std::unordered_map<别名, 标准词>，O(1) 查表归一；
 *   - 停用词表：std::unordered_set，O(1) 命中判定；
 *   - 休眠/唤醒：基于 mention_count 阈值（>=3）与是否存在边的简单状态机，无复杂算法。
 *
 * 依赖关系：
 *   - 依赖 core 公共层：ame/core/types.h（Node/Edge/RelType/Err、conduction_coeff、
 *     rel_undirected）；实现文件中另依赖 core::gen_uid 与 M9 存储引擎 Storage；
 *   - 上层被 M1 要素提取器、M5 种子生成器、M6 扩散引擎等模块依赖，
 *     作为关键词节点的读写入口。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/core/json.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ame {

class Storage;

class KeywordManager {
 public:
  explicit KeywordManager(Storage& storage);

  // 别名表归一：睡不着/失眠症 → 失眠
  std::string normalize(const std::string& word) const;
  void add_alias(const std::string& alias, const std::string& standard);
  // 别名表持久化（并入 M12 组合快照，解决 load-graph 必须另带词典的问题）
  Json aliases_to_json() const;
  void aliases_from_json(const Json& j);
  // 反向查询：返回归一到 standard 的全部别名（查询侧同义扩展用，最多 cap 个）
  std::vector<std::string> variants(const std::string& standard, int cap = 3) const;
  // 近义词表（词典灌库时记录，条件归一降级用）：word -> [(near, score)]
  void add_near(const std::string& word, const std::string& near, double score);
  std::vector<std::pair<std::string, double>> near_words(const std::string& word,
                                                         int cap = 3) const;

  bool is_stopword(const std::string& word) const;

  // 获取或创建 Keyword 节点（归一 + 停用词过滤 + 提及计数 + 低频休眠判定）。
  // 命中停用词返回空串。created 输出是否新建。
  std::string get_or_create(const std::string& word, const std::string& category,
                            bool* created = nullptr);

  // 建立/更新 RELATED 边（六类，系数在 core::conduction_coeff）
  Err link_related(const std::string& a, const std::string& b, RelType type, double weight);

  std::vector<const Node*> list_keywords(const std::string& category = "", int status = 0) const;

  void sleep_keyword(const std::string& word);
  void wake_keyword(const std::string& word);
  bool is_dormant(const std::string& uid) const;

  // 受控根主题表（20~50 个内置常量）
  static const std::vector<std::string>& root_topics();

  const Node* find(const std::string& standard_word) const;

 private:
  Storage& storage_;
  std::unordered_map<std::string, std::string> alias_;  // alias -> standard
  std::unordered_set<std::string> builtin_standards_;   // 内置标准词（不可被词典改写）
  // near -> [(近义词, score)]（词典灌库记录；与图边无关，即使节点未建也保留）
  std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> near_;
  std::unordered_set<std::string> stopwords_;
};

}  // namespace ame
