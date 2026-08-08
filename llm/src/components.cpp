/*
 * 所属模块：llm 接入层——三个 LLM 组件实现。
 * 本文件主体思路：prompt 模板 + 宽容解析。所有解析失败路径都回退（规则提取器/保守值），
 *   保证管线鲁棒；任何异常输出只记日志不中断批量评测。
 * 关键算法/数据结构：JSON 截取（首个 '[' 到末个 ']'）后交 core Json 解析；
 *   nugget 评分解析同样宽容（缺失按 0 计）。
 * 依赖关系：依赖 llm.h、core(json/log)；被 bench CLI 依赖。
 */
#include "ame/llm/components.h"

#include "ame/core/json.h"
#include "ame/core/log.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ame {

LlmExtractor::LlmExtractor(ILlmClient* client, IExtractor* fallback)
    : client_(client), fallback_(fallback) {}

// 宽容 JSON 解析辅助：截取首个 start_ch 到末个 end_ch 之间的子串再 parse
static Json parse_tolerant(const std::string& resp, char start_ch, char end_ch) {
  size_t a = resp.find(start_ch);
  size_t b = resp.rfind(end_ch);
  if (a == std::string::npos || b == std::string::npos || b <= a) return {};
  bool ok = false;
  Json j = Json::parse(resp.substr(a, b - a + 1), &ok);
  return ok ? j : Json{};
}

// 截断抢救解析（finish_reason=length 等导致的半截 JSON 数组）：
// 从首个 '[' 起按花括号深度扫描，完整闭合的顶层对象逐个单独 parse 后收成数组；
// 尾部不完整对象丢弃。一个完整对象都没有则返回 Null（调用方回退）。
static Json parse_array_salvage(const std::string& resp) {
  size_t a = resp.find('[');
  if (a == std::string::npos) return {};
  Json out = Json::array();
  int depth = 0;
  size_t obj_start = std::string::npos;
  for (size_t i = a + 1; i < resp.size(); ++i) {
    char ch = resp[i];
    if (ch == '"') {  // 跳过字符串内容（含转义）
      for (++i; i < resp.size() && resp[i] != '"'; ++i)
        if (resp[i] == '\\') ++i;
      continue;
    }
    if (ch == '{') {
      if (depth == 0) obj_start = i;
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0 && obj_start != std::string::npos) {
        bool ok = false;
        Json obj = Json::parse(resp.substr(obj_start, i - obj_start + 1), &ok);
        if (ok) out.arr.push_back(std::move(obj));
        obj_start = std::string::npos;
      }
    } else if (ch == ']' && depth == 0) {
      break;
    }
  }
  return out;
}

// 把一条 JSON 提取结果映射为 Extraction（event.text 由调用方填原文）
static Extraction json_to_extraction(const Json& j) {
  Extraction ex;
  const Json* type = j.find("type");
  ex.event.type = type ? type->as_str("generic") : "generic";
  static const char* kCats[] = {"action", "attr", "topic", "sentiment", "concept"};
  auto norm_cat = [&](std::string c) {
    for (auto* k : kCats)
      if (c == k) return c;
    return std::string("attr");  // 未知类别归并到 attr
  };
  const Json* ents = j.find("entities");
  if (ents) {
    for (auto& e : ents->arr) {
      ExtractedEntity ee;
      const Json* s = e.find("surface");
      ee.surface = s ? s->as_str() : "";
      const Json* r = e.find("role");
      ee.role = r ? r->as_str("人物") : "人物";
      const Json* t = e.find("type");
      ee.entity_type = t ? t->as_str("person") : "person";
      if (!ee.surface.empty()) ex.entities.push_back(std::move(ee));
    }
  }
  const Json* kws = j.find("keywords");
  if (kws) {
    for (auto& k : kws->arr) {
      ExtractedKeyword ek;
      const Json* w = k.find("word");
      ek.word = w ? w->as_str() : "";
      const Json* c = k.find("category");
      ek.category = norm_cat(c ? c->as_str() : "");
      if (!ek.word.empty()) ex.keywords.push_back(std::move(ek));
    }
  }
  // 属性三元组（实体档案槽位）：{"entity","slot","value"}
  const Json* attrs = j.find("attributes");
  if (attrs) {
    for (auto& a : attrs->arr) {
      ExtractedAttribute ea;
      const Json* en = a.find("entity");
      const Json* sl = a.find("slot");
      const Json* va = a.find("value");
      ea.entity = en ? en->as_str() : "";
      ea.slot = sl ? sl->as_str() : "";
      ea.value = va ? va->as_str() : "";
      if (!ea.entity.empty() && !ea.slot.empty() && !ea.value.empty())
        ex.attributes.push_back(std::move(ea));
    }
  }
  return ex;
}

// extract_session 伪代码：
//   步骤1：无 client 或 turns 为空 → 逐轮回退规则提取器；
//   步骤2：构造批量 prompt（编号列出各轮 speaker:text，要求输出 JSON 数组，
//          每轮 {type, entities[{surface,role,type}], keywords[{word,category}]}）；
//   步骤3：调用 LLM，宽容解析数组；按下标对齐各轮（event.text 填原文）；
//   步骤4：解析失败或数量不符 → 该轮次回退规则提取器，记日志。
std::vector<Extraction> LlmExtractor::extract_session(
    const std::vector<std::pair<std::string, std::string>>& turns) {
  std::vector<Extraction> out(turns.size());
  if (!client_ || turns.empty()) {
    for (size_t i = 0; i < turns.size(); ++i) out[i] = fallback_->extract(turns[i].second);
    return out;
  }
  std::string user = "对以下对话轮逐轮提取记忆要素。共 " + std::to_string(turns.size()) +
                     " 轮，输出 JSON 数组且必须正好 " + std::to_string(turns.size()) +
                     " 个对象，顺序与输入一致。每轮一个对象：\n"
                     "{\"type\":事件类型, \"entities\":[{\"surface\":名称,\"role\":主体|人物|客体,"
                     "\"type\":person|object|org}], \"keywords\":[{\"word\":词,"
                     "\"category\":action|attr|topic|sentiment|concept}], "
                     "\"attributes\":[{\"entity\":实体名,\"slot\":槽位,\"value\":值}]}。\n"
                     "attributes 提取人物属性事实（居住地 residence、职业 job、家庭成员 "
                     "family_xxx、偏好 preference、拥有物 owns_xxx 等，slot 用小写蛇形词），"
                     "没有则给空数组。关键词一律小写。"
                     "直接输出 JSON 数组，不要解释，不要省略任何一轮。对话轮：\n";
  for (size_t i = 0; i < turns.size(); ++i) {
    user += std::to_string(i) + ". " + turns[i].first + ": " + turns[i].second + "\n";
  }
  ChatOptions opts;
  opts.json = true;
  opts.max_tokens = 8192;  // 推理模型：max_tokens 含思考预算
  std::string resp = client_->chat({{"system", "你是记忆要素提取器，严格输出 JSON。"},
                                    {"user", user}},
                                   opts);
  Json arr = parse_tolerant(resp, '[', ']');
  if (arr.kind != Json::Arr) arr = parse_array_salvage(resp);  // 截断抢救
  if (arr.kind == Json::Arr && !arr.arr.empty()) {
    // 部分对齐：按下标映射命中的轮次，缺失/超出的轮回退规则提取器
    for (size_t i = 0; i < turns.size(); ++i) {
      if (i < arr.arr.size()) {
        out[i] = json_to_extraction(arr.arr[i]);
        out[i].event.text = turns[i].second;
      } else {
        out[i] = fallback_->extract(turns[i].second);
        out[i].event.text = turns[i].second;
      }
    }
    if (arr.arr.size() != turns.size())
      AME_LOGW("LlmExtractor: 数组长度 %zu 与轮数 %zu 不符，缺失轮回退规则提取",
               arr.arr.size(), turns.size());
  } else {
    AME_LOGW("LlmExtractor: 解析失败，整会话回退规则提取器");
    for (size_t i = 0; i < turns.size(); ++i) {
      out[i] = fallback_->extract(turns[i].second);
      out[i].event.text = turns[i].second;
    }
  }
  // speaker 保证为主体实体（LLM 可能漏提）
  for (size_t i = 0; i < turns.size(); ++i) {
    if (turns[i].first.empty()) continue;
    bool has = false;
    for (auto& e : out[i].entities)
      if (e.surface == turns[i].first) { has = true; break; }
    if (!has)
      out[i].entities.insert(out[i].entities.begin(),
                             {turns[i].first, "主体", "person", {}});
  }
  return out;
}

// extract 伪代码：复用 extract_session（单轮），保证接口语义一致。
Extraction LlmExtractor::extract(const std::string& text) {
  auto v = extract_session({{"", text}});
  return v.empty() ? fallback_->extract(text) : v[0];
}

// LlmAnswerer::build_request 伪代码：
//   步骤1：system 指令——根据证据回答问题，简短作答，证据不足只回答 NO INFORMATION；
//   步骤2：user 消息拼接 提问日期（若有）、编号证据列表（带时间戳）、问题。
ChatRequest LlmAnswerer::build_request(const std::string& question,
                                       const std::string& question_date,
                                       const std::vector<std::string>& evidence_texts) const {
  std::string user;
  if (!question_date.empty()) user += "提问日期: " + question_date + "\n";
  user += "相关记忆证据：\n";
  if (evidence_texts.empty()) user += "（无）\n";
  for (size_t i = 0; i < evidence_texts.size(); ++i)
    user += std::to_string(i + 1) + ". " + evidence_texts[i] + "\n";
  user += "问题: " + question + "\n简短回答；仅当证据与问题完全无关时才回答 NO INFORMATION。";
  return {{{"system",
            "你是个人记忆问答助手。只根据给出的证据回答问题，答案简短（几个词到一句话）。"
            "若证据中包含\"综合摘要\"，它仅用于定位相关原文——答案中的具体数值/日期/名称"
            "必须以原文片段为准，不得引用摘要中的改写表述。"
            "若多条证据涉及同一事实但时间不同，以时间最新的事实为准（知识更新场景）。"
            "证据部分相关时，基于证据给出最可能的简短答案（允许合理推断）；"
            "仅当证据与问题完全无关或缺失时，才只输出 NO INFORMATION。"},
           {"user", user}},
          // 答题需要多跳/时序推理：保留思考（实测关思考会过度弃答），max_tokens 含思考预算
          {0.0, 2048, false, false}};
}

// is_abstain 伪代码：小写化后查找 "no information" 子串。
bool LlmAnswerer::is_abstain(const std::string& resp) {
  std::string low;
  for (char c : resp) low += (char)std::tolower((unsigned char)c);
  return low.find("no information") != std::string::npos;
}

// is_aggregate_question 伪代码：通用聚合模式匹配（枚举/计数/求和/列举意图）。
bool LlmAnswerer::is_aggregate_question(const std::string& question) {
  std::string ql;
  for (char c : question) ql += (char)std::tolower((unsigned char)c);
  static const char* kAgg[] = {"how many",  "number of", "total",    "all the",
                               "list",      "every",     "each of",  "types of",
                               "kinds of",  "多少",      "几个",     "几",
                               "所有",      "哪些",      "一共",     "总共"};
  for (auto* w : kAgg)
    if (ql.find(w) != std::string::npos) return true;
  return false;
}

// build_extract_request 伪代码（阶段一·逐项提取）：
//   单条证据 → "这条证据中与问题相关的事实是什么？一句话内作答，无关则答 NONE"，
//   关思考加速（提取是机械任务）。
ChatRequest LlmAnswerer::build_extract_request(const std::string& question,
                                               const std::string& evidence_text) {
  std::string user = "问题: " + question + "\n证据片段: " + evidence_text +
                     "\n这条证据中与问题相关的事实是什么？一句话内作答；无关则只答 NONE。";
  ChatOptions o;
  o.max_tokens = 512;
  o.disable_thinking = false;  // 实测关思考会过度答 NONE（与答题器同因），保留思考
  return {{{"system", "你是事实提取器，只输出事实或 NONE。"}, {"user", user}}, o};
}

// build_extract_batch_request 伪代码（阶段一·批量提取，降本核心）：
//   n 条编号证据一次喂入，要求逐条输出 "[E#]: 事实" 或 "[E#]: NONE"；保留思考
//   （提取需要关联推理，实测关思考会过度 NONE）。
ChatRequest LlmAnswerer::build_extract_batch_request(
    const std::string& question, const std::vector<std::string>& evidence_texts) {
  std::string user = "问题: " + question + "\n编号证据片段:\n";
  for (size_t i = 0; i < evidence_texts.size(); ++i)
    user += "[E" + std::to_string(i + 1) + "] " + evidence_texts[i] + "\n";
  user += "逐条判断：该证据中与问题相关的事实是什么？每条输出一行 "
          "\"[E编号]: 事实（一句话内）\"；无关则输出 \"[E编号]: NONE\"。"
          "不要遗漏编号，不要输出其他内容。";
  ChatOptions o;
  o.max_tokens = 1024;
  o.disable_thinking = false;  // 保留思考（关思考会过度 NONE）
  return {{{"system", "你是事实提取器，逐条输出事实或 NONE。"}, {"user", user}}, o};
}

// parse_extract_batch 伪代码：逐行匹配 "[E数字]:" 前缀，编号在 [1,n] 且内容非 NONE
//   非空则收录；编号越界/缺失/重复宽容跳过（重复编号取首次）。
std::vector<std::pair<int, std::string>> LlmAnswerer::parse_extract_batch(
    const std::string& resp, int n_evidence) {
  std::vector<std::pair<int, std::string>> out;
  std::vector<bool> seen(n_evidence, false);
  std::istringstream ss(resp);
  std::string line;
  while (std::getline(ss, line)) {
    size_t lb = line.find("[E");
    if (lb == std::string::npos) continue;
    size_t rb = line.find("]:", lb);
    if (rb == std::string::npos) continue;
    int idx = std::atoi(line.substr(lb + 2, rb - lb - 2).c_str());
    if (idx < 1 || idx > n_evidence || seen[idx - 1]) continue;
    std::string fact = line.substr(rb + 2);
    while (!fact.empty() && (fact.back() == ' ' || fact.back() == '\r')) fact.pop_back();
    size_t ws = fact.find_first_not_of(' ');
    if (ws != std::string::npos) fact = fact.substr(ws);
    if (fact.empty() || is_none(fact)) continue;
    seen[idx - 1] = true;
    out.emplace_back(idx - 1, fact);
  }
  return out;
}

// is_none 伪代码：前缀 NONE 或空 → 无关。
bool LlmAnswerer::is_none(const std::string& resp) {
  std::string low;
  for (char c : resp)
    if (!std::isspace((unsigned char)c)) low += (char)std::tolower((unsigned char)c);
  return low.empty() || low.rfind("none", 0) == 0;
}

// build_aggregate_request 伪代码（阶段二·聚合作答）：
//   逐项事实列表（带编号，不含原文长片段）→ 基于列表计数/求和/枚举作答；
//   列表为空 → 直接返回 NO INFORMATION（调用方拦截，不发请求）。
ChatRequest LlmAnswerer::build_aggregate_request(const std::string& question,
                                                 const std::string& question_date,
                                                 const std::vector<std::string>& facts) const {
  std::string user = "问题: " + question + "\n";
  if (!question_date.empty()) user += "提问日期: " + question_date + "\n";
  user += "逐项确认的事实（每条独立核实过）：\n";
  for (size_t i = 0; i < facts.size(); ++i)
    user += std::to_string(i + 1) + ". " + facts[i] + "\n";
  user += "基于上述事实列表回答问题（计数/求和/枚举），答案简短；"
          "若事实列表与问题无关，只答 NO INFORMATION。";
  return {{{"system",
            "你是聚合问答器：只根据给定的事实列表作答，计数与求和以列表条目为准，"
            "不要臆造列表之外的事实。"},
           {"user", user}},
          {0.0, 1024, false, false}};
}

// LlmJudge::judge 伪代码（BEAM nugget 口径）：
//   步骤1：无 client/无 nugget → 返回全 0；
//   步骤2：prompt 列出 nugget 编号，要求输出 JSON 数组，每项 0/0.5/1；
//   步骤3：宽容解析；数量不符或失败 → 按 0 兜底。
std::vector<double> LlmJudge::judge(const std::string& question, const std::string& answer,
                                    const std::vector<std::string>& nuggets) {
  std::vector<double> scores(nuggets.size(), 0.0);
  if (!client_ || nuggets.empty()) return scores;
  std::string user = "问题: " + question + "\n回答: " + answer + "\n评分点:\n";
  for (size_t i = 0; i < nuggets.size(); ++i)
    user += std::to_string(i) + ". " + nuggets[i] + "\n";
  user += "逐条判断回答是否覆盖评分点：覆盖=1，部分覆盖=0.5，未覆盖=0。"
          "只输出 JSON 数组，如 [1,0.5,0]。";
  ChatOptions opts;
  opts.json = true;
  opts.max_tokens = 1024;  // 推理模型：含思考预算
  std::string resp = client_->chat(
      {{"system", "你是严格的答案评审，只输出 JSON。"}, {"user", user}}, opts);
  Json arr = parse_tolerant(resp, '[', ']');
  if (arr.kind == Json::Arr) {
    for (size_t i = 0; i < nuggets.size() && i < arr.arr.size(); ++i) {
      double v = arr.arr[i].as_num();
      scores[i] = v >= 0.75 ? 1.0 : (v >= 0.25 ? 0.5 : 0.0);
    }
  } else {
    AME_LOGW("LlmJudge: nugget 解析失败，按 0 兜底");
  }
  return scores;
}

// correctness 伪代码（LoCoMo/LongMemEval 口径）：
//   步骤1：prompt 给出问题/参考答案/模型回答，要求只答 yes 或 no；
//   步骤2：parse_yes_no 解析，yes→1.0，否则 0.0。
double LlmJudge::correctness(const std::string& question, const std::string& gold,
                             const std::string& pred) {
  if (!client_) return 0.0;
  std::string user = "问题: " + question + "\n参考答案: " + gold + "\n模型回答: " + pred +
                     "\n模型回答是否与参考答案语义一致（允许措辞不同）？只回答 yes 或 no。";
  ChatOptions opts;
  opts.max_tokens = 512;  // 推理模型：含思考预算
  std::string resp = client_->chat(
      {{"system", "你是答案评审，只输出 yes 或 no。"}, {"user", user}}, opts);
  return parse_yes_no(resp) ? 1.0 : 0.0;
}

// parse_yes_no 伪代码：去空白小写化，前缀 yes → true，其余 → false。
bool LlmJudge::parse_yes_no(const std::string& resp) {
  std::string low;
  for (char c : resp)
    if (!std::isspace((unsigned char)c)) low += (char)std::tolower((unsigned char)c);
  return low.rfind("yes", 0) == 0;
}

}  // namespace ame
