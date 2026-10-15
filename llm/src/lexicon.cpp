/*
 * 所属模块：llm 接入层——词典蒸馏（bench 词典构建器）。
 * 本文件主体思路：从词频表取高频内容词，批量调 LLM（关思考，JSON 输出）蒸馏
 *   synonyms/antonyms/related，产出版本化 lexicon.json；宽容解析 + 失败批重试。
 *   人名等专有名词由 prompt 约定返回空词条（实体走实体通道，不进词典）。
 * 关键算法/数据结构：50 词/批；复用 components.cpp 的截断抢救解析思路（此处对象级）。
 * 依赖关系：依赖 llm.h（ILlmClient）、core(json/log)；被 bench CLI 依赖。
 */
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ame/core/json.h"
#include "ame/core/log.h"
#include "ame/llm/components.h"

namespace ame {

// 词典单条：word + 同义/反义/强关联
struct LexEntry {
  std::string word;
  std::vector<std::string> synonyms;
  std::vector<std::string> antonyms;
  std::vector<std::pair<std::string, double>> related;  // (word, score)
};

// 词频表读取伪代码：支持多个词频文件（{"conv_vocab_top":[[w,f]...],...} 结构），
//   合并词频（同词跨文件取频次和）后按频次降序取 top_n。
static std::vector<std::string> top_vocab(const std::vector<std::string>& vocab_paths,
                                          int top_n) {
  std::unordered_map<std::string, double> freq;
  for (auto& vocab_path : vocab_paths) {
    std::ifstream f(vocab_path);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    bool ok = false;
    Json j = Json::parse(ss.str(), &ok);
    if (!ok) continue;
    for (const char* key : {"conv_vocab_top", "question_vocab_top"}) {
      const Json* arr = j.find(key);
      if (!arr) continue;
      for (auto& pair : arr->arr) {
        if (pair.arr.size() < 2) continue;
        std::string w = pair.arr[0].as_str();
        // 过滤：含撇号的缩略形式/过短词不蒸馏
        if (w.size() < 3 || w.find('\'') != std::string::npos) continue;
        freq[w] += pair.arr[1].as_num();
      }
    }
  }
  if (freq.empty()) return {};
  std::vector<std::pair<double, std::string>> v;
  for (auto& [w, f2] : freq) v.emplace_back(f2, w);
  std::sort(v.begin(), v.end(), std::greater<>());
  std::vector<std::string> out;
  for (size_t i = 0; i < v.size() && (int)i < top_n; ++i) out.push_back(v[i].second);
  return out;
}

// 词条解析伪代码：逐词取 syn_strong/syn_near/antonyms（兼容旧版 synonyms/related 字段），
//   缺失按空处理（宽容）。
static LexEntry parse_entry(const std::string& word, const Json& j) {
  LexEntry e;
  e.word = word;
  auto list_of_str = [&](const Json* arr) {
    std::vector<std::string> out;
    if (arr)
      for (auto& s : arr->arr) out.push_back(s.as_str());
    return out;
  };
  e.synonyms = list_of_str(j.find("syn_strong"));
  if (e.synonyms.empty()) e.synonyms = list_of_str(j.find("synonyms"));  // 旧版兼容
  e.antonyms = list_of_str(j.find("antonyms"));
  const Json* near = j.find("syn_near");
  if (!near) near = j.find("related");  // 旧版 related 视作近义
  if (near) {
    for (auto& r : near->arr) {
      const Json* w = r.find("word");
      const Json* s = r.find("score");
      if (w) e.related.emplace_back(w->as_str(), s ? s->as_num(0.5) : 0.5);
    }
  }
  return e;
}

// 词条序列化辅助（三分流格式：syn_strong/syn_near/antonyms）
static Json entry_to_json(const LexEntry& e) {
  Json je = Json::object();
  je["word"] = Json::string(e.word);
  Json js = Json::array();
  for (auto& s : e.synonyms) js.push(Json::string(s));
  je["syn_strong"] = std::move(js);
  Json ja = Json::array();
  for (auto& s : e.antonyms) ja.push(Json::string(s));
  je["antonyms"] = std::move(ja);
  Json jr = Json::array();
  for (auto& [w, sc] : e.related) {
    Json r = Json::object();
    r["word"] = Json::string(w);
    r["score"] = Json::number(sc);
    jr.push(std::move(r));
  }
  je["syn_near"] = std::move(jr);
  return je;
}

// 整表落盘辅助：version/top_n + entries 数组
static void write_lexicon(const std::string& out_path, int top_n, const Json& entries) {
  Json root = Json::object();
  root["version"] = Json::string("v1");
  root["top_n"] = Json::number(top_n);
  root["entries"] = entries;
  std::ofstream f(out_path + ".tmp", std::ios::binary | std::ios::trunc);
  f << root.dump();
  f.close();
  std::remove(out_path.c_str());  // Windows rename 不可覆盖已存在目标，先删再改名
  std::rename((out_path + ".tmp").c_str(), out_path.c_str());  // 原子落盘
}

// build_lexicon_json（增量模式）伪代码：
//   步骤1：读 out_path 已有词条（断点续跑），建立已蒸馏词集合；读词频表取 top_n；
//   步骤2：过滤掉已蒸馏词，剩余按 batch 分批构造 prompt
//         （专有人名/地名返回空词条；词表文件结构任意词频表均可，与具体数据集无关）；
//   步骤3：chat_batch 并发调用；失败批单批重试一次；
//   步骤4：每批解析后立即合并进 entries 并整表落盘（中断不丢已蒸馏部分）；
//   步骤5：返回本次新增词条数；打印覆盖率（已蒸馏/目标词表）。
int build_lexicon_json(ILlmClient& client, const std::vector<std::string>& vocab_paths,
                       const std::string& out_path, int top_n, int batch, int concurrency) {
  Json entries = Json::array();
  std::unordered_set<std::string> have;
  {
    std::ifstream f(out_path);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      bool ok = false;
      Json old = Json::parse(ss.str(), &ok);
      if (ok) {
        const Json* es = old.find("entries");
        if (es)
          for (auto& e : es->arr) {
            entries.push(e);
            const Json* w = e.find("word");
            if (w) have.insert(w->as_str());
          }
      }
    }
  }
  auto words_all = top_vocab(vocab_paths, top_n);
  if (words_all.empty()) return -1;
  std::vector<std::string> words;
  for (auto& w : words_all)
    if (!have.count(w)) words.push_back(w);
  AME_LOGI("lexicon: 目标 %zu 词，已蒸馏 %zu，本次新增 %zu", words_all.size(), have.size(),
           words.size());
  if (words.empty()) {
    write_lexicon(out_path, top_n, entries);
    return 0;
  }

  // 分块处理（每 100 批一轮：chat_batch → 解析 → 落盘），真断点续跑 + 进度可见 +
  // 失败隔离；缓存保证重跑零重复扣费。
  std::vector<std::vector<std::string>> batches;
  for (size_t i = 0; i < words.size(); i += (size_t)batch)
    batches.emplace_back(words.begin() + i, words.begin() + std::min(i + (size_t)batch, words.size()));
  const size_t kChunk = 100;
  int got = 0, failed_batches = 0;
  for (size_t c0 = 0; c0 < batches.size(); c0 += kChunk) {
    size_t c1 = std::min(c0 + kChunk, batches.size());
    std::vector<ChatRequest> reqs;
    for (size_t i = c0; i < c1; ++i) {
      std::string user = "为下列每个英文词生成词典条目。输出 JSON 对象：词为键，值为 "
                         "{\"syn_strong\":[同义词，含词形变化如 painting→paint], "
                         "\"syn_near\":[{\"word\":近义/上下位/强关联词,\"score\":0~1}], "
                         "\"antonyms\":[反义词]}。"
                         "syn_strong 只放可互换的真同义；近义但不可互换的放 syn_near。"
                         "专有名词（人名/地名/品牌）返回空词条。只输出 JSON。\n词表：\n";
      for (auto& w : batches[i]) user += w + "\n";
      ChatOptions o;
      o.json = true;
      o.max_tokens = 4096;
      reqs.push_back({{{"system", "你是词典编纂器，严格输出 JSON。"}, {"user", user}}, o});
    }
    auto resps = client.chat_batch(reqs, concurrency);
    // 失败批重试一次
    std::vector<int> retry_idx;
    for (size_t i = 0; i < resps.size(); ++i)
      if (resps[i].empty()) retry_idx.push_back((int)i);
    if (!retry_idx.empty()) {
      std::vector<ChatRequest> r2;
      for (int i : retry_idx) r2.push_back(reqs[i]);
      auto res2 = client.chat_batch(r2, concurrency);
      for (size_t k = 0; k < retry_idx.size(); ++k) resps[retry_idx[k]] = res2[k];
    }
    for (size_t i = 0; i < resps.size(); ++i) {
      size_t a = resps[i].find('{');
      size_t b2 = resps[i].rfind('}');
      if (a == std::string::npos || b2 <= a) { ++failed_batches; continue; }
      bool ok = false;
      Json obj = Json::parse(resps[i].substr(a, b2 - a + 1), &ok);
      if (!ok || obj.kind != Json::Obj) { ++failed_batches; continue; }
      for (auto& [word, ej] : obj.obj) {
        entries.push(entry_to_json(parse_entry(word, ej)));
        ++got;
      }
    }
    write_lexicon(out_path, top_n, entries);  // 每轮原子落盘
    AME_LOGI("lexicon: 进度 %zu/%zu 批（%.1f%%），失败批 %d", c1, batches.size(),
             100.0 * c1 / batches.size(), failed_batches);
  }
  AME_LOGI("lexicon: 本次新增 %d 条，总覆盖率 %zu/%zu，失败批 %d", got,
           have.size() + got, words_all.size(), failed_batches);
  return got;
}


// ---------------- 复活工程第四轮 M2：strong 流可互换性校验 ----------------
// validate_lexicon_json 伪代码（断点续跑 + 批量缓存）：
//   步骤1：读 in_path 全部词条；读 out_path 已有词条（若存在）建立已完成 word 集合；
//   步骤2：未完成词条按 batch 分批，每批把 strong 对序列化为 "A|B" 列表进 prompt，
//         要求 LLM 严格 JSON 数组回 [{"a":..,"b":..,"ok":true/false},...]；
//   步骤3：chat_batch 并发；失败批重试一次；仍失败则该批 fail-open 保留
//         （缓存使重跑零成本，宁可留噪声不丢真同义）；
//   步骤4：每批后合并：entry 的 syn_strong 只留 ok=true 的对，整表原子落盘
//         （source=distilled、validated=true；near/antonyms 原样保留）；
//   步骤5：返回处理 pair 总数。
int validate_lexicon_json(ILlmClient& client, const std::string& in_path,
                          const std::string& out_path, int batch, int concurrency) {
  std::ifstream fi(in_path);
  if (!fi) return -1;
  std::stringstream ss;
  ss << fi.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return -1;
  const Json* entries = root.find("entries");
  if (!entries) return -1;

  Json out = Json::object();
  out["version"] = Json::string("v1");
  out["source"] = Json::string("distilled");
  out["validated"] = Json::boolean(true);
  Json out_entries = Json::array();
  std::unordered_set<std::string> done;
  {
    std::ifstream fo(out_path);
    if (fo) {
      std::stringstream ss2;
      ss2 << fo.rdbuf();
      bool ok2 = false;
      Json old = Json::parse(ss2.str(), &ok2);
      if (ok2) {
        const Json* es = old.find("entries");
        if (es)
          for (auto& e : es->arr) {
            out_entries.push(e);
            const Json* w = e.find("word");
            if (w) done.insert(w->as_str());
          }
      }
    }
  }

  // 待处理：未完成词条的 (word, strong 对) 扁平序列，按 batch 对分组
  struct PairItem {
    std::string word, a, b;
  };
  std::vector<PairItem> pending;
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    if (!w || w->as_str().empty() || done.count(w->as_str())) continue;
    const Json* strong = e.find("syn_strong");
    if (!strong) continue;
    for (auto& s : strong->arr)
      if (!s.as_str().empty()) pending.push_back({w->as_str(), w->as_str(), s.as_str()});
  }
  // 整表落盘（带 source/validated 根字段；Windows rename 覆盖先删）
  auto dump_out = [&]() {
    out["entries"] = out_entries;
    std::ofstream f(out_path + ".tmp", std::ios::binary | std::ios::trunc);
    f << out.dump();
    f.close();
    std::remove(out_path.c_str());
    std::rename((out_path + ".tmp").c_str(), out_path.c_str());
  };

  const int total_pairs = (int)pending.size();
  AME_LOGI("validate: 目标 pair %zu（已完成 word %zu），批大小 %d", pending.size(),
           done.size(), batch);
  if (pending.empty()) {
    dump_out();
    return 0;
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> keep;  // word -> 保留对
  std::unordered_map<std::string, std::vector<std::string>> word_strong;  // 原始顺序回填
  std::unordered_map<std::string, const Json*> src_entry;
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    if (!w) continue;
    src_entry[w->as_str()] = &e;
    const Json* strong = e.find("syn_strong");
    if (strong)
      for (auto& s : strong->arr) word_strong[w->as_str()].push_back(s.as_str());
  }

  int failed_batches = 0;
  for (size_t c0 = 0; c0 < pending.size(); c0 += (size_t)batch * 100) {
    size_t c1 = std::min(c0 + (size_t)batch * 100, pending.size());
    std::vector<ChatRequest> reqs;
    std::vector<std::pair<size_t, size_t>> req_span;  // (起始, 结束) 对区间
    for (size_t i = c0; i < c1; i += (size_t)batch) {
      size_t j = std::min(i + (size_t)batch, c1);
      std::string user =
          "You are a strict synonym validator. For each pair A|B below, answer: in most "
          "sentences, can A be directly replaced by B without changing the meaning? "
          "Reply with a JSON array only: [{\"a\":A,\"b\":B,\"ok\":true/false},...] "
          "covering every pair in order.\nPairs:\n";
      for (size_t k = i; k < j; ++k) user += pending[k].a + "|" + pending[k].b + "\n";
      ChatOptions o;
      o.json = true;
      o.max_tokens = 4096;
      reqs.push_back({{{"system", "你是严格的同义词校验器，只输出 JSON。"}, {"user", user}}, o});
      req_span.emplace_back(i, j);
    }
    auto resps = client.chat_batch(reqs, concurrency);
    std::vector<int> retry_idx;
    for (size_t i = 0; i < resps.size(); ++i)
      if (resps[i].empty()) retry_idx.push_back((int)i);
    if (!retry_idx.empty()) {
      std::vector<ChatRequest> r2;
      for (int i : retry_idx) r2.push_back(reqs[i]);
      auto res2 = client.chat_batch(r2, concurrency);
      for (size_t k = 0; k < retry_idx.size(); ++k) resps[retry_idx[k]] = res2[k];
    }
    for (size_t i = 0; i < resps.size(); ++i) {
      size_t a = req_span[i].first, b = req_span[i].second;
      size_t p1 = resps[i].find('['), p2 = resps[i].rfind(']');
      bool parsed = false;
      if (p1 != std::string::npos && p2 > p1) {
        bool ok2 = false;
        Json arr = Json::parse(resps[i].substr(p1, p2 - p1 + 1), &ok2);
        if (ok2 && arr.kind == Json::Arr) {
          parsed = true;
          std::unordered_set<std::string> verdicts;  // "a|b" -> keep（只记 yes）
          for (auto& v : arr.arr) {
            const Json* ja = v.find("a");
            const Json* jb = v.find("b");
            const Json* jok = v.find("ok");
            if (!ja || !jb || !jok) continue;
            if (jok->kind == Json::Bool && jok->b) verdicts.insert(ja->as_str() + "|" + jb->as_str());
          }
          for (size_t k = a; k < b; ++k) {
            std::string key = pending[k].a + "|" + pending[k].b;
            if (verdicts.count(key)) keep[pending[k].word].insert(pending[k].b);
          }
        }
      }
      if (!parsed) {
        ++failed_batches;  // fail-open：该批对全部保留
        for (size_t k = a; k < b; ++k) keep[pending[k].word].insert(pending[k].b);
      }
    }
    // 本 chunk 覆盖到的 word 生成 out entry（保持原始词条序）
    std::unordered_set<std::string> touched;
    for (size_t k = c0; k < c1; ++k) touched.insert(pending[k].word);
    for (auto& e : entries->arr) {
      const Json* w = e.find("word");
      if (!w || !touched.count(w->as_str())) continue;
      Json ne = Json::object();
      ne["word"] = Json::string(w->as_str());
      Json js = Json::array();
      for (auto& s : word_strong[w->as_str()])
        if (keep[w->as_str()].count(s)) js.push(Json::string(s));
      ne["syn_strong"] = std::move(js);
      const Json* ant = e.find("antonyms");
      Json ja = Json::array();
      if (ant) for (auto& s : ant->arr) ja.push(Json::string(s.as_str()));
      ne["antonyms"] = std::move(ja);
      const Json* near = e.find("syn_near");
      Json jn = Json::array();
      if (near) for (auto& r : near->arr) jn.push(r);
      ne["syn_near"] = std::move(jn);
      out_entries.push(std::move(ne));
      done.insert(w->as_str());
    }
    dump_out();
    AME_LOGI("validate: 进度 %zu/%zu pair（失败批 %d）", c1, pending.size(), failed_batches);
  }
  // 无 strong 对的词条原样带过（在 in 中且未进 out 的）
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    if (!w || done.count(w->as_str())) continue;
    out_entries.push(e);
  }
  dump_out();
  AME_LOGI("validate: 完成 pair %d，失败批 %d（fail-open 保留）", total_pairs,
           failed_batches);
  (void)src_entry;
  return total_pairs;
}
}  // namespace ame
