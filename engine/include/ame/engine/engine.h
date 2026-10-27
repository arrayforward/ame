/*
 * 所属模块与定位：M12 API 网关/外观（engine）——联想记忆引擎对外的唯一入口，
 *                 聚合 M1~M11、M13、M14 全部子模块，暴露 register / remember / lookup /
 *                 recall / think / merge_entities / split_entity 等动词。
 * 本文件主体思路：定义 Engine 外观类与配套的结果/查询结构体（RememberResult、LookupQuery、
 *                 RecallResult），声明全部对外 API；Engine 的成员即各子模块实例，
 *                 依赖装配在构造函数的初始化列表中完成（实现见 src/engine.cpp）。
 * 关键算法/数据结构说明：无复杂算法；本文件仅为接口声明与 POD 结构体定义，
 *                        业务算法分散在各子模块内部。
 * 依赖关系：依赖 core(types.h) 及 M1 要素提取器、M2 实体解析器、M3 关键词管理器、
 *           M4 图写入器、M5 种子生成器、M6 扩散引擎、M7 打分重排器、M8 思考编排器、
 *           M9 存储引擎、M10 时间服务、M11 反馈采集器、M13 向量服务、M14 学习引擎的头文件；
 *           本头文件被上层应用 / demo / 测试直接包含。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/diffusion/diffusion_engine.h"
#include "ame/entity/entity_resolver.h"
#include "ame/extractor/extractor.h"
#include "ame/graph_writer/graph_writer.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/rerank/reranker.h"
#include "ame/seed/seed_generator.h"
#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "ame/vector/vector_service.h"

#include <memory>
#include <string>
#include <vector>

namespace ame {

struct RememberResult {
  Err code = Err::Ok;
  std::string msg;
  std::string event_id;
  std::vector<std::pair<std::string, std::string>> resolved_entities;  // surface -> uid
};

struct LookupQuery {
  std::vector<std::string> entity_uids;
  std::string place_uid;
  TimePoint time_from = 0, time_to = 0;
  std::vector<std::string> keywords;
  std::string order_by = "time";  // time|importance
  int limit = 50;
  bool current_only = false;  // true 时跳过已被取代（valid_to 非空）的过时事实
};

struct RecallResult {
  Err code = Err::Ok;
  std::string msg;
  std::vector<ScoredEvent> events;
};

class Engine {
 public:
  // data_dir 非空时启用 M11 反馈日志与 M14 配置目录
  explicit Engine(const std::string& data_dir = "");

  // 4.0：启用 PostgreSQL 存储后端（构造后、写入前调用；ns 为 session 隔离命名空间）。
  // 需以 AME_WITH_POSTGRES 编译，否则返回 Internal；成功后 recall/remember 全走 SQL。
  Err open_postgres(const std::string& conninfo, const std::string& ns = "default") {
    return storage_.connect_pg(conninfo, ns);
  }

  // 4.0 租户共享记忆：attach 后本 session 的 recall 自动合并租户引擎证据
  // （非拥有指针，生命周期由调用方保证；共同记忆写入直接调 tenant->remember_turn，
  // 仍各自 ns 隔离存储；lookup/实体档案等其它动词保持 session 私有）。
  void attach_tenant(Engine* tenant) { tenant_ = tenant; }
  void detach_tenant() { tenant_ = nullptr; }

  // ① 注册实体
  std::string register_entity(const std::string& type, const std::string& name,
                              const Attrs& identifiers = {},
                              const std::vector<std::string>& aliases = {});

  // ② 写入：自由文本或结构化事件
  RememberResult remember(const std::string& text);
  RememberResult remember_event(const Extraction& ex);

  // ②b 对话轮写入：speaker 复用为同一实体（跨轮消歧），ts 为 UTC 时间戳（0=当前），
  // dia_id/session_id 存入事件节点属性（供基准证据命中统计），空串则不写
  RememberResult remember_turn(const std::string& speaker, const std::string& text,
                               TimePoint ts = 0, const std::string& dia_id = "",
                               const std::string& session_id = "");

  // ②c 预提取版对话轮写入：ex 已由调用方（如 LlmExtractor 会话批提取）填好，
  // 本方法只负责补 speaker 主体实体、覆盖时间戳、落库与属性回填
  RememberResult remember_turn_ex(Extraction ex, const std::string& speaker, TimePoint ts,
                                  const std::string& dia_id = "",
                                  const std::string& session_id = "");

  // ③ 精确查找
  std::vector<const Node*> lookup(const LookupQuery& q) const;

  // ③b 双时态查询：as_of 时刻视角的事件（ts<=as_of 且当时未被取代）
  std::vector<const Node*> lookup_as_of(LookupQuery q, TimePoint as_of) const;

  // ③c 事件排序：沿 NEXT 链/时间戳给出全局先后（供 event_ordering 与 Kendall tau-b）
  std::vector<std::string> order_events(const std::vector<std::string>& uids) const;

  // ④ 联想检索（软→硬→软三层管道）
  RecallResult recall(const SeedQuery& seeds, const RecallFilters& filters = {},
                      int depth = 2, int limit = 20, bool explain = true);

  // ⑥ 纠错
  Err merge_entities(const std::string& uid_a, const std::string& into_b);
  std::string split_entity(const std::string& uid, const std::vector<std::string>& mention_ids);

  // ⑦ 实体档案：身份 + 当前/历史槽位 + 时间线（设计文档第八节）
  EntityProfile get_entity_profile(const std::string& uid) const {
    return entity_.get_profile(uid);
  }

  // 引擎统计（上下文外置可视化用，Android 状态栏数据通路）：
  //   turns   = 事件节点数（每轮 remember_turn 产 1 事件）；
  //   total_chars = 全部事件文本字符数（token 估算口径 chars/4 在调用侧做）；
  //   events  = 事件节点数（同 turns，语义别名便于 JSON 直出）；
  //   nodes   = 图节点总数（事件+实体+关键词等）。
  struct EngineStats {
    long turns = 0;
    long total_chars = 0;
    long events = 0;
    long nodes = 0;
  };
  EngineStats stats() const {
    EngineStats s;
    s.nodes = (long)storage_.node_count();
    for (auto* n : storage_.nodes_of_kind(NodeKind::Event)) {
      ++s.events;
      ++s.turns;
      s.total_chars += (long)n->text.size();
    }
    return s;
  }

  // 快照（组合格式：{"graph": M9 图快照, "slots": M2 实体档案槽位}，含 valid_to 历史）
  Err save(const std::string& path) const;
  Err load(const std::string& path);

  // 暴露子模块（测试/demo 用）
  Storage& storage() { return storage_; }
  EntityResolver& entities() { return entity_; }
  KeywordManager& keywords() { return keyword_; }
  VectorService& vectors() { return vector_; }
  TimeService& times() { return time_; }
  Reranker& reranker() { return rerank_; }
  RuleExtractor& extractor() { return extractor_; }

 private:
  Storage storage_;
  Engine* tenant_ = nullptr;  // 4.0 租户知识库（非拥有；attach_tenant 注入）
  VectorService vector_;
  TimeService time_;
  KeywordManager keyword_;
  EntityResolver entity_;
  RuleExtractor extractor_;
  GraphWriter writer_;
  SeedGenerator seed_;
  DiffusionEngine diffusion_;
  Reranker rerank_;
};

}  // namespace ame
