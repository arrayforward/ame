/*
 * 所属模块：M7 打分重排器（rerank）—— Recall 读取链路第③步"软重排"，
 *           对 M6 扩散引擎的输出做语境校准与最终排序，防止"图上相关但语境跑偏"。
 *
 * 本文件主体思路：实现 Reranker 的全部逻辑。核心流程是：先把 M6 扩散结果中各节点
 * 的能量按最大值归一化为图扩散分；再对每个 Event 节点计算语境余弦相似度（与查询
 * 向量）和时间衰减分（decay×importance）；按 α/β/γ 权重线性融合得到最终分；期间
 * 用 RecallFilters 做硬过滤（时间范围、人生阶段、场所时段、涉及实体类型）；最后
 * 按总分降序排序并截取 Top-N，每个结果附带一条扩散路径和人类可读的解释串。
 *
 * 关键算法/数据结构：
 *   - 三路信号线性融合：final = α·graph_norm + β·context + γ·decay×importance；
 *   - 图扩散分以当次扩散结果的最大能量做 max 归一化（max_energy<=0 时退化为 1）；
 *   - 语境分用向量余弦相似度，负值截断为 0；
 *   - 时间分用指数衰减因子（半衰期参数 90 天）乘以节点 importance；
 *   - 实体类型过滤借助 M9 的邻接查询，沿 INVOLVES 边检查是否涉及指定类型实体；
 *   - 解释串生成时对节点名按 UTF-8 码点边界截断，避免切断多字节字符。
 *
 * 依赖关系：
 *   - 依赖 core 公共层基础类型（经 reranker.h 引入）；
 *   - 依赖 M6 扩散引擎的 DiffusionResult 作为输入（经 reranker.h 引入）；
 *   - 依赖 M9 存储引擎 Storage：get_node 取节点、neighbors 查 INVOLVES 邻边、
 *     cosine 计算向量余弦；
 *   - 依赖 M10 时间服务 TimeService：decay_factor 计算时间衰减；
 *   - 依赖 M13 向量服务 VectorService（经构造函数持有，供语境向量能力扩展）；
 *   - 本模块被 M8 思考编排器（thinker）与 M12 API 网关（engine）在 Recall 管道中调用。
 */
#include "ame/rerank/reranker.h"

#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "ame/vector/vector_service.h"

#include <algorithm>

namespace ame {

// 构造函数：以引用注入 M9 存储、M10 时间服务、M13 向量服务三个依赖，仅保存引用，无其他初始化逻辑
Reranker::Reranker(Storage& storage, TimeService& ts, VectorService& vec)
    : storage_(storage), ts_(ts), vec_(vec) {}

// 三路信号线性加权融合：α·归一化图扩散分 + β·语境余弦 + γ·时间衰减×importance（单行公式，供 M14 调参复用）
double Reranker::fuse(double graph_norm, double context, double decay, double importance) const {
  return w_.alpha * graph_norm + w_.beta * context + w_.gamma * decay * importance;
}

// 伪代码：硬过滤器，逐项检查 RecallFilters，任一条件不满足即淘汰
//   步骤1：若设置了 time_from（>0）且事件时间戳早于它 → 淘汰；
//   步骤2：若设置了 time_to（>0）且事件时间戳晚于它 → 淘汰；
//   步骤3：若指定了人生阶段 phase：事件 attrs 中无 "phase" 属性或取值不等 → 淘汰；
//   步骤4：若指定了场所时段 period：事件 attrs 中无 "local_period" 属性或取值不等 → 淘汰；
//   步骤5：若指定了实体类型 entity_type：沿事件的 INVOLVES 出边遍历邻接节点，
//          若不存在 kind 为 Entity 且 type 匹配的邻居 → 淘汰；
//   步骤6：全部条件通过 → 返回 true。
bool Reranker::pass_filters(const Node& ev, const RecallFilters& f) const {
  if (f.time_from > 0 && ev.ts < f.time_from) return false;
  if (f.time_to > 0 && ev.ts > f.time_to) return false;
  if (!f.phase.empty()) {
    auto it = ev.attrs.find("phase");
    if (it == ev.attrs.end() || it->second != f.phase) return false;
  }
  if (!f.period.empty()) {
    auto it = ev.attrs.find("local_period");
    if (it == ev.attrs.end() || it->second != f.period) return false;
  }
  if (!f.entity_type.empty()) {
    bool hit = false;
    for (auto& [e, n] : storage_.neighbors(ev.uid, {RelType::INVOLVES})) {
      if (n->kind == NodeKind::Entity && n->type == f.entity_type) { hit = true; break; }
    }
    if (!hit) return false;
  }
  return true;
}

// 按 UTF-8 码点边界截断，避免切断多字节字符
// 伪代码：
//   步骤1：若字符串字节数不超过上限 → 原样返回；
//   步骤2：否则从第 max_bytes 字节处向前回退，跳过所有 UTF-8 续字节
//          （二进制 10xxxxxx，即 (b & 0xC0) == 0x80），直到码点起始字节；
//   步骤3：截取 [0, cut) 前缀并追加省略号"…"返回。
static std::string trunc_utf8(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t cut = max_bytes;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;  // 回退到码点起点
  return s.substr(0, cut) + "…";
}

// 伪代码：把一条扩散路径渲染成人类可读的链路解释串
//   步骤1：若路径为空（无步进）→ 返回"直接命中（种子节点）"；
//   步骤2：否则遍历路径的每条边：
//          a. 经 M9 存储取边的 from/to 节点；节点显示名取 name，name 为空则回退取 text，
//             节点指针为空则显示"?"；
//          b. 两端显示名均按 UTF-8 码点边界截断到 24 字节；
//          c. 拼接 "from -[关系名]-> to"，边之间以 "  →  " 连接；
//   步骤3：返回拼好的完整链路串。
std::string Reranker::make_explain(const Path& p) const {
  if (p.steps.empty()) return "直接命中（种子节点）";
  std::string s;
  for (size_t i = 0; i < p.steps.size(); ++i) {
    const Node* from = storage_.get_node(p.steps[i].from);
    const Node* to = storage_.get_node(p.steps[i].to);
    std::string fn = from ? (from->name.empty() ? from->text : from->name) : "?";
    std::string tn = to ? (to->name.empty() ? to->text : to->name) : "?";
    fn = trunc_utf8(fn, 24);
    tn = trunc_utf8(tn, 24);
    s += fn + " -[" + rel_name(p.steps[i].type) + "]-> " + tn;
    if (i + 1 < p.steps.size()) s += "  →  ";
  }
  return s;
}

// 伪代码：软重排主流程——对 M6 扩散结果做归一化、过滤、三路融合打分并输出 Top-N
//   步骤1：扫描扩散结果，取**事件节点**能量的最大值 max_event_energy 作为归一化基准
//         （候选只有事件；关键词/实体枢纽的能量是多路径累加的，量级远超事件，
//          若用全局 max 会把直接证据事件的图扩散分压到接近 0）；
//          若基准 <=0（无事件候选）则置为 1.0，避免除零；
//   步骤2：遍历扩散结果的每个节点：
//          a. 经 M9 存储取节点，取不到或不是 Event 节点 → 跳过；
//          b. 过 RecallFilters 硬过滤（时间/阶段/时段/实体类型），不通过 → 跳过；
//          c. 计算三路子分：
//             - 图扩散分 = 节点能量 / max_event_energy（max 归一化到 [0,1]）；
//             - 语境分 = 节点 embedding 与查询向量的余弦相似度，
//               节点无 embedding 时取 0，余弦为负时截断为 0；
//             - 时间分 = 指数衰减因子（半衰参数 90 天）× 节点 importance；
//          d. 用 fuse 按 α/β/γ 权重融合为总分；
//          e. 组装 ScoredEvent：总分、三路子分、首条扩散路径（若有）及其解释串，入结果集；
//   步骤3：按总分降序排序；
//   步骤4：结果数超过 limit 则截断为前 limit 条，返回。
std::vector<ScoredEvent> Reranker::rerank(const DiffusionResult& diff, const Vec& query_emb,
                                          const RecallFilters& filters, int limit) const {
  // 归一化图扩散分（仅以事件节点能量为基准）
  double max_energy = 0;
  for (auto& [uid, ne] : diff) {
    const Node* n = storage_.get_node(uid);
    if (n && n->kind == NodeKind::Event) max_energy = std::max(max_energy, ne.energy);
  }
  if (max_energy <= 0) max_energy = 1.0;

  std::vector<ScoredEvent> out;
  for (auto& [uid, ne] : diff) {
    const Node* n = storage_.get_node(uid);
    if (!n || n->kind != NodeKind::Event) continue;
    if (!pass_filters(*n, filters)) continue;

    double graph = ne.energy / max_energy;
    double ctx = n->embedding.empty() ? 0 : Storage::cosine(n->embedding, query_emb);
    if (ctx < 0) ctx = 0;
    double decay = TimeService::decay_factor(n->ts, 90.0);
    double score = fuse(graph, ctx, decay, n->importance);

    ScoredEvent se;
    se.event = n;
    se.score = score;
    se.graph_score = graph;
    se.context_score = ctx;
    se.time_score = decay * n->importance;
    if (!ne.paths.empty()) se.path = ne.paths.front();
    se.explain = make_explain(se.path);
    out.push_back(se);
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score > b.score; });
  if ((int)out.size() > limit) out.resize(limit);
  return out;
}

}  // namespace ame
