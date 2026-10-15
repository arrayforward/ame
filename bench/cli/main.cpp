/*
 * 所属模块：bench 基准适配层——CLI 入口 ame_bench。
 * 本文件主体思路：解析参数 → 加载数据集 → 逐 case 独立 Engine 摄入（rule 或 llm 会话批提取）
 *   → 离线检索取证（recall）→ 答题（baseline 组合 或 LLM 批量）→ 判分（离线 F1/nugget
 *   或 LLM judge 批量）→ 分组报表（控制台 + 可选 JSON 落盘）。
 *   LLM 调用集中在两个批量阶段（answer/judge），经 DeepSeekClient 磁盘缓存断点续跑。
 * 关键算法/数据结构：无；两阶段批量（先收集全部请求再一次并发）摊薄 LLM 延迟。
 * 依赖关系：依赖 ame_bench、ame_llm、ame_engine。
 * 用法：ame_bench --dataset locomo|longmemeval|beam --data <path>
 *       [--extractor rule|llm] [--answer baseline|llm] [--judge f1|llm]
 *       [--limit N] [--k 10] [--concurrency 8] [--model deepseek-v4-flash]
 *       [--cache-dir bench/cache] [--config bench/config/deepseek.local.json] [--out report.json]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ame/bench/bench.h"
#include "ame/bench/metrics.h"
#include "ame/bench/store.h"
#include "ame/core/json.h"
#include "ame/engine/engine.h"
#include "ame/llm/components.h"
#include "ame/llm/llm.h"
#include "ame/mem/mem.h"

using namespace ame;

namespace {

// 命令行参数 → BenchConfig + dataset/data/limit
struct Cli {
  BenchConfig cfg;
  std::string dataset, data;
  std::string store = "graph";   // --store graph|lmem：存储后端（默认 graph=1.x Engine）
  int limit = 0;
  std::string dump_path;       // --dump：逐题结果 NDJSON 落盘路径
  std::string cand_dump_path;  // --cand-dump：lmem 候选池全量 NDJSON 落盘路径（P3a miss 诊断）
  std::string build_lexicon;   // --build-lexicon：词频表路径（构建词典后退出）
  std::string validate_lexicon;  // --validate-lexicon：蒸馏词库 strong 流可互换校验（M2）
  std::string score_sims;        // --score-sims：词级近似 1-10 打分（第五轮 M3）
  std::string lexicon_out = "bench/data/lexicon.json";
  std::string load_lexicon;    // --load-lexicon：词典灌库路径
  std::string question_ids;    // --question-ids：只评测这些 question_id（JSON 数组）
  int lexicon_topn = 2000;
  std::string save_graph;      // --save-graph PATH：摄入后整图（含档案）落 JSON（每 case 一个文件）
  std::string load_graph;      // --load-graph PATH：直接加载已摄入的图，跳过摄入/提取
  std::string feedback_dir;    // --feedback-dir PATH：M11 埋点+judge 信号回流 + M14 调优对比
  std::string fb_exp;          // --fb-exp DIR：P2b 双 pass 反馈实验（仅 --store lmem；
                               // pass1 答题判分→feedback 落盘→pass2 同题单只跑召回对比排序）
  std::string extractor_dict;  // --extractor-dict FILE：M1 提取器大词表（wordnet_words.txt），
                               // 摄入后、答题前经 load_keyword_dict_cached 加载（查询侧词激活）
  std::string word_sim_path;   // --word-edges FILE[,FILE2]：统一 graded 边表（第六轮，默认空=关）
  std::string name_alias;      // --name-alias FILE：人名/专名严等价别名表（names_zh.json，
                               // 摄入前加载；M3 别名+M1 词面+M2 实体三通道贯通，默认空=关）
  int llm_rerank = 0;          // --llm-rerank N：句级语义 LLM 重排名额（0=关，默认 20 实验值）
  bool llm_rerank_all = false; // --llm-rerank-all on|off：重排窗口扩为全池（默认关）
  double entity_pass = -1.0;   // --entity-pass-threshold X：实体通道低置信放行（<0=关=现状）
  double rerank_guard = -1.0;  // --rerank-guard X：BM25 强命中豁免 LLM 重排（<0=关=现状）
  std::string cheap_rerank;    // --cheap-rerank PATH：特征逻辑重排器模型 JSON（空=关）
  std::string rerank_mode;     // --rerank bm25|cheap|llm（空=未显式指定，由别名/默认推定）
  bool has_weights = false;    // --weights α,β,γ 覆盖融合权重
  double w_alpha = 0.5, w_beta = 0.3, w_gamma = 0.2;
  bool has_final = false;      // --final-alpha/--final-beta：L2 终排混合开关（默认关闭）
  int prox_window = 2;         // --prox-window N：lmem P3b 邻近加成窗口（0=禁用，默认 2）
  bool expand_on = true;       // --no-expand：lmem P3b 共现联想扩散关闭
};

// parse_args 伪代码：键值扫描，未知键忽略；--abstain-categories 为逗号分隔整数列表。
Cli parse_args(int argc, char** argv) {
  Cli cli;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    std::string a = argv[i];
    if (a == "--dataset") cli.dataset = next();
    else if (a == "--data") cli.data = next();
    else if (a == "--store") cli.store = next();
    else if (a == "--limit") cli.limit = std::atoi(next().c_str());
    else if (a == "--k") cli.cfg.k = std::atoi(next().c_str());
    else if (a == "--abstain-threshold") cli.cfg.abstain_threshold = std::atof(next().c_str());
    else if (a == "--extractor") cli.cfg.extractor_mode = next();
    else if (a == "--answer") cli.cfg.answer_mode = next();
    else if (a == "--judge") cli.cfg.judge_mode = next();
    else if (a == "--concurrency") cli.cfg.concurrency = std::atoi(next().c_str());
    else if (a == "--model") cli.cfg.model = next();
    else if (a == "--cache-dir") cli.cfg.cache_dir = next();
    else if (a == "--config") cli.cfg.config_path = next();
    else if (a == "--out") cli.cfg.out_json = next();
    else if (a == "--dump") cli.dump_path = next();
    else if (a == "--cand-dump") cli.cand_dump_path = next();
    else if (a == "--build-lexicon") cli.build_lexicon = next();
    else if (a == "--validate-lexicon") cli.validate_lexicon = next();
    else if (a == "--score-sims") cli.score_sims = next();
    else if (a == "--lexicon-out") cli.lexicon_out = next();
    else if (a == "--lexicon-topn") cli.lexicon_topn = std::atoi(next().c_str());
    else if (a == "--load-lexicon") cli.load_lexicon = next();
    else if (a == "--question-ids") cli.question_ids = next();
    else if (a == "--entity-profile") cli.cfg.use_entity_profile = next() != "off";
    else if (a == "--think") cli.cfg.use_think = next() != "off";
    else if (a == "--think-rounds") cli.cfg.think_rounds = std::atoi(next().c_str());
    else if (a == "--conditional-normalize")
      cli.cfg.conditional_normalize = next() != "off";
    else if (a == "--beam") cli.cfg.beam_width = std::atoi(next().c_str());
    else if (a == "--think-mode") cli.cfg.think_mode = next();
    else if (a == "--answer-mode") cli.cfg.answer_mode_agg = next();
    else if (a == "--agg-prefilter-n") cli.cfg.agg_prefilter_n = std::atoi(next().c_str());
    else if (a == "--agg-batch") cli.cfg.agg_batch = std::atoi(next().c_str());
    else if (a == "--save-graph") cli.save_graph = next();
    else if (a == "--load-graph") cli.load_graph = next();
    else if (a == "--feedback-dir") cli.feedback_dir = next();
    else if (a == "--fb-exp") cli.fb_exp = next();
    else if (a == "--extractor-dict") cli.extractor_dict = next();
    else if (a == "--word-edges") cli.word_sim_path = next();
    else if (a == "--name-alias") cli.name_alias = next();
    else if (a == "--llm-rerank") cli.llm_rerank = std::atoi(next().c_str());
    else if (a == "--llm-rerank-all") cli.llm_rerank_all = true;  // 裸开关（默认关）
    else if (a == "--entity-pass-threshold") cli.entity_pass = std::atof(next().c_str());
    else if (a == "--rerank-guard") cli.rerank_guard = std::atof(next().c_str());
    else if (a == "--cheap-rerank") cli.cheap_rerank = next();
    else if (a == "--rerank") cli.rerank_mode = next();
    else if (a == "--final-alpha") {
      cli.cfg.final_alpha = std::atof(next().c_str());
      cli.has_final = cli.cfg.final_mix = true;
    }
    else if (a == "--final-beta") {
      cli.cfg.final_beta = std::atof(next().c_str());
      cli.has_final = cli.cfg.final_mix = true;
    }
    else if (a == "--prox-window") cli.prox_window = std::atoi(next().c_str());
    else if (a == "--no-expand") cli.expand_on = false;
    else if (a == "--weights") {  // α,β,γ 覆盖（M14 调优结果验证用）
      std::string v = next();
      std::sscanf(v.c_str(), "%lf,%lf,%lf", &cli.w_alpha, &cli.w_beta, &cli.w_gamma);
      cli.has_weights = true;
    }
    else if (a == "--abstain-categories") {
      cli.cfg.abstention_categories.clear();
      std::string v = next();
      size_t p = 0;
      while (p < v.size()) {
        size_t comma = v.find(',', p);
        cli.cfg.abstention_categories.push_back(std::atoi(v.substr(p, comma).c_str()));
        if (comma == std::string::npos) break;
        p = comma + 1;
      }
    }
  }
  return cli;
}

void usage() {
  std::fprintf(stderr,
               "用法: ame_bench --dataset locomo|longmemeval|beam --data <path>\n"
               "      [--store graph|lmem]（默认 graph=1.x Engine；lmem=2.0 分层存储）\n"
               "      [--extractor rule|llm] [--answer baseline|llm] [--judge f1|llm]\n"
               "      [--limit N] [--k 10] [--concurrency 8] [--model deepseek-v4-flash]\n"
               "      [--cache-dir bench/cache] [--config bench/config/deepseek.local.json]\n"
               "      [--out report.json] [--abstain-threshold 0.15] [--abstain-categories 5]\n"
               "      [--dump results.ndjson]（逐题结果落盘，失败分析用）\n"
               "      [--cand-dump cands.ndjson]（仅 lmem：候选池全量落盘，P3a miss 诊断用）\n"
               "      [--fb-exp DIR]（仅 lmem：P2b 双 pass 反馈实验，pass1 判分→feedback 落盘\n"
               "       →pass2 同题单只跑召回对比金标证据排序）\n"
               "      [--extractor-dict FILE]（graph：摄入后答题前加载 M1 大词表，查询侧词激活）\n"
               "      [--stem-en]（graph：M3 英文保守词干归一，查询/写入双侧，默认关）\n"
               "      [--word-sim FILE]（graph：词级近似表软匹配，graded sim，默认关）\n"
               "      [--name-alias FILE]（graph：人名/专名严等价别名表 names_zh.json，\n"
               "       摄入前灌入 M3 别名+M1 词面+M2 实体，默认关）\n"
               "      [--rerank bm25|cheap|llm]（graph：终排模式；默认 bm25=纯词面。\n"
               "       cheap=特征重排器零LLM需 --cheap-rerank PATH；llm=全池LLM打分）\n"
               "      [兼容别名·deprecated] --llm-rerank N / --llm-rerank-all / --cheap-rerank\n"
               "       PATH（映射到 --rerank；与 --rerank 不同档并用报互斥错）\n"
               "      [--llm-prompt-pin]（llm 档 prompt 变体：词面精确命中必须保留靠前）\n"
               "      [--entity-pass-threshold X]（graph：实体档案通道低置信放行——通道榜首\n"
               "       内容分 < X 时候选并入标准管道池；默认 -1=关=现状；建议实验值 0.5）\n"
               "      [--rerank-guard X]（graph：LLM 重排时 BM25 榜首内容分 ≥ X 则豁免、\n"
               "       保底 top-k 首槽；默认 -1=关；建议实验值 0.8）\n"
               "      [--cheap-rerank PATH]（graph：特征逻辑重排器替代 LLM 打分，零 LLM；\n"
               "       模型由 cheap_rerank_train.py 训练；默认空=关）\n"
               "      [--final-alpha 0.6 --final-beta 0.4]（graph：L2 终排混合 BM25+融合分）\n"
               "API key: bench/config/deepseek.local.json 或环境变量 AME_DEEPSEEK_API_KEY\n");
}

}  // namespace

int main(int argc, char** argv) {
  Cli cli = parse_args(argc, argv);
  cli.cfg.llm_rerank = cli.llm_rerank;  // --llm-rerank N → BenchConfig（0=关）
  cli.cfg.llm_rerank_all = cli.llm_rerank_all;      // --llm-rerank-all（默认关）
  cli.cfg.entity_pass_threshold = cli.entity_pass;  // --entity-pass-threshold（默认 -1=现状）
  cli.cfg.rerank_guard = cli.rerank_guard;          // --rerank-guard（默认 -1=关）
  cli.cfg.cheap_rerank_path = cli.cheap_rerank;     // --cheap-rerank（默认空=关）

  // ---- 终排模式消解（--rerank bm25|cheap|llm 一等公民；旧开关为兼容别名）----
  // 互斥：显式 --rerank 与别名开关（--llm-rerank N>0 / --llm-rerank-all / --cheap-rerank）
  // 请求不同档 → 报错退出（不猜优先级）；同档可叠加（参数合并，-all 优先窗口）。
  {
    std::unordered_set<std::string> modes;
    std::string req;
    auto add = [&](const std::string& m, const char* from) {
      modes.insert(m);
      req += std::string(req.empty() ? "" : ",") + from + "→" + m;
    };
    if (!cli.rerank_mode.empty()) add(cli.rerank_mode, "--rerank");
    if (cli.llm_rerank > 0) add("llm", "--llm-rerank");
    if (cli.llm_rerank_all) add("llm", "--llm-rerank-all");
    if (!cli.cheap_rerank.empty()) add("cheap", "--cheap-rerank");
    if (modes.size() > 1) {
      std::fprintf(stderr, "终排模式互斥冲突: %s（请只保留一个模式开关）\n", req.c_str());
      return 1;
    }
    std::string mode = modes.empty() ? "bm25" : *modes.begin();
    if (mode != "bm25" && mode != "cheap" && mode != "llm") {
      std::fprintf(stderr, "未知 --rerank 模式: %s（仅支持 bm25|cheap|llm）\n", mode.c_str());
      return 1;
    }
    if (mode == "llm" && !cli.llm_rerank_all && cli.llm_rerank <= 0)
      cli.llm_rerank_all = true;  // --rerank llm 未指定窗口 → 默认全池（机制2正形）
    cli.cfg.rerank_mode = mode;
  }

  // 词典构建模式：--build-lexicon <vocab_freq.json> [--lexicon-out PATH] [--lexicon-topn N]
  if (!cli.build_lexicon.empty()) {
    std::filesystem::create_directories(cli.cfg.cache_dir);
    auto client = DeepSeekClient::from_config(cli.cfg.config_path, cli.cfg.model,
                                              cli.cfg.cache_dir);
    if (!client) {
      std::fprintf(stderr, "未找到 API key\n");
      return 1;
    }
    std::vector<std::string> vocabs;
    {
      size_t p = 0;  // 逗号分隔多文件（合并词频）
      while (p < cli.build_lexicon.size()) {
        size_t comma = cli.build_lexicon.find(',', p);
        vocabs.push_back(cli.build_lexicon.substr(p, comma == std::string::npos
                                                       ? comma : comma - p));
        if (comma == std::string::npos) break;
        p = comma + 1;
      }
    }
    int added = build_lexicon_json(*client, vocabs, cli.lexicon_out,
                                   cli.lexicon_topn, 50, cli.cfg.concurrency);
    if (added < 0) {
      std::fprintf(stderr, "词典构建失败\n");
      return 1;
    }
    std::fprintf(stderr, "[lexicon] %s 新增 %d 条（api_calls=%d cache_hits=%d）\n",
                 cli.lexicon_out.c_str(), added, client->api_calls(), client->cache_hits());
    return 0;
  }

  // M2 strong 流可互换性校验：--validate-lexicon <in.json> [--lexicon-out PATH]
  // （复活工程第四轮；只保留 LLM 判 yes 的 strong 对，写新文件不动原始蒸馏产物）
  if (!cli.validate_lexicon.empty()) {
    std::filesystem::create_directories(cli.cfg.cache_dir);
    auto client = DeepSeekClient::from_config(cli.cfg.config_path, cli.cfg.model,
                                              cli.cfg.cache_dir);
    if (!client) {
      std::fprintf(stderr, "未找到 API key\n");
      return 1;
    }
    int pairs = validate_lexicon_json(*client, cli.validate_lexicon, cli.lexicon_out,
                                      50, cli.cfg.concurrency);
    if (pairs < 0) {
      std::fprintf(stderr, "校验失败（输入不可读或解析失败）\n");
      return 1;
    }
    std::fprintf(stderr, "[validate] %s -> %s（%d 对，api_calls=%d cache_hits=%d）\n",
                 cli.validate_lexicon.c_str(), cli.lexicon_out.c_str(), pairs,
                 client->api_calls(), client->cache_hits());
    return 0;
  }

  // M3 词级近似打分：--score-sims <in.json> [--lexicon-out PATH]
  if (!cli.score_sims.empty()) {
    std::filesystem::create_directories(cli.cfg.cache_dir);
    auto client = DeepSeekClient::from_config(cli.cfg.config_path, cli.cfg.model,
                                              cli.cfg.cache_dir);
    if (!client) {
      std::fprintf(stderr, "未找到 API key\n");
      return 1;
    }
    int pairs = score_word_sims(*client, cli.score_sims, cli.lexicon_out, 50,
                                cli.cfg.concurrency);
    if (pairs < 0) {
      std::fprintf(stderr, "打分失败（输入不可读或解析失败）\n");
      return 1;
    }
    std::fprintf(stderr, "[score-sims] %s -> %s（%d 对，api_calls=%d cache_hits=%d）\n",
                 cli.score_sims.c_str(), cli.lexicon_out.c_str(), pairs,
                 client->api_calls(), client->cache_hits());
    return 0;
  }

  if (cli.dataset.empty() || cli.data.empty()) {
    usage();
    return 1;
  }

  // 第六轮终局：--word-edges 统一 graded 边表（bench 级单例，各 case 共用只读指针）
  // FILE = lexicon_en_sim.json（蒸馏边）；可选 FILE2 = 蒸馏词库（antonyms 流 ×0.35 反义弱边）
  WordEdgeTable word_edge_table;
  if (!cli.word_sim_path.empty()) {
    size_t p2 = 0;
    bool ok = true;
    while (p2 <= cli.word_sim_path.size()) {
      size_t comma = cli.word_sim_path.find(',', p2);
      std::string path = cli.word_sim_path.substr(
          p2, comma == std::string::npos ? comma : comma - p2);
      bool good = (word_edge_table.edge_count() == 0 && word_edge_table.word_count() == 0)
                      ? word_edge_table.load(path)
                      : word_edge_table.load_antonyms(path);
      if (!good) {
        std::fprintf(stderr, "[warn] word-edges 加载失败（%s），按未加载继续\n",
                     path.c_str());
        ok = false;
      }
      if (comma == std::string::npos) break;
      p2 = comma + 1;
    }
    if (ok && word_edge_table.word_count() > 0) {
      std::fprintf(stderr, "[word-edges] 加载 %zu 词 %zu 边（%s）\n",
                   word_edge_table.word_count(), word_edge_table.edge_count(),
                   cli.word_sim_path.c_str());
      cli.cfg.word_edges = &word_edge_table;
    }
  }

  // 步骤1：加载数据集
  std::vector<BenchCase> cases;
  if (cli.dataset == "locomo") cases = load_locomo(cli.data);
  else if (cli.dataset == "longmemeval") cases = load_longmemeval(cli.data);
  else if (cli.dataset == "beam") cases = load_beam(cli.data);
  else {
    std::fprintf(stderr, "未知数据集: %s\n", cli.dataset.c_str());
    return 1;
  }
  if (cases.empty()) {
    std::fprintf(stderr, "未加载到数据（文件缺失或结构不符）: %s\n", cli.data.c_str());
    return 1;
  }
  if (cli.limit > 0 && (int)cases.size() > cli.limit) cases.resize(cli.limit);

  // --store 校验 + lmem 下 1.x 专属开关明确忽略（设计 §10.3 双后端纪律）
  if (cli.store != "graph" && cli.store != "lmem") {
    std::fprintf(stderr, "未知存储后端: %s（仅支持 graph|lmem）\n", cli.store.c_str());
    return 1;
  }
  if (cli.store == "lmem") {
    std::string ignored;
    auto mark = [&](const char* name, bool on) {
      if (on) { if (!ignored.empty()) ignored += ","; ignored += name; }
    };
    mark("save-graph", !cli.save_graph.empty());
    mark("load-graph", !cli.load_graph.empty());
    mark("load-lexicon", !cli.load_lexicon.empty());
    mark("extractor-dict", !cli.extractor_dict.empty());
    mark("word-sim", !cli.word_sim_path.empty());
    mark("name-alias", !cli.name_alias.empty());
    mark("llm-rerank", cli.llm_rerank > 0);
    mark("rerank", cli.cfg.rerank_mode != "bm25");
    mark("entity-pass-threshold", cli.entity_pass >= 0.0);
    mark("rerank-guard", cli.rerank_guard >= 0.0);
    mark("final-alpha/beta", cli.has_final);
    mark("feedback-dir", !cli.feedback_dir.empty());
    mark("beam", cli.cfg.beam_width > 0);
    mark("think-mode=ircot", cli.cfg.think_mode == "ircot");
    mark("answer-mode=aggregate", cli.cfg.answer_mode_agg == "aggregate");
    mark("weights", cli.has_weights);
    if (!ignored.empty())
      std::fprintf(stderr,
                   "[lmem] 忽略 1.x 专属开关: %s（lmem 取证走 Mem.recall 统一通道）\n",
                   ignored.c_str());
  }

  // --question-ids：只评测指定题目（固定小样本 A/B 用）
  std::unordered_set<std::string> only_qids;
  if (!cli.question_ids.empty()) {
    std::ifstream f(cli.question_ids);
    std::stringstream ss;
    ss << f.rdbuf();
    bool ok = false;
    Json j = Json::parse(ss.str(), &ok);
    if (ok && j.kind == Json::Arr)
      for (auto& v : j.arr) only_qids.insert(v.as_str());
  }

  // 步骤2：需要 LLM 时构造客户端（配置/env 缺失则报错退出，不静默降级）
  std::unique_ptr<DeepSeekClient> client;
  std::unique_ptr<LlmExtractor> llm_ext;
  std::unique_ptr<LlmAnswerer> llm_ans;
  std::unique_ptr<LlmJudge> llm_judge;
  RuleExtractor rule_ext;
  bool need_llm = cli.cfg.extractor_mode == "llm" || cli.cfg.answer_mode == "llm" ||
                  cli.cfg.judge_mode == "llm" || cli.cfg.rerank_mode == "llm";
  if (need_llm) {
    std::filesystem::create_directories(cli.cfg.cache_dir);
    client = DeepSeekClient::from_config(cli.cfg.config_path, cli.cfg.model, cli.cfg.cache_dir);
    if (!client) {
      std::fprintf(stderr, "未找到 API key（%s 或 AME_DEEPSEEK_API_KEY）\n",
                   cli.cfg.config_path.c_str());
      return 1;
    }
    cli.cfg.llm_client = client.get();  // 句级语义重排通道注入（--llm-rerank N）
    llm_ext = std::make_unique<LlmExtractor>(client.get(), &rule_ext);
    llm_ans = std::make_unique<LlmAnswerer>(client.get());
    llm_judge = std::make_unique<LlmJudge>(client.get());
  }

  // 步骤3：逐 case 摄入 + 离线取证；LLM 答题/判分请求先收集后批量发
  MetricAccumulator acc;
  struct EvRef {  // P2b fb-exp：一条证据的来源定位（ref_obs + kind + 通道）
    uint64_t ref_obs;
    std::string source_kind;
    std::string via;
  };
  struct Pending {
    const BenchQuestion* q;
    AnswerResult base;               // baseline 结果（含证据）
    std::vector<std::string> ev_texts;
    std::vector<EvRef> ev_refs;      // P2b fb-exp：逐证据来源（feedback 用，与 ev_texts 对齐）
    std::string conv;                // P2b fb-exp：所属 case（conversation_id）
    std::string group;
    std::string llm_answer;
    bool llm_done = false;
    size_t eng_idx = 0;              // 所属 Engine（M11 反馈回流出引用）
  };
  std::vector<Pending> pendings;
  std::vector<std::unique_ptr<Engine>> engines;  // 保活到判分阶段（反馈回流用）
  std::vector<std::unique_ptr<Mem>> lmem_keep;   // P2b fb-exp：lmem 保活到判分后 feedback
  std::unordered_map<std::string, Mem*> lmem_by_conv;
  // 聚合题候选证据（case 循环内召回，引擎存活期收集）
  struct AggJob {
    int qi;
    std::vector<std::string> evs;
  };
  std::vector<AggJob> agg_jobs;

  // P3a 诊断：lmem 候选池全量落盘（与正式取证同一 k_pool 口径）
  FILE* cand_df = nullptr;
  if (!cli.cand_dump_path.empty() && cli.store == "lmem")
    cand_df = std::fopen(cli.cand_dump_path.c_str(), "w");
  else if (!cli.cand_dump_path.empty())
    std::fprintf(stderr, "[warn] --cand-dump 仅 --store lmem 生效，已忽略\n");

  for (auto& c : cases) {
    // --question-ids 固定样本提速：该 case 无任何被请求的问题 → 整个跳过（含摄入）。
    // 安全性：每 case 独立引擎、跨 case 无共享状态，未命中 case 的摄入对结果零影响。
    if (!only_qids.empty()) {
      bool any = false;
      for (auto& q : c.questions)
        if (only_qids.count(q.id)) { any = true; break; }
      if (!any) continue;
    }
    // --store lmem：2.0 分层存储后端（M15–M19）。每 case 独立内存态 Mem，
    // owner=conversation_id；取证/证据回溯经 LmemStore（store.cpp），
    // LLM 答题/判分/报表与 graph 共用下游代码（同缓存同 judge 口径）。
    // P2b --fb-exp：Mem 落盘到 DIR/<conv>（观测+事实+可靠性分片），
    // 判分后 feedback 生效 → pass2 同目录重开（replay 恢复）只跑召回对比排序。
    if (cli.store == "lmem") {
      const std::string fb_root = cli.fb_exp.empty() ? "" : cli.fb_exp + "/" + c.conversation_id;
      auto mem_up = std::make_unique<Mem>(fb_root);
      Mem* mem_ptr = mem_up.get();
      // P3b 通道开关（--prox-window/--no-expand；P3a 诊断 P2 口径复现用）
      mem_ptr->view().set_proximity_window(cli.prox_window);
      mem_ptr->view().set_expand_enabled(cli.expand_on);
      LmemStore ls(mem_ptr, c.conversation_id);
      IngestStats st = ls.ingest_case(c);
      if (!cli.fb_exp.empty()) {
        lmem_keep.push_back(std::move(mem_up));
        lmem_by_conv[c.conversation_id] = mem_ptr;
      }
      std::fprintf(stderr, "[case %s] events=%d entities=%d keywords=%d questions=%zu\n",
                   c.conversation_id.c_str(), st.events, st.entities, st.keywords,
                   c.questions.size());
      for (auto& q : c.questions) {
        if (!only_qids.empty() && !only_qids.count(q.id)) continue;
        if (q.category > 0) {  // LoCoMo：对抗题类别判定与 graph 同口径
          q.unanswerable = false;
          for (int cat : cli.cfg.abstention_categories)
            if (q.category == cat) q.unanswerable = true;
        }
        Pending p;
        p.q = &q;
        p.group = q.qtype;
        p.eng_idx = 0;  // 无引擎反馈回流（feedback-dir 为 1.x 专属，已在上方忽略）
        p.conv = c.conversation_id;  // P2b fb-exp：pass2 按 case 重开 Mem 时对齐
        StoreAnswer sa = ls.answer_question(q, cli.cfg);
        p.base = std::move(sa.result);
        if (cand_df) {  // P3a：候选池全量（终排前，视图序 rank 从 1 计）
          Json j = Json::object();
          j["question_id"] = Json::string(q.id);
          j["category"] = Json::number(q.category);
          Json cands = Json::array();
          for (size_t ci = 0; ci < sa.candidates.size(); ++ci) {
            Json cj = Json::object();
            cj["rank"] = Json::number((double)ci + 1);
            cj["ref_obs"] = Json::number((double)sa.candidates[ci].ref_obs);
            cj["dia_id"] = Json::string(sa.candidates[ci].dia_id);
            cj["via"] = Json::string(sa.candidates[ci].via);
            cj["score"] = Json::number(sa.candidates[ci].score);
            cands.push(std::move(cj));
          }
          j["candidates"] = std::move(cands);
          std::string line = j.dump();
          std::fwrite(line.data(), 1, line.size(), cand_df);
          std::fputc('\n', cand_df);
        }
        for (auto& ev : sa.evidence) {
          p.ev_refs.push_back({ev.ref_obs, ev.source_kind, ev.via});  // P2b 反馈定位
          p.ev_texts.push_back(std::move(ev.text));
        }
        pendings.push_back(std::move(p));
      }
      continue;
    }
    // 跨 case 隔离；--feedback-dir 启用 M11/M14（每 case 独立目录）。
    // 注意：只有启用反馈时才保活 Engine（判分阶段回流用）；否则每 case 用完即销毁，
    // 避免百级 case 的全量图驻留内存（OOM 风险）。
    std::string fdir;
    if (!cli.feedback_dir.empty()) fdir = cli.feedback_dir + "/" + c.conversation_id;
    std::unique_ptr<Engine> eng_up;
    if (!cli.feedback_dir.empty()) {
      engines.push_back(std::make_unique<Engine>(fdir));
      eng_up.reset(engines.back().get());
    } else {
      eng_up = std::make_unique<Engine>(fdir);
    }
    Engine& e = *eng_up;
    if (cli.has_weights)
      e.reranker().set_weights({cli.w_alpha, cli.w_beta, cli.w_gamma});
    // --name-alias：人名/专名严等价别名表，须在摄入前加载（写入侧 M3 normalize、
    // M1 提取词面与 M2 实体别名均依赖它；load-graph 快照已含实体/别名时幂等跳过建档）。
    if (!cli.name_alias.empty()) {
      int applied = 0;
      if (apply_name_aliases(e, cli.name_alias, &applied) == Err::Ok)
        std::fprintf(stderr, "[name-alias] case %s 灌入 %d 条（%s）\n",
                     c.conversation_id.c_str(), applied, cli.name_alias.c_str());
      else
        std::fprintf(stderr, "[warn] name-alias 加载失败（%s），按未加载继续\n",
                     cli.name_alias.c_str());
    }
    // 引导式扩散阶段 1：beam 配置 + 反馈目录存在时先训监督门控
    if (cli.cfg.beam_width > 0) {
      e.set_diffusion_options(cli.cfg.beam_width, true);
      if (!fdir.empty()) e.train_gate();  // 门控：反馈 triples 驱动，不足则保留规则
    }
    IngestStats st;
    // --load-graph：整图（含档案槽位）已落盘则直接加载，跳过摄入与 LLM 提取（零调用）
    std::string gpath;
    if (!cli.load_graph.empty() || !cli.save_graph.empty())
      gpath = (cli.load_graph.empty() ? cli.save_graph : cli.load_graph) + "." +
              c.conversation_id + ".json";
    if (!gpath.empty() && std::filesystem::exists(gpath)) {
      if (e.load(gpath) == Err::Ok) {
        st.events = (int)e.storage().nodes_of_kind(NodeKind::Event).size();
        st.entities = (int)e.storage().nodes_of_kind(NodeKind::Entity).size();
        st.keywords = (int)e.storage().nodes_of_kind(NodeKind::Keyword).size();
        std::fprintf(stderr, "[graph] %s 加载 %s\n", c.conversation_id.c_str(),
                     gpath.c_str());
      }
    }
    if (st.events == 0) {  // 未命中图快照才摄入
      if (cli.cfg.extractor_mode == "llm" && llm_ext) {
      // 按会话批量提取（一次调用喂一个 session），逐轮 remember_turn_ex 落库
      st = IngestStats{};
      const TimePoint kBase = 1700000000;
      for (size_t si = 0; si < c.sessions.size(); ++si) {
        const BenchSession& bs = c.sessions[si];
        TimePoint base = parse_session_date(bs.date, kBase + (TimePoint)si * 86400);
        std::vector<std::pair<std::string, std::string>> turns;
        for (auto& t : bs.turns) turns.emplace_back(t.speaker, t.text);
        auto exts = llm_ext->extract_session(turns);
        for (size_t ti = 0; ti < bs.turns.size(); ++ti) {
          TimePoint ts = base + (TimePoint)ti * 60;
          auto r = e.remember_turn_ex(std::move(exts[ti]), bs.turns[ti].speaker, ts,
                                      bs.turns[ti].dia_id, bs.id);
          if (r.code == Err::Ok) ++st.events;
        }
      }
      st.entities = (int)e.storage().nodes_of_kind(NodeKind::Entity).size();
      st.keywords = (int)e.storage().nodes_of_kind(NodeKind::Keyword).size();
      } else {
        st = ingest(e, c);
      }
      // --save-graph：摄入完成后整图（含档案槽位）落盘，后续评测零摄入零提取
      if (!cli.save_graph.empty()) {
        std::string sp = cli.save_graph + "." + c.conversation_id + ".json";
        if (e.save(sp) == Err::Ok)
          std::fprintf(stderr, "[graph] %s 保存 %s\n", c.conversation_id.c_str(),
                       sp.c_str());
      }
    }
    std::fprintf(stderr, "[case %s] events=%d entities=%d keywords=%d questions=%zu\n",
                 c.conversation_id.c_str(), st.events, st.entities, st.keywords,
                 c.questions.size());

    // --load-lexicon：摄入完成后灌词典（别名归一/对立边/相似边）；
    // 逗号分隔多文件按序拼接（同词条 synonyms/antonyms/related 取并集，幂等）
    if (!cli.load_lexicon.empty()) {
      size_t p = 0;
      while (p < cli.load_lexicon.size()) {
        size_t comma = cli.load_lexicon.find(',', p);
        std::string path = cli.load_lexicon.substr(
            p, comma == std::string::npos ? comma : comma - p);
        int applied = 0;
        if (apply_lexicon(e, path, &applied) == Err::Ok)
          std::fprintf(stderr, "[lexicon] case %s 灌入 %d 条（%s）\n",
                       c.conversation_id.c_str(), applied, path.c_str());
        if (comma == std::string::npos) break;
        p = comma + 1;
      }
    }

    // --extractor-dict：L1 查询侧词激活——摄入完成后、答题前把大词表加载进 M1 提取器
    // （AC 缓存 <file>.ac 同目录；缓存命中直接加载，首 case 构建后覆写缓存）。
    // 加载后 gen_seeds 的自由文本通道（extractor_->extract(query)）与 CJK 词法通道
    // 才能从大词表产出关键词/中文词，经 M3 normalize 命中图上关键词节点 → 语义 graph 入口。
    if (!cli.extractor_dict.empty()) {
      int n = e.extractor().load_keyword_dict_cached(cli.extractor_dict, "seg", 700000,
                                                     cli.extractor_dict + ".ac");
      if (n < 0)
        std::fprintf(stderr, "[warn] extractor-dict 加载失败（%s），按未加载继续\n",
                     cli.extractor_dict.c_str());
      else
        std::fprintf(stderr, "[extractor-dict] case %s 加载 %d 条（%s）\n",
                     c.conversation_id.c_str(), n, cli.extractor_dict.c_str());
    }

    for (auto& q : c.questions) {
      if (!only_qids.empty() && !only_qids.count(q.id)) continue;  // 固定样本过滤
      if (q.category > 0) {  // LoCoMo：对抗题类别由配置判定
        q.unanswerable = false;
        for (int cat : cli.cfg.abstention_categories)
          if (q.category == cat) q.unanswerable = true;
      }
      Pending p;
      p.q = &q;
      p.group = q.qtype;
      p.eng_idx = engines.size() - 1;
      p.base = answer(e, q, cli.cfg);  // 离线取证（baseline 答案也由此产出）
      // 聚合题：引擎存活期内召回候选并本地预过滤（BM25-lite 排序截断，零 LLM）
      if (cli.cfg.answer_mode_agg == "aggregate" &&
          LlmAnswerer::is_aggregate_question(q.question)) {
        SeedQuery sq;
        sq.text = q.question;
        sq.soft_min_sim = cli.cfg.soft_min_sim;
        sq.lex_topk = cli.cfg.k_pool / 2;
        auto cands = e.recall(sq, {}, 2, cli.cfg.k_pool, false);
        int total = (int)cands.events.size();
        int kept = std::min(cli.cfg.agg_prefilter_n, total);
        AggJob job{(int)(pendings.size()), {}};
        for (int j = 0; j < kept; ++j) {
          const Node* n = cands.events[j].event;
          char dbuf[32] = "";
          std::time_t t = (std::time_t)n->ts;
          std::tm tm{};
          gmtime_r(&t, &tm);
          std::snprintf(dbuf, sizeof(dbuf), "[%04d-%02d-%02d] ", tm.tm_year + 1900,
                        tm.tm_mon + 1, tm.tm_mday);
          job.evs.push_back(dbuf + n->text);
        }
        if (total > kept)
          std::fprintf(stderr, "  [prefilter] %s: 候选 %d → 保留 %d（砍 %d）\n",
                       q.id.c_str(), total, kept, total - kept);
        if (kept > 0) agg_jobs.push_back(std::move(job));
      }
      // IRCoT 模式：LLM 引导迭代检索补证据（多轮子查询，去重并入证据前部）
      if (cli.cfg.think_mode == "ircot" && client) {
        auto llm_fn = [&](const std::string& prompt) {
          return client->chat({{"user", prompt}}, {0.0, 1024, false});
        };
        auto chain = e.think_ircot(q.question, cli.cfg.think_rounds + 2, llm_fn);
        std::unordered_set<std::string> have(p.base.evidence_uids.begin(),
                                             p.base.evidence_uids.end());
        for (auto& round : chain.rounds)
          for (auto& f : round.findings)
            if (f.event && !have.count(f.event->uid)) {
              have.insert(f.event->uid);
              p.base.evidence_uids.insert(p.base.evidence_uids.begin(), f.event->uid);
            }
        if (!chain.subqueries.empty())
          std::fprintf(stderr, "  [ircot] %s → %zu 轮子查询\n", q.id.c_str(),
                       chain.subqueries.size());
      }
      for (auto& uid : p.base.evidence_uids) {
        const Node* n = e.storage().get_node(uid);
        if (n) {
          // 可读日期前缀（epoch 数字 LLM 无法解读，时序题需要真实日期）
          char tsbuf[40] = "";
          std::time_t t = (std::time_t)n->ts;
          std::tm tm{};
          gmtime_r(&t, &tm);
          std::snprintf(tsbuf, sizeof(tsbuf), "[%04d-%02d-%02d] ", tm.tm_year + 1900,
                        tm.tm_mon + 1, tm.tm_mday);
          std::string ev_text = tsbuf + n->text;
          // 证据打包增强：沿 NEXT 链带前/后一轮上下文（缓解片段信息不足）
          std::string prev_text, next_text;
          for (auto& [ed, nb] : e.storage().neighbors(uid, {RelType::NEXT})) {
            if (nb->kind != NodeKind::Event) continue;
            if (nb->ts < n->ts && prev_text.empty()) prev_text = nb->text;
            if (nb->ts > n->ts && next_text.empty()) next_text = nb->text;
          }
          if (!prev_text.empty()) ev_text += "（前一轮: " + prev_text + "）";
          if (!next_text.empty()) ev_text += "（后一轮: " + next_text + "）";
          p.ev_texts.push_back(ev_text);
        }
      }
      pendings.push_back(std::move(p));
    }
  }

  // 步骤4：LLM 批量答题
  if (cli.cfg.answer_mode == "llm" && llm_ans) {
    // 两阶段聚合作答（--answer-mode aggregate）：候选池 k_pool → 本地预过滤（BM25-lite
    // 排序截断，零 LLM）→ 批量提取（agg_batch 条/次）→ 聚合作答
    // 注意：候选召回在 case 循环内完成（引擎存活期），此处只做请求组装与执行。
    if (cli.cfg.answer_mode_agg == "aggregate") {
      std::vector<AggJob> jobs = std::move(agg_jobs);
      // 批量提取（跨题合并批量）
      std::vector<ChatRequest> ex_reqs;
      std::vector<std::pair<int, int>> ex_map;  // (job 下标, 批内序号)
      for (size_t ji = 0; ji < jobs.size(); ++ji) {
        auto& evs = jobs[ji].evs;
        for (size_t b = 0; b < evs.size(); b += (size_t)cli.cfg.agg_batch) {
          size_t e = std::min(b + (size_t)cli.cfg.agg_batch, evs.size());
          std::vector<std::string> part(evs.begin() + b, evs.begin() + e);
          ex_reqs.push_back(LlmAnswerer::build_extract_batch_request(
              pendings[jobs[ji].qi].q->question, part));
          ex_map.emplace_back((int)ji, (int)b);
        }
      }
      if (!jobs.empty())
        std::fprintf(stderr, "[llm] 两阶段聚合 %zu 题：预过滤+批量提取 %zu 次...\n",
                     jobs.size(), ex_reqs.size());
      auto ex_resps = client->chat_batch(ex_reqs, cli.cfg.concurrency);
      // 阶段二：按题聚合事实列表
      std::vector<ChatRequest> ag_reqs;
      std::vector<int> ag_map;
      for (size_t ji = 0; ji < jobs.size(); ++ji) {
        std::vector<std::string> facts;
        for (size_t k = 0; k < ex_map.size(); ++k) {
          if (ex_map[k].first != (int)ji) continue;
          for (auto& [ei, fact] : LlmAnswerer::parse_extract_batch(
                   ex_resps[k], cli.cfg.agg_batch))
            facts.push_back("[E" + std::to_string(ex_map[k].second + ei + 1) + "] " + fact);
        }
        int ti = jobs[ji].qi;
        if (facts.empty()) {
          pendings[ti].llm_answer = "NO INFORMATION";  // 空列表直接弃答
          pendings[ti].llm_done = true;
          continue;
        }
        ag_reqs.push_back(llm_ans->build_aggregate_request(
            pendings[ti].q->question, pendings[ti].q->question_date, facts));
        ag_map.push_back(ti);
        std::fprintf(stderr, "  [agg] %s: %zu 条事实（%zu 次提取）\n",
                     pendings[ti].q->id.c_str(), facts.size(),
                     (jobs[ji].evs.size() + cli.cfg.agg_batch - 1) / cli.cfg.agg_batch);
      }
      if (!ag_reqs.empty()) {
        auto ag_resps = client->chat_batch(ag_reqs, cli.cfg.concurrency);
        for (size_t k = 0; k < ag_map.size(); ++k) {
          if (ag_resps[k].empty()) continue;
          pendings[ag_map[k]].llm_answer = ag_resps[k];
          pendings[ag_map[k]].llm_done = true;
        }
      }
    }
    std::vector<ChatRequest> reqs;
    for (size_t i = 0; i < pendings.size(); ++i) {
      if (pendings[i].llm_done) continue;  // 两阶段已答
      reqs.push_back(llm_ans->build_request(pendings[i].q->question,
                                            pendings[i].q->question_date,
                                            pendings[i].ev_texts));
    }
    std::vector<size_t> req_map;
    for (size_t i = 0; i < pendings.size(); ++i)
      if (!pendings[i].llm_done) req_map.push_back(i);
    std::fprintf(stderr, "[llm] 答题批量 %zu 题（并发 %d）...\n", reqs.size(),
                 cli.cfg.concurrency);
    auto resps = client->chat_batch(reqs, cli.cfg.concurrency);
    for (size_t k = 0; k < req_map.size(); ++k) {
      if (resps[k].empty()) continue;  // 失败保留 baseline 结果
      pendings[req_map[k]].llm_answer = resps[k];
      pendings[req_map[k]].llm_done = true;
    }
  }

  // 步骤5：判分（离线或 LLM 批量）并累计；judge_scores 与 pendings 同序（-1=无 LLM 判分）
  std::vector<double> judge_scores(pendings.size(), -1.0);
  std::vector<ChatRequest> judge_reqs;
  if (cli.cfg.judge_mode == "llm" && llm_judge) {
    for (auto& p : pendings) {
      std::string pred = p.llm_done ? p.llm_answer : p.base.answer_text;
      judge_reqs.push_back(
          {{{"system", "你是答案评审，只输出 yes 或 no。"},
            {"user", "问题: " + p.q->question + "\n参考答案: " + p.q->answer +
                         "\n模型回答: " + pred +
                         "\n模型回答是否与参考答案语义一致（允许措辞不同）？只回答 yes 或 no。"}},
           {0.0, 512, false}});  // 推理模型：max_tokens 含思考预算
    }
    std::fprintf(stderr, "[llm] 判分批量 %zu 题...\n", judge_reqs.size());
    auto jresps = client->chat_batch(judge_reqs, cli.cfg.concurrency);
    for (size_t i = 0; i < pendings.size(); ++i)
      judge_scores[i] = LlmJudge::parse_yes_no(jresps[i]) ? 1.0 : 0.0;
  }
  // 汇总最终回答（llm 优先，失败回退 baseline）并累计
  std::vector<AnswerResult> finals(pendings.size());
  for (size_t i = 0; i < pendings.size(); ++i) {
    Pending& p = pendings[i];
    AnswerResult r = p.base;
    if (p.llm_done) {
      r.answer_text = p.llm_answer;
      r.abstained = LlmAnswerer::is_abstain(p.llm_answer);
    }
    finals[i] = r;
    acc.add(p.group, *p.q, r, judge_scores[i]);
    // M11 反馈回流：judge 结果作为监督信号（选中/忽略）落 JSONL；
    // 实体/think 通道绕过 recall 的题（query_id==0）补记 log_query。
    // 仅 graph 后端：lmem 的反馈闭环走 --fb-exp（P2b，下方双 pass 块），两者不混。
    if (!cli.feedback_dir.empty() && cli.store == "graph" && judge_scores[i] >= 0) {
      long qid = p.base.query_id;
      if (qid == 0) qid = engines[p.eng_idx]->log_query(p.q->question,
                                                        p.base.evidence_uids);
      if (qid > 0)
        engines[p.eng_idx]->feedback(
            qid, judge_scores[i] >= 1.0 ? SignalType::Select : SignalType::Ignore,
            p.base.evidence_uids.empty() ? "" : p.base.evidence_uids.front(),
            p.q->id);
    }
  }

  // ---- P2b 双 pass 反馈实验（仅 --store lmem + --fb-exp）----
  // pass1 = 上方正常答题判分；此处按 1.x 语义回流（judge≥1 → 正反馈，否则负反馈，
  // 作用于该题 top-1 证据的来源 kind），reliability 立即落盘 reliability.json；
  // pass2 = 同目录重开 Mem（replay 恢复观测/事实/可靠性），同题单只跑召回
  // （零 LLM 调用——本块在全部 LLM 阶段之后，不触碰 client），对比金标证据
  // 在 pass1/pass2 证据序中的首位命中 rank（上升/不变/下降）。
  std::vector<int> fb_rank1(pendings.size(), -1), fb_rank2(pendings.size(), -1);
  if (cli.store == "lmem" && !cli.fb_exp.empty()) {
    auto gold_rank = [](const AnswerResult& r, const BenchQuestion& q) -> int {
      for (size_t t = 0; t < r.evidence_dia_ids.size(); ++t) {
        const std::string& e = r.evidence_dia_ids[t];
        if (e.rfind("S:", 0) == 0) continue;  // 会话标记非 dia_id
        for (auto& g : q.evidence_dia_ids)
          if (e == g) return (int)t + 1;
      }
      return 0;  // 金标未进证据序
    };
    std::unordered_map<std::string, size_t> pend_idx;
    for (size_t i = 0; i < pendings.size(); ++i) pend_idx[pendings[i].q->id] = i;

    // pass1 → feedback 落盘
    size_t fb_applied = 0;
    for (size_t i = 0; i < pendings.size(); ++i) {
      if (judge_scores[i] < 0) continue;  // 无判分不回流（同 1.x judge>=0 门槛）
      Pending& p = pendings[i];
      auto it = lmem_by_conv.find(p.conv);
      if (it == lmem_by_conv.end() || p.ev_refs.empty()) continue;
      EvidenceItem ev;
      ev.ref_obs = p.ev_refs[0].ref_obs;
      ev.source_kind = p.ev_refs[0].source_kind;
      ev.via = p.ev_refs[0].via;
      if (it->second->feedback(p.conv, ev, judge_scores[i] >= 1.0) == Err::Ok) ++fb_applied;
    }
    std::fprintf(stderr, "[fbexp] pass1 判分 %zu 题，反馈落盘 %zu 条\n", pendings.size(),
                 fb_applied);

    // pass2：同题单只跑召回，逐题对比金标 rank
    int up = 0, same = 0, down = 0, nogold = 0;
    for (auto& c : cases) {
      if (!lmem_by_conv.count(c.conversation_id)) continue;
      Mem m2(cli.fb_exp + "/" + c.conversation_id);  // 持久化分片 replay 恢复
      LmemStore ls2(&m2, c.conversation_id);
      for (auto& q : c.questions) {
        if (!only_qids.empty() && !only_qids.count(q.id)) continue;
        auto pi = pend_idx.find(q.id);
        if (pi == pend_idx.end()) continue;
        StoreAnswer sa2 = ls2.answer_question(q, cli.cfg);  // 只召回，零 LLM
        const int r1 = gold_rank(pendings[pi->second].base, q);
        const int r2 = gold_rank(sa2.result, q);
        fb_rank1[pi->second] = r1;
        fb_rank2[pi->second] = r2;
        const char* dir = "nogold";
        if (r1 > 0 && r2 > 0) {
          if (r2 < r1) { ++up; dir = "up"; }
          else if (r2 > r1) { ++down; dir = "down"; }
          else { ++same; dir = "same"; }
        } else if (r2 > 0) {
          ++up; dir = "up";  // pass1 未命中 → pass2 命中，记上升
        } else {
          ++nogold;
        }
        std::printf("[fbexp] %s %-8s gold_rank pass1=%d pass2=%d %s\n",
                    c.conversation_id.c_str(), q.id.c_str(), r1, r2, dir);
      }
    }
    std::printf("[fbexp-summary] questions=%zu up=%d same=%d down=%d nogold=%d\n",
                pendings.size(), up, same, down, nogold);
  }

  // M14 学习闭环：用回流 triples 离线调 α/β/γ，打印调优前后对比（离线复评 baseline）
  if (!cli.feedback_dir.empty() && cli.store == "graph") {
    for (size_t ei = 0; ei < engines.size(); ++ei) {
      Engine& e = *engines[ei];
      auto w0 = e.reranker().weights();
      std::string ver = e.train();  // train_offline + promote
      if (ver.empty()) continue;
      auto w1 = e.reranker().weights();
      std::printf("[M14] case %zu train=%s α/β/γ: %.2f/%.2f/%.2f → %.2f/%.2f/%.2f\n", ei,
                  ver.c_str(), w0.alpha, w0.beta, w0.gamma, w1.alpha, w1.beta, w1.gamma);
    }
  }

  // 步骤5b：--dump 逐题结果 NDJSON（失败分析用）
  if (!cli.dump_path.empty()) {
    FILE* df = std::fopen(cli.dump_path.c_str(), "w");
    if (df) {
      for (size_t i = 0; i < pendings.size(); ++i) {
        const Pending& p = pendings[i];
        Json j = Json::object();
        j["question_id"] = Json::string(p.q->id);
        j["question"] = Json::string(p.q->question);
        j["category"] = Json::number(p.q->category);
        j["qtype"] = Json::string(p.q->qtype);
        j["reference_answer"] = Json::string(p.q->answer);
        j["answer"] = Json::string(finals[i].answer_text);
        j["abstained"] = Json::boolean(finals[i].abstained);
        j["unanswerable"] = Json::boolean(p.q->unanswerable);
        j["score"] = Json::number(finals[i].score);
        if (judge_scores[i] >= 0) j["judge"] = Json::number(judge_scores[i]);
        Json ev = Json::array();
        for (size_t t = 0; t < p.ev_texts.size() && t < 3; ++t)
          ev.push(Json::string(p.ev_texts[t]));
        j["evidence_top3"] = std::move(ev);
        Json eids = Json::array();
        // 上限按条目数放宽到 30（≈ top-k=10 条证据 × dia_id+会话标记 2 条目）：
        // 旧上限 10 会把 top-k 后半的证据从逐题 dump 截掉，覆盖率对照被低估
        for (size_t t = 0; t < p.base.evidence_dia_ids.size() && t < 30; ++t)
          eids.push(Json::string(p.base.evidence_dia_ids[t]));
        j["evidence_dia_ids"] = std::move(eids);
        Json gold_ev = Json::array();
        for (auto& e : p.q->evidence_dia_ids) gold_ev.push(Json::string(e));
        j["gold_evidence"] = std::move(gold_ev);
        if (fb_rank1[i] >= 0) {  // P2b fb-exp：双 pass 金标 rank 对照
          j["fb_rank_pass1"] = Json::number((double)fb_rank1[i]);
          j["fb_rank_pass2"] = Json::number((double)fb_rank2[i]);
        }
        std::string line = j.dump();
        std::fwrite(line.data(), 1, line.size(), df);
        std::fputc('\n', df);
      }
      std::fclose(df);
      std::fprintf(stderr, "[dump] %s (%zu 题)\n", cli.dump_path.c_str(), pendings.size());
    }
  }
  if (cand_df) {
    std::fclose(cand_df);
    std::fprintf(stderr, "[cand-dump] %s\n", cli.cand_dump_path.c_str());
  }

  // 步骤6：报表输出（控制台 + 可选 JSON）
  std::printf("\n== %s 报表（extractor=%s answer=%s judge=%s k=%d）==\n", cli.dataset.c_str(),
              cli.cfg.extractor_mode.c_str(), cli.cfg.answer_mode.c_str(),
              cli.cfg.judge_mode.c_str(), cli.cfg.k);
  std::printf("%s", acc.report().c_str());
  std::printf("total questions: %zu\n", pendings.size());
  if (client)
    std::printf("llm api_calls=%d cache_hits=%d\n", client->api_calls(), client->cache_hits());
  if (!cli.cfg.out_json.empty()) {
    FILE* f = std::fopen(cli.cfg.out_json.c_str(), "w");
    if (f) {
      std::string js = acc.report_json();
      std::fwrite(js.data(), 1, js.size(), f);
      std::fclose(f);
      std::fprintf(stderr, "[out] %s\n", cli.cfg.out_json.c_str());
    }
  }
  return 0;
}
