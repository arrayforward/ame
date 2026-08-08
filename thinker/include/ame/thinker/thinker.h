/*
 * 模块：M8 思考编排器（Thinker）
 * 定位：把"软（种子生成）→硬（图扩散）→软（重排打分）"的单轮检索管道编排为多轮
 *       循环，实现深度思考式多跳联想检索，输出带推理链的最终答案。
 *
 * 本文件主体思路：
 *   声明 Thinker 类及其输入输出数据结构。Thinker 对外只暴露一个 think() 接口，
 *   内部持有 M5/M6/M7/M9/M13 各服务的引用，按轮驱动"种子→扩散→重排→反思→
 *   焦点迁移"的循环，直到满足终止条件。
 *
 * 关键算法/数据结构：
 *   - ThinkRound：单轮思考记录（焦点、种子、发现、反思文本）。
 *   - ReasoningChain：完整推理链（答案、各轮记录、命中路径、置信度、停止原因）。
 *   - 本头文件只含数据结构与接口声明，无复杂算法；多轮编排与终止判定逻辑见
 *     src/thinker.cpp。
 *
 * 模块依赖：
 *   - core（types.h）：SeedSet、ScoredEvent、Path 等公共类型。
 *   - seed（M5）、rerank（M7）：通过其头文件拿到 SeedGenerator、Reranker 类型。
 *   - storage（M9）、vector（M13）、diffusion（M6）：仅前置声明，实现处再包含。
 *   - 本模块被 engine（M12 API 网关）依赖，作为深度思考查询的入口。
 */
#pragma once
#include "ame/core/types.h"
#include "ame/rerank/reranker.h"
#include "ame/seed/seed_generator.h"

#include <functional>
#include <string>
#include <vector>

namespace ame {

class Storage;
class VectorService;
class SeedGenerator;
class DiffusionEngine;
class Reranker;

struct ThinkRound {
  std::string focus;                  // 本轮思考焦点（首轮为查询，之后为焦点迁移结果）
  SeedSet seeds;                      // 本轮扩散的种子集合（含上轮高分发现回灌）
  std::vector<ScoredEvent> findings;  // 本轮重排后的高分发现
  std::string reflection;             // 本轮反思文本（供人阅读的轮次小结）
};

struct ReasoningChain {
  std::string answer;                 // 汇总答案：全局最高分发现的文本
  std::vector<ThinkRound> rounds;     // 各轮思考记录
  std::vector<Path> paths;            // 各轮最高分发现的扩散路径
  double confidence = 0;              // 全程最高发现得分
  std::string stop_reason;            // 循环终止原因
  std::vector<std::string> subqueries;  // IRCoT 模式的子查询历史（ThinkState 扩展）
};

class Thinker {
 public:
  Thinker(Storage& storage, VectorService& vec, SeedGenerator& seed,
          DiffusionEngine& diff, Reranker& rerank);

  ReasoningChain think(const std::string& query, int max_rounds = 5, int budget = 1000);

  // IRCoT 式 LLM 引导迭代检索：每轮由 LLM 基于"问题+已积累证据+子查询历史"
  // 产出 {reasoning, next_query|done}，next_query 走完整检索链路（M5→M6→M7）
  // 取证并入；done/轮数上限/无新证据终止。llm_fn 为注入的 LLM 回调
  //（prompt→completion），避免 thinker 依赖具体 LLM 客户端。
  ReasoningChain think_ircot(const std::string& query, int max_rounds,
                             const std::function<std::string(const std::string&)>& llm_fn,
                             int budget = 1000);

 private:
  Storage& storage_;
  VectorService& vec_;
  SeedGenerator& seed_;
  DiffusionEngine& diff_;
  Reranker& rerank_;
};

}  // namespace ame
