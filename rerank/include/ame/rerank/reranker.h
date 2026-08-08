/*
 * 所属模块：M7 打分重排器（rerank）—— Recall 读取链路第③步"软重排"，
 *           对 M6 扩散引擎的输出做语境校准与最终排序，防止"图上相关但语境跑偏"。
 *
 * 本文件主体思路：声明 Reranker 类及其配套数据结构。Reranker 接收 DiffusionResult
 * （M6 扩散结果：节点能量 + 可解释路径），对每个 Event 候选节点计算三路子分——
 * 归一化图扩散分、查询向量语境余弦相似度、时间衰减×importance——按可调权重
 * 线性融合为最终分，再经召回过滤器（时间范围/人生阶段/场所时段/实体类型）筛选后
 * 降序截取 Top-N，并携带扩散路径与人类可读的解释串返回。
 *
 * 关键算法/数据结构：
 *   - 三路信号线性融合打分：final = α·graph_norm + β·context + γ·decay×importance，
 *     权重封装在 FusionWeights 中，可由 M14 学习引擎调节；
 *   - 结果载体 ScoredEvent：保存事件指针、总分、三路子分、扩散路径 Path 与解释串，
 *     支撑结果可解释性；RecallFilters 为可选的硬过滤条件集合。
 *   - 本头文件仅含声明与数据结构定义，无复杂算法实现。
 *
 * 依赖关系：
 *   - 依赖 core 公共层（types.h：Node/Path/Vec/TimePoint 等基础类型）；
 *   - 依赖 M6 扩散引擎（diffusion_engine.h：DiffusionResult 作为输入）；
 *   - 前向声明并组合 M9 存储引擎 Storage（取节点/邻边）、M10 时间服务 TimeService
 *     （衰减因子）、M13 向量服务 VectorService（余弦相似度）；
 *   - 本模块被 M8 思考编排器（thinker）与 M12 API 网关（engine）在 Recall 管道中调用。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/diffusion/diffusion_engine.h"

#include <string>
#include <vector>

namespace ame {

class Storage;
class TimeService;
class VectorService;

struct RecallFilters {
  TimePoint time_from = 0, time_to = 0;  // 时间范围（0=不限）
  std::string entity_type;               // 涉及实体的类型
  std::string phase;                     // 人生阶段
  std::string period;                    // 场所时段（饭点/深夜/...）
};

struct ScoredEvent {
  const Node* event = nullptr;
  double score = 0;
  double graph_score = 0, context_score = 0, time_score = 0;
  Path path;
  std::string explain;
};

struct FusionWeights {
  double alpha = 0.5, beta = 0.3, gamma = 0.2;  // 可调/可学习（M14）
};

class Reranker {
 public:
  Reranker(Storage& storage, TimeService& ts, VectorService& vec);

  std::vector<ScoredEvent> rerank(const DiffusionResult& diff, const Vec& query_emb,
                                  const RecallFilters& filters = {}, int limit = 20) const;

  // 设置融合权重（供 M14 学习引擎在线调参）
  void set_weights(const FusionWeights& w) { w_ = w; }
  // 读取当前融合权重
  FusionWeights weights() const { return w_; }

  // 单候选融合打分（M14 调参复用）
  double fuse(double graph_norm, double context, double decay, double importance) const;

 private:
  bool pass_filters(const Node& ev, const RecallFilters& f) const;
  std::string make_explain(const Path& p) const;
  Storage& storage_;
  TimeService& ts_;
  VectorService& vec_;
  FusionWeights w_;
};

}  // namespace ame
