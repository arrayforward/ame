#pragma once
// ============================================================================
// 模块：M6 扩散引擎（diffusion）——联想记忆的"思考主体"，从种子节点出发沿
//       联想图做加权能量扩散，产出各节点的激活能量与可解释传播路径。
//
// 本文件主体思路：定义扩散引擎的对外接口与数据结构。调用方给出若干带权
// 种子（DiffusionSeed），diffuse() 沿图的边逐跳传播能量，返回
// node_uid -> 能量与 top 路径 的结果表（DiffusionResult），供 M7 打分重排、
// M8 思考编排消费。
//
// 关键算法/数据结构：加权逐层 BFS（波前扩展）。
//   - 边传导系数 = conduction_coeff(边类型) × 边 weight × 类型级权重乘子；
//     weight > 5 的高频边用 5 + log1p(w-5) 软化，避免头部边淹没全图。
//   - 能量按 1/(hop+1) 逐跳衰减，深度硬限 [2,3]，budget 限制扩展节点数，
//     能量 < 0.01 剪枝。
//   - 每节点保留能量 top3 的传播路径（Path），保证结果可解释。
//   - 休眠关键词（Keyword 且 status==1）不参与扩散。
//   V2 可学习边调制（能量 d 维向量 + MLP 门控）预留接口注释，见 learning 模块。
//
// 依赖关系：依赖 core 公共层（types.h 的 RelType/NodeKind/conduction_coeff）；
// 依赖 M9 storage（取节点、查邻居邻接表）与 M3 keyword（休眠关键词判定，
// 当前实现直接查节点 status，kw_ 为预留）。本接口被 M7 rerank、M8 thinker、
// M12 engine 调用。
// ============================================================================
#include "ame/core/types.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace ame {

class Storage;
class KeywordManager;

// 传播路径上的一步：from --type--> to
struct PathStep {
  std::string from, to;
  RelType type;
};
// 一条完整传播路径：steps 为逐步边序列，energy 为到达终点时的剩余能量
struct Path {
  std::vector<PathStep> steps;
  double energy = 0;
};

// 节点的扩散结果：累计能量 + 到达该节点的 top 路径
struct NodeEnergy {
  double energy = 0;
  std::vector<Path> paths;  // 保留能量最高的若干条路径
};
// node_uid -> 能量与路径
using DiffusionResult = std::unordered_map<std::string, NodeEnergy>;

// 扩散种子：起始节点 + 初始能量权重
struct DiffusionSeed {
  std::string node_uid;
  double weight = 1.0;
};

class DiffusionEngine {
 public:
  DiffusionEngine(Storage& storage, KeywordManager* kw = nullptr);

  // depth 硬限 [2,3]；rel_types 空 = 全通道；weights 为类型级权重乘子（可空）；
  // budget 限制扩展节点数
  DiffusionResult diffuse(const std::vector<DiffusionSeed>& seeds, int depth = 2,
                          const std::vector<RelType>* rel_types = nullptr,
                          const std::unordered_map<int, double>* weights = nullptr,
                          int budget = 1000) const;

  // 束搜索模式（引导式扩散阶段 1，接口向后兼容的增强版）：
  // beam_width>0 时逐跳只扩能量 top-beam_width 个前沿节点（best-first 剪枝，
  // budget 语义不变）；gate_fn 非空时边传导分由门控函数给出
  //（默认 = 类型系数×weight，与规则传导一致）；beam_width==0 退化为原 BFS。
  using GateFn = std::function<double(const Edge& e, const Node& head, const Node& tail,
                                      int hop)>;
  DiffusionResult diffuse_beam(const std::vector<DiffusionSeed>& seeds, int depth,
                               const std::vector<RelType>* rel_types,
                               const std::unordered_map<int, double>* weights,
                               int budget, int beam_width, const GateFn& gate_fn) const;

 private:
  Storage& storage_;
  KeywordManager* kw_;  // 休眠关键词判定（可空，空则查节点 status）
};

}  // namespace ame
