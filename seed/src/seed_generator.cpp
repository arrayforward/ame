/*
 * =============================================================================
 * 所属模块：M5 种子生成器（seed）
 * 定位：联想链的软入口，把任意形式的用户查询转换为图上的一组带权种子节点。
 *
 * 本文件主体思路：
 *   实现 SeedGenerator::gen_seeds。按"硬种子优先、软种子补充"的策略，把
 *   SeedQuery 中的五类线索（uid 直连、强标识符、精确关键词、地理坐标、
 *   自由文本）逐一落到图节点上，汇入同一个 SeedSet。所有候选经统一的去重
 *   合并入口 add：同一节点多次命中时保留最大权重及其来源。
 *
 * 关键算法/数据结构说明：
 *   - 线性扫描去重合并：种子规模小（硬种子若干 + ANN topK 5~10），直接
 *     O(n) 扫描 SeedSet 查重，命中时取 max 权重，无需哈希表。
 *   - 地理通道：调用 M9 的 geo_lookup 在 1km 半径内取最近 Place，权重按
 *     haversine 距离线性衰减（max(0.3, 1 - d/1000)）。
 *   - 软种子通道：M13 embed 文本向量后经 M9 ann_search 召回 topK，
 *     相似度低于 soft_min_sim（默认 0.5）的结果丢弃，权重即相似度。
 *
 * 依赖关系：
 *   依赖 core 公共层（haversine_m 距离函数、Node/NodeKind/Extraction/Vec
 *   等类型）、extractor(M1) 的文本要素提取、keyword(M3) 的关键词归一化与
 *   精确查找、storage(M9) 的节点/属性/geo/ANN 查询、vector(M13) 的文本
 *   向量嵌入。被 engine(M12) 装配调用。
 * =============================================================================
 */
#include "ame/seed/seed_generator.h"

#include "ame/extractor/extractor.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace ame {

// 构造函数：保存 M9 存储、M13 向量服务、M3 关键词管理器的引用，M1 提取器可为空
SeedGenerator::SeedGenerator(Storage& storage, VectorService& vec, KeywordManager& kw,
                             IExtractor* extractor)
    : storage_(storage), vec_(vec), kw_(kw), extractor_(extractor) {}

/*
 * gen_seeds 伪代码：
 * 步骤1：定义合并入口 add(uid, w, src)——uid 为空直接忽略；线性扫描已有种子，
 *        若同 uid 已存在则仅当新权重更大时更新权重与来源，否则追加新种子。
 * 步骤2：硬种子通道（均 weight=1.0，跳过向量通道）：
 *        - 若 query.uid 非空且在 M9 中存在该节点，加入为 hard 种子；
 *        - 逐条强标识符，按 "id:键"=值 在实体节点中精确查找，命中加入；
 *        - 逐个关键词经 M3 归一化后精确查找，命中加入。
 * 步骤3：坐标种子：若带坐标，geo_lookup 取 1km 内最近 Place；若存在则按
 *        haversine 距离 d 赋权 max(0.3, 1 - d/1000)，来源标记 geo。
 * 步骤4：自由文本通道（text 非空时）：
 *        - 若 M1 提取器可用：先 extract 文本，其中实体、关键词、地点名
 *          分别经名称精确查找/关键词查找，命中即视为硬种子（weight=1.0，
 *          来源 text）；
 *        - 软种子：M13 embed 文本得向量，M9 ann_search 召回 topK，
 *          若相似度 < soft_min_sim 则丢弃，否则以相似度为权重加入 soft。
 * 步骤5：返回合并去重后的种子集合。
 */
SeedSet SeedGenerator::gen_seeds(const SeedQuery& q) {
  SeedSet seeds;
  auto add = [&](const std::string& uid, double w, const std::string& src) {
    if (uid.empty()) return;
    for (auto& s : seeds) {  // 合并去重，取最大权重
      if (s.node_uid == uid) {
        if (w > s.weight) { s.weight = w; s.source = src; }
        return;
      }
    }
    seeds.push_back({uid, w, src});
  };

  // ---- 硬种子（weight=1.0，跳过向量通道）----
  if (!q.uid.empty() && storage_.get_node(q.uid)) add(q.uid, 1.0, "hard");
  for (auto& kv : q.identifiers) {
    const Node* n = storage_.find_by_attr(NodeKind::Entity, "id:" + kv.first, kv.second);
    if (n) add(n->uid, 1.0, "hard");
  }
  for (auto& w : q.keywords) {
    const Node* n = kw_.find(kw_.normalize(w));
    if (n) add(n->uid, 1.0, "hard");
  }

  // ---- 坐标种子：geo_lookup 最近 Place ----
  if (q.has_geo) {
    auto places = storage_.geo_lookup(q.lat, q.lng, 1000.0);
    if (!places.empty()) {
      double d = haversine_m(q.lat, q.lng, places.front()->lat, places.front()->lng);
      add(places.front()->uid, std::max(0.3, 1.0 - d / 1000.0), "geo");
    }
  }

  // ---- 自由文本：先过 M1 提取，再两路并行 ----
  if (!q.text.empty()) {
    if (extractor_) {
      Extraction ex = extractor_->extract(q.text);
      for (auto& e : ex.entities) {
        const Node* n = storage_.find_by_name(NodeKind::Entity, e.surface);
        if (n) add(n->uid, 1.0, "text");  // 文本中的实体命中 = 硬种子
      }
      for (auto& k : ex.keywords) {
        const Node* n = kw_.find(kw_.normalize(k.word));
        if (n) add(n->uid, 1.0, "text");
      }
      if (!ex.place.name.empty()) {
        const Node* n = storage_.find_by_name(NodeKind::Place, ex.place.name);
        if (n) add(n->uid, 1.0, "text");
      }
    }
    // 软种子：embed → ANN topK，相似度 <0.5 丢弃，按相似度赋权
    Vec emb = vec_.embed(q.text);
    for (auto& [uid, sim] : storage_.ann_search(emb, q.soft_topk)) {
      if (sim < q.soft_min_sim) continue;
      add(uid, sim, "soft");
    }

    // 词法种子（lex）：查询内容词与事件原文的词面重叠（对哈希向量语义弱的补充通道，
    // 通用机制，BM25-lite：命中词按 IDF 加权，罕见词区分度高）。
    // 伪代码：
    //   步骤1：查询切词——ASCII 小写、长度≥3、去停用词，得内容词集合；
    //   步骤2：第一遍扫描全部 Event 节点文本（同样切词），统计每个查询词的文档频数 df；
    //   步骤3：第二遍按 Σ命中idf/Σ查询idf 归一打分（idf=log(1+N/df)），
    //          收录条件 m≥2 或（m==1 且词长≥6 的罕见词）；
    //   步骤4：权重 = 0.3 + 0.7×归一分，按权重取 top10。
    {
      static const std::unordered_set<std::string> kLexStop = {
          "the", "and", "for", "are", "was", "were", "has", "have", "had", "did",
          "does", "what", "when", "where", "which", "who", "whom", "whose", "how",
          "with", "from", "that", "this", "these", "those", "they", "them", "their",
          "would", "could", "should", "about", "into", "your", "you", "his", "her",
          "she", "him", "not", "but", "all", "any", "can", "will", "just"};
      auto tokenize = [](const std::string& text) {
        std::unordered_set<std::string> out;
        std::string cur;
        auto flush = [&] {
          if (cur.size() >= 3) out.insert(cur);
          cur.clear();
        };
        for (unsigned char c : text) {
          if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
          else flush();
        }
        flush();
        return out;
      };
      auto qtoks = tokenize(q.text);
      std::unordered_set<std::string> qcontent;
      for (auto& w : qtoks)
        if (!kLexStop.count(w)) qcontent.insert(w);
      if (!qcontent.empty()) {
        auto events = storage_.nodes_of_kind(NodeKind::Event);
        std::vector<std::unordered_set<std::string>> etoks;
        etoks.reserve(events.size());
        std::unordered_map<std::string, int> df;
        for (auto* n : events) {
          etoks.push_back(tokenize(n->text));
          for (auto& w : qcontent)
            if (etoks.back().count(w)) ++df[w];
        }
        // 查询侧词典扩展（条件归一）：
        //   默认（无条件）：每个内容词经 M3 反向别名取 ≤3 个同义词形，×0.6 打折；
        //   条件归一：有直接命中（关键词节点或事件词面 df>0）保持原词；人名不扩展；
        //   无命中 → strong 别名 ×0.6，无 strong 再降级 syn_near ×0.4（≤3 个）。
        std::unordered_map<std::string, std::pair<std::string, double>> expand_of;  // 扩展词 -> (原词, 折扣)
        for (auto& w : qcontent) {
          if (q.conditional_normalize) {
            bool direct = df[w] > 0 || kw_.find(kw_.normalize(w)) != nullptr;
            std::string capw = w;
            capw[0] = (char)std::toupper((unsigned char)capw[0]);
            bool is_person = storage_.find_by_name(NodeKind::Entity, capw) != nullptr;
            if (direct || is_person) continue;
            auto strong = kw_.variants(w, 3);
            if (!strong.empty()) {
              for (auto& v : strong)
                if (!qcontent.count(v)) expand_of[v] = {w, 0.6};
            } else {
              for (auto& [v, sc] : kw_.near_words(w, 3))
                if (!qcontent.count(v)) expand_of[v] = {w, 0.4};
            }
          } else {
            for (auto& v : kw_.variants(w, 3))
              if (!qcontent.count(v)) expand_of[v] = {w, 0.6};
          }
        }
        for (auto& [v, w] : expand_of)
          for (auto& t : etoks)
            if (t.count(v)) ++df[v];
        double idf_sum = 0;
        std::unordered_map<std::string, double> idf;
        for (auto& w : qcontent) {
          idf[w] = std::log(1.0 + (double)events.size() / (1.0 + df[w]));
          idf_sum += idf[w];
        }
        for (auto& [v, w] : expand_of)
          idf[v] = std::log(1.0 + (double)events.size() / (1.0 + df[v]));
        std::vector<std::pair<double, std::string>> scored;  // (weight, event uid)
        for (size_t i = 0; i < events.size(); ++i) {
          int m = 0;
          bool rare_hit = false;
          double s = 0;
          for (auto& w : qcontent)
            if (etoks[i].count(w)) {
              ++m;
              s += idf[w];
              if (w.size() >= 6) rare_hit = true;
            }
          for (auto& [v, ws] : expand_of)
            if (etoks[i].count(v)) {
              ++m;  // 扩展命中同样计入（条件归一兜底依赖此路径）
              if (v.size() >= 6) rare_hit = true;
              s += ws.second * idf[v];  // 扩展命中按 strong/near 折扣
            }
          if (m >= 2 || (m == 1 && rare_hit)) {
            double w = 0.3 + 0.7 * (idf_sum > 0 ? s / idf_sum : 0);
            scored.emplace_back(w, events[i]->uid);
          }
        }
        std::sort(scored.begin(), scored.end(), std::greater<>());
        for (size_t i = 0; i < scored.size() && i < (size_t)q.lex_topk; ++i)
          add(scored[i].second, scored[i].first, "lex");
      }

      // CJK 词法种子（中文无分词器：以 M1 词典提取的中文词为内容词，
      // 子串匹配事件原文；同义扩展经别名表，权重规则与 ASCII 通道一致）
      if (extractor_) {
        Extraction cex = extractor_->extract(q.text);
        std::vector<std::string> cjks;
        for (auto& k : cex.keywords)
          if (!k.word.empty() && (unsigned char)k.word[0] >= 128) cjks.push_back(k.word);
        if (!cjks.empty()) {
          auto events = storage_.nodes_of_kind(NodeKind::Event);
          std::unordered_map<std::string, int> df;
          std::unordered_map<std::string, std::pair<std::string, double>> expand_of;
          for (auto& w : cjks) {
            std::string nw = kw_.normalize(w);
            for (auto* n : events)
              if (n->text.find(w) != std::string::npos) ++df[w];
            bool direct = df[w] > 0 || kw_.find(nw) != nullptr;
            if (!q.conditional_normalize || !direct) {
              for (auto& v : kw_.variants(nw, 8))  // CJK 同义组整体扩展（cap 放宽）
                if (v != w) {
                  expand_of[v] = {w, 0.6};
                  for (auto* n : events)
                    if (n->text.find(v) != std::string::npos) ++df[v];
                }
            }
          }
          double idf_sum = 0;
          std::unordered_map<std::string, double> idf;
          for (auto& w : cjks) {
            idf[w] = std::log(1.0 + (double)events.size() / (1.0 + df[w]));
            idf_sum += idf[w];
          }
          for (auto& [v, ws] : expand_of)
            idf[v] = std::log(1.0 + (double)events.size() / (1.0 + df[v]));
          for (auto* n : events) {
            int m = 0;
            double s = 0;
            for (auto& w : cjks)
              if (n->text.find(w) != std::string::npos) {
                ++m;
                s += idf[w];
              }
            for (auto& [v, ws] : expand_of)
              if (n->text.find(v) != std::string::npos) {
                ++m;
                s += ws.second * idf[v];
              }
            if (m >= 1 && s > 0)  // CJK 词多为整词，m≥1 即收录
              add(n->uid, 0.3 + 0.7 * (idf_sum > 0 ? s / idf_sum : 0), "lex");
          }
        }
      }
    }
  }
  return seeds;
}

}  // namespace ame
