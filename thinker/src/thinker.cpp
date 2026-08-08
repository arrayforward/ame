/*
 * 模块：M8 思考编排器（Thinker）
 * 定位：把"软（种子生成）→硬（图扩散）→软（重排打分）"的单轮检索管道编排为多轮
 *       循环，实现深度思考式多跳联想检索，输出带推理链的最终答案。
 *
 * 本文件主体思路：
 *   实现 Thinker::think() 的多轮循环：每轮以当前焦点调用 M5 生成种子，并把上轮
 *   高分发现回灌为附加种子（焦点回灌）；调用 M6 做图扩散、M7 按目标向量重排；
 *   统计本轮总能量增量并生成反思文本；随后按终止条件（无发现 / 能量增量枯竭 /
 *   高置信 / 焦点漂移 / 轮数上限）决定是否继续；循环结束后取全局最高分发现作为
 *   最终答案。
 *
 * 关键算法/数据结构：
 *   - 多轮焦点迁移：下轮 focus 取本轮最高分发现的文本，形成链式多跳联想。
 *   - 能量增量监控：对每轮扩散结果的能量求和，增量低于阈值（0.01）判定枯竭。
 *   - 焦点漂移校验：用余弦相似度（Storage::cosine）比较当前焦点向量与目标向量
 *     （goal_emb，首轮由查询生成且跨轮不变），相似度低于 0.2 判定漂移超限。
 *   - 本身无复杂数据结构，复杂逻辑由 M5/M6/M7 完成，本模块只做编排。
 *
 * 模块依赖：
 *   - core（types.h）：公共类型。
 *   - storage（M9）：Storage::cosine 余弦相似度工具。
 *   - vector（M13）：文本向量化 embed()。
 *   - seed（M5）/ diffusion（M6）/ rerank（M7）：每轮"种子→扩散→重排"原语。
 *   - 本模块被 engine（M12 API 网关）依赖，作为深度思考查询的入口。
 */
#include "ame/thinker/thinker.h"

#include "ame/core/json.h"
#include "ame/diffusion/diffusion_engine.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"

#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace ame {

// 构造函数：保存 M9/M13/M5/M6/M7 各服务的引用，无初始化逻辑。
Thinker::Thinker(Storage& storage, VectorService& vec, SeedGenerator& seed,
                 DiffusionEngine& diff, Reranker& rerank)
    : storage_(storage), vec_(vec), seed_(seed), diff_(diff), rerank_(rerank) {}

/*
 * 多轮思考编排：对 query 做深度联想检索，返回完整推理链。
 * 伪代码：
 *   步骤1  初始化：用 M13 将查询向量化得到目标向量 goal_emb（跨轮不变），
 *          初始焦点 focus = query，前轮总能量 prev_total_energy = 0。
 *   步骤2  逐轮循环（round = 1 .. max_rounds）：
 *          2.1 记录本轮焦点；调用 M5 由焦点文本生成种子集合。
 *          2.2 焦点回灌：若非首轮，把上一轮所有高分发现以权重 0.8、来源
 *              "prev_round" 追加为本轮附加种子。
 *          2.3 把种子转为 DiffusionSeed，调用 M6 扩散（固定 2 跳，预算 budget），
 *              再调用 M7 以 goal_emb 重排，取前 5 作为本轮发现。
 *          2.4 能量统计：累加本轮扩散结果的全部能量，得 total_energy，
 *              与上一轮之差记为 increment，并更新 prev_total_energy。
 *          2.5 反思与焦点迁移：
 *              若本轮有发现：用其最高分更新全局置信度（取历史最大）、生成反思
 *              文本、记录其路径，并把 focus 迁移为最高分发现的文本；
 *              否则：反思记为"本轮无发现"。
 *          2.6 终止判定（按序检查，命中即 break）：
 *              - 本轮无任何发现 → "能量增量枯竭"；
 *              - 第 2 轮起能量增量 < 0.01 → "能量增量枯竭"；
 *              - 全局置信度 > 0.9 → "结论高置信"；
 *              - 第 2 轮起 focus 向量与 goal_emb 余弦相似度 < 0.2 → "焦点漂移超限"。
 *   步骤3  若循环自然结束（未触发任何终止条件），停止原因为"轮数上限"。
 *   步骤4  汇总答案：遍历全部轮次的所有发现，取得分最高者，其事件文本即最终
 *          答案；无任何发现则答案留空。返回推理链。
 */
ReasoningChain Thinker::think(const std::string& query, int max_rounds, int budget) {
  ReasoningChain chain;
  const Vec goal_emb = vec_.embed(query);  // ThinkState.goal_emb，跨轮不变
  std::string focus = query;
  double prev_total_energy = 0;

  for (int round = 1; round <= max_rounds; ++round) {
    ThinkRound tr;
    tr.focus = focus;

    // 每轮 M5 → M6 → M7（原语无状态）
    SeedQuery sq;
    sq.text = focus;
    tr.seeds = seed_.gen_seeds(sq);
    // 上轮高分发现 → 下轮种子（焦点迁移）
    if (!chain.rounds.empty()) {
      for (auto& prev : chain.rounds.back().findings)
        if (prev.event) tr.seeds.push_back({prev.event->uid, 0.8, "prev_round"});
    }

    std::vector<DiffusionSeed> ds;
    for (auto& s : tr.seeds) ds.push_back({s.node_uid, s.weight});
    DiffusionResult dr = diff_.diffuse(ds, 2, nullptr, nullptr, budget);
    tr.findings = rerank_.rerank(dr, goal_emb, {}, 5);

    // 能量增量统计（能量跨轮累加并衰减的体现）
    double total_energy = 0;
    for (auto& [uid, ne] : dr) total_energy += ne.energy;
    double increment = total_energy - prev_total_energy;
    prev_total_energy = total_energy;

    if (!tr.findings.empty()) {
      chain.confidence = std::max(chain.confidence, tr.findings.front().score);
      tr.reflection = "本轮聚焦「" + focus + "」，最高分发现：" +
                      tr.findings.front().event->text;
      chain.paths.push_back(tr.findings.front().path);
      // 焦点迁移：以上轮最高分事件文本作为下轮 focus
      focus = tr.findings.front().event->text;
    } else {
      tr.reflection = "本轮无发现";
    }
    chain.rounds.push_back(std::move(tr));

    // 终止四条件（轮数上限由 for 保证）
    if (round >= 1 && chain.rounds.back().findings.empty()) {
      chain.stop_reason = "能量增量枯竭";
      break;
    }
    if (increment < 0.01 && round > 1) {
      chain.stop_reason = "能量增量枯竭";
      break;
    }
    if (chain.confidence > 0.9) {
      chain.stop_reason = "结论高置信";
      break;
    }
    // 焦点漂移校验：focus 与 goal_emb 距离超限则停止
    double drift = Storage::cosine(vec_.embed(focus), goal_emb);
    if (drift < 0.2 && round > 1) {
      chain.stop_reason = "焦点漂移超限";
      break;
    }
  }
  if (chain.stop_reason.empty()) chain.stop_reason = "轮数上限";

  // 汇总答案：最高分发现的文本
  const ScoredEvent* best = nullptr;
  for (auto& r : chain.rounds)
    for (auto& f : r.findings)
      if (!best || f.score > best->score) best = &f;
  if (best && best->event) chain.answer = best->event->text;
  return chain;
}

// think_ircot 伪代码（IRCoT 式 LLM 引导迭代检索）：
//   步骤1：首轮用完整检索链路（M5→M6→M7）对原问题取证，装入证据池（uid 去重）；
//   步骤2：每轮把 问题 + 证据池（编号+日期+文本）+ 子查询历史 交给 LLM，
//          宽容解析 JSON：{"reasoning":..., "next_query":"..."} 或 {"done":true}；
//   步骤3：done/解析失败/无 next_query → 终止；否则 next_query 记入子查询历史，
//          走检索链路取新证据并入池；无新证据 → 终止；
//   步骤4：轮数上限兜底；输出 ReasoningChain（answer=最高分发现文本，
//          rounds 记各轮子查询与 reasoning，subqueries 为子查询历史）。
ReasoningChain Thinker::think_ircot(
    const std::string& query, int max_rounds,
    const std::function<std::string(const std::string&)>& llm_fn, int budget) {
  ReasoningChain chain;
  // 检索一轮（与原语复用）：text → seeds → diffuse → rerank top5
  auto retrieve = [&](const std::string& text) {
    SeedQuery sq;
    sq.text = text;
    SeedSet seeds = seed_.gen_seeds(sq);
    std::vector<DiffusionSeed> ds;
    for (auto& s : seeds) ds.push_back({s.node_uid, s.weight});
    DiffusionResult dr = diff_.diffuse(ds, 2, nullptr, nullptr, budget);
    return rerank_.rerank(dr, vec_.embed(text), {}, 5);
  };

  std::unordered_map<std::string, const Node*> pool;  // uid -> 事件
  auto first = retrieve(query);
  for (auto& f : first)
    if (f.event) pool[f.event->uid] = f.event;
  ThinkRound r0;
  r0.focus = query;
  r0.findings = first;
  r0.reflection = "初始检索";
  chain.rounds.push_back(r0);

  for (int round = 1; round <= max_rounds; ++round) {
    // 组 prompt：问题 + 证据池（编号+日期） + 子查询历史
    std::string p = "问题: " + query + "\n已收集证据:\n";
    int idx = 1;
    for (auto& [uid, n] : pool) {
      char dbuf[24];
      std::time_t t = (std::time_t)n->ts;
      std::tm tm{};
      gmtime_r(&t, &tm);
      std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", tm.tm_year + 1900,
                    tm.tm_mon + 1, tm.tm_mday);
      p += "E" + std::to_string(idx++) + " [" + dbuf + "] " + n->text + "\n";
    }
    if (!chain.subqueries.empty()) {
      p += "已尝试的子查询:\n";
      for (auto& s : chain.subqueries) p += "- " + s + "\n";
    }
    p += "判断证据是否足以回答问题。输出 JSON：不足则 "
         "{\"reasoning\":\"缺什么\",\"next_query\":\"下一个检索查询（短句）\"}；"
         "足够则 {\"done\":true}。只输出 JSON。";
    std::string resp = llm_fn(p);
    size_t a = resp.find('{'), b = resp.rfind('}');
    if (a == std::string::npos || b <= a) {
      chain.stop_reason = "LLM 输出解析失败";
      break;
    }
    bool ok = false;
    Json j = Json::parse(resp.substr(a, b - a + 1), &ok);
    if (!ok) {
      chain.stop_reason = "LLM 输出解析失败";
      break;
    }
    const Json* done = j.find("done");
    if (done && done->as_bool()) {
      chain.stop_reason = "LLM 判定证据充分（done）";
      break;
    }
    const Json* nq = j.find("next_query");
    std::string next_q = nq ? nq->as_str() : "";
    std::string reasoning = (j.find("reasoning") && j.find("reasoning")->kind == Json::Str)
                                ? j.find("reasoning")->as_str()
                                : "";
    if (next_q.empty()) {
      chain.stop_reason = "无后续子查询";
      break;
    }
    chain.subqueries.push_back(next_q);
    auto found = retrieve(next_q);
    ThinkRound tr;
    tr.focus = next_q;
    tr.findings = found;
    tr.reflection = reasoning.empty() ? "（无 reasoning）" : reasoning;
    int fresh = 0;
    for (auto& f : found)
      if (f.event && !pool.count(f.event->uid)) {
        pool[f.event->uid] = f.event;
        ++fresh;
      }
    chain.rounds.push_back(std::move(tr));
    if (fresh == 0) {
      chain.stop_reason = "无新证据";
      break;
    }
  }
  if (chain.stop_reason.empty()) chain.stop_reason = "轮数上限";

  // 汇总：证据池中时间最新且得分最高的发现作为答案线索
  const ScoredEvent* best = nullptr;
  for (auto& r : chain.rounds)
    for (auto& f : r.findings)
      if (!best || f.score > best->score) best = &f;
  if (best && best->event) {
    chain.answer = best->event->text;
    chain.confidence = best->score;
    chain.paths.push_back(best->path);
  }
  return chain;
}

}  // namespace ame
