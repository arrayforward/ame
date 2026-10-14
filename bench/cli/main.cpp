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
#include <unordered_set>

#include "ame/bench/bench.h"
#include "ame/bench/metrics.h"
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
  std::string lexicon_out = "bench/data/lexicon.json";
  std::string load_lexicon;    // --load-lexicon：词典灌库路径
  std::string question_ids;    // --question-ids：只评测这些 question_id（JSON 数组）
  int lexicon_topn = 2000;
  std::string save_graph;      // --save-graph PATH：摄入后整图（含档案）落 JSON（每 case 一个文件）
  std::string load_graph;      // --load-graph PATH：直接加载已摄入的图，跳过摄入/提取
  std::string feedback_dir;    // --feedback-dir PATH：M11 埋点+judge 信号回流 + M14 调优对比
  bool has_weights = false;    // --weights α,β,γ 覆盖融合权重
  double w_alpha = 0.5, w_beta = 0.3, w_gamma = 0.2;
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
               "      [--extractor rule|llm] [--answer baseline|llm] [--judge f1|llm]\n"
               "      [--limit N] [--k 10] [--concurrency 8] [--model deepseek-v4-flash]\n"
               "      [--cache-dir bench/cache] [--config bench/config/deepseek.local.json]\n"
               "      [--out report.json] [--abstain-threshold 0.15] [--abstain-categories 5]\n"
               "      [--dump results.ndjson]（逐题结果落盘，失败分析用）\n"
               "API key: bench/config/deepseek.local.json 或环境变量 AME_DEEPSEEK_API_KEY\n");
}

}  // namespace

int main(int argc, char** argv) {
  Cli cli = parse_args(argc, argv);

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
                  cli.cfg.judge_mode == "llm";
  if (need_llm) {
    std::filesystem::create_directories(cli.cfg.cache_dir);
    client = DeepSeekClient::from_config(cli.cfg.config_path, cli.cfg.model, cli.cfg.cache_dir);
    if (!client) {
      std::fprintf(stderr, "未找到 API key（%s 或 AME_DEEPSEEK_API_KEY）\n",
                   cli.cfg.config_path.c_str());
      return 1;
    }
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
    size_t eng_idx = 0;              // 所属 Engine（M11 反馈回流出引用）
  };
  std::vector<Pending> pendings;
  std::vector<std::unique_ptr<Engine>> engines;  // 保活到判分阶段（反馈回流用）
  // 聚合题候选证据（case 循环内召回，引擎存活期收集）
  struct AggJob {
    int qi;
    std::vector<std::string> evs;
  };
  std::vector<AggJob> agg_jobs;

  for (auto& c : cases) {
    // --question-ids 固定样本提速：该 case 无任何被请求的问题 → 整个跳过（含摄入）。
    // 安全性：每 case 独立引擎、跨 case 无共享状态，未命中 case 的摄入对结果零影响。
    if (!only_qids.empty()) {
      bool any = false;
      for (auto& q : c.questions)
        if (only_qids.count(q.id)) { any = true; break; }
      if (!any) continue;
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
    // 实体/think 通道绕过 recall 的题（query_id==0）补记 log_query
    if (!cli.feedback_dir.empty() && judge_scores[i] >= 0) {
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

  // M14 学习闭环：用回流 triples 离线调 α/β/γ，打印调优前后对比（离线复评 baseline）
  if (!cli.feedback_dir.empty()) {
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
        for (size_t t = 0; t < p.base.evidence_dia_ids.size() && t < 10; ++t)
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
