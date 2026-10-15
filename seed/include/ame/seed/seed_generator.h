/*
 * =============================================================================
 * 所属模块：M5 种子生成器（seed）
 * 定位：联想链的软入口，把任意形式的用户查询转换为图上的一组带权种子节点。
 *
 * 本文件主体思路：
 *   声明 SeedGenerator 的对外接口。查询被结构化为 SeedQuery（uid、强标识符、
 *   关键词、自由文本、地理坐标五类线索），产出 SeedSet（节点 uid + 权重 +
 *   来源标记）。种子分为两类：硬种子（精确命中，weight=1.0）与软种子
 *   （向量相似命中，按相似度赋权），供 M6 扩散引擎作为遍历起点。
 *
 * 关键算法/数据结构说明：
 *   本头文件只含数据结构声明，无复杂算法。SeedQuery 携带软种子通道的两个
 *   调参旋钮：soft_topk（ANN 召回数，5~10）与 soft_min_sim（相似度阈值）。
 *
 * 依赖关系：
 *   依赖 core 公共层（ame/core/types.h 中的 Attrs 等基础类型）；
 *   通过前向声明引用 storage(M9)、vector(M13)、keyword(M3)、extractor(M1)，
 *   具体调用逻辑在 src/seed_generator.cpp。
 *   被依赖方：engine(M12) 装配本模块，diffusion(M6) 消费其产出的 SeedSet。
 * =============================================================================
 */
#pragma once
#include "ame/core/types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ame {

class Storage;
class VectorService;
class KeywordManager;
class IExtractor;

// ---- 复活工程第六轮终局架构：统一 graded 边表（WordEdgeTable）----
// 一张 {word: {word2: score∈(0,1]}} 边表，三类边共表（撞词取 max）：
//   1) 规则铸边（词形）：'s/复数 0.95、过去/进行 0.9，由 generator 现场铸造
//      （index 侧每词首现铸家族、query 侧未登录词现场铸；不规则变化不处理）；
//   2) 蒸馏边：lexicon_en_sim.json 的语义近似分（0.3-1.0）；
//   3) 反义弱边：lexicon_en.json antonyms 流 ×0.35 折入（弱联想档）。
// 消费方唯一逻辑：命中分 = max(直接词面 1.0, 最佳边分)；收录计数沿用
// "命中分≥0.5 算一个内容词命中"（第五轮语义）；IDF 照常。
// 退役对应物：等价别名改写（add_alias_distilled）与查询侧词干改写（--stem-en）
// 不再存在——词形等价降为 0.9/0.95 分边，软匹配统一走本表。
class WordEdgeTable {
 public:
  // 加载蒸馏 sim 边（source=distilled-sim）；失败返回 false
  bool load(const std::string& path);
  // 追加反义弱边：读蒸馏词库 antonyms 流，score=0.35；幂等（重复 load 安全）
  bool load_antonyms(const std::string& lexicon_path);
  float sim(const std::string& a, const std::string& b) const;  // 无边返回 0
  // 词 w 的全部出边（存表边 ∪ 现场铸造的词形家族边），按 score 降序
  std::vector<std::pair<std::string, float>> edges_of(const std::string& w) const;
  size_t word_count() const { return rows_.size(); }
  size_t edge_count() const { return edges_; }  // 仅存表边（铸边按需生成不计）
  // 加边（撞词取 max）；load/load_antonyms 内部用
  void add_edge(const std::string& a, const std::string& b, float sc);

 private:
  std::unordered_map<std::string, std::vector<std::pair<std::string, float>>> rows_;
  size_t edges_ = 0;
};

struct Seed {
  std::string node_uid;
  double weight = 1.0;
  std::string source;  // hard|soft|geo|text
};
using SeedSet = std::vector<Seed>;

struct SeedQuery {
  std::string uid;                 // 硬种子：uid 直连
  Attrs identifiers;               // 硬种子：强标识符
  std::vector<std::string> keywords;  // 硬种子：精确关键词
  std::string text;                // 软种子/自由文本
  double lat = 0, lng = 0;
  bool has_geo = false;            // 坐标种子
  int soft_topk = 8;               // ANN topK 5~10
  int lex_topk = 30;               // 词法种子上限（候选池配额，融合排序前不砍死）
  double soft_min_sim = 0.5;       // 相似度阈值
  // 条件归一：仅当查询词在图中无直接命中（关键词节点与事件词面均无）时才词典扩展——
  // strong 别名 ×0.6，无 strong 降级 syn_near ×0.4（≤3 个）；有命中保持原词；人名不扩展
  bool conditional_normalize = false;
  // 统一 graded 边表（第六轮终局架构）：非空时词法通道唯一逻辑
  // 命中分=max(1.0, 最佳边分)，m 按 ≥0.5 计，旁路 expand_of 两档（默认空=历史行为）。
  const WordEdgeTable* word_edges = nullptr;
};

class SeedGenerator {
 public:
  SeedGenerator(Storage& storage, VectorService& vec, KeywordManager& kw, IExtractor* extractor);

  SeedSet gen_seeds(const SeedQuery& query);

 private:
  Storage& storage_;
  VectorService& vec_;
  KeywordManager& kw_;
  IExtractor* extractor_;  // 自由文本先过 M1，可为空
};
}  // namespace ame
