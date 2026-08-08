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
//   步骤1：从字符串中扫描出前 3 个整数（忽略非数字分隔符）；
//   步骤2：若首段长度为 4 → 按 Y-M-D 解释，否则按 M/D/Y 解释（美式）；
//   步骤3：再尝试扫描时分（第 4/5 个整数）；timegm 转 UTC epoch；
//   步骤4：任何一步失败或数量不足 → 返回 fallback。
TimePoint parse_session_date(const std::string& s, TimePoint fallback) {
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
//          每轮映射 {speaker,text,dia_id}；
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
