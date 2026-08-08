/*
 * 模块定位：M2 实体解析器（Entity Resolver）——负责 Mention → Entity 的消歧归并
 *           与实体 uid 的生命周期管理（注册、别名、合并、拆分、旧号跳转）。
 *
 * 本文件主体思路：声明 EntityResolver 对外的消歧与实体管理接口，以及配套的
 *   数据结构。消歧采用三级定位策略，逐级降级：第 1 级 uid 直连（置信度 1.0）；
 *   第 2 级强标识符精确匹配，查不到则直接建档；第 3 级裸名字召回候选后结合
 *   上下文证据投票打分，按分数区间决定直连 / 待确认 / 新建并标记疑似重复。
 *
 * 关键算法/数据结构：
 *   - 三级消歧流水线（uid 直连 → 强标识符匹配 → 名字+证据投票）；
 *   - 证据加权投票：基础分 + 共同经历 + 空间一致 + 唯一候选加权；
 *   - redirect_ 跳转表（unordered_map，旧 uid -> 新 uid），合并后旧号永不复用。
 *
 * 依赖关系：依赖 core 公共层的基础类型（Node/Edge/Attrs/TimePoint/Err 等）；
 *   依赖 M9 存储引擎（Storage）读写节点与边；依赖 M13 向量服务（VectorService）
 *   生成实体名向量。本头文件被 M12 API 网关、M8 思考编排器等上层模块依赖。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/core/json.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ame {

class Storage;
class VectorService;

struct ResolveResult {
  std::string uid;
  double confidence = 0;
  bool is_new = false;
  bool pending = false;             // 0.5~0.9 标记待确认（人在环路）
  bool suspected_dup = false;       // <0.5 新建，标记疑似重复
  std::string mention_uid;          // 本次提及创建的 Mention 节点
  std::vector<std::string> candidates;
};

struct EntityCard {
  std::string uid, name, type;
  std::vector<std::string> aliases;
  Attrs identifiers;
  int event_count = 0;
  TimePoint latest_event_ts = 0;
};

// 属性槽位记录（实体档案·槽位层）：一条 (slot, value, 有效期, 来源事件) 四元组。
// 同实体同槽位写新值时旧记录标 valid_to（与图层 SUPERSEDES 同一语义），当前值 valid_to==0。
struct SlotRecord {
  std::string slot;           // 小写蛇形开放词表：residence / job / preference ...
  std::string value;
  TimePoint valid_from = 0;
  TimePoint valid_to = 0;     // 0 = 当前有效
  std::string source_event;   // 来源事件 uid
};

// 实体档案：身份层 + 槽位层 + 时间线视图（事件 uid 按时间升序）
struct EntityProfile {
  EntityCard card;
  std::vector<SlotRecord> slots;            // 全部槽位记录（含历史）
  std::vector<std::string> timeline;        // 事件 uid，按 ts 升序
  // 当前有效槽位查询（无则空串）
  std::string current_slot(const std::string& slot) const {
    for (auto& s : slots)
      if (s.slot == slot && s.valid_to == 0) return s.value;
    return "";
  }
};

// 消歧上下文证据
struct ResolveContext {
  std::vector<std::string> co_entity_uids;  // 同事件其他实体
  std::string place_uid;                    // 空间一致性
};

class EntityResolver {
 public:
  EntityResolver(Storage& storage, VectorService& vec);

  ResolveResult resolve(const std::string& surface, const ResolveContext& context = {},
                        const Attrs& identifiers = {});

  std::string register_entity(const std::string& type, const std::string& name,
                              const Attrs& identifiers = {},
                              const std::vector<std::string>& aliases = {});

  EntityCard get_entity(const std::string& uid) const;   // uid 不存在返回空 card
  std::string find_entity(const Attrs& identifiers) const;  // 按强标识符反查，无则空串

  Err merge_entities(const std::string& uid_a, const std::string& into_b);
  std::string split_entity(const std::string& uid, const std::vector<std::string>& mention_ids);
  Err add_alias(const std::string& uid, const std::string& surface);

  // 写入属性槽位：同实体同槽位且值不同 → 旧记录标 valid_to（知识更新语义）；
  // 值相同则只更新来源（幂等）
  Err put_slot(const std::string& uid, const std::string& slot, const std::string& value,
               TimePoint ts, const std::string& source_event);
  // 实体档案：身份 + 槽位（含历史）+ 时间线（INVOLVES 事件按 ts 升序）
  EntityProfile get_profile(const std::string& uid) const;
  // as_of 时刻的槽位值（双时态查询；无则空串）
  std::string slot_as_of(const std::string& uid, const std::string& slot, TimePoint as_of) const;

  // 槽位持久化：导出/导入 JSON（并入 M12 组合快照，含 valid_to 历史）
  Json slots_to_json() const;
  void slots_from_json(const Json& j);

  // 旧 uid 跳转（merge 后旧号永不复用）
  std::string redirect(const std::string& uid) const;

 private:
  std::string create_mention(const std::string& surface, const std::string& entity_uid,
                             double confidence);
  Storage& storage_;
  VectorService& vec_;
  std::unordered_map<std::string, std::string> redirect_;  // old uid -> new uid
  std::unordered_map<std::string, std::vector<SlotRecord>> slots_;  // uid -> 槽位记录（内存态）
};

}  // namespace ame
