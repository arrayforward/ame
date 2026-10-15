/*
 * AME 2.0 分层记忆（M15–M19）单元测试：设计 §10 P0 验收六项
 *   ① erase 无残留：分片物理消失 + tombstone 空壳；eraseChannel 只删该会话
 *   ② fold 三序裁决：Declared 权威 / 同权威 (ts,seq) 时序 / corrects 冲销留痕
 *   ③ 半衰期：effective 随 now 衰减，基准字段零改写
 *   ④ 索引可重建：drop+rebuild 后 recall/profile 逐字段/逐比特一致
 *   ⑤ 视图打包：Q→A "→ " 打包；synthesize 零副作用（replay 逐比特一致）
 *   ⑥ 公共域隔离：owner A 抹除后 B 与 public 的 recall/profile 不变
 *   ⑦ P2a 兜底召回：零词面重叠 → fallback 证据（user 优先+新鲜度）；
 *     正常路径无 fallback 且排序不变；部分命中时 fallback 恒居正常候选之后
 *   ⑧ P2b 可靠性闭环：负反馈 rank 降/正反馈 rank 升；reliability.json 重启保留
 *   ⑨ P2c G1–G3 校验门：declare 权威、allocate 先开槽、长度硬校验、public 拦截
 */
#include "ame/mem/mem.h"
#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ame;

// ---------------- 测试工具 ----------------

// 临时持久化根目录（每个用例独立，先清后建）
static std::string temp_root(const std::string& name) {
  const std::string p = (std::filesystem::temp_directory_path() / name).string();
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p;
}

static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// UTF-8 码点计数（与网关 G2 校验同口径）
static size_t utf8_check(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

// 观测层 replay 全量 dump（零副作用验证用：逐比特一致的判据）
static std::string dump_obs(const ObsStore& s) {
  std::string out;
  s.replay_all([&](const Observation& o) { out += s.to_json(o).dump(); out += "\n"; });
  return out;
}

// 事实层 replay 全量 dump
static std::string dump_facts(const FactStore& s) {
  std::string out;
  s.replay_all([&](const Fact& f) { out += s.to_json(f).dump(); out += "\n"; });
  return out;
}

// ---------------- ① erase 无残留 ----------------

static void test_erase_semantics() {
  const std::string root = temp_root("ame_lstore_erase");
  const std::string secret = "秘密暗号 xyzzy12345";
  {
    Mem m(root);
    m.remember("u1", "c1", "user", secret, 1700000000);
    m.remember("u1", "c2", "user", "u1 的第二个会话", 1700000100);
    m.declare("u1", "u1:self", {{"预备槽", "开"}}, 1700000000);  // P2c G1：先开槽
    m.rememberFact("u1", "u1:self", "口令", secret, 1700000000);
    CHECK(std::filesystem::exists(root + "/shards/obs/u1.jsonl"));
    CHECK(std::filesystem::exists(root + "/shards/fact/u1.jsonl"));

    CHECK(m.eraseOwner("u1"));
    // 分片文件物理消失（观测 + 事实两店）
    CHECK(!std::filesystem::exists(root + "/shards/obs/u1.jsonl"));
    CHECK(!std::filesystem::exists(root + "/shards/fact/u1.jsonl"));
    // tombstone 存在：含 owner 标记、不含任何原文（空壳无内容）
    CHECK(std::filesystem::exists(root + "/shards/tombstones.jsonl"));
    const std::string tomb = read_file(root + "/shards/tombstones.jsonl");
    CHECK(tomb.find("u1") != std::string::npos);
    CHECK(tomb.find(secret) == std::string::npos);
    CHECK(tomb.find("口令") == std::string::npos);
    // 内存侧也清空
    CHECK(!m.obs_store().has_owner("u1"));
  }
  // 重开：被抹除的 owner 不复活（分片已删，恢复不到数据）
  {
    Mem m(root);
    CHECK(!m.obs_store().has_owner("u1"));
    CHECK(m.recall("暗号", "u1", 5, 1700000200).empty());
  }

  // eraseChannel：只删该会话观测，owner 其他 channel 不受影响
  const std::string root2 = temp_root("ame_lstore_erase_ch");
  Mem m2(root2);
  m2.remember("u2", "c1", "user", "会话一的独有内容 alpha", 1700000000);
  m2.remember("u2", "c2", "user", "会话二的独有内容 beta", 1700000100);
  const size_t n = m2.eraseChannel("u2", "c1");
  CHECK(n == 1);
  // 内存：只剩 c2
  std::vector<std::string> left;
  m2.obs_store().for_owner("u2", [&](const Observation& o) {
    left.push_back(o.channel + "|" + o.text);
  });
  CHECK(left.size() == 1);
  CHECK(left[0].find("beta") != std::string::npos);
  // 分片重写：文件含 c2 内容、不含 c1 内容
  const std::string shard = read_file(root2 + "/shards/obs/u2.jsonl");
  CHECK(shard.find("beta") != std::string::npos);
  CHECK(shard.find("alpha") == std::string::npos);
  // u2 的 c2 观测仍可召回
  CHECK(!m2.recall("beta", "u2", 5, 1700000200).empty());

  // 文件名清洗：owner 含非法字符不炸、分片按清洗名落盘
  const std::string root3 = temp_root("ame_lstore_sanitize");
  Mem m3(root3);
  m3.remember("a/b\\c:d", "c1", "user", "清洗测试", 1700000000);
  CHECK(std::filesystem::exists(root3 + "/shards/obs/a_b_c_d.jsonl"));
}

// ---------------- ② fold 三序裁决 ----------------

static void test_fold_arbitration() {
  FactStore fs;  // 内存态
  const int64_t T = 1700000000;

  // Declared 权威：即使 observed 时刻更晚，declared（事件时刻更早）仍胜
  Fact obs;
  obs.ts = T + 100; obs.owner = "u"; obs.entity = "u:self"; obs.attr = "饮料";
  obs.value = "可乐"; obs.reality = Reality::Observed; obs.confidence = 0.9;
  const Fact f_obs = fs.append(obs);
  Fact dec;
  dec.ts = T; dec.owner = "u"; dec.entity = "u:self"; dec.attr = "饮料";
  dec.value = "龙井"; dec.reality = Reality::Declared; dec.confidence = 1.0;
  fs.append(dec);
  Folded fd;
  CHECK(fs.latest("u", "u:self", "饮料", T + 1000, fd));
  CHECK(fd.fact.value == "龙井");
  CHECK(fd.fact.reality == Reality::Declared);

  // 同权威（Observed）内按 (ts, seq)：ts 大者胜
  FactStore fs2;
  Fact a, b;
  a.ts = T; a.owner = "u"; a.entity = "u:self"; a.attr = "地点"; a.value = "北京";
  a.reality = Reality::Observed; a.confidence = 0.8;
  b = a; b.ts = T + 50; b.value = "上海";
  fs2.append(a);
  fs2.append(b);
  Folded fd2;
  CHECK(fs2.latest("u", "u:self", "地点", T + 1000, fd2));
  CHECK(fd2.fact.value == "上海");
  // 同 ts 按 seq：后写者胜
  FactStore fs3;
  Fact c = a, d = a;
  c.value = "先写"; d.value = "后写";  // 同 ts，d.seq > c.seq
  fs3.append(c);
  fs3.append(d);
  Folded fd3;
  CHECK(fs3.latest("u", "u:self", "地点", T + 1000, fd3));
  CHECK(fd3.fact.value == "后写");

  // corrects 冲销：latest 反映新值，原 fact 保留可查（留痕，不被改写）
  FactStore fs4;
  Fact f1, f2;
  f1.ts = T; f1.owner = "u"; f1.entity = "u:self"; f1.attr = "身高"; f1.value = "170";
  f1.reality = Reality::Observed; f1.confidence = 0.9;
  const Fact g1 = fs4.append(f1);
  f2 = f1; f2.ts = T + 10; f2.value = "172"; f2.corrects = g1.seq;  // 红字冲销
  const Fact g2 = fs4.append(f2);
  CHECK(g2.corrects == g1.seq);
  Folded fd4;
  CHECK(fs4.latest("u", "u:self", "身高", T + 1000, fd4));
  CHECK(fd4.fact.value == "172");
  CHECK(fd4.fact.seq == g2.seq);
  const Fact* old = fs4.get(g1.seq);  // 原记录留痕可查，字段零改写
  CHECK(old != nullptr);
  CHECK(old->value == "170");
  CHECK(old->corrects == 0);
  // 冲销连冲销：再冲掉 g2，latest 无候选
  Fact f3 = f2; f3.ts = T + 20; f3.value = "173"; f3.corrects = g2.seq;
  fs4.append(f3);
  Folded fd5;
  CHECK(fs4.latest("u", "u:self", "身高", T + 1000, fd5));
  CHECK(fd5.fact.value == "173");
  // g1 仍留痕
  CHECK(fs4.get(g1.seq) != nullptr);
}

// ---------------- ③ 半衰期 ----------------

static void test_half_life() {
  FactStore fs;
  const int64_t T0 = 1700000000;
  Fact f;
  f.ts = T0; f.owner = "u"; f.entity = "u:self"; f.attr = "饮料"; f.value = "龙井";
  f.reality = Reality::Observed; f.confidence = 0.9;
  const Fact g = fs.append(f);
  CHECK_NEAR(fs.half_life_days(Reality::Observed), 90.0, 1e-9);
  CHECK_NEAR(fs.half_life_days(Reality::Derived), 7.0, 1e-9);

  Folded f1, f2;
  // 事件时刻：不衰减
  CHECK(fs.latest("u", "u:self", "饮料", T0, f1));
  CHECK_NEAR(f1.effective, 0.9, 1e-9);
  CHECK_NEAR(f1.base, 0.9, 1e-9);
  // 一个半衰期后：effective 减半（Observed 默认 90 天）
  const int64_t T1 = T0 + (int64_t)90 * 86400;
  CHECK(fs.latest("u", "u:self", "饮料", T1, f2));
  CHECK_NEAR(f2.effective, 0.45, 1e-9);
  // 单调衰减
  Folded f3;
  CHECK(fs.latest("u", "u:self", "饮料", T0 + (int64_t)45 * 86400, f3));
  CHECK(f1.effective > f3.effective && f3.effective > f2.effective);
  // Declared 不衰减：任意 now 有效置信度恒为 confidence
  FactStore fs2;
  Fact d;
  d.ts = T0; d.owner = "u"; d.entity = "u:self"; d.attr = "身份"; d.value = "游客0427";
  d.reality = Reality::Declared; d.confidence = 1.0;
  fs2.append(d);
  Folded fd;
  CHECK(fs2.latest("u", "u:self", "身份", T0 + (int64_t)3650 * 86400, fd));
  CHECK_NEAR(fd.effective, 1.0, 1e-9);

  // 半衰期纪律：两次查询后基准字段逐比特一致（confidence/ts 不被改写）
  FactStore fs3;
  Fact o;
  o.ts = T0; o.owner = "u"; o.entity = "u:self"; o.attr = "旧观测"; o.value = "某值";
  o.reality = Reality::Observed; o.confidence = 0.7;
  const Fact og = fs3.append(o);
  const Fact* before = fs3.get(og.seq);
  const double conf_before = before->confidence;
  const int64_t ts_before = before->ts;
  const std::string val_before = before->value;
  Folded x1, x2;
  fs3.latest("u", "u:self", "旧观测", T0 + 100, x1);
  fs3.latest("u", "u:self", "旧观测", T0 + (int64_t)200 * 86400, x2);
  CHECK(x1.effective > x2.effective);  // 衰减只体现在 effective
  const Fact* after = fs3.get(og.seq);
  CHECK(after->confidence == conf_before);
  CHECK(after->ts == ts_before);
  CHECK(after->value == val_before);
  // fold 返回的副本也不带衰减污染
  CHECK_NEAR(x2.fact.confidence, 0.7, 1e-12);
  CHECK(x2.fact.ts == T0);
}

// ---------------- ④ 索引可重建 ----------------

static void test_index_rebuild() {
  const std::string root = temp_root("ame_lstore_rebuild");
  Mem m(root);
  // 固定种子的随机写入（跨平台确定性）
  std::mt19937 rng(42);
  const char* vocab[] = {"袋子", "龙井", "天气", "北京", "西湖", "雨伞"};
  for (int i = 0; i < 40; ++i) {
    const std::string ch = (i % 2 == 0) ? "c1" : "c2";
    const std::string txt = std::string("内容") + vocab[rng() % 6] + "编号" +
                            std::to_string(rng() % 100);
    m.remember("u1", ch, (i % 3 == 1) ? "assistant" : "user", txt, 1700000000 + i * 60);
  }
  m.remember("u1", "c1", "user", "袋子 上 写 了 什么", 1700005000);
  m.remember("u1", "c1", "assistant", "袋子 上 写 着 龙井", 1700005060);
  m.declare("u1", "u1:self", {{"预备槽", "开"}}, 1700005080);  // P2c G1：先开槽
  m.rememberFact("u1", "u1:self", "喜欢的茶", "龙井", 1700005100);
  m.declare("u1", "u1:self", {{"身份", "游客0427"}, {"常去", "西湖"}}, 1700005200);

  const int64_t now = 1700006000;
  const std::vector<std::string> queries = {"袋子", "龙井", "天气 西湖"};
  std::vector<std::vector<EvidenceItem>> r1;
  for (const auto& q : queries) r1.push_back(m.recall(q, "u1", 5, now));
  const std::string p1 = m.profile("u1", now).dump();
  const std::string i1 = m.index().term_count() > 0 ? "nonempty" : "empty";

  // drop + rebuild：索引是可重建缓存，必须完全恢复到逐比特一致
  m.index().drop();
  CHECK(m.index().term_count() == 0);
  m.rebuild_index();
  CHECK(m.index().term_count() > 0);

  for (size_t qi = 0; qi < queries.size(); ++qi) {
    const auto r2 = m.recall(queries[qi], "u1", 5, now);
    CHECK(r2.size() == r1[qi].size());
    for (size_t i = 0; i < r1[qi].size() && i < r2.size(); ++i) {
      CHECK(r2[i].text == r1[qi][i].text);
      CHECK(r2[i].ts == r1[qi][i].ts);
      CHECK(r2[i].score == r1[qi][i].score);  // 逐比特：同序浮点运算
      CHECK(r2[i].source_kind == r1[qi][i].source_kind);
      CHECK(r2[i].ref_obs == r1[qi][i].ref_obs);  // 证据回溯字段同 rebuild 逐比特一致
    }
  }
  CHECK(m.profile("u1", now).dump() == p1);
  (void)i1;
}

// ---------------- ⑤ 视图打包 + 零副作用 ----------------

static void test_view_packaging_and_no_side_effect() {
  Mem m;  // 内存态
  const std::string q = "袋子上写了什么呢？";
  const std::string a = "袋子上写的是龙井茶。";
  m.remember("u1", "c1", "user", q, 1700000000);
  m.remember("u1", "c1", "assistant", a, 1700000060);

  // 打包：user 提问被召回时自动附其下一条 assistant 观测
  const auto r = m.recall("袋子", "u1", 5, 1700000100);
  CHECK(!r.empty());
  bool packed = false;
  for (const auto& e : r) {
    if (e.text.find("→ ") != std::string::npos) {
      CHECK(e.text.find(q) != std::string::npos);  // 问题在包内
      CHECK(e.text.find(a) != std::string::npos);  // 回答在包内
      packed = true;
    }
  }
  CHECK(packed);

  // 证据回溯：ref_obs 指向的观测经 Mem::obs 取回，原文与 meta 一致（dia_id 通道）
  m.remember("u1", "c1", "user", "回溯测试 轮", 1700000120, [] {
    Json meta = Json::object();
    meta.set("dia_id", Json::string("D9:9"));
    return meta;
  }());
  const auto r2 = m.recall("回溯测试", "u1", 5, 1700000200);
  CHECK(!r2.empty());
  CHECK(r2[0].ref_obs != 0);
  const Observation* back = m.obs(r2[0].ref_obs);
  CHECK(back != nullptr);
  CHECK(back->text.find("回溯测试") != std::string::npos);
  const Json* dia = back->meta.find("dia_id");
  CHECK(dia != nullptr && dia->as_str() == "D9:9");

  // 零副作用：synthesize 调用前后观测/事实 replay 逐比特一致
  const std::string obs_before = dump_obs(m.obs_store());
  const std::string fact_before = dump_facts(m.fact_store());
  for (const auto& q2 : {"袋子", "龙井", "不存在的东西"}) {
    (void)m.recall(q2, "u1", 8, 1700000200);
    (void)m.profile("u1", 1700000200);
  }
  CHECK(dump_obs(m.obs_store()) == obs_before);
  CHECK(dump_facts(m.fact_store()) == fact_before);
}

// ---------------- ⑥ 公共域不受影响 ----------------

static void test_public_domain_isolation() {
  const std::string root = temp_root("ame_lstore_isolation");
  Mem m(root);
  m.remember("A", "c1", "user", "A 的私人行程 隐si", 1700000000);
  m.declare("A", "A:self", {{"预备槽", "开"}}, 1700000000);  // P2c G1：先开槽
  m.rememberFact("A", "A:self", "爱好", "集邮", 1700000000);
  m.remember("B", "c1", "user", "B 喜欢 西湖 和 龙井", 1700000100);
  m.declare("B", "B:self", {{"预备槽", "开"}}, 1700000100);  // P2c G1：先开槽
  m.rememberFact("B", "B:self", "爱好", "摄影", 1700000100);
  m.remember("public", "pub", "note", "公共 导览 方案 v1", 1700000200);
  m.declare("public", "public:景点X", {{"开放时间", "8:00-17:00"}}, 1700000200);

  const int64_t now = 1700000300;
  const auto b_recall = m.recall("爱好", "B", 5, now);
  const std::string b_prof = m.profile("B", now).dump();
  const auto p_recall = m.recall("景点X", "public", 5, now);
  const std::string p_prof = m.profile("public", now).dump();
  CHECK(!b_recall.empty());
  CHECK(!p_recall.empty());

  CHECK(m.eraseOwner("A"));
  // A 的分片物理消失
  CHECK(!std::filesystem::exists(root + "/shards/obs/A.jsonl"));
  CHECK(!std::filesystem::exists(root + "/shards/fact/A.jsonl"));

  // B 与 public 的 recall/profile 逐比特不变
  const auto b_recall2 = m.recall("爱好", "B", 5, now);
  CHECK(b_recall2.size() == b_recall.size());
  for (size_t i = 0; i < b_recall.size() && i < b_recall2.size(); ++i) {
    CHECK(b_recall2[i].text == b_recall[i].text);
    CHECK(b_recall2[i].score == b_recall[i].score);
  }
  CHECK(m.profile("B", now).dump() == b_prof);
  const auto p_recall2 = m.recall("景点X", "public", 5, now);
  CHECK(p_recall2.size() == p_recall.size());
  CHECK(m.profile("public", now).dump() == p_prof);

  // public 域不可被个人 erase
  CHECK(!m.eraseOwner("public"));
  CHECK(std::filesystem::exists(root + "/shards/fact/public.jsonl"));
}

// ---------------- ⑦ P2a 兜底召回通道 ----------------

static void test_fallback_recall() {
  Mem m;  // 内存态
  const int64_t T = 1700000000;
  m.remember("u1", "c1", "user", "我喜欢在雨天读科幻小说", T);
  m.remember("u1", "c1", "assistant", "科幻是个好东西", T + 60);
  m.remember("u1", "c1", "user", "周末去爬山拍照", T + 120);
  m.remember("u1", "c1", "assistant", "山上风景特别好", T + 180);

  // 零词面重叠查询（P0 头号缺陷）：不再零证据弃答，返回兜底证据且带 fallback 标记
  const auto r = m.recall("quantum physics trivia", "u1", 5, T + 1000);
  CHECK(!r.empty());
  for (const auto& e : r) CHECK(e.via == "fallback");
  CHECK(r[0].ref_obs != 0);
  const Observation* o0 = m.obs(r[0].ref_obs);
  CHECK(o0 != nullptr);
  CHECK(o0->kind == "user");          // speaker=user 优先
  CHECK(o0->ts == T + 120);           // 同优先级 ts 新鲜在前

  // 正常查询路径行为不变：命中 ≥ k/2 时不启用兜底，通道为 lex/fact
  const auto r2 = m.recall("科幻", "u1", 5, T + 1000);
  CHECK(!r2.empty());
  bool has_lex = false;
  for (const auto& e : r2) {
    CHECK(e.via != "fallback");
    if (e.via == "lex") has_lex = true;
  }
  CHECK(has_lex);

  // 部分命中（0 < 命中 < k/2）：兜底只作尾部补足——首个 fallback 排全部 lex 证据之后
  const auto r3 = m.recall("爬山", "u1", 6, T + 1000);  // 仅 1 条词面命中 < 3
  CHECK(!r3.empty());
  size_t first_fb = r3.size(), last_lex = 0;
  bool seen_fb = false, seen_lex = false;
  for (size_t i = 0; i < r3.size(); ++i) {
    if (r3[i].via == "fallback") { if (!seen_fb) first_fb = i; seen_fb = true; }
    if (r3[i].via == "lex") { last_lex = i; seen_lex = true; }
  }
  CHECK(seen_lex && seen_fb);
  CHECK(last_lex < first_fb);  // 不与正常召回混排竞争

  // 兜底证据同文本去重：词面已命中的观测不以兜底身份重复出现（seq 级跳过）
  for (size_t i = 0; i < r3.size(); ++i)
    for (size_t j = i + 1; j < r3.size(); ++j)
      CHECK(r3[i].ref_obs != r3[j].ref_obs);
}

// ---------------- ⑧ P2b 来源可靠性闭环 ----------------

static void test_feedback_reliability() {
  const std::string root = temp_root("ame_lstore_feedback");
  const int64_t T = 1700000000;
  std::vector<uint64_t> user_seqs, asst_seqs;
  auto mk_ev = [&](uint64_t seq) {
    EvidenceItem ev;
    ev.ref_obs = seq;
    ev.via = "fallback";
    ev.source_kind = "user";  // 以观测实际 kind 为准（feedback 内解析）
    return ev;
  };
  auto rank_of = [&](Mem& mm, const std::string& q, uint64_t seq) -> int {
    const auto r = mm.recall(q, "u1", 10, T + 10000);
    for (size_t i = 0; i < r.size(); ++i)
      if (r[i].ref_obs == seq) return (int)i + 1;
    return 0;
  };
  const std::string q = "completely unrelated zymurgy question";
  {
    Mem m(root);
    for (int i = 0; i < 5; ++i) {
      auto ru = m.remember("u1", "c1", "user", "用户陈述 甲" + std::to_string(i), T + i * 120);
      auto ra = m.remember("u1", "c1", "assistant", "助手回复 乙" + std::to_string(i),
                           T + i * 120 + 60);
      user_seqs.push_back(ru.value.seq);
      asst_seqs.push_back(ra.value.seq);
    }
    // 零重叠 → 全兜底；初始可靠性 user 1.0 > assistant 0.7：最新 user 排第一
    CHECK(rank_of(m, q, user_seqs[4]) == 1);
    CHECK(rank_of(m, q, asst_seqs[4]) == 6);

    // 负反馈（同 1.x 罚>奖语义，−0.05/次）：user 证据 rank 下降，assistant 相对上升
    for (int i = 0; i < 7; ++i) CHECK(m.feedback("u1", mk_ev(user_seqs[4]), false) == Err::Ok);
    CHECK(rank_of(m, q, user_seqs[4]) == 6);   // 下降
    CHECK(rank_of(m, q, asst_seqs[4]) == 1);   // 相对上升

    // 正反馈（+0.02/次）：该来源证据 rank 回升
    for (int i = 0; i < 4; ++i) CHECK(m.feedback("u1", mk_ev(user_seqs[4]), true) == Err::Ok);
    CHECK(rank_of(m, q, user_seqs[4]) == 1);   // 0.65+0.08=0.73 > 0.70

    // 可靠性表当前值核查（封底 0.05 / 封顶 1.0 内）
    CHECK(m.reliability().get("user") > m.reliability().get("assistant"));
    // 持久化文件已落盘
    CHECK(std::filesystem::exists(root + "/shards/reliability.json"));
  }
  // 重启保留：新 Mem 从分片恢复，排序与反馈后一致
  {
    Mem m2(root);
    CHECK(rank_of(m2, q, user_seqs[4]) == 1);
    CHECK(rank_of(m2, q, asst_seqs[4]) == 6);
    CHECK(m2.reliability().get("user") > m2.reliability().get("assistant"));
  }
}

// ---------------- ⑨ P2c declare 原语 + G1–G3 校验门 ----------------

static void test_declare_gates() {
  Mem m;  // 内存态
  const int64_t T = 1700000000;

  // declare 创建槽位 → latest 取 declared 权威值
  CHECK(m.declare("u1", "u1:self", {{"身份", "游客0427"}}, T) == Err::Ok);
  Folded fd;
  CHECK(m.fact_store().latest("u1", "u1:self", "身份", T + 100, fd));
  CHECK(fd.fact.value == "游客0427");
  CHECK(fd.fact.reality == Reality::Declared);

  // G1：槽位不存在 → rememberFact（allocate）拒绝；declare 创建后放行
  CHECK(m.rememberFact("u1", "u1:newslot", "x", "y", T).code == Err::InvalidArg);
  CHECK(m.declare("u1", "u1:newslot", {{"开启", "1"}}, T) == Err::Ok);
  CHECK(m.rememberFact("u1", "u1:newslot", "x", "y", T).code == Err::Ok);

  // G2：attr 空 / attr>64 字 / value>256 字 → 拒绝且不写入
  const size_t fc0 = m.fact_store().owner_count("u1");
  CHECK(m.declare("u1", "u1:self", {{"", "v"}}, T) == Err::InvalidArg);
  CHECK(m.declare("u1", "u1:self", {{std::string(65, 'a'), "v"}}, T) == Err::InvalidArg);
  CHECK(m.declare("u1", "u1:self", {{"ok", std::string(257, 'b')}}, T) == Err::InvalidArg);
  CHECK(m.rememberFact("u1", "u1:self", std::string(65, 'c'), "v", T).code == Err::InvalidArg);
  CHECK(m.rememberFact("u1", "u1:self", "ok", std::string(257, 'd'), T).code == Err::InvalidArg);
  CHECK(m.fact_store().owner_count("u1") == fc0);  // 全部未写入
  // 边界：64 字 attr / 256 字 value（CJK 按码点计）刚好放行
  const std::string cjk_cha = "\xE8\x8C\xB6";  // "茶"
  std::string attr64, val256;
  for (int i = 0; i < 64; ++i) attr64 += cjk_cha;
  for (int i = 0; i < 256; ++i) val256 += cjk_cha;
  CHECK(utf8_check(attr64) == 64 && utf8_check(val256) == 256);  // 辅助断言防拼错
  CHECK(m.declare("u1", "u1:self", {{attr64, val256}}, T) == Err::Ok);

  // G3：个人域写 public 槽位拦截（declare 与 rememberFact 同拦）；public owner 放行
  CHECK(m.declare("u1", "public:景点X", {{"开放时间", "8:00"}}, T) == Err::InvalidArg);
  CHECK(m.rememberFact("u1", "public:景点X", "a", "b", T).code == Err::InvalidArg);
  CHECK(m.declare("public", "public:景点X", {{"开放时间", "8:00"}}, T) == Err::Ok);
  Folded fd2;
  CHECK(m.fact_store().latest("public", "public:景点X", "开放时间", T + 100, fd2));
  CHECK(fd2.fact.value == "8:00");
}

int main() {
  test_erase_semantics();
  test_fold_arbitration();
  test_half_life();
  test_index_rebuild();
  test_view_packaging_and_no_side_effect();
  test_public_domain_isolation();
  test_fallback_recall();
  test_feedback_reliability();
  test_declare_gates();
  return AME_TEST_REPORT();
}
