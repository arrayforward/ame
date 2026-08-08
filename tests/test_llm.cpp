/*
 * llm 接入层测试（mock 模式，不打真实 API）：
 * DeepSeekClient 注入 fake transport 验证缓存键/重试/响应解析；
 * LlmExtractor 宽容解析与规则回退；LlmAnswerer 弃答判定；LlmJudge 三种口径；
 * fake client 跑通 ingest→answer→judge 夹具级端到端链路。
 * 集成测试（真实 API）默认跳过，设 AME_RUN_LLM_IT=1 才执行。
 */
#include <atomic>
#include <cstdlib>
#include <filesystem>

#include "ame/bench/bench.h"
#include "ame/engine/engine.h"
#include "ame/llm/components.h"
#include "ame/llm/llm.h"
#include "test_util.hpp"

using namespace ame;

static void test_client_cache_and_retry() {
  std::string dir = "/tmp/ame_test_llm_cache";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  DeepSeekClient c("https://example.invalid", "test-key-not-real", "m", dir);
  int calls = 0;
  c.set_transport([&](const std::string& body, std::string* out) -> int {
    ++calls;
    CHECK(body.find("\"model\":\"m\"") != std::string::npos);
    *out = R"({"choices":[{"message":{"content":"hello"}}]})";
    return 200;
  });
  // 首次调用走 transport，第二次同请求命中缓存
  CHECK(c.chat({{"user", "hi"}}, {}) == "hello");
  CHECK(c.chat({{"user", "hi"}}, {}) == "hello");
  CHECK(calls == 1);
  CHECK(c.api_calls() == 1 && c.cache_hits() == 1);
  // 缓存文件存在且 content 不含 key
  int files = 0;
  for (auto it : std::filesystem::directory_iterator(dir)) {
    (void)it;
    ++files;
  }
  CHECK(files == 1);

  // 重试：两次 429 后 200
  DeepSeekClient c2("https://example.invalid", "k", "m", "");
  int tries = 0;
  c2.set_transport([&](const std::string&, std::string* out) -> int {
    if (++tries < 3) return 429;
    *out = R"({"choices":[{"message":{"content":"ok"}}]})";
    return 200;
  });
  CHECK(c2.chat({{"user", "x"}}, {}) == "ok");
  CHECK(tries == 3);

  // 不可重试错误（400）立即失败
  DeepSeekClient c3("https://example.invalid", "k", "m", "");
  int tries3 = 0;
  c3.set_transport([&](const std::string&, std::string*) -> int {
    ++tries3;
    return 400;
  });
  CHECK(c3.chat({{"user", "x"}}, {}).empty());
  CHECK(tries3 == 1);
}

static void test_chat_batch() {
  DeepSeekClient c("https://example.invalid", "k", "m", "");
  std::atomic<int> calls{0};
  c.set_transport([&](const std::string& body, std::string* out) -> int {
    ++calls;
    // 按问题内容回显，验证结果与请求同序（取 q- 后的数字编号）
    size_t p = body.find("q-");
    size_t e = body.find('"', p);
    *out = "{\"choices\":[{\"message\":{\"content\":\"ans-" + body.substr(p + 2, e - p - 2) +
           "\"}}]}";
    return 200;
  });
  std::vector<ChatRequest> reqs;
  for (int i = 0; i < 16; ++i)
    reqs.push_back({{{"user", "q-" + std::to_string(i)}}, {}});
  auto resps = c.chat_batch(reqs, 8);
  CHECK(calls == 16);
  CHECK(resps.size() == 16);
  for (int i = 0; i < 16; ++i)
    CHECK(resps[i] == "ans-" + std::to_string(i));
}

static void test_llm_extractor() {
  FakeLlmClient fake;
  fake.default_response = "not json at all";  // 触发回退
  fake.rules.push_back(
      {"Mochi",
       R"([{"type":"life","entities":[{"surface":"Mochi","role":"客体","type":"object"}],"keywords":[{"word":"adopted","category":"action"},{"word":"cat","category":"concept"}]}])"});
  RuleExtractor rule;
  LlmExtractor ext(&fake, &rule);

  // LLM 正常路径：JSON 数组解析
  auto v = ext.extract_session({{"Alice", "I adopted a cat named Mochi."}});
  CHECK(v.size() == 1);
  CHECK(v[0].event.text == "I adopted a cat named Mochi.");
  bool has_mochi = false, has_alice = false;
  for (auto& e : v[0].entities) {
    if (e.surface == "Mochi") has_mochi = true;
    if (e.surface == "Alice") has_alice = true;  // speaker 主体实体兜底
  }
  CHECK(has_mochi && has_alice);
  CHECK(v[0].keywords.size() == 2);
  CHECK(v[0].event.type == "life");

  // 回退路径：LLM 输出不合法 → 规则提取器
  auto v2 = ext.extract_session({{"Bob", "老张在朝阳公园跑步"}});
  CHECK(v2.size() == 1);
  bool rule_hit = false;
  for (auto& k : v2[0].keywords)
    if (k.word == "跑步") rule_hit = true;  // 规则词典命中
  CHECK(rule_hit);
}

static void test_answerer_and_judge() {
  FakeLlmClient fake;
  fake.rules.push_back({"favorite restaurant", "NO INFORMATION"});
  fake.rules.push_back({"cat", "Mochi"});
  LlmAnswerer ans(&fake);
  auto req = ans.build_request("What is Alice's favorite restaurant?", "2023-09-01",
                               {"[ts=1700000000] I adopted a cat named Mochi."});
  std::string resp = fake.chat(req.messages, req.opts);
  CHECK(LlmAnswerer::is_abstain(resp));
  auto req2 = ans.build_request("What is the name of the cat?", "", {"...Mochi..."});
  CHECK(!LlmAnswerer::is_abstain(fake.chat(req2.messages, req2.opts)));

  // judge yes/no 解析
  CHECK(LlmJudge::parse_yes_no("yes"));
  CHECK(LlmJudge::parse_yes_no(" Yes.\n"));
  CHECK(!LlmJudge::parse_yes_no("no"));
  CHECK(!LlmJudge::parse_yes_no(""));

  FakeLlmClient fake2;
  fake2.default_response = "[1, 0.5, 0]";
  LlmJudge judge(&fake2);
  auto scores = judge.judge("q", "a", {"n1", "n2", "n3"});
  CHECK(scores.size() == 3);
  CHECK_NEAR(scores[0], 1.0, 1e-9);
  CHECK_NEAR(scores[1], 0.5, 1e-9);
  CHECK_NEAR(scores[2], 0.0, 1e-9);

  FakeLlmClient fake3;
  fake3.default_response = "yes";
  LlmJudge judge2(&fake3);
  CHECK(judge2.correctness("q", "gold", "pred") == 1.0);
}

// 夹具级端到端：fake client → llm 提取摄入 → llm 答题 → llm 判分
static void test_fake_pipeline_e2e() {
  FakeLlmClient fake;
  fake.rules.push_back(
      {"Mochi",
       R"([{"type":"life","entities":[],"keywords":[{"word":"cat","category":"concept"},{"word":"mochi","category":"attr"}]}])"});
  RuleExtractor rule;
  LlmExtractor ext(&fake, &rule);

  Engine e;
  auto exts = ext.extract_session({{"Alice", "I adopted a cat named Mochi."}});
  auto wr = e.remember_turn_ex(std::move(exts[0]), "Alice", 1700000000, "D1:1", "s1");
  CHECK(wr.code == Err::Ok);
  // LLM 提取的关键词已落图
  CHECK(e.keywords().find("mochi") != nullptr);

  // fake 答题 + 判分（judge 用独立的 fake，避免规则串扰）
  FakeLlmClient fake2;
  fake2.rules.push_back({"cat", "The cat is Mochi"});
  FakeLlmClient fake3;
  fake3.default_response = "yes";
  LlmAnswerer ans(&fake2);
  LlmJudge judge(&fake3);
  auto req = ans.build_request("What is the name of Alice's cat?", "", {});
  std::string pred = fake2.chat(req.messages, req.opts);
  CHECK(!LlmAnswerer::is_abstain(pred));
  CHECK(judge.correctness("What is the name of Alice's cat?", "Mochi", pred) == 1.0);
}

// 词典蒸馏 mock 测试：批量响应解析 + 增量幂等 + 灌库（不打真实 API）
static void test_lexicon() {
  FakeLlmClient fake;
  fake.default_response =
      R"({"paint":{"synonyms":["painting","artwork"],"antonyms":[],"related":[{"word":"canvas","score":0.9}]},"run":{"synonyms":["running","jog"],"antonyms":["walk"],"related":[]}})";
  // 词频表不存在 → 失败
  CHECK(build_lexicon_json(fake, {"/nonexistent_vocab.json"}, "/tmp/ame_test_lex.json", 10,
                           50, 1) < 0);

  // 写两个小词频表再构建（多文件合并词频，先清理产物保证可重入）
  std::filesystem::remove("/tmp/ame_test_lex.json");
  FILE* f = std::fopen("/tmp/ame_test_vocab.json", "w");
  std::fputs(R"({"conv_vocab_top":[["paint",100]],"question_vocab_top":[],"stats":{}})", f);
  std::fclose(f);
  FILE* f2 = std::fopen("/tmp/ame_test_vocab2.json", "w");
  std::fputs(R"({"conv_vocab_top":[["run",80]],"question_vocab_top":[["paint",10]],"stats":{}})", f2);
  std::fclose(f2);
  CHECK(build_lexicon_json(fake, {"/tmp/ame_test_vocab.json", "/tmp/ame_test_vocab2.json"},
                           "/tmp/ame_test_lex.json", 10, 50, 1) == 2);
  // 增量幂等：再跑一次 → 0 新增，不再调 LLM
  int calls_before = (int)fake.received.size();
  CHECK(build_lexicon_json(fake, {"/tmp/ame_test_vocab.json", "/tmp/ame_test_vocab2.json"},
                           "/tmp/ame_test_lex.json", 10, 50, 1) == 0);
  CHECK((int)fake.received.size() == calls_before);

  // 灌库：synonyms 进别名表，antonyms/related 建边；重复灌库幂等
  Engine e;
  e.remember("老张今天 paint 很开心");  // 让 paint/run 节点存在
  e.remember("老王昨天 run 很累");
  int applied = 0;
  CHECK(apply_lexicon(e, "/tmp/ame_test_lex.json", &applied) == Err::Ok);
  CHECK(applied > 0);
  CHECK(e.keywords().normalize("painting") == "paint");
  CHECK(e.keywords().normalize("jog") == "run");
  size_t edges1 = e.storage().edge_count();
  int applied2 = 0;
  CHECK(apply_lexicon(e, "/tmp/ame_test_lex.json", &applied2) == Err::Ok);
  CHECK(e.storage().edge_count() == edges1);  // 幂等：无重复边

  // 多文件拼接：第二本词典含同词条 paint 的新同义词 sketch → 并集生效
  FILE* f3 = std::fopen("/tmp/ame_test_lex2.json", "w");
  std::fputs(R"({"version":"v1","entries":[{"word":"paint","synonyms":["sketch"],"antonyms":[],"related":[]}]})", f3);
  std::fclose(f3);
  CHECK(apply_lexicon(e, "/tmp/ame_test_lex2.json", nullptr) == Err::Ok);
  CHECK(e.keywords().normalize("sketch") == "paint");   // 新同义词生效
  CHECK(e.keywords().normalize("painting") == "paint"); // 旧同义词保留（并集）
  CHECK(e.storage().edge_count() == edges1);            // 纯别名追加，无边变化
}

// 三分流单测：syn_strong 归并别名 / syn_near 只建相似边不归并 / antonyms 对立边
static void test_lexicon_split() {
  FILE* f = std::fopen("/tmp/ame_test_lex3.json", "w");
  std::fputs(R"({"version":"v2","entries":[{"word":"paint","syn_strong":["painting","artwork"],"syn_near":[{"word":"sketch","score":0.9}],"antonyms":["erase"]}]})", f);
  std::fclose(f);
  Engine e;
  e.remember("老张今天 paint 了一幅 sketch，没有用 erase");  // 让 paint/sketch/erase 节点存在
  int applied = 0;
  CHECK(apply_lexicon(e, "/tmp/ame_test_lex3.json", &applied) == Err::Ok);
  // 同义 → 别名归并
  CHECK(e.keywords().normalize("painting") == "paint");
  CHECK(e.keywords().normalize("artwork") == "paint");
  // 近义 → 不归一，只建 REL_SIMILAR 边（双节点保留）
  CHECK(e.keywords().normalize("sketch") != "paint");
  const Node* kp = e.keywords().find("paint");
  const Node* ks = e.keywords().find("sketch");
  const Node* ke = e.keywords().find("erase");
  CHECK(kp && ks && ke);
  CHECK(e.storage().find_edge(kp->uid, ks->uid, RelType::REL_SIMILAR) != nullptr);
  // 反义 → REL_OPPOSITE 边（core 系数 0.5，扩散最低档）
  CHECK(e.storage().find_edge(kp->uid, ke->uid, RelType::REL_OPPOSITE) != nullptr);
  CHECK(conduction_coeff(RelType::REL_OPPOSITE) < conduction_coeff(RelType::REL_SIMILAR));
}

// 证据整合器 mock 测试：分块摘要 + 合并轮 + 失败回退（不打真实 API）
// 注：digest 功能经两轮 A/B 判负已移除（README「已否证的假设」）。
// 两阶段聚合作答 mock 测试：逐项 NONE 过滤、空列表弃答、聚合输入只含事实列表
static void test_two_stage_aggregate() {
  // 模式识别
  CHECK(LlmAnswerer::is_aggregate_question("How many trips did I take?"));
  CHECK(LlmAnswerer::is_aggregate_question("总共去了几个城市"));
  CHECK(!LlmAnswerer::is_aggregate_question("Where does Alice live?"));
  // NONE 判定
  CHECK(LlmAnswerer::is_none("NONE"));
  CHECK(LlmAnswerer::is_none("none."));
  CHECK(!LlmAnswerer::is_none("她搬到了 Portland"));
  // 阶段一请求：保留思考（关思考会过度 NONE）+ 含问题与证据
  auto r1 = LlmAnswerer::build_extract_request("我去了几个城市？", "[2023-01-01] 搬到 Seattle");
  CHECK(!r1.opts.disable_thinking);
  CHECK(r1.messages[1].content.find("搬到 Seattle") != std::string::npos);
  // 阶段二请求：输入只含事实列表，不含原文长片段
  FakeLlmClient fake;
  LlmAnswerer ans(&fake);
  auto r2 = ans.build_aggregate_request("我去了几个城市？", "", {"[E1] 搬到 Seattle",
                                                               "[E2] 搬到 Portland"});
  CHECK(r2.messages[1].content.find("搬到 Portland") != std::string::npos);
  CHECK(r2.messages[1].content.find("问题: 我去了几个城市？") != std::string::npos);
  CHECK(r2.messages[1].content.find("证据片段") == std::string::npos);  // 不走单阶段原文通道
}

// 批量提取 mock 测试：批量请求构造、解析（部分 NONE/编号错位/越界）、空列表判定
static void test_extract_batch() {
  // 批量请求：编号 + 保留思考
  auto req = LlmAnswerer::build_extract_batch_request(
      "我去了几个城市？", {"[2023-01-01] 搬到 Seattle", "[2023-02-01] 爬山", "[2023-03-01] 搬到 Portland"});
  CHECK(req.messages[1].content.find("[E2]") != std::string::npos);
  CHECK(!req.opts.disable_thinking);
  // 解析：E1 事实、E2 NONE、E4 越界、E1 重复取首次
  auto facts = LlmAnswerer::parse_extract_batch(
      "[E1]: 搬到 Seattle\n[E2]: NONE\n[E3]: 搬到 Portland\n[E4]: 越界\n[E1]: 重复忽略\n", 3);
  CHECK(facts.size() == 2);
  CHECK(facts[0].first == 0 && facts[1].first == 2);
  // 全 NONE → 空列表
  CHECK(LlmAnswerer::parse_extract_batch("[E1]: NONE\n[E2]: none\n", 2).empty());
  // 乱序/杂行宽容
  auto f2 = LlmAnswerer::parse_extract_batch("前言废话\n[E2]: 只收 E2\n", 2);
  CHECK(f2.size() == 1 && f2[0].first == 1);
}

int main() {
  test_client_cache_and_retry();
  test_chat_batch();
  test_llm_extractor();
  test_answerer_and_judge();
  test_fake_pipeline_e2e();
  test_lexicon();
  test_lexicon_split();
  test_two_stage_aggregate();
  test_extract_batch();
  // 集成测试（真实 API）默认跳过
  if (std::getenv("AME_RUN_LLM_IT")) {
    auto client = DeepSeekClient::from_config("bench/config/deepseek.local.json",
                                              "deepseek-v4-flash", "/tmp/ame_llm_it_cache");
    if (client) {
      std::string r = client->chat({{"user", "只回答 ok"}}, {0.0, 8, false});
      CHECK(!r.empty());
    }
  }
  return AME_TEST_REPORT();
}
