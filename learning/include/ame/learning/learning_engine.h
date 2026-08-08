/*
 * 模块：M14 学习引擎（learning）—— α/β/γ 融合权重调优 + 边权赫布调整 + 模型版本管理。
 *
 * 本文件主体思路：声明 LearningEngine 对外接口。离线训练基于反馈三元组
 * （query、正样本、负样本）做两件事：对正/负样本节点的邻边做赫布式加强/减弱；
 * 再网格搜索 α/β/γ，使正负样本对的 pairwise 排序准确率最优。训练产物以 JSON
 * 配置文件按版本落盘，支持影子评估（shadow_eval）、上线（promote）与回滚（rollback）。
 * V1 只做网格/坐标搜索调 α/β/γ 与赫布边权调整；边调制 MLP
 * （M(edge)=σ(MLP(emb头,emb尾,边类型,上下文特征))）与 LoRA 留待 V2，
 * 接口已预留（见 train_edge_modulator 注释）。
 *
 * 关键算法/数据结构：约束网格搜索（α+β+γ=1、步长 0.1）、赫布学习规则
 * （正样本边权 ×1.05、负样本边权 ×0.95）、pairwise 排序准确率（NDCG 代理指标）；
 * 模型版本用内存版本栈（versions_）+ 磁盘 JSON 配置文件管理。
 *
 * 依赖关系：依赖 core 公共层（types.h 基础类型/错误码）、feedback（M11 反馈采集器的
 * FeedbackTriple）；通过前向声明持有 storage（M9 存储引擎）、vector（M13 向量服务）、
 * rerank（M7 打分重排器）的引用。本模块被 engine（M12 API 网关）装配调用。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/feedback/feedback_collector.h"
#include "ame/learning/gate_model.h"

#include <string>
#include <vector>

namespace ame {

class Storage;
class VectorService;
class Reranker;

struct ShadowReport {
  double acc_current = 0;   // 当前版本 pairwise 准确率（NDCG 代理指标）
  double acc_candidate = 0;
  std::string diff_report;
};

class LearningEngine {
 public:
  LearningEngine(Storage& storage, VectorService& vec, Reranker& rerank,
                 const std::string& config_dir);

  // 离线训练：赫布边权调整 + α/β/γ 网格搜索；返回新模型版本号
  std::string train_offline(const std::vector<FeedbackTriple>& triples);

  ShadowReport shadow_eval(const std::string& model_version,
                           const std::vector<FeedbackTriple>& sample_triples);

  Err promote(const std::string& model_version);  // 上线（写入 active 配置并应用）
  Err rollback();                                  // 回滚上一版本

  // ---- 引导式扩散阶段 1：监督边门控 ----
  // 训练门控：triples（judge=1→正、0→负）太少（<kMinTriples）则跳过训练保留规则版，
  // 返回版本号（空串=跳过）；门控参数随模型版本 JSON 一并版本化（promote/rollback 复用）
  std::string train_gate(const std::vector<FeedbackTriple>& triples);
  const GateModel& gate() const { return gate_; }  // 当前门控模型（trained=false 时恒等规则）
  static constexpr int kMinTriples = 20;

  // V2 预留：边调制 MLP 训练（能量 d 维向量 + 逐维门控），接口占位
  // std::string train_edge_modulator(const std::vector<FeedbackTriple>& triples);

 private:
  struct ModelCfg { double alpha, beta, gamma; };
  double eval_cfg(const ModelCfg& c, const std::vector<FeedbackTriple>& triples) const;
  std::string cfg_path(const std::string& version) const;
  Err apply(const ModelCfg& c);

  Storage& storage_;
  VectorService& vec_;
  Reranker& rerank_;
  std::string dir_;
  std::vector<std::string> versions_;  // 版本栈（回滚用）
  GateModel gate_;                     // 监督边门控（阶段 1）
};

}  // namespace ame
