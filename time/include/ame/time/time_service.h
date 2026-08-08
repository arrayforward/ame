#pragma once
// ============================================================================
// 模块定位：M10 时间服务 —— UTC 为唯一事实源，地方时/时段标签/人生阶段皆为由
//           UTC 推导出的投影，不重复存储。
// 本文件思路：声明 TimeService 与投影结果结构 LocalProjection。对外提供四类能力：
//   UTC 存储校验、事件 × Place 的地方时投影（含饭点/打烊后等时段标签）、
//   事件 × Entity.LIFE_PHASE 的人生阶段匹配、时间指数衰减因子计算。
//   简化说明：时区用固定小时偏移（存于 Place.attrs["tz_offset"]），不引入 tz 数据库。
// 关键算法/数据结构：无复杂算法；时段划分为按小时的区间分段函数，
//   衰减因子为指数衰减公式 0.5^((now-ts)/half_life)。
// 依赖关系：依赖 core 公共层的 types.h（TimePoint/Node/RelType/NodeKind 等类型）；
//   前向声明 M9 存储引擎的 Storage（match_phase 经其读取实体邻边）；
//   本模块为基础服务，被 M6 扩散引擎、M7 重排器、M12 API 网关等上层模块依赖。
// ============================================================================
#include "ame/core/types.h"

#include <string>

namespace ame {

class Storage;

struct LocalProjection {
  TimePoint local_ts = 0;     // 地方时（epoch 偏移后，仅推导不存储）
  int local_hour = 0;
  std::string period;         // 清晨/上午/饭点/下午/傍晚/深夜
  bool after_hours = false;   // 打烊后（按 Place open_hours "HH:MM-HH:MM"）
};

class TimeService {
 public:
  // 构造：注入存储引擎指针（可为空，为空时 match_phase 直接返回空结果）
  explicit TimeService(const Storage* storage = nullptr) : storage_(storage) {}

  // UTC 存储校验：>0 且不超过当前时间太多（10 年容差）
  static bool validate_utc(TimePoint ts);

  // 事件 × Place.timezone/open_hours → 地方时投影
  LocalProjection project_local(TimePoint ts, const Node* place) const;

  // 事件 × Entity.LIFE_PHASE → 人生阶段名（无匹配返回空串）
  std::string match_phase(TimePoint ts, const std::string& entity_uid) const;

  // 指数衰减：0.5^((now-ts)/half_life)
  static double decay_factor(TimePoint ts, double half_life_days = 90.0, TimePoint now = 0);

  // 相对时间词锚定解析：以 anchor 为基准解析"昨天/上周/去年/yesterday/last week"等
  // 常见中英相对时间词，返回目标时间点；未命中返回 0（规则实现，可扩展词表）
  static TimePoint parse_relative(const std::string& text, TimePoint anchor);

  static TimePoint now_utc();

 private:
  const Storage* storage_;
};

}  // namespace ame
