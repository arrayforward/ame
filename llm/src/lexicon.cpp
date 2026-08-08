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

}  // namespace ame
