/*
 * 所属模块：bench 基准适配层——三个基准的数据加载器。
 * 本文件主体思路：用 core 极简 JSON 解析器读取 LoCoMo / LongMemEval / BEAM 的
 *   官方 JSON 结构，映射到统一 BenchCase；对字段缺失宽容（默认值），结构异常跳过该条。
 *   LoCoMo 的 category 按不透明整数处理（名称映射属配置，不在加载器内解释语义）。
 * 关键算法/数据结构：递归取值辅助函数；会话日期解析用简单的整数扫描（Y-M-D / M/D/Y）
 *   + timegm 转 UTC epoch。
 * 依赖关系：依赖 core(json.h/types.h)；产出 BenchCase 供 ingest/CLI 使用。
 */
#include "ame/bench/bench.h"

#include "ame/core/json.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace ame {

// 读取整个文件并解析为 JSON；失败返回 Null
static Json read_json(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *ok = false; return {}; }
  std::stringstream ss;
  ss << f.rdbuf();
  return Json::parse(ss.str(), ok);
}

static std::string jstr(const Json* j, const char* key) {
  if (!j) return "";
  const Json* v = j->find(key);
  return v ? v->as_str() : "";
}

// parse_session_date 伪代码：
//   步骤0（月名格式优先）：串中含英文月名（jan..dec，词边界匹配）→ 按 D-Mon-Y 解析
//          （LoCoMo："1:56 pm on 8 May, 2023" / "May 8, 2023"）——月名不在时数字
//          扫描会把时分 1:56 误判为月/日导致整体解析失败落入合成时间
//          （locomo_v3 cat2 judge 0.072 的根因：全部会话时间戳合成）；
//          年取首个 ≥1970 的 4 位数，日取月名紧邻的 1~31 数字（前邻优先，后邻次之），
//          时刻取 "H:MM" 冒号对 + am/pm 修正，无时刻默认正午；
//   步骤1：从字符串中扫描出前 3 个整数（忽略非数字分隔符）；
//   步骤2：若首段长度为 4 → 按 Y-M-D 解释，否则按 M/D/Y 解释（美式）；
//   步骤3：再尝试扫描时分（第 4/5 个整数）；timegm 转 UTC epoch；
//   步骤4：任何一步失败或数量不足 → 返回 fallback。
TimePoint parse_session_date(const std::string& s, TimePoint fallback) {
  // 步骤0：月名格式（D Mon Y / Mon D Y）
  {
    static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    static const char* kMonthsFull[] = {
        "january", "february", "march",     "april",   "may",      "june",
        "july",    "august",   "september", "october", "november", "december"};
    std::string sl;
    for (char c : s) sl += (char)std::tolower((unsigned char)c);
    int mon = 0;
    size_t mpos = std::string::npos, mlen = 0;
    for (int m = 0; m < 12 && mon == 0; ++m) {
      size_t p = 0;
      while ((p = sl.find(kMonths[m], p)) != std::string::npos) {
        bool lb = p == 0 || !std::isalpha((unsigned char)sl[p - 1]);
        // 右边界：缩写命中要求后继非字母；全名命中（july/sept 等）按全名长度要求
        size_t fl = std::strlen(kMonthsFull[m]);
        bool full = sl.compare(p, fl, kMonthsFull[m]) == 0;
        size_t end = p + (full ? fl : 3);
        bool rb = end >= sl.size() || !std::isalpha((unsigned char)sl[end]);
        if (lb && rb) { mon = m + 1; mpos = p; mlen = full ? fl : 3; break; }
        ++p;
      }
    }
    if (mon > 0) {
      // 扫描全部整数及其位置（时分识别用冒号，am/pm 修正时刻）
      std::vector<std::pair<int, size_t>> nums;  // (值, 起始位置)
      for (size_t i = 0; i < sl.size();) {
        if (std::isdigit((unsigned char)sl[i])) {
          size_t st = i;
          int v = 0;
          while (i < sl.size() && std::isdigit((unsigned char)sl[i]))
            v = v * 10 + (sl[i++] - '0');
          nums.emplace_back(v, st);
        } else {
          ++i;
        }
      }
      int year = 0, day = 0;
      for (auto& [v, p] : nums)
        if (v >= 1970 && v <= 2100) { year = v; break; }
      // 日：月名前邻数字（"8 May"）优先，后邻（"May 8"）次之
      for (auto& [v, p] : nums)
        if (p < mpos && v >= 1 && v <= 31) day = v;  // 取月名前最后一个候选
      if (day == 0)
        for (auto& [v, p] : nums)
          if (p >= mpos + mlen && v >= 1 && v <= 31 && v != year) { day = v; break; }
      if (year == 0 || day == 0) return fallback;
      // 时刻："H:MM" 冒号对（pm 且 H<12 → +12；am 且 H==12 → 0）
      int hour = 12, minute = 0;
      for (size_t k = 0; k + 1 < nums.size(); ++k) {
        size_t colon = nums[k].second + std::to_string(nums[k].first).size();
        if (colon < sl.size() && sl[colon] == ':') {
          hour = nums[k].first;
          minute = nums[k + 1].first;
          bool pm = sl.find("pm", colon) != std::string::npos;
          bool am = sl.find("am", colon) != std::string::npos;
          if (pm && hour < 12) hour += 12;
          if (am && hour == 12) hour = 0;
          break;
        }
      }
      if (hour > 23 || minute > 59) return fallback;
      std::tm tm{};
      tm.tm_year = year - 1900;
      tm.tm_mon = mon - 1;
      tm.tm_mday = day;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      return (TimePoint)timegm(&tm);
    }
  }
  // 步骤1~4：纯数字格式（Y-M-D / M/D/Y）
  int nums[5] = {0};
  int n = 0;
  size_t i = 0;
  while (i < s.size() && n < 5) {
    if (std::isdigit((unsigned char)s[i])) {
      int v = 0;
      while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
      nums[n++] = v;
    } else {
      ++i;
    }
  }
  if (n < 3) return fallback;
  int year, mon, day;
  if (nums[0] > 31) { year = nums[0]; mon = nums[1]; day = nums[2]; }  // Y-M-D
  else { mon = nums[0]; day = nums[1]; year = nums[2]; }              // M/D/Y
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31) return fallback;
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = n >= 4 ? nums[3] : 12;  // 无时刻默认正午，避免时区边界
  tm.tm_min = n >= 5 ? nums[4] : 0;
  return (TimePoint)timegm(&tm);
}

// load_locomo 伪代码：
//   步骤1：解析顶层列表；逐样本取 conversation 对象；
//   步骤2：按 session_N 顺序收集会话（date 取 session_N_date_time），
//          每轮映射 {speaker,text,dia_id}；可选 tags 数组以 " [tags: ...]" 附入 text；
//   步骤3：qa 列表逐条映射 {question,answer,category(不透明整数),evidence(dia_id 列表)}；
//   步骤4：字段缺失用默认值、结构异常跳过该条。
std::vector<BenchCase> load_locomo(const std::string& path) {
  std::vector<BenchCase> cases;
  bool ok = false;
  Json root = read_json(path, &ok);
  if (!ok || root.kind != Json::Arr) return cases;
  for (auto& sample : root.arr) {
    BenchCase c;
    c.conversation_id = jstr(&sample, "sample_id");
    if (c.conversation_id.empty()) c.conversation_id = "locomo_" + std::to_string(cases.size());
    const Json* conv = sample.find("conversation");
    if (conv) {
      for (int si = 1; si <= 100; ++si) {  // session_1..session_N 顺序收集
        std::string key = "session_" + std::to_string(si);
        const Json* sess = conv->find(key);
        if (!sess || sess->kind != Json::Arr) {
          if (si > 1) break;  // 会话编号连续，断号即止
          continue;
        }
        BenchSession bs;
        bs.id = key;
        bs.date = jstr(conv, (key + "_date_time").c_str());
        for (auto& turn : sess->arr) {
          BenchTurn t;
          t.speaker = jstr(&turn, "speaker");
          t.text = jstr(&turn, "text");
          t.dia_id = jstr(&turn, "dia_id");
          // 写时语义增强（write-time enrichment spike）：轮内可选 "tags"（字符串数组，
          // enrich_corpus.py 离线产出）→ 以 " [tags: ...]" 附于 text 末尾。挂载理由：
          // 词法通道与终排 BM25 都按事件文本切词，附在文尾即天然命中，引擎核心零改动；
          // 代价是标签词参与 BM25 计分（干扰风险 spike 内接受，见
          // docs/reports/write_time_enrichment.md 蓝方项）；若 spike 转正应改为
          // 事件 attr 专用标签通道（仅查询扩展侧使用，不进内容分）。
          const Json* tags = turn.find("tags");
          if (tags && tags->kind == Json::Arr) {
            std::string extra;
            for (auto& tg : tags->arr) {
              std::string s = tg.as_str();
              if (s.empty()) continue;
              extra += (extra.empty() ? "" : ", ") + s;
            }
            if (!extra.empty()) t.text += " [tags: " + extra + "]";
          }
          if (!t.text.empty()) bs.turns.push_back(std::move(t));
        }
        c.sessions.push_back(std::move(bs));
      }
    }
    const Json* qa = sample.find("qa");
    if (qa) {
      for (auto& q : qa->arr) {
        BenchQuestion bq;
        bq.id = c.conversation_id + ":q" + std::to_string(c.questions.size());
        bq.question = jstr(&q, "question");
        bq.answer = jstr(&q, "answer");
        const Json* cat = q.find("category");
        bq.category = cat ? (int)cat->as_num() : 0;  // 不透明整数
        const Json* ev = q.find("evidence");
        if (ev)
          for (auto& e : ev->arr) bq.evidence_dia_ids.push_back(e.as_str());
        bq.qtype = "category_" + std::to_string(bq.category);
        if (bq.question.empty()) continue;
        c.questions.push_back(std::move(bq));
      }
    }
    cases.push_back(std::move(c));
  }
  return cases;
}

// load_longmemeval 伪代码：
//   步骤1：顶层列表每条为一个题目（haystack 内联），一题映射为一个 BenchCase；
//   步骤2：haystack_sessions（会话列表，每会话为 {role,content} 轮列表）与
//          haystack_session_ids/haystack_dates 按下标对齐；
//   步骤3：question_type 为 "abstention" 的题目标 unanswerable=true；
//   步骤4：answer_session_ids 作为证据会话保留。
std::vector<BenchCase> load_longmemeval(const std::string& path) {
  std::vector<BenchCase> cases;
  bool ok = false;
  Json root = read_json(path, &ok);
  if (!ok || root.kind != Json::Arr) return cases;
  for (auto& item : root.arr) {
    BenchCase c;
    c.conversation_id = jstr(&item, "question_id");
    const Json* sess_ids = item.find("haystack_session_ids");
    const Json* dates = item.find("haystack_dates");
    const Json* sessions = item.find("haystack_sessions");
    if (sessions) {
      for (size_t i = 0; i < sessions->arr.size(); ++i) {
        BenchSession bs;
        if (sess_ids && i < sess_ids->arr.size()) bs.id = sess_ids->arr[i].as_str();
        if (dates && i < dates->arr.size()) bs.date = dates->arr[i].as_str();
        for (auto& turn : sessions->arr[i].arr) {
          BenchTurn t;
          t.speaker = jstr(&turn, "role");
          t.text = jstr(&turn, "content");
          if (!t.text.empty()) bs.turns.push_back(std::move(t));
        }
        c.sessions.push_back(std::move(bs));
      }
    }
    BenchQuestion bq;
    bq.id = c.conversation_id;
    bq.qtype = jstr(&item, "question_type");
    bq.question = jstr(&item, "question");
    bq.answer = jstr(&item, "answer");
    bq.question_date = jstr(&item, "question_date");
    const Json* asi = item.find("answer_session_ids");
    if (asi)
      for (auto& s : asi->arr) bq.answer_session_ids.push_back(s.as_str());
    bq.unanswerable = (bq.qtype == "abstention");
    c.questions.push_back(std::move(bq));
    cases.push_back(std::move(c));
  }
  return cases;
}

// load_beam 伪代码（宽容加载：官方结构以描述为准，键名做多种兼容）：
//   步骤1：顶层列表每条为一段对话：会话取 "sessions"（[{date,turns:[{role|speaker,content|text}]}]），
//          缺失时尝试 "dialogue" 平铺轮列表；
//   步骤2：问题列表取 "questions"/"probes"，ability 键兼容 "ability"/"type"/"question_type"；
//   步骤3：nuggets 缺失时以 answer 整体作单 nugget；ability=="abstention" 标 unanswerable；
//   步骤4：结构异常跳过该条。
std::vector<BenchCase> load_beam(const std::string& path) {
  std::vector<BenchCase> cases;
  bool ok = false;
  Json root = read_json(path, &ok);
  if (!ok || root.kind != Json::Arr) return cases;
  for (auto& item : root.arr) {
    BenchCase c;
    c.conversation_id = jstr(&item, "conversation_id");
    if (c.conversation_id.empty())
      c.conversation_id = "beam_" + std::to_string(cases.size());
    const Json* sessions = item.find("sessions");
    if (sessions) {
      for (auto& s : sessions->arr) {
        BenchSession bs;
        bs.date = jstr(&s, "date");
        bs.id = jstr(&s, "id");
        const Json* turns = s.find("turns");
        if (turns) {
          for (auto& turn : turns->arr) {
            BenchTurn t;
            t.speaker = jstr(&turn, "role");
            if (t.speaker.empty()) t.speaker = jstr(&turn, "speaker");
            t.text = jstr(&turn, "content");
            if (t.text.empty()) t.text = jstr(&turn, "text");
            t.dia_id = jstr(&turn, "dia_id");
            if (!t.text.empty()) bs.turns.push_back(std::move(t));
          }
        }
        c.sessions.push_back(std::move(bs));
      }
    }
    const Json* qs = item.find("questions");
    if (!qs) qs = item.find("probes");
    if (qs) {
      for (auto& q : qs->arr) {
        BenchQuestion bq;
        bq.id = jstr(&q, "id");
        bq.qtype = jstr(&q, "ability");
        if (bq.qtype.empty()) bq.qtype = jstr(&q, "type");
        if (bq.qtype.empty()) bq.qtype = jstr(&q, "question_type");
        bq.question = jstr(&q, "question");
        bq.answer = jstr(&q, "answer");
        bq.question_date = jstr(&q, "question_date");
        const Json* ng = q.find("nuggets");
        if (ng)
          for (auto& n : ng->arr) bq.nuggets.push_back(n.as_str());
        if (bq.nuggets.empty() && !bq.answer.empty()) bq.nuggets.push_back(bq.answer);
        const Json* ev = q.find("evidence");
        if (ev)
          for (auto& e : ev->arr) bq.evidence_dia_ids.push_back(e.as_str());
        bq.unanswerable = (bq.qtype == "abstention");
        if (bq.question.empty()) continue;
        c.questions.push_back(std::move(bq));
      }
    }
    cases.push_back(std::move(c));
  }
  return cases;
}

}  // namespace ame
