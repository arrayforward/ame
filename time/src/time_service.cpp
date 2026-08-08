// ============================================================================
// 模块定位：M10 时间服务 —— TimeService 各方法的实现，UTC 唯一事实源之上的
//           时间投影与衰减计算。
// 本文件思路：实现头文件声明的五个方法：取当前 UTC、UTC 合法性校验、
//   事件时间戳到 Place 当地时/时段/打烊状态的投影、实体人生阶段匹配、
//   记忆强度的时间衰减因子。
// 关键算法/数据结构：
//   - 时段划分 period_of_hour：按小时的区间分段（清晨/上午/饭点/下午/傍晚/深夜）；
//   - 打烊判定：解析 "HH:MM-HH:MM" 营业区间，支持跨零点区间（如 17:00-02:00）；
//   - 人生阶段匹配：遍历实体 LIFE_PHASE 邻边，取时间区间包含 ts 的阶段节点；
//   - 衰减因子：指数衰减 0.5^(经过天数/半衰期)，负时长截断为 0。
// 依赖关系：依赖 core types.h（TimePoint/Node/RelType/NodeKind）与
//   M9 存储引擎 storage.h（Storage::neighbors 读取实体邻边）；
//   被上层 M6/M7/M12 等模块作为基础时间服务使用。
// ============================================================================
#include "ame/time/time_service.h"

#include "ame/storage/storage.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ame {

// 取当前 UTC 秒级时间戳
TimePoint TimeService::now_utc() { return (TimePoint)std::time(nullptr); }

// UTC 存储校验伪代码：
//   步骤1：计算上限 = 当前 UTC + 10 年（容忍时钟偏差导致的近未来时间）
//   步骤2：合法当且仅当 ts > 0 且 ts < 上限
bool TimeService::validate_utc(TimePoint ts) {
  const TimePoint kMax = now_utc() + 10LL * 365 * 86400;  // 允许 10 年内的未来时间
  return ts > 0 && ts < kMax;
}

// 小时 → 时段标签的分段映射伪代码：
//   5~9 清晨；9~11 上午；11~14 或 17~20 饭点；14~17 下午；20~23 傍晚；
//   其余（23~次日 5）为深夜
static std::string period_of_hour(int h) {
  if (h >= 5 && h < 9) return "清晨";
  if (h >= 9 && h < 11) return "上午";
  if ((h >= 11 && h < 14) || (h >= 17 && h < 20)) return "饭点";
  if (h >= 14 && h < 17) return "下午";
  if (h >= 20 && h < 23) return "傍晚";
  return "深夜";  // 23~5
}

// 地方时投影伪代码：
//   步骤1：默认时区偏移东八区（+8h），open_hours 置空
//   步骤2：若传入 place 节点，从其 attrs 读 "tz_offset"（小时偏移）与
//          "open_hours"（"HH:MM-HH:MM" 营业时段），存在则覆盖默认值
//   步骤3：local_ts = UTC + 偏移秒数；用 gmtime_r 对偏移后的时间取 UTC 分解，
//          等效得到当地小时，再经 period_of_hour 得到时段标签
//   步骤4：若有 open_hours，解析出开/关店时刻并折算为当日分钟数；
//          区间不跨零点（开<=关）时判断 cur 是否落在 [开,关) 内；
//          跨零点（开>关）时判断 cur 是否在 [开,24h) 或 [0,关) 内；
//          不在营业区间内即 after_hours = true
//   步骤5：返回投影结果
LocalProjection TimeService::project_local(TimePoint ts, const Node* place) const {
  LocalProjection lp;
  double offset_h = 8.0;  // 默认东八区
  std::string open_hours;
  if (place) {
    auto it = place->attrs.find("tz_offset");
    if (it != place->attrs.end()) offset_h = std::stod(it->second);
    auto oh = place->attrs.find("open_hours");
    if (oh != place->attrs.end()) open_hours = oh->second;
  }
  lp.local_ts = ts + (TimePoint)(offset_h * 3600);
  std::time_t t = (std::time_t)lp.local_ts;
  std::tm tm{};
  gmtime_r(&t, &tm);
  lp.local_hour = tm.tm_hour;
  lp.period = period_of_hour(tm.tm_hour);

  // open_hours "HH:MM-HH:MM" → 打烊后判定
  if (!open_hours.empty()) {
    int h1, m1, h2, m2;
    if (std::sscanf(open_hours.c_str(), "%d:%d-%d:%d", &h1, &m1, &h2, &m2) == 4) {
      int cur = tm.tm_hour * 60 + tm.tm_min;
      int o = h1 * 60 + m1, c = h2 * 60 + m2;
      bool in = (o <= c) ? (cur >= o && cur < c) : (cur >= o || cur < c);
      lp.after_hours = !in;
    }
  }
  return lp;
}

// 人生阶段匹配伪代码：
//   步骤1：若无存储引擎注入，直接返回空串
//   步骤2：取实体节点的全部邻边（经 Storage::neighbors）
//   步骤3：遍历邻边，跳过非 LIFE_PHASE 类型或邻居非 Phase 节点的边
//   步骤4：从边 attrs 读阶段起止时间 start/end（缺省分别为 0 / INT64_MAX，
//          即无界开放区间）
//   步骤5：退化兼容——若边上无 start，则改从阶段节点自身 attrs 读 start/end
//   步骤6：若 ts 落在 [start, end] 闭区间内，返回该阶段节点名（首个匹配即返回）
//   步骤7：全部不匹配则返回空串
std::string TimeService::match_phase(TimePoint ts, const std::string& entity_uid) const {
  if (!storage_) return "";
  auto nbs = storage_->neighbors(entity_uid);
  for (auto& [e, n] : nbs) {
    if (e.type != RelType::LIFE_PHASE || n->kind != NodeKind::Phase) continue;
    TimePoint start = 0, end = INT64_MAX;
    auto si = e.attrs.find("start");
    if (si != e.attrs.end()) start = std::stoll(si->second);
    auto ei = e.attrs.find("end");
    if (ei != e.attrs.end()) end = std::stoll(ei->second);
    if (si == e.attrs.end()) {  // 退化：阶段节点自身 attrs 存 start/end
      auto ns = n->attrs.find("start");
      if (ns != n->attrs.end()) start = std::stoll(ns->second);
      auto ne = n->attrs.find("end");
      if (ne != n->attrs.end()) end = std::stoll(ne->second);
    }
    if (ts >= start && ts <= end) return n->name;
  }
  return "";
}

// 时间衰减因子伪代码：
//   步骤1：ts 非法（<=0）时视为无衰减，返回 1.0
//   步骤2：now 未指定（<=0）时取当前 UTC
//   步骤3：计算 ts 距 now 的天数；若为负（未来时间）截断为 0
//   步骤4：返回 0.5^(天数/半衰期天数)，默认半衰期 90 天
double TimeService::decay_factor(TimePoint ts, double half_life_days, TimePoint now) {
  if (ts <= 0) return 1.0;
  if (now <= 0) now = now_utc();
  double days = (double)(now - ts) / 86400.0;
  if (days < 0) days = 0;
  return std::pow(0.5, days / half_life_days);
}

// 相对时间词锚定解析伪代码：
//   步骤1：anchor 非法时以当前 UTC 为锚；
//   步骤2：按词表逐项匹配（长词优先，中文词直接子串匹配，英文词用小写化子串匹配）——
//          前天/-2d、昨天|yesterday/-1d、今天|today/0、上周|last week/-7d、
//          上个月|last month/-30d、去年|last year/-365d、几个月前|a few months ago/-90d；
//   步骤3：命中返回 anchor - 偏移，全部未命中返回 0（调用方据此不施加时间过滤）。
TimePoint TimeService::parse_relative(const std::string& text, TimePoint anchor) {
  if (anchor <= 0) anchor = now_utc();
  std::string lower;
  lower.reserve(text.size());
  for (char c : text) lower += (char)std::tolower((unsigned char)c);
  auto has = [&](const std::string& w) { return text.find(w) != std::string::npos; };
  auto hasl = [&](const std::string& w) { return lower.find(w) != std::string::npos; };
  const TimePoint D = 86400;
  if (has("前天")) return anchor - 2 * D;
  if (has("昨天") || hasl("yesterday")) return anchor - D;
  if (has("今天") || hasl("today")) return anchor;
  if (has("上周") || has("上星期") || hasl("last week")) return anchor - 7 * D;
  if (has("上个月") || hasl("last month")) return anchor - 30 * D;
  if (has("几个月前") || hasl("a few months ago")) return anchor - 90 * D;
  if (has("去年") || hasl("last year")) return anchor - 365 * D;
  return 0;
}

}  // namespace ame
