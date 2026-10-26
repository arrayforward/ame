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

using namespace ame;

namespace {

// 命令行参数 → BenchConfig + dataset/data/limit
struct Cli {
  BenchConfig cfg;
  std::string dataset, data;
  int limit = 0;
  std::string dump_path;       // --dump：逐题结果 NDJSON 落盘路径
  std::string build_lexicon;   // --build-lexicon：词频表路径（构建词典后退出）
  std::string validate_lexicon;  // --validate-lexicon：蒸馏词库 strong 流可互换校验（M2）
  std::string score_sims;        // --score-sims：词级近似 1-10 打分（第五轮 M3）
  std::string lexicon_out = "bench/data/lexicon.json";
  std::string question_ids;    // --question-ids：只评测这些 question_id（JSON 数组）
  int lexicon_topn = 2000;
  std::string save_graph;      // --save-graph PATH：摄入后整图（含档案）落 JSON（每 case 一个文件）
  std::string load_graph;      // --load-graph PATH：直接加载已摄入的图，跳过摄入/提取
                               // pass1 答题判分→feedback 落盘→pass2 同题单只跑召回对比排序）
                               // 摄入后、答题前经 load_keyword_dict_cached 加载（查询侧词激活）
  std::string name_alias;      // --name-alias FILE：人名/专名严等价别名表（names_zh.json，
                               // 摄入前加载；M3 别名+M1 词面+M2 实体三通道贯通，默认空=关）
  double entity_pass = -1.0;   // --entity-pass-threshold X：实体通道低置信放行（<0=关=现状）
  double rerank_guard = -1.0;  // --rerank-guard X：BM25 强命中豁免 LLM 重排（<0=关=现状）
  std::string cheap_rerank;    // --cheap-rerank PATH：特征逻辑重排器模型 JSON（空=关）
  std::string rerank_mode;     // --rerank bm25|cheap|llm（3.0 刀5：别名收敛，仅此开关）
  bool has_final = false;      // --final-alpha/--final-beta：L2 终排混合开关（默认关闭）
};

// parse_args 伪代码：键值扫描，未知键忽略；--abstain-categories 为逗号分隔整数列表。
Cli parse_args(int argc, char** argv) {
  Cli cli;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    std::string a = argv[i];
    if (a == "--dataset") cli.dataset = next();
    else if (a == "--data") cli.data = next();
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
    else if (a == "--build-lexicon") cli.build_lexicon = next();
    else if (a == "--validate-lexicon") cli.validate_lexicon = next();
    else if (a == "--score-sims") cli.score_sims = next();
    else if (a == "--lexicon-out") cli.lexicon_out = next();
    else if (a == "--lexicon-topn") cli.lexicon_topn = std::atoi(next().c_str());
    else if (a == "--question-ids") cli.question_ids = next();
    else if (a == "--entity-profile") cli.cfg.use_entity_profile = next() != "off";
    else if (a == "--conditional-normalize")
      cli.cfg.conditional_normalize = next() != "off";
    else if (a == "--answer-mode") cli.cfg.answer_mode_agg = next();
    else if (a == "--agg-prefilter-n") cli.cfg.agg_prefilter_n = std::atoi(next().c_str());
    else if (a == "--agg-batch") cli.cfg.agg_batch = std::atoi(next().c_str());
    else if (a == "--save-graph") cli.save_graph = next();
    else if (a == "--load-graph") cli.load_graph = next();
    else if (a == "--name-alias") cli.name_alias = next();
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
               "      [--extractor rule|llm] [--answer baseline|llm] [--judge f1|llm]\n"
               "      [--limit N] [--k 10] [--concurrency 8] [--model deepseek-v4-flash]\n"
               "      [--cache-dir bench/cache] [--config bench/config/deepseek.local.json]\n"
               "      [--out report.json] [--abstain-threshold 0.15] [--abstain-categories 5]\n"
               "      [--dump results.ndjson]（逐题结果落盘，失败分析用）\n"
               "      [--stem-en]（graph：M3 英文保守词干归一，查询/写入双侧，默认关）\n"
               "      [--word-sim FILE]（graph：词级近似表软匹配，graded sim，默认关）\n"
               "      [--name-alias FILE]（graph：人名/专名严等价别名表 names_zh.json，\n"
               "       摄入前灌入 M3 别名+M1 词面+M2 实体，默认关）\n"
               "      [--rerank bm25|cheap|llm]（graph：终排模式；默认 bm25=纯词面。\n"
               "       cheap=特征重排器零LLM需 --cheap-rerank PATH；llm=全池LLM打分）\n"
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
  cli.cfg.entity_pass_threshold = cli.entity_pass;  // --entity-pass-threshold（默认 -1=现状）
  cli.cfg.rerank_guard = cli.rerank_guard;          // --rerank-guard（默认 -1=关）
  cli.cfg.cheap_rerank_path = cli.cheap_rerank;     // --cheap-rerank（默认空=关）
  // ---- 终排模式消解（--rerank bm25|cheap|llm；3.0 刀5 起历史别名全部收敛删除，
  // --cheap-rerank 降为 cheap 档纯模型路径参数，不再参与模式推断）----
  {
    std::string mode = cli.rerank_mode.empty() ? "bm25" : cli.rerank_mode;
    if (mode != "bm25" && mode != "cheap" && mode != "llm") {
      std::fprintf(stderr, "未知 --rerank 模式: %s（仅支持 bm25|cheap|llm）\n", mode.c_str());
      return 1;
    }
    if (mode == "cheap" && cli.cheap_rerank.empty()) {
      std::fprintf(stderr, "--rerank cheap 需 --cheap-rerank PATH（模型 JSON）\n");
      return 1;
    }
    cli.cfg.cheap_rerank_path = cli.cheap_rerank;     // --cheap-rerank（默认空=关）
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
    cli.cfg.llm_client = client.get();  // llm 档全池语义打分通道注入
    llm_ext = std::make_unique<LlmExtractor>(client.get(), &rule_ext);
    llm_ans = std::make_unique<LlmAnswerer>(client.get());
    llm_judge = std::make_unique<LlmJudge>(client.get());
  }

  // 步骤3：逐 case 摄入 + 离线取证；LLM 答题/判分请求先收集后批量发
  MetricAccumulator acc;
  struct Pending {
    const BenchQuestion* q;
    AnswerResult base;               // baseline 结果（含证据）
    std::vector<std::string> ev_texts;
    std::string group;
    std::string llm_answer;
    bool llm_done = false;
  };
  std::vector<Pending> pendings;

  // 聚合题候选证据（case 循环内召回，引擎存活期收集）
  struct AggJob {
    int qi;
    std::vector<std::string> evs;
  };
  std::vector<AggJob> agg_jobs;

  for (auto& c : cases) {
    // --question-ids 固定样本提速：该 case 无任何被请求的问题 → 整个跳过（含摄入）。
    if (!only_qids.empty()) {
      bool any = false;
      for (auto& q : c.questions)
        if (only_qids.count(q.id)) { any = true; break; }
      if (!any) continue;
    }
    Engine e;  // 跨 case 隔离：每 case 用完即销毁，避免百级 case 全量图驻留

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
    // 对抗题口径修正 v2（作答最后一公里轮）：数据集标注不可答且参考答案为空时，
    // v1 规则（弃答=1/硬答=0）会把"正确指出错误前提"的高质量硬答也判 0（三栏核对
    // 实测 3/7 属此类）。改为 cat5 专用 LLM rubric：弃答 或 正确指出前提问题/给出
    // 正确归属 = 合格；照单接受错误前提直答 = 不合格。只作用于 unanswerable 题，
    // cat1-4 的 yes/no 判分路径不变。
    std::vector<size_t> jidx;
    for (size_t i = 0; i < pendings.size(); ++i) {
      auto& p = pendings[i];
      std::string pred = p.llm_done ? p.llm_answer : p.base.answer_text;
      if (p.q->unanswerable && p.q->answer.empty()) {
        judge_reqs.push_back(
            {{{"system", "你是答案评审，只输出 yes 或 no。"},
              {"user", "问题: " + p.q->question + "\n模型回答: " + pred +
                           "\n该问题可能基于错误前提（人物/归属/事件与对话事实不符）"
                           "或确实无法回答。模型回答若满足任一：明确指出前提问题并给出正确归属；"
                           "或明确表示无法回答/信息不足——则合格。照单接受错误前提直接作答则不合格。"
                           "只回答 yes 或 no。"}},
             {0.0, 512, false}});
        jidx.push_back(i);
        continue;
      }
      judge_reqs.push_back(
          {{{"system", "你是答案评审，只输出 yes 或 no。"},
            {"user", "问题: " + p.q->question + "\n参考答案: " + p.q->answer +
                         "\n模型回答: " + pred +
                         "\n模型回答是否与参考答案语义一致（允许措辞不同）？只回答 yes 或 no。"}},
           {0.0, 512, false}});  // 推理模型：max_tokens 含思考预算
      jidx.push_back(i);
    }
    std::fprintf(stderr, "[llm] 判分批量 %zu 题...\n", judge_reqs.size());
    auto jresps = client->chat_batch(judge_reqs, cli.cfg.concurrency);
    for (size_t k = 0; k < jidx.size(); ++k)
      judge_scores[jidx[k]] = LlmJudge::parse_yes_no(jresps[k]) ? 1.0 : 0.0;
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
        std::string line = j.dump();
        std::fwrite(line.data(), 1, line.size(), df);
        std::fputc('\n', df);
      }
      std::fclose(df);
      std::fprintf(stderr, "[dump] %s (%zu 题)\n", cli.dump_path.c_str(), pendings.size());
    }
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
