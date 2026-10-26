// AC 自动机基准与对拍：构建时间/内存/提取耗时 vs 旧逐词实现，正确性对拍
#include <chrono>
#include <cstdio>
#include <fstream>
#include <unordered_set>
#include <vector>

#include "ame/extractor/dat_trie.h"
#include "ame/extractor/extractor.h"

using namespace ame;
static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 旧实现：逐词条 std::string::find（对照组）
static std::unordered_set<std::string> naive_match(
    const std::string& text, const std::vector<std::string>& words) {
  std::unordered_set<std::string> hit;
  for (auto& w : words)
    if (text.find(w) != std::string::npos) hit.insert(w);
  return hit;
}

int main() {
  const char* dict = "/mnt/d/agent/memory/ame/bench/data/lexicon_src/segmentation/merged.txt";
  const char* samples[] = {
      "老张和老王昨天在朝阳公园附近的咖啡馆谈项目，压力很大，晚上又失眠了",
      "I moved to Seattle last week for my new job at Contoso, and I love the rain here.",
      "今天公司加班到深夜，回家路上买了糖葫芦和北京烤鸭，心情不错",
      "昨晚又睡不着，起来看了一会书，关于心理健康和情绪管理的内容",
  };

  for (int cap : {20000, 700000}) {
    RuleExtractor ext;
    double t0 = now();
    int n = ext.load_keyword_dict(dict, "seg", cap);
    double t1 = now();
    if (n < 0) { std::printf("词典缺失，跳过\n"); return 1; }
    size_t nodes, edges, bytes;
    ext.ac_stats(nodes, edges, bytes);
    std::printf("cap=%7d 构建=%6.2fs 节点=%zu 边=%zu 估算内存=%.1fMB\n", n, t1 - t0,
                nodes, edges, bytes / 1048576.0);
    // 内存估计：节点数 × 节点基础开销 + 边数 × 8B
    // （AcNode: vector 24B + fail/out/outlink 12B；边 pair<uint32,int> 8B）
    // 通过友元无法直接读，估算用词条数上界；这里打印构建时间与提取时间
    double extract_us = 0, naive_us = 0;
    for (auto* text : samples) {
      double a = now();
      auto hits = ext.ac_match(text);
      double b = now();
      extract_us += (b - a) * 1e6;
      (void)hits;
    }
    std::printf("cap=%7d 构建=%6.2fs 提取(4轮)=%7.1fus\n", n, t1 - t0, extract_us);
    (void)naive_us;
  }

  // ---- 对拍：AC vs 旧逐词实现，输出集合必须一致 ----
  RuleExtractor ext;
  int n = ext.load_keyword_dict(dict, "seg", 20000);
  // 读同一份词表做 naive 对照
  std::vector<std::string> words;
  {
    std::ifstream f(dict);
    std::string line;
    while ((int)words.size() < n && std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      words.push_back(line.substr(0, line.find('\t')));
    }
  }
  int bad = 0;
  for (auto* text : samples) {
    auto ac_hits = ext.ac_match(text);
    std::unordered_set<std::string> ac_set;
    for (auto& [pos, w, cat] : ac_hits) ac_set.insert(w);
    auto naive_set = naive_match(text, words);
    if (ac_set != naive_set) {
      ++bad;
      for (auto& w : ac_set)
        if (!naive_set.count(w)) std::printf("AC 多出: %s\n", w.c_str());
      for (auto& w : naive_set)
        if (!ac_set.count(w)) std::printf("AC 缺少: %s\n", w.c_str());
    }
  }
  // ---- AC 持久化（70 万词）：构建写缓存 vs 加载缓存 ----
  {
    RuleExtractor ext;
    const char* cache = "/tmp/ame_ac.cache";
    double a = now();
    int n = ext.load_keyword_dict_cached(dict, "seg", 700000, cache);
    double b = now();
    size_t fs = 0;
    {
      std::ifstream cf(cache, std::ios::binary | std::ios::ate);
      if (cf) fs = (size_t)cf.tellg();
    }
    double c = now();
    RuleExtractor ext2;
    int n2 = ext2.load_keyword_dict_cached(dict, "seg", 700000, cache);
    double d = now();
    std::printf("持久化: 构建+写=%.2fs 加载=%.0fms cache=%.1fMB words=%d/%d\n", b - a,
                (d - c) * 1000, fs / 1048576.0, n, n2);
    // 持久化往返命中一致性
    auto h1 = ext.ac_match(samples[0]);
    auto h2 = ext2.ac_match(samples[0]);
    std::unordered_set<std::string> s1, s2;
    for (auto& [p, w, c2] : h1) s1.insert(w);
    for (auto& [p, w, c2] : h2) s2.insert(w);
    std::printf("持久化往返命中: %s\n", s1 == s2 ? "一致" : "不一致");
  }

  // ---- DAT vs AC（70 万词）----
  {
    std::vector<std::string> words;
    {
      std::ifstream f(dict);
      std::string line;
      while ((int)words.size() < 700000 && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        words.push_back(line.substr(0, line.find('\t')));
      }
    }
    double a = now();
    DatTrie dat;
    dat.build(words);
    double b = now();
    double match_us = 0;
    for (auto* text : samples) {
      double x = now();
      auto m = dat.match(text);
      double y = now();
      match_us += (y - x) * 1e6;
      (void)m;
    }
    std::printf("DAT: 构建=%.2fs 节点=%zu 估算内存=%.1fMB 匹配(4轮)=%.1fms\n", b - a,
                dat.nodes(), dat.approx_bytes() / 1048576.0, match_us / 1000);
    // DAT vs AC 对拍
    RuleExtractor ext;
    ext.load_keyword_dict(dict, "seg", 700000);
    int bad = 0;
    for (auto* text : samples) {
      auto ac_hits = ext.ac_match(text);
      std::unordered_set<std::string> ac_set;
      for (auto& [p, w, c2] : ac_hits) ac_set.insert(w);
      auto dat_set = dat.match(text);
      if (ac_set != dat_set) {
        ++bad;
        std::printf("对拍不一致: AC=%zu DAT=%zu\n", ac_set.size(), dat_set.size());
        for (auto& w : dat_set)
          if (!ac_set.count(w)) std::printf("  DAT 多出: %s\n", w.c_str());
        for (auto& w : ac_set)
          if (!dat_set.count(w)) std::printf("  AC 多出: %s\n", w.c_str());
      }
    }
    std::printf("DAT vs AC 对拍: %s\n", bad == 0 ? "一致" : "不一致");
  }
  return bad ? 2 : 0;
}
