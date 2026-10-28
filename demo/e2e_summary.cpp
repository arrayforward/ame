/*
 * E2E 场景验证器（dev-4.0，研究期工具）：C++ 直调 AME + MiniMax-M3 真实 API，
 * 复刻 Android MemorySummaryService 的 summarize/store/fetch/联想/buildContext 全流程。
 * 场景：
 *   S1 知识整理→AME 检索→分层注入→LLM 作答（摘要质量 + AME 检索 + 端到端）
 *   S2 联想检索（直接命中 A，A 的联想词带出相关知识 B）
 *   S3 对话上下文整理→新提问→摘要回忆作答
 * 配置读取 android/local.properties（MINIMAX_API_KEY/BASE_URL/MODEL，key 不打印、
 * 只写入临时 curl 配置文件）。运行：./build-win/demo/ame_e2e_summary（仓库根）。
 */
#include "ame/engine/engine.h"
#include "ame/core/json.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ame;

namespace {

int failures = 0;
// UTF-8 码点计数（L1 长度断言按字符而非字节，避免中文 3 字节误判）
size_t utf8_len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}
#define CHECK(cond, name)                                            \
  do {                                                               \
    if (cond) { std::printf("  PASS %s\n", name); }                  \
    else { std::printf("  FAIL %s\n", name); ++failures; }           \
  } while (0)

std::map<std::string, std::string> read_config() {
  std::map<std::string, std::string> cfg;
  std::ifstream f("android/local.properties");
  std::string line;
  while (std::getline(f, line)) {
    auto p = line.find('=');
    if (p != std::string::npos && !line.empty() && line[0] != '#')
      cfg[line.substr(0, p)] = line.substr(p + 1);
  }
  return cfg;
}

// curl 子进程 POST（key 只落临时配置文件，不进命令行/日志）
std::string http_post(const std::string& url, const std::string& key, const std::string& body) {
  auto dir = std::filesystem::temp_directory_path();
  auto bodyf = dir / "ame_e2e_body.json";
  auto cfgf = dir / "ame_e2e_curl.cfg";
  auto outf = dir / "ame_e2e_resp.json";
  {
    std::ofstream(bodyf) << body;
    std::ofstream cfg(cfgf);
    cfg << "header = \"Content-Type: application/json\"\n"
        << "header = \"Authorization: Bearer " << key << "\"\n";
  }
  std::string cmd = "curl -sS -m 240 -K \"" + cfgf.string() + "\" --data-binary @\"" +
                    bodyf.string() + "\" \"" + url + "\" -o \"" + outf.string() + "\"";
  int rc = std::system(cmd.c_str());
  if (rc != 0) throw std::runtime_error("curl 失败 rc=" + std::to_string(rc));
  std::ifstream in(outf);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string chat(const std::map<std::string, std::string>& cfg, const std::string& system,
                 const std::string& user, int max_tokens) {
  Json root = Json::object();
  root["model"] = Json::string(cfg.at("MINIMAX_MODEL"));
  root["max_tokens"] = Json::number(max_tokens);
  Json msgs = Json::array();
  Json m1 = Json::object();
  m1["role"] = Json::string("system");
  m1["content"] = Json::string(system);
  Json m2 = Json::object();
  m2["role"] = Json::string("user");
  m2["content"] = Json::string(user);
  msgs.push(m1);
  msgs.push(m2);
  root["messages"] = msgs;
  std::string base = cfg.at("MINIMAX_BASE_URL");
  while (!base.empty() && base.back() == '/') base.pop_back();
  std::string resp = http_post(base + "/chat/completions", cfg.at("MINIMAX_API_KEY"), root.dump());
  bool ok = false;
  Json jr = Json::parse(resp, &ok);
  if (!ok) throw std::runtime_error("响应非 JSON: " + resp.substr(0, 200));
  const Json* choices = jr.find("choices");
  if (!choices || choices->arr.empty()) throw std::runtime_error("无 choices: " + resp.substr(0, 300));
  const Json* msg = choices->arr[0].find("message");
  if (!msg) throw std::runtime_error("无 message");
  const Json* content = msg->find("content");
  return content ? content->as_str() : "";
}

struct Summary {
  std::string l1, l2, l3;
  std::vector<std::string> keywords;  std::string toJson() const {
    Json j = Json::object();
    j["l1"] = Json::string(l1);
    j["l2"] = Json::string(l2);
    Json ka = Json::array();
    for (auto& k : keywords) ka.push(Json::string(k));
    j["keywords"] = ka;
    j["l3"] = Json::string(l3);
    return j.dump();
  }
};

// 与 Android MemorySummaryService.summarize 完全同 prompt/降级策略
Summary summarize(const std::map<std::string, std::string>& cfg, const std::string& source,
                  const std::string& question) {
  std::string system =
      "你是记忆整理器。把给定材料整理成严格 JSON（不要输出任何其他文字）："
      "{\"l1\":\"不超过60字的基础摘要，言简意赅\","
      "\"l2\":\"不超过300字的详细整理，面向难以理解的问题给出要点、因果和时序\","
      "\"keywords\":[\"3-6个联想词，用于扩展检索相关知识，名词优先\"]}。"
      "要求：l1 必须提炼关键事实（人物/时间/地点/数字/结论），不得只复述首句；"
      "keywords 必填且不得为空数组。";
  std::string user = (question.empty() ? "" : "当前问题：" + question + "\n") + "材料：\n" + source;
  Summary s;
  s.l3 = source;
  try {
    std::string text = chat(cfg, system, user, 4096);
    size_t a = text.find('{'), b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a)
      throw std::runtime_error("摘要响应无 JSON 对象");
    bool ok = false;
    Json j = Json::parse(text.substr(a, b - a + 1), &ok);
    if (!ok) throw std::runtime_error("摘要 JSON 解析失败");
    if (const Json* v = j.find("l1")) s.l1 = v->as_str();
    if (const Json* v = j.find("l2")) s.l2 = v->as_str();
    if (const Json* v = j.find("keywords"))
      for (auto& k : v->arr) s.keywords.push_back(k.as_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  [降级] 摘要失败: %s\n", e.what());
    s.l1 = source.substr(0, 60);
    s.l2 = source.substr(0, 300);
  }
  return s;
}

std::vector<const Node*> recall(Engine& e, const std::string& q, int limit) {
  SeedQuery sq;
  sq.text = q;
  sq.lex_topk = 30;
  sq.soft_min_sim = 0.2;
  auto rr = e.recall(sq, {}, 2, limit, false);
  std::vector<const Node*> out;
  for (auto& ev : rr.events)
    if (ev.event) out.push_back(ev.event);
  return out;
}

bool any_contains(Engine& e, const std::string& q, const std::string& needle, int limit = 5) {
  for (auto* n : recall(e, q, limit))
    if (n->text.find(needle) != std::string::npos) return true;
  return false;
}

void store(Engine& e, const Summary& s, long ts) {
  auto r = e.remember_turn("summary", "MEMSUM:" + s.toJson(), ts);
  std::printf("  入图 rc=%d | L1(%zu字): %s\n  联想词: ", (int)r.code, s.l1.size(), s.l1.c_str());
  for (auto& k : s.keywords) std::printf("[%s] ", k.c_str());
  std::printf("\n  L2(%zu字): %.120s\n", s.l2.size(), s.l2.c_str());
}

}  // namespace

int main() {
  auto cfg = read_config();
  if (!cfg.count("MINIMAX_API_KEY")) {
    std::fprintf(stderr, "缺 android/local.properties MINIMAX_API_KEY\n");
    return 1;
  }
  long ts = 1700000000;

  // ---------- S1+S2：企业知识库（三篇文档） ----------
  std::string docA =
      "公司远程办公管理制度（2024修订）：自2024年3月起，全体员工可申请远程办公。"
      "申请须经直属主管审批，审批通过后报人事部备案。远程期间每周到岗不少于1天，"
      "考勤采用线上打卡，每日9:30前完成。连续远程超过30天需重新审批。";
  std::string docB =
      "考勤与请假实施细则：员工请假分事假、病假、年假三类。事假需提前2个工作日"
      "在OA系统申请，病假需提供医院证明。线上打卡异常须在3个工作日内补登，"
      "逾期按旷工处理。年假按工龄5-15天递增。";
  std::string docC =
      "产品发布编年史：2021年首发AME原型；2022年加入图扩散检索；2023年落地"
      "手机端会话隔离记忆；2024年引入三级摘要与联想检索。";

  std::printf("\n===== S1 知识整理 → AME 检索 → 分层注入 → LLM 作答 =====\n");
  Engine e1;
  Summary sa = summarize(cfg, docA, "");
  std::printf("[文档A 远程办公]\n");
  CHECK(!sa.l1.empty() && utf8_len(sa.l1) <= 60, "S1 A L1 非空且言简（≤60字符）");
  CHECK(sa.keywords.size() >= 2 && sa.keywords.size() <= 8, "S1 A 联想词数量 2-8");
  CHECK(sa.l3 == docA, "S1 A L3 保原文");
  store(e1, sa, ts + 1);
  Summary sb = summarize(cfg, docB, "");
  std::printf("[文档B 考勤请假]\n");
  store(e1, sb, ts + 2);
  Summary sc = summarize(cfg, docC, "");
  std::printf("[文档C 编年史]\n");
  store(e1, sc, ts + 3);

  CHECK(any_contains(e1, "远程办公申请需要谁审批", "远程办公"), "S1 AME 检索：审批问题命中 A（MEMSUM）");
  CHECK(any_contains(e1, "请假需要提前几天", "请假"), "S1 AME 检索：请假问题命中 B");

  // 分层注入：L1 上下文应远小于 L3 全量
  std::string l1ctx = "【摘要】" + sa.l1;
  std::string l3ctx = "【摘要】" + sa.l1 + "\n【详述】" + sa.l2 + "\n【原文】" + sa.l3;
  CHECK(l1ctx.size() < l3ctx.size() / 3, "S1 分层注入：L1 上下文 < 全量 1/3");

  std::string ans = chat(cfg,
      "你是企业制度问答助手，只依据所给摘要回答，简洁中文。",
      "摘要：\n" + l1ctx + "\n问题：远程办公申请需要谁审批？超过30天怎么办？", 1024);
  std::printf("  LLM(L1 注入) 作答: %s\n", ans.c_str());
  CHECK(ans.find("主管") != std::string::npos, "S1 端到端：L1 摘要支撑答出审批人");
  CHECK(ans.find("30") != std::string::npos, "S1 端到端：L1 摘要支撑答出 30 天条款");

  // ---------- S2 联想检索：直接命中 A，联想词带出 B ----------
  std::printf("\n===== S2 联想检索 =====\n");
  bool direct_b = any_contains(e1, "远程办公", "请假");
  std::printf("  直接查询'远程办公'命中 B(考勤请假)=%d（预期多数为 false，靠联想扩展）\n", direct_b);
  bool assoc_b = false;
  for (auto& kw : sa.keywords) {
    bool hit = any_contains(e1, kw, "请假", 3);
    std::printf("  联想词[%s] 命中 B=%d\n", kw.c_str(), hit);
    if (hit) assoc_b = true;
  }
  CHECK(assoc_b, "S2 联想效果：A 的联想词能检索出相关知识 B");

  // ---------- S3 对话上下文整理 ----------
  std::printf("\n===== S3 对话上下文整理 → 摘要回忆 =====\n");
  std::string conv =
      "user: 我国庆想去青岛玩五天，帮我规划一下。\n"
      "assistant: 可以安排栈桥、八大关、崂山三条线，住市南区方便。\n"
      "user: 预算大概四千，住民宿就行，10月2日出发。\n"
      "assistant: 好，10月2日高铁到青岛站，前两天市区，第三天崂山，第五天返程。\n";
  Engine e3;
  Summary s3 = summarize(cfg, conv, "");
  std::printf("[对话摘要]\n");
  store(e3, s3, ts + 100);
  CHECK(any_contains(e3, "我国庆原计划去哪", "青岛"), "S3 AME 检索：提问命中对话摘要");
  std::string ans3 = chat(cfg, "你是个人记忆助手，只依据摘要回答。",
                          "摘要：\n【摘要】" + s3.l1 + "\n问题：我国庆原计划去哪？什么时候出发？预算多少？", 1024);
  std::printf("  LLM(L1 注入) 作答: %s\n", ans3.c_str());
  CHECK(ans3.find("青岛") != std::string::npos, "S3 端到端：答出目的地");
  CHECK(ans3.find("10月2日") != std::string::npos || ans3.find("10月2") != std::string::npos,
        "S3 端到端：答出出发日期");

  // ---------- S4 渐进式上下文：首问 L1 简答省 token，追问用联想结果+L2 ----------
  std::printf("\n===== S4 渐进式：首问 L1 → 追问 联想结果+L2 =====\n");
  std::string q1ctx = "【摘要】" + sa.l1;
  std::string ans41 = chat(cfg, "你是企业制度助手，只依据摘要简答，一两句话。",
                           q1ctx + "\n问题：远程办公要谁审批？", 512);
  std::printf("  首问(L1，上下文 %zu 字符) 作答: %s\n", q1ctx.size(), ans41.c_str());
  CHECK(ans41.find("主管") != std::string::npos, "S4 首问 L1 简答正确");
  // 追问：联想词结果 + L2（A 与 B 的详述，不给原文）
  std::string follow = "那连续远程40天、打卡异常3天没补怎么办？";
  std::string l2ctx = "【A 详述】" + sa.l2 + "\n";
  bool got_b = false;
  for (auto& kw : sa.keywords) {
    for (auto* n : recall(e1, kw, 3)) {
      if (n->text.find("请假") != std::string::npos) got_b = true;
    }
  }
  if (got_b) l2ctx += "【B 详述】" + sb.l2 + "\n";
  l2ctx += "【直接检索补充】";
  for (auto* n : recall(e1, follow, 3)) l2ctx += n->text.substr(0, 400) + "\n";
  std::string ans42 = chat(cfg, "你是企业制度助手，只依据所给详述回答，分点简洁作答。",
                           l2ctx + "\n问题：" + follow, 1024);
  std::printf("  追问(联想+L2，上下文 %zu 字符，含B=%d) 作答: %s\n", l2ctx.size(), got_b, ans42.c_str());
  CHECK(ans42.find("重新") != std::string::npos, "S4 追问：答出 40 天需重新审批（A-L2）");
  CHECK(ans42.find("补登") != std::string::npos || ans42.find("旷工") != std::string::npos,
        "S4 追问：答出打卡异常处理（联想带出 B-L2）");
  std::string fullctx = docA + docB;
  std::printf("  token 账：首问 L1=%zu 字符 / 追问 L2=%zu 字符 / 全量原文=%zu 字符\n",
              q1ctx.size(), l2ctx.size(), fullctx.size());

  // ---------- S5 追问判定器 + 阶段状态机（与 Android FollowupGate 同规则） ----------
  std::printf("\n===== S5 追问判定与阶段切换（零 LLM） =====\n");
  {
    auto tokens_of = [](const std::string& s) {
      std::vector<std::string> out;
      std::string ascii, cjk;
      auto flush = [&] {
        if (ascii.size() >= 3) out.push_back(ascii);
        ascii.clear();
      };
      for (unsigned char c : s) {
        if (c < 128 && std::isalnum(c)) ascii += (char)std::tolower(c);
        else {
          flush();
          if (c >= 128) {
            cjk += (char)c;
            // UTF-8 中文 3 字节/字：每 6 字节取一个二元组
            if (cjk.size() >= 6) {
              out.push_back(cjk.substr(cjk.size() - 6));
              cjk = cjk.substr(3);
            }
          } else cjk.clear();
        }
      }
      flush();
      return out;
    };
    auto overlap = [&](const std::string& a, const std::string& b) {
      auto ta = tokens_of(a), tb = tokens_of(b);
      int n = 0;
      for (auto& t : ta)
        for (auto& u : tb)
          if (t == u) { ++n; break; }
      return n;
    };
    auto is_followup = [&](const std::string& q, const std::string& prev) {
      if (prev.empty() || q.empty()) return false;
      if (q.find("换个话题") != std::string::npos || q.find("新话题") != std::string::npos) return false;
      if (q.size() <= 90) {  // Java length()≈字符数×3（中文 UTF-8），阈值同比例放宽
        const char* marks[] = {"那", "还有", "继续", "详细", "具体", "为什么", "怎么办", "呢", "吗", "再"};
        for (auto* m : marks)
          if (q.find(m) != std::string::npos) return true;
      }
      return q.size() <= 120 && overlap(q, prev) >= 2;
    };
    auto is_newtopic = [&](const std::string& q, const std::string& prev) {
      if (prev.empty() || q.empty()) return false;
      if (q.find("换个话题") != std::string::npos || q.find("新话题") != std::string::npos) return true;
      return q.size() > 36 && overlap(q, prev) == 0 &&
             q.find("呢") == std::string::npos && q.find("吗") == std::string::npos;
    };
    std::string stage = "SIMPLE", prev;
    auto onTurn = [&](const std::string& q) {
      if (is_followup(q, prev)) stage = "DEEP";
      else if (is_newtopic(q, prev)) stage = "SIMPLE";
      prev = q;
      return stage;
    };
    CHECK(onTurn("远程办公要谁审批？") == "SIMPLE", "S5 首问→SIMPLE");
    CHECK(onTurn("那连续40天怎么办？") == "DEEP", "S5 短句'那…怎么办'→DEEP");
    CHECK(onTurn("详细说说打卡异常的处理") == "DEEP", "S5 '详细说说'→DEEP");
    CHECK(onTurn("换个话题，产品发布历史讲讲") == "SIMPLE", "S5 显式换题→SIMPLE");
    std::string l1c = "【摘要】" + sa.l1, l2c = "【详述】" + sa.l2;
    CHECK(l1c.size() < l2c.size(), "S5  SIMPLE 上下文恒小于 DEEP（token 递减方向正确）");
  }

  std::printf("\n===== 汇总：%s（失败 %d 项）=====\n", failures == 0 ? "全部通过" : "有失败项", failures);
  return failures == 0 ? 0 : 1;
}
