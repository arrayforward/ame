/*
 * 模块：M14 学习引擎（learning）—— α/β/γ 融合权重调优 + 边权赫布调整 + 模型版本管理。
 *
 * 本文件主体思路：LearningEngine 的实现。解决"如何根据用户反馈持续优化检索排序"
 * 的问题：离线训练时先按赫布规则调整图边权（正样本关联加强、负样本减弱），再在
 * α+β+γ=1 的约束网格上搜索 rerank 融合权重，以离线可重算特征（语境相似度、
 * 时间衰减×importance、度归一）的 pairwise 排序准确率为目标；最优配置按版本
 * 写入 JSON 文件，经 shadow_eval 与当前线上权重对比后由 promote 上线，rollback 回滚。
 *
 * 关键算法/数据结构：
 * - 赫布学习规则：正样本节点邻边权重 ×1.05（上限 10），负样本 ×0.95（下限 0.1）；
 * - 约束网格搜索：α、β、γ 以 0.1 为步长且和为 1，逐点评估取最优；
 * - pairwise 排序准确率：对每个反馈三元组比较正/负样本融合得分，统计正确率；
 * - 版本管理：内存版本栈 + 磁盘 model_<版本>.json + active 指针文件。
 *
 * 依赖关系：依赖 core 公共层（json.h JSON 解析/序列化、types.h）、storage（M9 存取
 * 节点/邻边、余弦相似度）、time（M10 时间衰减因子）、vector（M13 查询向量化）、
 * rerank（M7 读取/设置融合权重）、feedback（M11 FeedbackTriple）。被 engine（M12）装配调用。
 */
#include "ame/learning/learning_engine.h"

#include "ame/core/json.h"
#include "ame/core/log.h"
#include "ame/rerank/reranker.h"
#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "ame/vector/vector_service.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ame {

// 构造函数：注入存储、向量、重排三个服务引用并记录配置目录，无初始化逻辑
LearningEngine::LearningEngine(Storage& storage, VectorService& vec, Reranker& rerank,
                               const std::string& config_dir)
    : storage_(storage), vec_(vec), rerank_(rerank), dir_(config_dir) {}

// 拼接指定版本对应的模型配置文件路径（dir_/model_<version>.json）
std::string LearningEngine::cfg_path(const std::string& version) const {
  return dir_ + "/model_" + version + ".json";
}

// 伪代码：
// 步骤1：初始化 pairwise 计数器 correct/total。
// 步骤2：遍历每个反馈三元组（query、正样本、负样本）：
//   步骤2.1：从存储取正/负样本节点，任一不存在则跳过该三元组；
//   步骤2.2：对查询文本做向量化得到 qemb；
//   步骤2.3：分别对正、负样本计算三个离线可重算特征：
//     - ctx：节点 embedding 与 qemb 的余弦相似度（无 embedding 取 0，负值截断为 0）；
//     - tm：90 天半衰时间衰减因子 × 节点 importance；
//     - g：节点度归一（邻居数/10，封顶 1.0）；
//   步骤2.4：按候选权重 c 计算双方融合得分 s = α·g + β·ctx + γ·tm。
// 步骤3：若正样本得分 > 负样本得分则 correct+1；total+1。
// 步骤4：返回 correct/total 作为该配置的 pairwise 准确率（total 为 0 时返回 0）。
double LearningEngine::eval_cfg(const ModelCfg& c,
                                const std::vector<FeedbackTriple>& triples) const {
  // 代理评估：用可离线重算的特征（语境相似度、时间衰减×importance、度归一）
  // 近似 recall 打分，统计 pairwise 排序正确率
  int correct = 0, total = 0;
  for (auto& t : triples) {
    const Node* pos = storage_.get_node(t.pos_uid);
    const Node* neg = storage_.get_node(t.neg_uid);
    if (!pos || !neg) continue;
    Vec qemb = vec_.embed(t.query);
    auto feat = [&](const Node* n, double& g, double& ctx, double& tm) {
      ctx = n->embedding.empty() ? 0 : std::max(0.0, Storage::cosine(n->embedding, qemb));
      tm = TimeService::decay_factor(n->ts, 90.0) * n->importance;
      g = std::min(1.0, (double)storage_.neighbors(n->uid).size() / 10.0);
    };
    double g1, c1, t1, g2, c2, t2;
    feat(pos, g1, c1, t1);
    feat(neg, g2, c2, t2);
    double s1 = c.alpha * g1 + c.beta * c1 + c.gamma * t1;
    double s2 = c.alpha * g2 + c.beta * c2 + c.gamma * t2;
    if (s1 > s2) ++correct;
    ++total;
  }
  return total ? (double)correct / total : 0.0;
}

// 伪代码：
// 步骤1（赫布边权调整）：遍历每个反馈三元组：
//   - 对正样本节点的每条邻边：清除反向标记属性，权重 ×1.05（封顶 10.0）后写回；
//   - 对负样本节点的每条邻边：同样清除反向标记，权重 ×0.95（不低于 0.1）后写回。
// 步骤2（α/β/γ 网格搜索）：在 α、β、γ 均以 0.1 为步长且 α+β+γ=1 的网格上
//   逐点调用 eval_cfg 评估 pairwise 准确率，保留最优配置（初值 0.5/0.3/0.2，
//   初始最优分 -1 保证至少被第一个网格点更新）。
// 步骤3（版本落盘）：生成版本号 "v<序号>"，将版本号、最优 α/β/γ 及准确率
//   写成 JSON 配置文件；版本号压入内存版本栈（供 rollback 使用）并返回。
std::string LearningEngine::train_offline(const std::vector<FeedbackTriple>& triples) {
  // ---- 边权赫布调整：正样本关联加强、负样本减弱 ----
  for (auto& t : triples) {
    for (auto& [e, nb] : storage_.neighbors(t.pos_uid)) {
      Edge u = e;
      u.attrs.erase("_reversed");
      u.weight = std::min(10.0, e.weight * 1.05);  // 关联越用越强
      storage_.upsert_edge(u, false);
    }
    for (auto& [e, nb] : storage_.neighbors(t.neg_uid)) {
      Edge u = e;
      u.attrs.erase("_reversed");
      u.weight = std::max(0.1, e.weight * 0.95);   // 长期不用衰减
      storage_.upsert_edge(u, false);
    }
  }

  // ---- α/β/γ 网格搜索（步长 0.1，α+β+γ=1）----
  ModelCfg best{0.5, 0.3, 0.2};
  double best_acc = -1;
  for (int ai = 1; ai <= 8; ++ai) {
    for (int bi = 1; bi <= 9 - ai; ++bi) {
      int gi = 10 - ai - bi;
      if (gi < 1) continue;
      ModelCfg c{ai / 10.0, bi / 10.0, gi / 10.0};
      double acc = eval_cfg(c, triples);
      if (acc > best_acc) { best_acc = acc; best = c; }
    }
  }

  // ---- 保存模型版本（权重配置文件 JSON）----
  std::string version = "v" + std::to_string(versions_.size() + 1);
  Json j = Json::object();
  j["version"] = Json::string(version);
  j["alpha"] = Json::number(best.alpha);
  j["beta"] = Json::number(best.beta);
  j["gamma"] = Json::number(best.gamma);
  j["pairwise_acc"] = Json::number(best_acc);
  std::ofstream f(cfg_path(version), std::ios::trunc);
  if (f) f << j.dump();
  versions_.push_back(version);
  return version;
}

// 伪代码：
// 步骤1：读取 rerank 当前线上权重，用样本三元组评估得到 acc_current。
// 步骤2：读取候选版本配置文件；若文件不存在，置 diff_report 为"模型版本不存在"并返回。
// 步骤3：解析 JSON；若解析失败，置 diff_report 为"配置解析失败"并返回。
// 步骤4：取候选 α/β/γ（缺省 0.5/0.3/0.2），同样本评估得到 acc_candidate。
// 步骤5：diff_report 写入"current=… candidate=…"对比串，返回报告。
ShadowReport LearningEngine::shadow_eval(const std::string& model_version,
                                         const std::vector<FeedbackTriple>& sample) {
  ShadowReport r;
  auto cur = rerank_.weights();
  r.acc_current = eval_cfg({cur.alpha, cur.beta, cur.gamma}, sample);
  std::ifstream f(cfg_path(model_version));
  if (!f) { r.diff_report = "模型版本不存在"; return r; }
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json j = Json::parse(ss.str(), &ok);
  if (!ok) { r.diff_report = "配置解析失败"; return r; }
  ModelCfg c{j.find("alpha")->as_num(0.5), j.find("beta")->as_num(0.3),
             j.find("gamma")->as_num(0.2)};
  r.acc_candidate = eval_cfg(c, sample);
  r.diff_report = "current=" + std::to_string(r.acc_current) +
                  " candidate=" + std::to_string(r.acc_candidate);
  return r;
}

// 将候选配置写入 rerank 融合权重，使其立即生效；恒返回 Err::Ok
Err LearningEngine::apply(const ModelCfg& c) {
  rerank_.set_weights({c.alpha, c.beta, c.gamma});
  return Err::Ok;
}

// 伪代码：
// 步骤1：打开指定版本的配置文件，不存在返回 Err::NotFound。
// 步骤2：解析 JSON，失败返回 Err::ParseError。
// 步骤3：取出 α/β/γ（缺省 0.5/0.3/0.2）并调用 apply 应用到 rerank；
//   若 apply 失败则透传错误码。
// 步骤4：将版本号写入 dir_/active 指针文件，标记当前线上版本，返回 Err::Ok。
Err LearningEngine::promote(const std::string& model_version) {
  std::ifstream f(cfg_path(model_version));
  if (!f) return Err::NotFound;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json j = Json::parse(ss.str(), &ok);
  if (!ok) return Err::ParseError;
  Err r = apply({j.find("alpha")->as_num(0.5), j.find("beta")->as_num(0.3),
                 j.find("gamma")->as_num(0.2)});
  if (r != Err::Ok) return r;
  std::ofstream af(dir_ + "/active", std::ios::trunc);
  if (af) af << model_version;
  return Err::Ok;
}

// 伪代码：
// 步骤1：若版本栈不足两个版本（无可回滚的历史），将 rerank 权重恢复为
//   规则版兜底值 0.5/0.3/0.2，返回 Err::Ok。
// 步骤2：弹出栈顶当前版本，对新的栈顶版本调用 promote 重新上线并返回其结果。
Err LearningEngine::rollback() {
  if (versions_.size() < 2) {
    rerank_.set_weights({0.5, 0.3, 0.2});  // 规则版兜底
    return Err::Ok;
  }
  versions_.pop_back();
  return promote(versions_.back());
}

// train_gate 伪代码（引导式扩散阶段 1 · 监督边门控训练）：
//   步骤1：样本量 < kMinTriples → 跳过训练保留规则版（如实返回空串）；
//   步骤2：构造样本——正样本节点（judge 选中）的关联边 y=1，负样本节点的关联边 y=0
//         （设计文档"正样本答案路径上的边得高传导"的节点级近似：扩散路径已在 M6
//          记录，端到端路径样本在阶段 2 接入 RL 时替换为同构接口）；
//   步骤3：SGD 训练（BCE + l2，epochs 轮全量扫过），打印首尾损失；
//   步骤4：版本化落盘 {"version", "gate": GateModel JSON}，返回版本号。
std::string LearningEngine::train_gate(const std::vector<FeedbackTriple>& triples) {
  if ((int)triples.size() < kMinTriples) return "";  // 数据不足，保留规则版
  // 构造 (特征, y) 样本对
  std::vector<std::pair<std::vector<float>, double>> samples;
  for (auto& t : triples) {
    const Node* pos = storage_.get_node(t.pos_uid);
    const Node* neg = storage_.get_node(t.neg_uid);
    if (!pos || !neg) continue;
    for (auto& [e, nb] : storage_.neighbors(pos->uid))
      samples.emplace_back(gate_.features(e, *pos, *nb, 1), 1.0);
    for (auto& [e, nb] : storage_.neighbors(neg->uid))
      samples.emplace_back(gate_.features(e, *neg, *nb, 1), 0.0);
  }
  if (samples.empty()) return "";
  double first = 0, last = 0;
  const int epochs = 30;
  for (int ep = 0; ep < epochs; ++ep) {
    double loss = 0;
    for (auto& [x, y] : samples) loss += gate_.train_step(x, y, 0.05, 1e-4);
    if (ep == 0) first = loss / samples.size();
    last = loss / samples.size();
  }
  AME_LOGI("train_gate: triples=%zu samples=%zu loss %.4f -> %.4f", triples.size(),
           samples.size(), first, last);

  // 版本化落盘（与 α/β/γ 模型同一目录/命名约定）
  std::string version = "gate_v" + std::to_string(versions_.size() + 1);
  Json j = Json::object();
  j["version"] = Json::string(version);
  j["loss_first"] = Json::number(first);
  j["loss_last"] = Json::number(last);
  j["gate"] = gate_.to_json();
  std::ofstream f(cfg_path(version), std::ios::trunc);
  if (f) f << j.dump();
  versions_.push_back(version);
  return version;
}

}  // namespace ame
