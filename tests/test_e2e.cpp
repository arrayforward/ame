/*
 * 端到端测试（e2e）：完整链路
 * remember（重名消歧/因果链/空间 NEAR）→ lookup → recall（多跳 explain，优于纯向量对照）
 * → think → 反馈→训练→promote → 快照 save/load 后 recall 结果一致
 */
#include <filesystem>

#include "ame/engine/engine.h"
#include "test_util.hpp"

using namespace ame;

// 写入样例数据并返回引擎（含因果知识边）
static void seed_data(Engine& e) {
  CHECK(e.remember("老张和老王昨天在朝阳公园跑步，压力很大").code == Err::Ok);
  CHECK(e.remember("老张今天在公司加班到深夜，压力大").code == Err::Ok);
  CHECK(e.remember("老张最近总是睡不着，整夜失眠").code == Err::Ok);
  CHECK(e.remember("老王今天在咖啡馆谈项目").code == Err::Ok);
  CHECK(e.remember("老张的车在望京爆胎了，去维修").code == Err::Ok);
  // 因果链冷启动灌库
  CHECK(e.keywords().link_related("压力", "失眠", RelType::REL_CAUSE, 1.0) == Err::Ok);
  CHECK(e.keywords().link_related("加班", "压力", RelType::REL_CAUSE, 1.0) == Err::Ok);
}

static const Node* find_event_by_text(Engine& e, const std::string& key) {
  for (auto* n : e.storage().nodes_of_kind(NodeKind::Event))
    if (n->text.find(key) != std::string::npos) return n;
  return nullptr;
}

int main() {
  std::string dir = "/tmp/ame_test_e2e";
  std::filesystem::remove_all(dir);
  Engine e(dir);

  // ---------- 1. remember：重名消歧 + 因果链 + NEAR ----------
  seed_data(e);
  // 重名消歧：另一个老张（手机号强标识）与投票解析的老张并存
  std::string lz_phone = e.register_entity("person", "老张", {{"phone", "13811112222"}});
  const Node* lz_vote = find_event_by_text(e, "跑步") ? nullptr : nullptr;
  (void)lz_vote;
  int lz_count = 0;
  for (auto* n : e.storage().nodes_of_kind(NodeKind::Entity))
    if (n->name == "老张") ++lz_count;
  CHECK(lz_count == 2);  // 两个「老张」实体并存
  // 手机号强标识反查精确命中第二个
  CHECK(e.entities().find_entity({{"phone", "13811112222"}}) == lz_phone);
  // NEAR 边自生长：朝阳公园 ↔ 咖啡馆
  const Node* ppark = e.storage().find_by_name(NodeKind::Place, "朝阳公园");
  const Node* pcafe = e.storage().find_by_name(NodeKind::Place, "咖啡馆");
  CHECK(ppark && pcafe);
  CHECK(e.storage().find_edge(ppark->uid, pcafe->uid, RelType::NEAR) != nullptr);

  // ---------- 2. lookup 精确检索 ----------
  const Node* lz = nullptr;
  for (auto* n : e.storage().nodes_of_kind(NodeKind::Entity))
    if (n->name == "老张" && !e.storage().neighbors(n->uid, {RelType::INVOLVES}).empty())
      lz = n;
  CHECK(lz != nullptr);
  LookupQuery lq;
  lq.entity_uids = {lz->uid};
  CHECK(e.lookup(lq).size() == 4);
  LookupQuery lq2;
  lq2.keywords = {"睡不着"};  // 归一 → 失眠
  CHECK(e.lookup(lq2).size() == 1);

  // ---------- 3. recall：多跳联想 + explain，优于纯向量对照 ----------
  const Node* ev_insomnia = find_event_by_text(e, "失眠");
  CHECK(ev_insomnia != nullptr);
  SeedQuery sq;
  sq.text = "压力";
  auto rr = e.recall(sq, {}, 2, 20, true);
  CHECK(rr.code == Err::Ok);
  bool found = false;
  for (auto& se : rr.events) {
    if (se.event->uid == ev_insomnia->uid) {
      found = true;
      CHECK(se.graph_score > 0);          // 图扩散分驱动（多跳：压力→失眠→事件）
      CHECK(se.context_score < 0.3);      // 语境相似度低（文本不相干）
      CHECK(!se.explain.empty());
    }
  }
  CHECK(found);
  // 对照：纯向量检索 top10 找不到语义不相干的失眠事件
  Vec q = e.vectors().embed("压力");
  auto ann = e.storage().ann_search(q, 10);
  for (auto& [uid, sim] : ann) CHECK(uid != ev_insomnia->uid);

  // ---------- 4. think 深度思考 ----------
  auto chain = e.think("老张为什么睡不着", 3);
  CHECK(!chain.rounds.empty());
  CHECK(!chain.answer.empty());
  CHECK(!chain.stop_reason.empty());

  // ---------- 5. 反馈 → 训练 → promote ----------
  CHECK(rr.query_id > 0);
  CHECK(e.feedback(rr.query_id, SignalType::Select, ev_insomnia->uid) == Err::Ok);
  if (!rr.events.empty() && rr.events[0].event->uid != ev_insomnia->uid)
    e.feedback(rr.query_id, SignalType::Ignore, rr.events[0].event->uid);
  std::string ver = e.train();  // 内部 promote
  CHECK(!ver.empty());
  CHECK(std::filesystem::exists(dir + "/model_" + ver + ".json"));

  // ---------- 6. 快照 save/load 后 recall 结果一致 ----------
  std::string snap = "/tmp/ame_test_e2e_snapshot.json";
  CHECK(e.save(snap) == Err::Ok);
  auto before = e.recall(sq, {}, 2, 20, true);
  Engine e2;
  CHECK(e2.load(snap) == Err::Ok);
  CHECK(e2.storage().node_count() == e.storage().node_count());
  CHECK(e2.storage().edge_count() == e.storage().edge_count());
  // 融合权重属运行时配置（不在图快照内），对齐后再比较
  e2.reranker().set_weights(e.reranker().weights());
  auto after = e2.recall(sq, {}, 2, 20, true);
  CHECK(after.code == Err::Ok);
  CHECK(after.events.size() == before.events.size());
  for (size_t i = 0; i < before.events.size(); ++i) {
    CHECK(after.events[i].event->uid == before.events[i].event->uid);
    CHECK_NEAR(after.events[i].score, before.events[i].score, 1e-4);
  }

  return AME_TEST_REPORT();
}
