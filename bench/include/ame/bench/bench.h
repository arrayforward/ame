/*
 * 所属模块：bench 基准适配层（无 M 编号，工程附加层）——让 M12 引擎适配并能离线运行
 *           LoCoMo / LongMemEval / BEAM 三个长期记忆基准。
 * 本文件主体思路：定义三个基准的统一中间表示 BenchCase/BenchQuestion/BenchConfig，
 *   声明三个宽容的数据加载器（字段缺失容忍，用 core 极简 JSON 解析）、
 *   摄入器 ingest（会话→引擎记忆）、问答器 answer（离线基线：recall 取证+组合答案+弃答门控）。
 * 关键算法/数据结构：无复杂算法；统一中间表示为 POD 结构体；
 *   会话日期解析支持多种常见格式，解析失败按会话顺序合成递增时间。
 * 依赖关系：依赖 core(types.h) 与 M12 engine；被 bench 的 CLI 与 tests/test_bench 依赖。
 */
#pragma once
#include "ame/core/types.h"

#include <string>
#include <vector>

namespace ame {

class Engine;
class IExtractor;
class ILlmClient;

// ---- 统一中间表示 ----
struct BenchTurn {
  std::string speaker;   // LoCoMo speaker / LME/BEAM role
  std::string text;
  std::string dia_id;    // LoCoMo 证据引用（如 "D3:4"），可无
};

struct BenchSession {
  std::string date;               // 原始日期字符串（解析在摄入时做）
  std::string id;                 // 会话标识（LongMemEval haystack_session_ids），可无
  std::vector<BenchTurn> turns;
};

struct BenchQuestion {
  std::string id;
  int category = 0;                        // LoCoMo 不透明类别整数（口径可配置）
  std::string qtype;                       // LongMemEval question_type / BEAM ability
  std::string question;
  std::string answer;
  std::vector<std::string> evidence_dia_ids;      // LoCoMo evidence（dia_id 引用）
  std::vector<std::string> answer_session_ids;    // LongMemEval 证据会话
  std::vector<std::string> nuggets;               // BEAM 原子评分点
  std::string question_date;
  bool unanswerable = false;  // 对抗/弃答题标准答案标记
};

struct BenchCase {
  std::string conversation_id;
  std::vector<BenchSession> sessions;
  std::vector<BenchQuestion> questions;
};

// ---- 运行配置（阈值/映射均为配置项，不写死在引擎里）----
struct BenchConfig {
  int k = 10;                       // recall@k / 取证上限
  int k_pool = 60;                  // 候选池：多通道并集后先取 k_pool 进 M7 融合排序，
                                    // 再截 top-k（权重学习才能影响 top-k 成员）
  double abstain_threshold = 0.15;  // 弃答门控：证据最高分或语境相似度低于此值 → 弃答
  double soft_min_sim = 0.15;       // 软种子相似度阈值（英文哈希向量偏稀疏，需低于默认 0.5）
  std::vector<int> abstention_categories = {5};  // LoCoMo 视为对抗题的类别（可配置）
  bool filter_superseded = true;    // 取证时过滤已被取代的过时事实（知识更新）
  bool use_entity_profile = true;   // 实体档案通道：两阶段检索/槽位直查（--entity-profile off 关闭）
  bool use_think = true;            // think 通道：时序/多跳题升级 M8 多轮推理取证（--think off 关闭）
  int think_rounds = 2;             // think 轮数（控制耗时）
  bool conditional_normalize = false;  // 条件归一：查询词无直接命中才词典扩展（--conditional-normalize on）
  int beam_width = 0;                  // 束搜索宽度（0=BFS；--beam N 启用引导式扩散阶段 1）
  std::string think_mode = "energy";   // energy（M8 能量扩散）| ircot（LLM 引导迭代检索）
  std::string answer_mode_agg = "";    // "aggregate" 时聚合类问题走两阶段（逐项提取+聚合）
  int agg_prefilter_n = 20;            // 聚合题本地预过滤保留名额（BM25-lite 排序截断）
  int agg_batch = 5;                   // 批量提取：每次喂入的证据条数

  // ---- L2 终排混合（语义 graph 激活工程）----
  // 开启后 answer() 证据终排从纯 BM25-lite 内容分改为混合：
  //   final = α·BM25内容分 + β·minmax_norm(1.x 融合分)
  // 仅在显式传 --final-alpha/--final-beta 时开启（默认关闭=历史纯 BM25 行为，
  // 保证 L1（词激活）与 L1+L2（混合终排）可逐级 A/B 不互相污染）。
  bool final_mix = false;              // --final-alpha/--final-beta 任一传入即开启
  double final_alpha = 0.6;            // BM25 内容分权重（默认 0.6）
  double final_beta = 0.4;             // 1.x 融合分权重（默认 0.4）
  // ---- 第六轮终局架构：统一 graded 边表（--word-edges 开启，默认空=历史行为）----
  // 非空时 M5 词法通道唯一逻辑：命中分=max(1.0, 最佳边分)（词形铸边 0.9/0.95、
  // 蒸馏边 0.3-1.0、反义弱边 0.35），m 按 ≥0.5 计；旁路 expand_of 两档。
  const class WordEdgeTable* word_edges = nullptr;

  // ---- 句级语义 spike：LLM 重排通道（--llm-rerank N，默认 0=关=历史行为）----
  // 标准管道证据终排（内容词 BM25，含 think 并入）截 top-k 之前：取终排序 top-N
  // 证据（带 dia_id 与首 200 字）做一次 LLM 调用，按其返回的编号序把前 k 个置顶
  // 进 evidence，其余按 BM25 原序补齐；调用失败/无有效编号 → 优雅降级为 BM25
  // 原序（stderr 标注）。LLM 调用走 DeepSeekClient 磁盘缓存（prompt 含题 id 与
  // 证据集内容，缓存键天然防污染），复跑零成本。仅标准管道生效；实体档案通道
  // 提前返回，不经过此处。
  int llm_rerank = 0;                       // 重排候选名额（BM25 终排序 top-N）
  ILlmClient* llm_client = nullptr;         // main 注入；空指针 = 通道关闭

  // ---- LLM 管线配置（任务二）----
  std::string extractor_mode = "rule";    // rule|llm（llm 按会话批量提取）
  std::string answer_mode = "baseline";   // baseline|llm
  std::string judge_mode = "f1";          // f1|llm
  int concurrency = 8;                    // LLM 批量并发
  std::string model = "deepseek-v4-flash";
  std::string cache_dir = "bench/cache";  // LLM 响应磁盘缓存
  std::string config_path = "bench/config/deepseek.local.json";
  std::string out_json;                   // 非空则输出 JSON 报告到该路径
};

// ---- 数据加载器（宽容：字段缺失用默认值，结构异常跳过该条）----
std::vector<BenchCase> load_locomo(const std::string& path);
std::vector<BenchCase> load_longmemeval(const std::string& path);
std::vector<BenchCase> load_beam(const std::string& path);

// 会话日期解析：支持 "2023-01-10 [15:00]" / "2023/01/10" / "1/15/2023" 等格式，
// 解析失败返回 fallback（摄入侧用递增合成时间兜底）
TimePoint parse_session_date(const std::string& s, TimePoint fallback);

// ---- 摄入器：会话 → 引擎记忆（speaker 跨轮复用同一实体 uid；逐轮 remember_turn）----
struct IngestStats {
  int events = 0;
  int entities = 0;
  int keywords = 0;
};
IngestStats ingest(Engine& engine, const BenchCase& c);
// llm 提取器版本：非空 extractor 时按会话批量提取后落库，失败自动回退规则提取
IngestStats ingest(Engine& engine, const BenchCase& c, IExtractor* extractor);

// ---- 词典灌库：lexicon.json → M3 别名表 + RELATED 边（实现见 lexicon_apply.cpp）----
Err apply_lexicon(Engine& engine, const std::string& path, int* applied = nullptr);

// ---- 姓名别名表灌库：names_zh.json → M3 别名表 + M1 提取词面 + M2 实体/Alias
// （人名/专名归一域，严格等价；须在摄入前调用，实现见 lexicon_apply.cpp）----
Err apply_name_aliases(Engine& engine, const std::string& path, int* applied = nullptr);

// ---- 问答器（离线基线）：recall 取证 → 组合答案 → 低置信弃答 ----
struct AnswerResult {
  std::string answer_text;
  bool abstained = false;
  std::vector<std::string> evidence_uids;
  std::vector<std::string> evidence_dia_ids;
  double score = 0;    // 证据最高分（弃答门控依据）
  long query_id = 0;   // M11 埋点 id（引擎反馈采集启用时非 0）
};
AnswerResult answer(Engine& engine, const BenchQuestion& q, const BenchConfig& cfg);

// LLM 重排响应解析（容错，导出供单测）：从模型输出提取 E<num> 编号序——
// 按出现顺序、去重、限 1..n_cands、截前 k 个；无任何有效编号（含 NONE/空响应）
// 返回空 vector = 降级为 BM25 原序的信号。编号外文本（解释、markdown 等）不影响。
std::vector<size_t> parse_llm_rerank_ids(const std::string& resp, size_t n_cands, size_t k);

}  // namespace ame
