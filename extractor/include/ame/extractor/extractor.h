/*
 * =============================================================================
 * 模块定位：M1 要素提取器 —— 将用户输入的自由文本解析为结构化记忆要素
 *           （事件、实体、地点、关键词、时间），为后续的实体解析（M2）、
 *           关键词管理（M3）与种子生成（M5）提供输入。
 *
 * 本文件主体思路：
 *   定义提取结果的数据结构（Extraction 及其子结构）与提取器接口 IExtractor，
 *   并声明基于启发式规则的默认实现 RuleExtractor。简化说明：无 LLM，
 *   RuleExtractor 用内置词典匹配 + 称呼模式 + 时间词完成提取；
 *   接口 IExtractor 可插拔，生产环境可替换为 LLM 提取器。
 *
 * 关键算法/数据结构：
 *   无复杂算法。数据结构为纯 POD 聚合体：Extraction 内嵌 event 匿名结构体，
 *   外接实体列表、单一地点与关键词列表；RuleExtractor 内部以 vector 顺序表
 *   存放地名词典与关键词词典，匹配时按词条长度降序做最长优先的子串匹配。
 *
 * 依赖关系：
 *   依赖 core 公共层 ame/core/types.h（Attrs、TimePoint、Precision 等基础类型）。
 *   实现文件依赖 M10 时间服务取当前时间。本模块被 M4 图写入器、M8 思考编排器
 *   等上游链路依赖，作为文本进入记忆系统的第一道加工环节。
 * =============================================================================
 */
#pragma once
#include "ame/core/types.h"

#include <string>
#include <vector>

namespace ame {

struct ExtractedEntity {
  std::string surface;
  std::string role = "人物";                 // 主体|人物|客体
  std::string entity_type = "person";        // person|object|org
  Attrs identifiers;                          // 强标识（可为空，可含 uid）
};

struct ExtractedPlace {
  std::string name;
  double lat = 0, lng = 0;
  bool has_coord = false;
};

struct ExtractedKeyword {
  std::string word;
  std::string category;  // action|attr|topic|sentiment|concept
};

// 属性三元组（LLM 提取专属）：(entity, slot, value)，slot 用小写蛇形开放词表
struct ExtractedAttribute {
  std::string entity;  // 实体 surface（speaker 或提及的人/物）
  std::string slot;    // residence / job / family_son / preference ...
  std::string value;
};

struct Extraction {
  struct {
    std::string text;
    std::string type = "generic";
    TimePoint time = 0;  // 0 = 写入时取当前时间
    Precision precision = Precision::Day;
  } event;
  std::vector<ExtractedEntity> entities;
  ExtractedPlace place;  // name 为空表示无地点
  std::vector<ExtractedKeyword> keywords;
  std::vector<ExtractedAttribute> attributes;  // 实体档案槽位（规则提取器不产出）
};

class IExtractor {
 public:
  virtual ~IExtractor() = default;
  virtual Extraction extract(const std::string& text) = 0;
};

// 规则版提取器（启发式：词典匹配 + "老X/小X"称呼模式 + 时间词解析）
class RuleExtractor : public IExtractor {
 public:
  RuleExtractor();
  Extraction extract(const std::string& text) override;

  // 向地名词典/关键词词典追加词条（供 demo/测试扩展）
  void add_place(const std::string& name, double lat, double lng);
  void add_keyword(const std::string& word, const std::string& category);

  // 运行时加载中文词典文件（领域词/实体词/分词合并词表，word[\tfreq][\tcat] 格式，
  // 按文件顺序（频次降序）取前 cap 条追加进关键词词典；返回加载条数。
  // cap > kAcThreshold 时走 AC 自动机（一遍扫描），否则逐词条子串匹配。
  int load_keyword_dict(const std::string& path, const std::string& category, int cap);

  // AC 自动机匹配入口（对拍/基准测试用）：返回文本中命中的 (起始码点, 词, 类别)
  std::vector<std::tuple<size_t, std::string, std::string>> ac_match(
      const std::string& text) const;

  // AC 统计（基准/报告用）：节点数、边数、估算内存字节
  void ac_stats(size_t& nodes, size_t& edges, size_t& approx_bytes) const;

  // AC 二进制持久化：dump 为紧凑二进制（magic+版本+词典校验），load 直接建结构
  // 跳过逐词插入与 BFS。load 校验词典 (size,mtime)，不一致返回 false 由调用方重建。
  bool ac_save(const std::string& cache_path, uint64_t dict_size, uint64_t dict_mtime) const;
  bool ac_load(const std::string& cache_path, uint64_t dict_size, uint64_t dict_mtime);
  // 带缓存的词典加载：cache 有效则直接加载（跳过构建），否则解析词典构建后写 cache
  int load_keyword_dict_cached(const std::string& path, const std::string& category,
                               int cap, const std::string& cache_path);

 private:
  struct PlaceEntry { std::string name; double lat, lng; };
  std::vector<PlaceEntry> places_;
  std::vector<std::pair<std::string, std::string>> kw_dict_;  // word -> category

  // ---- AC 自动机（Unicode 码点转移，防中文按字节错配）----
  // 紧凑节点：转移边为按码点排序的小数组 + 二分查找（70 万词级内存可接受，
  // 见 bench/scripts/bench_ac.cpp 实测）。
  struct AcNode {
    std::vector<std::pair<uint32_t, int>> next;  // (码点, 目标节点)，按码点排序
    int fail = 0;                                // 失败指针
    int out = -1;                                // 词条 index（非终结为 -1）
    int outlink = 0;                             // 最近的带输出祖先（输出链）
  };
  std::vector<AcNode> ac_nodes_;
  std::vector<std::string> ac_words_;
  std::vector<std::string> ac_cats_;
  std::vector<int> ac_lens_;  // 各词条码点长度（输出定位用）
  bool ac_built_ = false;
  void ac_insert(uint32_t node, const uint32_t* cps, size_t n, int word_idx);
  static int ac_find(const AcNode& nd, uint32_t cp);  // 二分查转移，无则 -1
  void ac_build_fail();
};

}  // namespace ame
