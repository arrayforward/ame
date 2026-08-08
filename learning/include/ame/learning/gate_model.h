/*
 * 所属模块：M14 学习引擎 —— 边门控网络（监督版，引导式扩散阶段 1）。
 * 本文件主体思路：声明 GateModel——M(edge)=sigmoid(logit(类型系数)+MLP(x)），
 *   特征 = [emb(头)压缩; emb(尾)压缩; onehot(边类型); 手工上下文特征]，
 *   MLP 前向/反向纯手写（小网络，零依赖）；初始化为恒等规则传导（未训练不回归）。
 * 关键算法/数据结构：
 *   - emb 压缩 128→16：分段均值（8 维一段，确定性；注释说明可换学习投影）；
 *   - MLP 结构 57→32(relu)→1，残差式输出（MLP 输出初始为 0 → 输出恒等于类型系数）；
 *   - 训练为 SGD 反向传播（pairwise 损失，见 learning_engine.cpp）。
 * 依赖关系：依赖 core(types.h/json.h)；被 M14 learning_engine 与 M6 diffuse_beam 使用。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/core/json.h"

#include <cmath>
#include <string>
#include <vector>

namespace ame {

class GateModel {
 public:
  static constexpr int kEmbIn = 128, kEmbC = 16;   // emb 压缩：128 → 16 分段均值
  static constexpr int kRel = 19;                  // RelType 枚举数（onehot 维度）
  static constexpr int kCtx = 6;                   // 手工上下文特征维数
  static constexpr int kIn = 2 * kEmbC + kRel + kCtx;  // 57
  static constexpr int kHid = 32;

  GateModel();

  // 边传导门控分：sigmoid(logit(类型系数) + MLP(特征))，输出 (0,1)
  double gate(const Edge& e, const Node& head, const Node& tail, int hop) const;

  // 特征抽取（压缩 emb + onehot 类型 + 上下文：weight/系数/头尾出度 log/跳数/反向标记）
  std::vector<float> features(const Edge& e, const Node& head, const Node& tail,
                              int hop) const;

  // 训练一步（SGD）：输入特征与目标传导 y∈{0,1}（正样本 1 / 负样本 0），
  // 返回该样本 BCE 损失；lr 学习率，l2 正则系数。
  double train_step(const std::vector<float>& x, double y, double lr, double l2);

  // 序列化（版本化模型 JSON，promote/rollback 机制复用）
  Json to_json() const;
  bool from_json(const Json& j);

  bool trained = false;  // 是否经训练（false 时输出恒等于类型系数）

 private:
  static double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }
  static double logit(double p) { return std::log(p / (1.0 - p)); }

  std::vector<float> w1_;  // kHid × kIn
  std::vector<float> b1_;  // kHid
  std::vector<float> w2_;  // kHid
  float b2_ = 0;
};

}  // namespace ame
