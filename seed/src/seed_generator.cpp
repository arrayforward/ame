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
#include "ame/core/json.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ame {

// ---- 词形家族铸边 generator（第六轮：词干"改写"退役后的等价物， graded 化）----
// 从词 w（视作 lemma）正向铸造高频屈折家族：所有格/复数 0.95、过去/进行 0.9；
// 不规则变化不处理；e-尾只加 d（made→mad 类过度生成靠"不生成"避免）。
static void mint_inflections(const std::string& w,
                             std::vector<std::pair<std::string, float>>& out) {
  if (w.size() < 3 || w.size() > 20) return;
  auto is_ascii_alpha = [](const std::string& s) {
    for (unsigned char c : s)
      if (c >= 128 || !std::isalpha(c)) return false;
    return true;
  };
  if (!is_ascii_alpha(w)) return;
  auto add = [&](std::string form, float sc) {
    if (form.size() >= 3 && form != w) out.emplace_back(std::move(form), sc);
  };
  char last = w.back();
  bool es_pl = (last == 's' || last == 'x' || last == 'z' ||
                w.size() >= 2 && (w.compare(w.size() - 2, 2, "ch") == 0 ||
                                  w.compare(w.size() - 2, 2, "sh") == 0));
  bool consonant_y = (last == 'y' && w.size() >= 2 &&
                      !std::strchr("aeiou", w[w.size() - 2]));
  // 所有格/复数档 0.95
  add(w + "'s", 0.95f);
  if (es_pl) add(w + "es", 0.95f);
  else if (consonant_y) add(w.substr(0, w.size() - 1) + "ies", 0.95f);
  else add(w + "s", 0.95f);
  // 过去/进行档 0.9（e 尾：去 e +ing / 直接 +d；CVC 短词双写尾字母；
  // make→made 类不规则不处理——"maked" 边无害，文本中不存在）
  bool e_tail = last == 'e';
  bool dbl = w.size() >= 3 && !std::strchr("aeiou", last) &&
             std::strchr("aeiou", w[w.size() - 2]) &&
             !std::strchr("aeiou", w[w.size() - 3]) && last != 'w' && last != 'x';
  add(e_tail ? w + "d" : dbl ? w + last + "ed" : w + "ed", 0.9f);
  add(e_tail ? w.substr(0, w.size() - 1) + "ing" : dbl ? w + last + "ing" : w + "ing",
      0.9f);
}

// WordEdgeTable::load：蒸馏 sim 边 {"sims": {word: {word2: score}}}（行降序；
// 消费方依赖降序做 0.3 下限截断）。撞词（多源同对）由 add_edge 取 max。
static void sort_row(std::vector<std::pair<std::string, float>>& row) {
  std::sort(row.begin(), row.end(), [](auto& a, auto& b) { return a.second > b.second; });
}

void WordEdgeTable::add_edge(const std::string& a, const std::string& b, float sc) {
  if (sc <= 0.0f || a.empty() || b.empty() || a == b) return;
  auto& row = rows_[a];
  for (auto& [v, s2] : row)
    if (v == b) {
      if (sc > s2) s2 = sc;  // 撞词取 max（三类边共表语义）
      return;
    }
  row.emplace_back(b, sc);
  ++edges_;
}

bool WordEdgeTable::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return false;
  const Json* sims = root.find("sims");
  if (!sims || sims->kind != Json::Obj) return false;
  for (auto& [w, rowj] : sims->obj) {
    if (rowj.kind != Json::Obj) continue;
    for (auto& [v, scj] : rowj.obj) add_edge(w, v, (float)scj.as_num(0.0));
  }
  for (auto& [w, row] : rows_) sort_row(row);
  return true;
}

// load_antonyms：蒸馏词库 antonyms 流 ×0.35 折入（反义弱联想档）。幂等（撞词取 max）。
bool WordEdgeTable::load_antonyms(const std::string& lexicon_path) {
  std::ifstream f(lexicon_path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return false;
  const Json* entries = root.find("entries");
  if (!entries) return false;
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    const Json* ant = e.find("antonyms");
    if (!w || !ant) continue;
    for (auto& s : ant->arr) add_edge(w->as_str(), s.as_str(), 0.35f);
  }
  for (auto& [w, row] :rows_) sort_row(row);
  return true;
}

// sim(a,b)：存表边双向查 + 现场铸边（a 家族含 b 或 b 家族含 a），取 max。
float WordEdgeTable::sim(const std::string& a, const std::string& b) const {
  float best = 0.0f;
  auto it = rows_.find(a);
  if (it != rows_.end())
    for (auto& [v, sc] : it->second)
      if (v == b && sc > best) best = sc;
  auto jt = rows_.find(b);
  if (jt != rows_.end())
    for (auto& [v, sc] : jt->second)
      if (v == a && sc > best) best = sc;
  if (best >= 0.95f) return best;  // 不可能被铸边超越
  std::vector<std::pair<std::string, float>> fam;
  mint_inflections(a, fam);
  for (auto& [v, sc] : fam)
    if (v == b && sc > best) best = sc;
  if (best >= 0.95f) return best;
  fam.clear();
  mint_inflections(b, fam);
  for (auto& [v, sc] : fam)
    if (v == a && sc > best) best = sc;
  return best;
}

// edges_of(w)：存表行 ∪ 现场铸造的词形家族，按 score 降序合并（撞词取 max）。
std::vector<std::pair<std::string, float>> WordEdgeTable::edges_of(
    const std::string& w) const {
  std::vector<std::pair<std::string, float>> out;
  auto it = rows_.find(w);
  if (it != rows_.end()) out = it->second;
  std::vector<std::pair<std::string, float>> fam;
  mint_inflections(w, fam);
  for (auto& [v, sc] : fam) {
    bool dup = false;
    for (auto& [v2, sc2] : out)
      if (v2 == v) {
        if (sc > sc2) sc2 = sc;
        dup = true;
        break;
      }
    if (!dup) out.emplace_back(v, sc);
  }
  sort_row(out);
  return out;
}

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
    std::unordered_set<std::string> name_canons;  // 人名归一：alias_of 命中的 canonical 集合
    if (extractor_) {
      Extraction ex = extractor_->extract(q.text);
      for (auto& e : ex.entities) {
        // 人名/专名归一（--name-alias 域）：称呼/昵称类实体词面先经别名表精确命中
        // 判定（alias_of，区别于 normalize 的小写化）。命中时：
        // ① canonical 实体节点入硬种子（INVOLVES 通道；CONTAINS/INVOLVES 传导弱，
        //    实体种子只作补充，见 ②）；
        // ② canonical 词面注入下方 CJK 词法通道的扩展集合（name_canons）——事件按
        //    变体子串直接成种子（lex 权重），这是称呼类变体达事件的主通道
        //    （如查询"小李说过什么"圈定"李建国"全部轮次）。
        // 未加载姓名别名表时 alias_of 必返回空串，行为与历史逐行一致（零回归）。
        std::string canon = kw_.alias_of(e.surface);
        const Node* n = canon.empty() ? nullptr
                                      : storage_.find_by_name(NodeKind::Entity, canon);
        if (!n) n = storage_.find_by_name(NodeKind::Entity, e.surface);
        if (n) add(n->uid, 1.0, "text");  // 文本中的实体命中 = 硬种子
        if (!canon.empty()) {
          const Node* k = kw_.find(canon);
          if (k) add(k->uid, 1.0, "text");
          name_canons.insert(canon);
        }
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
        // 第六轮终局：WordEdgeTable 非空时整段旁路（统一 graded 边表，单边消费），
        // 词形等价不再走 normalize 改写，由 edges_of 现场铸 0.9/0.95 分边承载。
        const bool use_sim = q.word_edges != nullptr;
        // tok_best：事件词 -> (最佳命中分, 所属查询词)；边表路径预计算（跨查询词取 max）。
        // 预注册下限 0.3：LLM 1-3/10 分（仅弱相关）不触发，防高频假阳性。
        std::unordered_map<std::string, std::pair<double, std::string>> tok_best;
        std::unordered_map<std::string, std::pair<std::string, double>> expand_of;  // 扩展词 -> (原词, 折扣)
        if (use_sim) {
          for (auto& w : qcontent) {
            // 查询侧：词以小写原词查表（词形家族由 edges_of 现场铸边覆盖）
            for (auto& [v, sc] : q.word_edges->edges_of(w)) {
              if (sc < 0.3) break;  // 行按 score 降序，首个 <0.3 即截断
              auto& tb = tok_best[v];
              if (sc > tb.first) tb = {(double)sc, w};
            }
          }
        } else {
          for (auto& w : qcontent) {
            // R2 查询侧：查询词先过 M3 归一（词干+别名）， loves→love 后取 love 的别名组，
            // 词形变化（loves/loved）作为别名精确匹配事件词面
            const std::string nw = kw_.normalize(w);
            if (q.conditional_normalize) {
              bool direct = df[w] > 0 || df[nw] > 0 || kw_.find(nw) != nullptr;
              std::string capw = w;
              capw[0] = (char)std::toupper((unsigned char)capw[0]);
              bool is_person = storage_.find_by_name(NodeKind::Entity, capw) != nullptr;
              if (direct || is_person) continue;
              auto strong = kw_.variants(nw, 3);
              if (!strong.empty()) {
                for (auto& v : strong)
                  if (!qcontent.count(v)) expand_of[v] = {w, 0.6};
              } else {
                for (auto& [v, sc] : kw_.near_words(nw, 3))
                  if (!qcontent.count(v)) expand_of[v] = {w, 0.4};
              }
            } else {
              for (auto& v : kw_.variants(nw, 3))
                if (!qcontent.count(v)) expand_of[v] = {w, 0.6};
            }
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
          if (!use_sim) {
            // ---- 历史路径（word_edges 为空 = 行为逐行不变）----
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
            continue;
          }
          // ---- 软匹配路径（第五轮 pivot）：命中分 = max(直接 1.0, sim(q,v)) ----
          // hit：查询词 -> (最佳命中分, 命中词元)。预注册 m 定义：命中分 ≥0.5 的
          // 查询词计一个内容词命中；s = Σ idf[w]×命中分（每查询词取最佳词元，防重复计）。
          std::unordered_map<std::string, std::pair<double, std::string>> hit;
          for (auto& w : qcontent)
            if (etoks[i].count(w)) hit[w] = {1.0, w};
          for (auto& t : etoks[i]) {
            if (qcontent.count(t)) continue;  // 直接命中已计（1.0 即上限）
            auto it = tok_best.find(t);
            if (it == tok_best.end()) continue;
            auto& h = hit[it->second.second];
            if (it->second.first > h.first) h = it->second;
          }
          int m = 0;
          bool rare_hit = false;
          double s = 0;
          for (auto& [w2, h] : hit) {
            s += h.first * idf[w2];
            if (h.first >= 0.5) {
              ++m;
              if (h.second.size() >= 6) rare_hit = true;
            }
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
      {
        Extraction cex = extractor_ ? extractor_->extract(q.text) : Extraction{};
        std::vector<std::string> cjks;
        for (auto& k : cex.keywords)
          if (!k.word.empty() && (unsigned char)k.word[0] >= 128) cjks.push_back(k.word);
        // 人名归一：alias_of 命中的 canonical 词面并入内容词——提问里的称呼/昵称
        // （老X/小X 提取为实体、不作关键词）由此把全变体轮次直接种子化；canonical
        // 自身经全变体扩展（variants），覆盖非称呼变体（李经理/建国）轮次。
        // name_canons 仅在别名表命中时非空（--name-alias 域），off 时零行为差。
        for (auto& c : name_canons)
          if (std::find(cjks.begin(), cjks.end(), c) == cjks.end()) cjks.push_back(c);
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
        } else {
          // 词典未加载/Android 端等场景 M1 提不出中文词时，退回查询文本的
          // CJK 字符二元组做子串匹配（不扩展同义词）；≥2 个二元组命中才收录，
          // 防"的/是/了"周边二元组单命中噪声。纯兜底通道，词典在时不会走到。
          std::vector<std::string> grams;
          auto push_cp = [&](unsigned cp, std::string& out) {
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
              out += (char)(0xC0 | (cp >> 6));
              out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
              out += (char)(0xE0 | (cp >> 12));
              out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            } else {
              out += (char)(0xF0 | (cp >> 18));
              out += (char)(0x80 | ((cp >> 12) & 0x3F));
              out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            }
          };
          std::vector<unsigned> cps;
          for (size_t i = 0; i < q.text.size();) {
            unsigned char c = (unsigned char)q.text[i];
            unsigned cp;
            size_t len;
            if (c < 0x80) { cp = c; len = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
            else { ++i; continue; }
            bool ok = i + len <= q.text.size();
            for (size_t j = 1; ok && j < len; ++j) {
              unsigned char cc = (unsigned char)q.text[i + j];
              if ((cc & 0xC0) != 0x80) ok = false;
              else cp = (cp << 6) | (cc & 0x3F);
            }
            i += ok ? len : 1;
            if (ok && cp > 0x2E7F) cps.push_back(cp);
          }
          for (size_t i = 0; i + 1 < cps.size(); ++i) {
            std::string g;
            push_cp(cps[i], g);
            push_cp(cps[i + 1], g);
            grams.push_back(g);
          }
          if (grams.empty() && cps.size() == 1) {
            std::string g;
            push_cp(cps[0], g);
            grams.push_back(g);
          }
          if (!grams.empty()) {
            auto events = storage_.nodes_of_kind(NodeKind::Event);
            std::unordered_map<std::string, int> df;
            for (auto& g : grams)
              for (auto* n : events)
                if (n->text.find(g) != std::string::npos) ++df[g];
            double idf_sum = 0;
            std::unordered_map<std::string, double> idf;
            for (auto& g : grams) {
              idf[g] = std::log(1.0 + (double)events.size() / (1.0 + df[g]));
              idf_sum += idf[g];
            }
            for (auto* n : events) {
              int m = 0;
              double s = 0;
              for (auto& g : grams)
                if (n->text.find(g) != std::string::npos) {
                  ++m;
                  s += idf[g];
                }
              if ((m >= 2 || grams.size() == 1) && s > 0)
                add(n->uid, 0.3 + 0.7 * (idf_sum > 0 ? s / idf_sum : 0), "lex");
            }
          }
        }
      }
    }
  }
  return seeds;
}

}  // namespace ame
