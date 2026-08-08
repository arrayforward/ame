/*
 * 所属模块：M14 学习引擎 —— GateModel 实现（边门控网络，监督版）。
 * 本文件主体思路：实现特征抽取、sigmoid(logit(系数)+MLP) 前向、BCE 单步 SGD 反向
 *   与 JSON 序列化。初始化 w2_=b2_=0、w1_ 小随机 → MLP 输出≈0 → 输出恒等于类型系数
 *   （未训练行为不回归，有单测断言）。
 * 关键算法/数据结构：57→32(relu)→1 MLP；反向传播手写（交叉熵对 logits 的导数）。
 * 依赖关系：依赖 core(types.h/json.h)；被 learning_engine / diffuse_beam 使用。
 */
#include "ame/learning/gate_model.h"

#include <cmath>
#include <random>

namespace ame {

// 构造函数伪代码：w1_ 小随机（std 0.01，固定种子保证确定性）、b1_/w2_/b2_ 置零——
//   MLP(x)≈0，输出恒等于类型系数。
GateModel::GateModel() {
  std::mt19937 rng(42);
  std::normal_distribution<float> nd(0.0f, 0.01f);
  w1_.resize((size_t)kHid * kIn);
  for (auto& w : w1_) w = nd(rng);
  b1_.assign(kHid, 0.0f);
  w2_.assign(kHid, 0.0f);
  b2_ = 0.0f;
}

// emb 压缩：128 维按 8 维一段取均值 → 16 维（确定性分段均值；
//   注释：可换学习投影/PCA，接口不变）。空向量返回全零。
static std::vector<float> compress_emb(const Vec& v) {
  std::vector<float> out(GateModel::kEmbC, 0.0f);
  if (v.empty()) return out;
  const int seg = GateModel::kEmbIn / GateModel::kEmbC;  // 8
  for (int i = 0; i < GateModel::kEmbC; ++i) {
    float s = 0;
    for (int j = 0; j < seg; ++j) s += v[(size_t)i * seg + j];
    out[i] = s / seg;
  }
  return out;
}

// features 伪代码：
//   [emb(头)压缩 16 | emb(尾)压缩 16 | onehot(边类型) 19 | 上下文 6]
//   上下文 = [log1p(weight), 类型系数, log(1+头出度), log(1+尾出度), hop/3, 反向标记]
//   （头尾出度由 Node.attrs 无记录时退化：调用方无法取度数则用 embedding 维度常数兜底——
//     本实现从 gate() 调用点传入的 Node 估计，见下）。
std::vector<float> GateModel::features(const Edge& e, const Node& head, const Node& tail,
                                       int hop) const {
  std::vector<float> x;
  x.reserve(kIn);
  auto ch = compress_emb(head.embedding);
  auto ct = compress_emb(tail.embedding);
  x.insert(x.end(), ch.begin(), ch.end());
  x.insert(x.end(), ct.begin(), ct.end());
  for (int i = 0; i < kRel; ++i) x.push_back(i == (int)e.type ? 1.0f : 0.0f);
  x.push_back((float)std::log1p(e.weight));
  x.push_back((float)conduction_coeff(e.type));
  // 出度无法从 Node 直接拿到（邻接表在 Storage），这里用 mention_count/status 的
  // 组合特征近似占位（真实度数由训练器在图侧补充，见 learning_engine 的用法说明）
  x.push_back((float)std::log1p(head.mention_count));
  x.push_back((float)std::log1p(tail.mention_count));
  x.push_back((float)hop / 3.0f);
  x.push_back(e.attrs.count("_reversed") ? 1.0f : 0.0f);
  return x;
}

// gate 伪代码：z = logit(类型系数) + MLP(features)，返回 sigmoid(z)。
double GateModel::gate(const Edge& e, const Node& head, const Node& tail, int hop) const {
  auto x = features(e, head, tail, hop);
  double z = logit(conduction_coeff(e.type)) + b2_;
  for (int h = 0; h < kHid; ++h) {
    float s = b1_[h];
    for (int i = 0; i < kIn; ++i) s += w1_[(size_t)h * kIn + i] * x[i];
    z += w2_[h] * (s > 0 ? s : 0.0f);  // relu
  }
  return sigmoid(z);
}

// train_step 伪代码（单样本 BCE + SGD）：
//   步骤1：前向缓存 h=relu(w1x+b1) 与 p=sigmoid(z)；
//   步骤2：dL/dz = p - y；w2/b2 反传；dz→dh（relu 掩码）→dw1/db1；
//   步骤3：SGD 更新（附 l2 权重衰减），返回 BCE 损失。
double GateModel::train_step(const std::vector<float>& x, double y, double lr, double l2) {
  std::vector<float> h(kHid);
  double z = b2_;
  for (int i = 0; i < kHid; ++i) {
    float s = b1_[i];
    for (int j = 0; j < kIn; ++j) s += w1_[(size_t)i * kIn + j] * x[j];
    h[i] = s > 0 ? s : 0.0f;
    z += w2_[i] * h[i];
  }
  double p = sigmoid(z);
  double loss = -(y * std::log(p + 1e-12) + (1 - y) * std::log(1 - p + 1e-12));
  double dz = p - y;
  for (int i = 0; i < kHid; ++i) {
    float dw2 = dz * h[i] + (float)l2 * w2_[i];
    float dh = dz * w2_[i] * (h[i] > 0 ? 1.0f : 0.0f);
    w2_[i] -= (float)lr * dw2;
    for (int j = 0; j < kIn; ++j)
      w1_[(size_t)i * kIn + j] -= (float)lr * (dh * x[j] + (float)l2 * w1_[(size_t)i * kIn + j]);
    b1_[i] -= (float)lr * dh;
  }
  b2_ -= (float)lr * dz;
  trained = true;
  return loss;
}

// to_json 伪代码：{"gate_version":1,"w1":flat数组,"b1":...,"w2":...,"b2":...}
Json GateModel::to_json() const {
  Json j = Json::object();
  j["gate_version"] = Json::number(1);
  auto put = [&](const char* k, const std::vector<float>& v) {
    Json a = Json::array();
    for (float f : v) a.push(Json::number(f));
    j[k] = std::move(a);
  };
  put("w1", w1_);
  put("b1", b1_);
  put("w2", w2_);
  j["b2"] = Json::number(b2_);
  return j;
}

// from_json 伪代码：读回四个参数数组，维度校验，loaded→trained=true。
bool GateModel::from_json(const Json& j) {
  auto get = [&](const char* k, std::vector<float>& v, size_t n) {
    const Json* a = j.find(k);
    if (!a || a->arr.size() != n) return false;
    v.resize(n);
    for (size_t i = 0; i < n; ++i) v[i] = (float)a->arr[i].as_num();
    return true;
  };
  if (!get("w1", w1_, (size_t)kHid * kIn)) return false;
  if (!get("b1", b1_, kHid)) return false;
  if (!get("w2", w2_, kHid)) return false;
  const Json* b2 = j.find("b2");
  b2_ = b2 ? (float)b2->as_num() : 0.0f;
  trained = true;
  return true;
}

}  // namespace ame
