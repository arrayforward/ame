/*
 * 模块定位：M4 图写入器（graph_writer）——记忆落库执行器，负责把 M1 提取结果
 *           与 M2 实体解析结果物化为图中的 Event 节点与各类锚点边，即"写入即生长"。
 * 本文件主体思路：定义 GraphWriter 类接口（write_event / import_events /
 *           summarize_entity_events / get_or_create_place）及写入结果结构
 *           WriteResult，仅声明、不含实现；实现位于 src/graph_writer.cpp。
 * 关键算法/数据结构：接口层无复杂算法；实现层涉及 geohash 区域划分、
 *           500 米内近邻双向 NEAR 边（每点上限 10 邻居）、关键词共现边
 *           计数结晶为组成边（≥3 次）、NEXT 叙事时间链等。
 * 依赖关系：本文件包含 core/types.h（Node/Edge/Err 等基础类型）与
 *           extractor/extractor.h（Extraction/ExtractedPlace 输入结构）；
 *           对 M9 storage、M3 keyword、M10 time、M13 vector 仅做前置声明，
 *           通过构造注入其引用。被 M12 engine 网关及上层调用方依赖。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/extractor/extractor.h"

#include <string>
#include <vector>

namespace ame {

class Storage;
class KeywordManager;
class TimeService;
class VectorService;

struct WriteResult {
  std::string event_id;
  std::vector<std::pair<std::string, std::string>> resolved;  // surface -> uid
  Err code = Err::Ok;
};

class GraphWriter {
 public:
  GraphWriter(Storage& storage, KeywordManager& kw, TimeService& ts, VectorService& vec);

  // 创建 Event 节点 + 四类锚点边 + 自动生长五项
  WriteResult write_event(const Extraction& extraction,
                          const std::vector<std::pair<std::string, std::string>>& resolved_entities);

  // 批量导入，分批（默认每批 1000）
  std::vector<WriteResult> import_events(
      const std::vector<std::pair<Extraction, std::vector<std::pair<std::string, std::string>>>>& batch,
      int batch_size = 1000);

  // 高频实体摘要聚合：把 window 秒内该实体的事件汇总为一个摘要事件
  std::string summarize_entity_events(const std::string& uid, TimePoint window_seconds);

  // 获取或创建 Place；新 Place 自动挂 IN(Region, geohash 前缀) 与 NEAR(≤500m, ≤10 邻居)
  std::string get_or_create_place(const ExtractedPlace& p, bool* created = nullptr);

 private:
  void auto_grow(const Node& ev, const std::vector<std::string>& entity_uids,
                 const std::vector<std::string>& kw_uids, const std::string& place_uid);
  // 知识更新：同实体同槽位词的旧事件标 valid_to 并连 SUPERSEDES 边
  void supersede_pass(const Node& ev, const std::vector<std::string>& entity_uids,
                      const std::vector<std::string>& kw_uids);
  Storage& storage_;
  KeywordManager& kw_;
  TimeService& ts_;
  VectorService& vec_;
  std::string last_event_uid_;  // NEXT 叙事时间链
};

}  // namespace ame
