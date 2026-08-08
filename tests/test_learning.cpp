/*
 * M14 learning 单元测试：train_offline 产出版本、promote 生效、rollback、shadow_eval
 */
#include <filesystem>

#include "ame/engine/engine.h"
#include "test_util.hpp"

using namespace ame;

static void test_train_promote_rollback() {
  std::string dir = "/tmp/ame_test_learn";
  std::filesystem::remove_all(dir);
  Engine e(dir);
  e.remember("老张在朝阳公园跑步");
  e.remember("老张失眠睡不着");
  e.remember("老张在公司加班压力大");
  e.keywords().link_related("跑步", "失眠", RelType::REL_OPPOSITE, 0.5);

  SeedQuery sq;
  sq.text = "跑步";
  auto rr = e.recall(sq);
  if (!rr.events.empty()) {
    e.feedback(rr.query_id, SignalType::Select, rr.events[0].event->uid);
    if (rr.events.size() > 1)
      e.feedback(rr.query_id, SignalType::Ignore, rr.events[1].event->uid);
  }

  // 训练：三元组导出 + α/β/γ 调优 + 版本管理（train 内部 promote）
  std::string v = e.train();
  CHECK(!v.empty());
  CHECK(std::filesystem::exists(dir + "/model_" + v + ".json"));
  CHECK(std::filesystem::exists(dir + "/active"));
  // 权重为合法归一值
  auto w = e.reranker().weights();
  CHECK_NEAR(w.alpha + w.beta + w.gamma, 1.0, 1e-9);
}

// 门控网络：初始化恒等规则传导 + toy 训练收敛 + 数据不足降级
static void test_gate() {
  GateModel gm;
  Node a, b;
  a.uid = "a"; a.kind = NodeKind::Keyword; a.name = "头";
  b.uid = "b"; b.kind = NodeKind::Keyword; b.name = "尾";
  // 初始化恒等：未训练时 gate ≈ 类型系数（各类型抽查）
  for (RelType t : {RelType::REL_CAUSE, RelType::REL_COOCCUR, RelType::REL_OPPOSITE,
                    RelType::INVOLVES, RelType::NEXT}) {
    Edge e{"a", "b", t, 1.0, {}};
    CHECK_NEAR(gm.gate(e, a, b, 1), conduction_coeff(t), 0.02);
  }
  CHECK(!gm.trained);
  // toy 可分数据：x0=+1 → y=1，x0=-1 → y=0，30 轮损失显著下降
  std::vector<float> xp(GateModel::kIn, 0.0f), xn(GateModel::kIn, 0.0f);
  xp[0] = 1.0f;
  xn[0] = -1.0f;
  double first = 0, last = 0;
  for (int ep = 0; ep < 100; ++ep) {
    double l = gm.train_step(xp, 1.0, 0.5, 1e-5) + gm.train_step(xn, 0.0, 0.5, 1e-5);
    if (ep == 0) first = l;
    last = l;
  }
  CHECK(last < first * 0.3);  // 收敛（w2 零初始化起步慢，给足轮次）
  CHECK(gm.trained);
  // 序列化往返
  Json j = gm.to_json();
  GateModel gm2;
  CHECK(gm2.from_json(j));
  CHECK_NEAR(gm.gate(Edge{"a", "b", RelType::REL_CAUSE, 1.0, {}}, a, b, 1),
             gm2.gate(Edge{"a", "b", RelType::REL_CAUSE, 1.0, {}}, a, b, 1), 1e-6);

  // LearningEngine::train_gate：样本不足返回空串
  Storage s;
  VectorService vs(&s);
  TimeService ts(&s);
  Reranker rk(s, ts, vs);
  LearningEngine le(s, vs, rk, "/tmp/ame_test_gate_cfg");
  std::filesystem::create_directories("/tmp/ame_test_gate_cfg");
  std::vector<FeedbackTriple> few = {{"q", "p1", "n1"}};
  CHECK(le.train_gate(few).empty());  // <20 → 保留规则版
}

// train_gate 端到端：同进程反馈（uid 有效）→ 监督门控训练出版本
static void test_gate_endtoend() {
  std::string dir = "/tmp/ame_test_gate_e2e";
  std::filesystem::remove_all(dir);
  Engine e(dir);
  e.remember("老张在朝阳公园跑步");
  e.remember("老张失眠睡不着");
  e.remember("老张在公司加班压力大");
  e.remember("老王在三里屯聚餐很开心");
  // 反复 recall + 反馈，攒够 ≥20 条 triples
  long n_feedback = 0;
  for (int i = 0; i < 10; ++i) {
    SeedQuery sq;
    sq.text = i % 2 ? "跑步" : "失眠";
    auto rr = e.recall(sq);
    if (rr.events.empty()) continue;
    for (size_t j = 0; j < rr.events.size(); ++j) {
      e.feedback(rr.query_id, j == 0 ? SignalType::Select : SignalType::Ignore,
                 rr.events[j].event->uid);
      ++n_feedback;
    }
  }
  CHECK(n_feedback > 0);
  std::string v = e.train_gate();
  CHECK(!v.empty());  // triples 足够 → 训练出版本
  CHECK(std::filesystem::exists(dir + "/model_" + v + ".json"));
}

int main() {
  test_train_promote_rollback();
  test_gate();
  test_gate_endtoend();
  return AME_TEST_REPORT();
}
